// mf3110usb: minimal CUPS USB backend for Canon MF3110 (CARPS)
// Env vars for testing:
//   MF3110_IFACE=0|1      (default 1)  select printer interface
//   MF3110_CHUNK=N        (default 1=512B; >1=N-byte chunks; 0=64KB)
//   MF3110_SOFT_RESET=1   (default off) send USB Printer Class SOFT_RESET
//   MF3110_ZLP=1          (default off) send zero-length packet if total%512==0
//   MF3110_CLOSE=1        (default off) explicitly close USB interface/device
//   MF3110_HOLD=N         (default 2)   seconds to hold USB open after write
//   MF3110_REENUM=1       (default off) re-enumerate USB device before write
// One-shot:  sudo ./mf3110usb [--iface0] [--soft-reset] [--zlp] [--close] [--hold N] [--reenum] <file>
// Probe:     sudo ./mf3110usb probe
// CUPS:      installed as /usr/libexec/cups/backend/mf3110usb
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <signal.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#define VID 0x04A9
#define PID 0x2660
#define WANT_SERIAL "235AT2134669"
#define DEFAULT_SOCK "/var/run/mf3110usb.sock"

static void logmsg(const char *fmt, ...) {
    va_list ap;
    fputs("DEBUG: mf3110usb: ", stderr);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr); fflush(stderr);
}

static IOUSBDeviceInterface **find_device(void) {
    const char *classes[] = { "IOUSBHostDevice", kIOUSBDeviceClassName };
    for (int ci = 0; ci < 2; ci++) {
        CFMutableDictionaryRef match = IOServiceMatching(classes[ci]);
        if (!match) continue;
        io_iterator_t iter = 0;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) != KERN_SUCCESS) continue;
        io_service_t svc;
        while ((svc = IOIteratorNext(iter))) {
            IOCFPlugInInterface **plugin = NULL; SInt32 score = 0; IOUSBDeviceInterface **dev = NULL;
            if (IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
                    kIOCFPlugInInterfaceID, &plugin, &score) == KERN_SUCCESS && plugin) {
                (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&dev);
                (*plugin)->Release(plugin);
            }
            IOObjectRelease(svc);
            if (!dev) continue;
            UInt16 vid = 0, pid = 0; (*dev)->GetDeviceVendor(dev, &vid); (*dev)->GetDeviceProduct(dev, &pid);
            if (vid == VID && pid == PID) { logmsg("found %04x:%04x", vid, pid); IOObjectRelease(iter); return dev; }
            (*dev)->Release(dev);
        }
        IOObjectRelease(iter);
    }
    return NULL;
}

static int probe(IOUSBDeviceInterface **dev) {
    UInt8 nconf = 0; (*dev)->GetNumberOfConfigurations(dev, &nconf);
    IOUSBConfigurationDescriptorPtr cfg = NULL;
    if ((*dev)->GetConfigurationDescriptorPtr(dev, 0, &cfg) != kIOReturnSuccess || !cfg) { printf("no config descriptor\n"); return 1; }
    const unsigned char *p = (const unsigned char *)cfg;
    int total = p[2] | (p[3] << 8);
    printf("configs=%u bConfigurationValue=%u bNumInterfaces=%u wTotalLength=%d\n", nconf, p[5], p[4], total);
    for (int i = 0; i + 2 <= total; ) {
        int len = p[i], type = p[i+1]; if (len == 0) break;
        if (type == 4 && i + 9 <= total)
            printf("  IFACE num=%u alt=%u class=%u sub=%u proto=%u nEP=%u\n", p[i+2],p[i+3],p[i+5],p[i+6],p[i+7],p[i+4]);
        else if (type == 5 && i + 6 <= total) {
            int addr = p[i+2], attr = p[i+3] & 3; const char *tt = attr==2?"bulk":attr==3?"intr":attr==1?"iso":"ctrl";
            printf("      EP 0x%02x %-3s %s max=%u\n", addr, (addr&0x80)?"IN":"OUT", tt, p[i+4]|(p[i+5]<<8));
        }
        i += len;
    }
    return 0;
}

// Check if interface `wantIf` has a classic printer (7/1/1|2) bulk-OUT endpoint
static int find_classic(IOUSBDeviceInterface **dev, UInt8 wantIf, UInt8 *alt) {
    IOUSBConfigurationDescriptorPtr cfg = NULL;
    if ((*dev)->GetConfigurationDescriptorPtr(dev, 0, &cfg) != kIOReturnSuccess || !cfg) return 0;
    const unsigned char *p = (const unsigned char *)cfg; int total = p[2]|(p[3]<<8);
    int curCls=-1,curSub=-1,curProto=-1; UInt8 curIf=0,curAlt=0;
    for (int i=0; i+2<=total;) {
        int len=p[i], type=p[i+1]; if(!len) break;
        if (type==4 && i+9<=total) { curIf=p[i+2]; curAlt=p[i+3]; curCls=p[i+5]; curSub=p[i+6]; curProto=p[i+7]; }
        else if (type==5 && i+6<=total) {
            int addr=p[i+2], bulk=((p[i+3]&3)==2), out=!(addr&0x80);
            if (curCls==7 && curSub==1 && (curProto==1||curProto==2) && bulk && out && curIf == wantIf) {
                *alt=curAlt; return 1;
            }
        }
        i += len;
    }
    return 0;
}

static IOUSBInterfaceInterface220 **open_printer_iface(IOUSBDeviceInterface **dev, UInt8 *outPipe, UInt8 *inPipe, UInt8 wantIf) {
    *outPipe = 0; *inPipe = 0;
    IOReturn od = (*dev)->USBDeviceOpenSeize(dev);
    if (od != kIOReturnSuccess) {
        logmsg("USBDeviceOpenSeize -> 0x%08x, trying USBDeviceOpen", od);
        od = (*dev)->USBDeviceOpen(dev);
        logmsg("USBDeviceOpen -> 0x%08x", od);
    } else {
        logmsg("USBDeviceOpenSeize -> 0x%08x", od);
    }

    UInt8 wantAlt=0; int have=find_classic(dev, wantIf, &wantAlt);
    logmsg("target iface=%u classic=%d alt=%u", wantIf, have, wantAlt);

    IOUSBFindInterfaceRequest req = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
    io_iterator_t iter=0;
    if ((*dev)->CreateInterfaceIterator(dev,&req,&iter) != kIOReturnSuccess) return NULL;
    IOUSBInterfaceInterface220 **result=NULL; io_service_t usbIf;
    while ((usbIf = IOIteratorNext(iter))) {
        IOCFPlugInInterface **plugin=NULL; SInt32 score=0; IOUSBInterfaceInterface220 **intf=NULL;
        if (IOCreatePlugInInterfaceForService(usbIf, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin,&score)==kIOReturnSuccess && plugin) {
            (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID220),(LPVOID*)&intf); (*plugin)->Release(plugin);
        }
        IOObjectRelease(usbIf); if(!intf) continue;
        UInt8 cls=0,sub=0,proto=0,altNum=0,ifNum=0; (*intf)->GetInterfaceClass(intf,&cls); (*intf)->GetInterfaceSubClass(intf,&sub); (*intf)->GetInterfaceProtocol(intf,&proto); (*intf)->GetAlternateSetting(intf,&altNum); (*intf)->GetInterfaceNumber(intf,&ifNum);
        logmsg("  seen iface num=%u class=%u sub=%u proto=%u alt=%u", ifNum,cls,sub,proto,altNum);
        if (ifNum == wantIf) {
            IOReturn ir = (*intf)->USBInterfaceOpenSeize(intf);
            logmsg("  seize iface %u -> 0x%08x", ifNum, ir);
            if (ir != kIOReturnSuccess) { (*intf)->Release(intf); continue; }
            if (have && altNum != wantAlt) {
                IOReturn ar = (*intf)->SetAlternateInterface(intf,wantAlt);
                logmsg("  SetAlternateInterface(%u)->0x%08x", wantAlt, ar);
            } else {
                logmsg("  alt already %u, skipping SetAlternateInterface", altNum);
            }
            UInt8 n=0; (*intf)->GetNumEndpoints(intf,&n);
            for (UInt8 pipe=1; pipe<=n; pipe++) {
                UInt8 dir=0,num=0,tt=0,interval=0; UInt16 mps=0;
                (*intf)->GetPipeProperties(intf,pipe,&dir,&num,&tt,&mps,&interval);
                logmsg("    pipe=%u ep=0x%02x dir=%u tt=%u mps=%u", pipe,num,dir,tt,mps);
                if (dir==kUSBOut && tt==kUSBBulk) { *outPipe=pipe; logmsg("  -> bulk OUT pipe %u (ep 0x%02x)", pipe,num); }
                if (dir==kUSBIn  && tt==kUSBBulk) { *inPipe=pipe;  logmsg("  -> bulk IN  pipe %u (ep 0x%02x)", pipe,num); }
            }
            if (*outPipe) { result = intf; break; }
            (*intf)->USBInterfaceClose(intf);
        }
        (*intf)->Release(intf);
    }
    IOObjectRelease(iter);
    return result;
}

static void printer_ctrl_requests(IOUSBInterfaceInterface220 **intf, UInt8 wantIf, int doSoftReset) {
    // 1. USB Printer Class GET_DEVICE_ID (bmRequestType=0xA1, bRequest=0, wValue=0, wIndex=wantIf<<8)
    unsigned char devid[1024];
    memset(devid, 0, sizeof(devid));
    IOUSBDevRequest reqId = {
        .bmRequestType = USBmakebmRequestType(kUSBIn, kUSBClass, kUSBInterface),
        .bRequest = 0,
        .wValue = 0,
        .wIndex = (UInt16)(wantIf << 8),
        .wLength = sizeof(devid) - 1,
        .pData = devid,
        .wLenDone = 0
    };
    IOReturn rId = (*intf)->ControlRequest(intf, 0, &reqId);
    if (rId == kIOReturnSuccess && reqId.wLenDone >= 2) {
        UInt16 idLen = ((UInt16)devid[0] << 8) | devid[1];
        if (idLen > 2 && idLen <= reqId.wLenDone) {
            devid[idLen < sizeof(devid) ? idLen : sizeof(devid) - 1] = '\0';
            logmsg("GET_DEVICE_ID -> ok (%u bytes): %s", (unsigned)reqId.wLenDone, (char *)(devid + 2));
        } else {
            logmsg("GET_DEVICE_ID -> ok (%u bytes, idLen=%u)", (unsigned)reqId.wLenDone, (unsigned)idLen);
        }
    } else {
        logmsg("GET_DEVICE_ID -> 0x%08x (len=%u)", rId, (unsigned)reqId.wLenDone);
    }

    // 2. USB Printer Class GET_PORT_STATUS (bmRequestType=0xA1, bRequest=1, wValue=0, wIndex=wantIf)
    UInt8 portStatus = 0;
    IOUSBDevRequest reqSt = {
        .bmRequestType = USBmakebmRequestType(kUSBIn, kUSBClass, kUSBInterface),
        .bRequest = 1,
        .wValue = 0,
        .wIndex = wantIf,
        .wLength = 1,
        .pData = &portStatus,
        .wLenDone = 0
    };
    IOReturn rSt = (*intf)->ControlRequest(intf, 0, &reqSt);
    logmsg("GET_PORT_STATUS -> 0x%08x status=0x%02x (paper_empty=%u selected=%u not_error=%u)",
           rSt, portStatus, (portStatus >> 5) & 1, (portStatus >> 4) & 1, (portStatus >> 3) & 1);

    // 3. Optional USB Printer Class SOFT_RESET (bmRequestType=0x21, bRequest=2, wValue=0, wIndex=wantIf)
    if (doSoftReset) {
        IOUSBDevRequest reqRst = {
            .bmRequestType = USBmakebmRequestType(kUSBOut, kUSBClass, kUSBInterface),
            .bRequest = 2,
            .wValue = 0,
            .wIndex = wantIf,
            .wLength = 0,
            .pData = NULL,
            .wLenDone = 0
        };
        IOReturn rRst = (*intf)->ControlRequest(intf, 0, &reqRst);
        logmsg("SOFT_RESET -> 0x%08x", rRst);
        usleep(100000);
        portStatus = 0; reqSt.wLenDone = 0;
        rSt = (*intf)->ControlRequest(intf, 0, &reqSt);
        logmsg("GET_PORT_STATUS (post-reset) -> 0x%08x status=0x%02x", rSt, portStatus);
    }
}

static int send_job(int fd) {
    const char *ifaceEnv = getenv("MF3110_IFACE");
    UInt8 wantIf = ifaceEnv ? (UInt8)atoi(ifaceEnv) : 1;
    int doSoftReset = getenv("MF3110_SOFT_RESET") ? atoi(getenv("MF3110_SOFT_RESET")) : 0;
    int doZlp = getenv("MF3110_ZLP") ? atoi(getenv("MF3110_ZLP")) : 0;
    int doClose = getenv("MF3110_CLOSE") ? atoi(getenv("MF3110_CLOSE")) : 0;
    int doReenum = getenv("MF3110_REENUM") ? atoi(getenv("MF3110_REENUM")) : 0;
    int holdSec = getenv("MF3110_HOLD") ? atoi(getenv("MF3110_HOLD")) : 2;
    int chunkEnv = getenv("MF3110_CHUNK") ? atoi(getenv("MF3110_CHUNK")) : 4096;
    size_t chunkSize = (chunkEnv == 1) ? 512 : (chunkEnv > 1 ? (size_t)chunkEnv : 0);

    logmsg("config: iface=%u chunk=%zu soft_reset=%d zlp=%d close=%d hold=%ds reenum=%d",
           wantIf, chunkSize, doSoftReset, doZlp, doClose, holdSec, doReenum);

    IOUSBDeviceInterface **dev = find_device();
    if (!dev) { fprintf(stderr,"ERROR: mf3110usb: printer not found\n"); return 1; }

    if (doReenum) {
        IOReturn od = (*dev)->USBDeviceOpenSeize(dev);
        logmsg("reenum: USBDeviceOpenSeize -> 0x%08x", od);
        IOReturn rr = (*dev)->USBDeviceReEnumerate(dev, 0);
        logmsg("reenum: USBDeviceReEnumerate -> 0x%08x", rr);
        (*dev)->Release(dev);
        dev = NULL;
        for (int attempt = 0; attempt < 20; attempt++) {
            usleep(500000);
            dev = find_device();
            if (dev) { logmsg("reenum: re-found device after %d ms", (attempt + 1) * 500); break; }
        }
        if (!dev) { fprintf(stderr,"ERROR: mf3110usb: printer not found after reenum\n"); return 1; }
    }

    UInt8 outPipe=0, inPipe=0; IOUSBInterfaceInterface220 **intf = open_printer_iface(dev,&outPipe,&inPipe,wantIf);
    if (!intf || !outPipe) { fprintf(stderr,"ERROR: mf3110usb: no bulk-OUT on iface %u\n", wantIf); if(intf){(*intf)->Release(intf);} (*dev)->USBDeviceClose(dev); (*dev)->Release(dev); return 1; }

    printer_ctrl_requests(intf, wantIf, doSoftReset);

    IOReturn cOut = (*intf)->ClearPipeStallBothEnds(intf, outPipe);
    logmsg("ClearPipeStallBothEnds(outPipe=%u) -> 0x%08x", outPipe, cOut);
    if (inPipe) {
        IOReturn cIn = (*intf)->ClearPipeStallBothEnds(intf, inPipe);
        logmsg("ClearPipeStallBothEnds(inPipe=%u) -> 0x%08x", inPipe, cIn);
    }

    usleep(100000);

    unsigned char buf[65536]; ssize_t n; size_t total=0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (chunkSize > 0) {
            size_t off = 0;
            while (off < (size_t)n) {
                UInt32 w = (UInt32)(((size_t)n - off > chunkSize) ? chunkSize : ((size_t)n - off));
                IOReturn r = (*intf)->WritePipe(intf, outPipe, buf + off, w);
                if (r != kIOReturnSuccess) {
                    fprintf(stderr, "ERROR: mf3110usb: WritePipe failed 0x%08x after %zu bytes (chunk=%u)\n", r, total, w);
                    return 1;
                }
                off += w; total += w;
            }
        } else {
            UInt32 w = (UInt32)n;
            IOReturn r = (*intf)->WritePipe(intf, outPipe, buf, w);
            if (r != kIOReturnSuccess) {
                fprintf(stderr, "ERROR: mf3110usb: WritePipe failed 0x%08x after %zu bytes (single=%u)\n", r, total, w);
                return 1;
            }
            total += w;
        }
    }
    if (doZlp && total > 0 && (total % 512) == 0) {
        IOReturn zr = (*intf)->WritePipe(intf, outPipe, buf, 0);
        logmsg("sent ZLP (total=%zu %% 512 == 0) -> 0x%08x", total, zr);
    }
    logmsg("wrote %zu bytes total (chunk=%zu)", total, chunkSize);

    // Drain bulk-IN (short 2s window) to see if device sends any status/ack
    if (inPipe) {
        unsigned char rb[512];
        for (int i = 0; i < 4; i++) {
            UInt32 rlen = sizeof(rb);
            IOReturn rr = (*intf)->ReadPipeTO(intf, inPipe, rb, &rlen, 500, 500);
            logmsg("  ReadPipeTO[%d] -> 0x%08x len=%u", i, rr, (unsigned)rlen);
            if (rr != kIOReturnSuccess || rlen == 0) break;
        }
    }

    if (holdSec > 0) {
        logmsg("holding USB open for %ds...", holdSec);
        sleep((unsigned)holdSec);
    }
    if (doClose) {
        IOReturn ic = (*intf)->USBInterfaceClose(intf);
        (*intf)->Release(intf);
        IOReturn dc = (*dev)->USBDeviceClose(dev);
        (*dev)->Release(dev);
        logmsg("closed USB interface (0x%08x) and device (0x%08x)", ic, dc);
    } else {
        logmsg("exiting without explicit USB close");
    }
    return 0;
}

static int write_all_to_pipe(IOUSBInterfaceInterface220 **intf, UInt8 outPipe, int fd, size_t chunkSize) {
    size_t step = (chunkSize > 0) ? chunkSize : 4096;
    int delayUs = getenv("MF3110_DELAY_US") ? atoi(getenv("MF3110_DELAY_US")) : 0;
    unsigned char *buf = malloc(step);
    if (!buf) return -1;
    size_t fill = 0, total = 0;
    ssize_t n;
    while ((n = read(fd, buf + fill, step - fill)) > 0) {
        fill += (size_t)n;
        if (fill == step) {
            IOReturn r = (*intf)->WritePipe(intf, outPipe, buf, (UInt32)fill);
            if (r != kIOReturnSuccess) {
                fprintf(stderr, "ERROR: mf3110usb: WritePipe failed 0x%08x after %zu bytes (chunk=%zu)\n", r, total, fill);
                free(buf);
                return -1;
            }
            total += fill;
            fill = 0;
            if (delayUs > 0) usleep((useconds_t)delayUs);
        }
    }
    if (fill > 0) {
        IOReturn r = (*intf)->WritePipe(intf, outPipe, buf, (UInt32)fill);
        if (r != kIOReturnSuccess) {
            fprintf(stderr, "ERROR: mf3110usb: WritePipe failed 0x%08x after %zu bytes (tail=%zu)\n", r, total, fill);
            free(buf);
            return -1;
        }
        total += fill;
    }
    free(buf);
    logmsg("wrote %zu bytes total (chunk=%zu)", total, step);
    return 0;
}

static int run_daemon(const char *sockPath) {
    signal(SIGPIPE, SIG_IGN);
    const char *ifaceEnv = getenv("MF3110_IFACE");
    UInt8 wantIf = ifaceEnv ? (UInt8)atoi(ifaceEnv) : 1;
    int chunkEnv = getenv("MF3110_CHUNK") ? atoi(getenv("MF3110_CHUNK")) : 4096;
    size_t chunkSize = (chunkEnv == 1) ? 512 : (chunkEnv > 1 ? (size_t)chunkEnv : 0);

    unlink(sockPath);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockPath, sizeof(addr.sun_path) - 1);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); close(srv); return 1; }
    chmod(sockPath, 0666);
    if (listen(srv, 4) < 0) { perror("listen"); close(srv); return 1; }
    logmsg("daemon: listening on %s", sockPath);

    IOUSBDeviceInterface **dev = NULL;
    UInt8 outPipe = 0, inPipe = 0;
    IOUSBInterfaceInterface220 **intf = NULL;

    for (;;) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) continue;
        logmsg("daemon: accepted job connection");

        // Verify existing USB handle is still alive (handles printer power-cycle/replug)
        if (intf) {
            UInt8 st = 0;
            IOUSBDevRequest reqSt = {
                .bmRequestType = USBmakebmRequestType(kUSBIn, kUSBClass, kUSBInterface),
                .bRequest = 1, .wValue = 0, .wIndex = wantIf, .wLength = 1, .pData = &st, .wLenDone = 0
            };
            IOReturn rSt = (*intf)->ControlRequest(intf, 0, &reqSt);
            if (rSt != kIOReturnSuccess) {
                logmsg("daemon: stale USB handle (0x%08x), releasing and re-opening...", rSt);
                (*intf)->Release(intf); intf = NULL;
                (*dev)->Release(dev); dev = NULL;
            }
        }

        if (!intf) {
            dev = find_device();
            if (dev) {
                intf = open_printer_iface(dev, &outPipe, &inPipe, wantIf);
                if (intf && outPipe) {
                    printer_ctrl_requests(intf, wantIf, 0);
                } else {
                    if (intf) { (*intf)->Release(intf); intf = NULL; }
                    (*dev)->USBDeviceClose(dev); (*dev)->Release(dev); dev = NULL;
                }
            }
        }
        if (!intf || !outPipe) {
            fprintf(stderr, "ERROR: mf3110usb: printer not available\n");
            close(cli);
            continue;
        }

        (*intf)->ClearPipeStallBothEnds(intf, outPipe);
        if (inPipe) (*intf)->ClearPipeStallBothEnds(intf, inPipe);

        if (write_all_to_pipe(intf, outPipe, cli, chunkSize) < 0) {
            logmsg("daemon: write failed, resetting USB handle for next job");
            (*intf)->USBInterfaceClose(intf); (*intf)->Release(intf); intf = NULL;
            (*dev)->USBDeviceClose(dev); (*dev)->Release(dev); dev = NULL;
        } else {
            // Hold client socket open briefly so CUPS does not start the next queued job
            // while the printer is still feeding/fusing the current job.
            int cooldown = getenv("MF3110_COOLDOWN") ? atoi(getenv("MF3110_COOLDOWN")) : 8;
            if (cooldown > 0) {
                logmsg("daemon: waiting %ds cooldown before releasing CUPS job...", cooldown);
                sleep((unsigned)cooldown);
            }
        }
        close(cli);
    }
    return 0;
}

static int send_to_socket(const char *sockPath, int fd) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockPath, sizeof(addr.sun_path) - 1);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("connect"); close(s); return 1; }
    unsigned char buf[65536];
    ssize_t n;
    size_t total = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t w = write(s, buf + off, (size_t)n - off);
            if (w <= 0) { perror("write socket"); close(s); return 1; }
            off += (size_t)w; total += (size_t)w;
        }
    }
    shutdown(s, SHUT_WR);
    while (read(s, buf, sizeof(buf)) > 0) {}
    close(s);
    logmsg("sent %zu bytes to daemon socket %s", total, sockPath);
    return 0;
}

int main(int argc, char **argv) {
    const char *daemonSock = NULL;
    const char *sendSock = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iface0") == 0) setenv("MF3110_IFACE", "0", 1);
        else if (strcmp(argv[i], "--soft-reset") == 0) setenv("MF3110_SOFT_RESET", "1", 1);
        else if (strcmp(argv[i], "--zlp") == 0) setenv("MF3110_ZLP", "1", 1);
        else if (strcmp(argv[i], "--close") == 0) setenv("MF3110_CLOSE", "1", 1);
        else if (strcmp(argv[i], "--reenum") == 0) setenv("MF3110_REENUM", "1", 1);
        else if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc) setenv("MF3110_HOLD", argv[++i], 1);
        else if (strcmp(argv[i], "--daemon") == 0 && i + 1 < argc) daemonSock = argv[++i];
        else if (strcmp(argv[i], "--send") == 0 && i + 1 < argc) sendSock = argv[++i];
    }
    if (daemonSock) return run_daemon(daemonSock);
    if (sendSock) {
        const char *fname = NULL;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--send") == 0 || strcmp(argv[i], "--hold") == 0) { i++; continue; }
            if (argv[i][0] != '-') fname = argv[i];
        }
        int infd = fname ? open(fname, O_RDONLY) : 0;
        if (infd < 0) { perror("open job file"); return 1; }
        int rc = send_to_socket(sendSock, infd);
        if (infd != 0) close(infd);
        return rc;
    }
    const char *uri = getenv("DEVICE_URI");
    int isBackend = (uri && strstr(uri, "mf3110usb") != NULL) || strstr(argv[0], "mf3110usb") != NULL;

    if (argc >= 2 && strcmp(argv[argc-1], "probe") == 0) {
        IOUSBDeviceInterface **dev = find_device(); if(!dev){printf("printer not found\n"); return 1;}
        int rc = probe(dev);
        UInt8 outPipe=0, inPipe=0;
        IOUSBInterfaceInterface220 **intf = open_printer_iface(dev, &outPipe, &inPipe, 1);
        if (intf) {
            printer_ctrl_requests(intf, 1, 0);
            (*intf)->USBInterfaceClose(intf);
            (*intf)->Release(intf);
        }
        (*dev)->USBDeviceClose(dev);
        (*dev)->Release(dev);
        return rc;
    }

    if (isBackend) {
        int infd = 0;
        if (argc >= 6) {
            // CUPS backend invocation with filter chain: always read filtered CARPS from stdin
            infd = 0;
        } else {
            // One-shot mode: file at last non-flag arg
            const char *fname = NULL;
            for (int i = 1; i < argc; i++) {
                if (argv[i][0] != '-') fname = argv[i];
            }
            if (fname) infd = open(fname, O_RDONLY);
        }
        if (infd < 0) { perror("open job file"); return 1; }
        int rc = send_to_socket(DEFAULT_SOCK, infd);
        if (rc != 0) {
            logmsg("daemon socket %s unavailable, falling back to direct USB", DEFAULT_SOCK);
            if (infd != 0) lseek(infd, 0, SEEK_SET);
            rc = send_job(infd);
        }
        if (infd != 0) close(infd);
        return rc ? 1 : 0;
    }

    if (argc == 1) {
        printf("direct mf3110usb:/Canon/MF3110?serial=%s \"Canon MF3110\" \"Canon MF3110 (CARPS IOKit)\" \"MFG:Canon;MDL:MF3110;CMD:CARPS;CLS:PRINTER;DES:Canon MF3110;\"\n", WANT_SERIAL);
        return 0;
    }

    fprintf(stderr, "usage: %s [probe] [--iface0] [--soft-reset] [--zlp] [--close] [file]\n", argv[0]); return 1;
}
