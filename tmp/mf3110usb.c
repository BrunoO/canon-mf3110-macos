// mf3110usb: minimal CUPS USB backend for Canon MF3110 (CARPS)
// Env vars for testing:
//   MF3110_IFACE=0|1  (default 1)  select printer interface
//   MF3110_RESET=1     (default off) USB re-enumerate before write
// One-shot:  sudo ./mf3110usb [--iface0] [--reset] <file>
// CUPS:      installed as /usr/libexec/cups/backend/mf3110usb
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#define VID 0x04A9
#define PID 0x2660
#define WANT_SERIAL "235AT2134669"

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

static IOUSBInterfaceInterface **open_printer_iface(IOUSBDeviceInterface **dev, UInt8 *outPipe, UInt8 *inPipe, UInt8 wantIf) {
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
    IOUSBInterfaceInterface **result=NULL; io_service_t usbIf;
    while ((usbIf = IOIteratorNext(iter))) {
        IOCFPlugInInterface **plugin=NULL; SInt32 score=0; IOUSBInterfaceInterface **intf=NULL;
        if (IOCreatePlugInInterfaceForService(usbIf, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin,&score)==kIOReturnSuccess && plugin) {
            (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID),(LPVOID*)&intf); (*plugin)->Release(plugin);
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

static int send_job(int fd) {
    const char *ifaceEnv = getenv("MF3110_IFACE");
    UInt8 wantIf = ifaceEnv ? (UInt8)atoi(ifaceEnv) : 1;
    int doReset = getenv("MF3110_RESET") ? 1 : 0;

    logmsg("config: iface=%u reset=%d", wantIf, doReset);

    IOUSBDeviceInterface **dev = find_device();
    if (!dev) { fprintf(stderr,"ERROR: mf3110usb: printer not found\n"); return 1; }

    if (doReset) {
        // No reset — it doesn't help. Just log and continue.
        logmsg("MF3110_RESET=1: reset disabled (not effective), skipping");
    }

    UInt8 outPipe=0, inPipe=0; IOUSBInterfaceInterface **intf = open_printer_iface(dev,&outPipe,&inPipe,wantIf);
    if (!intf || !outPipe) { fprintf(stderr,"ERROR: mf3110usb: no bulk-OUT on iface %u\n", wantIf); if(intf){(*intf)->Release(intf);} (*dev)->USBDeviceClose(dev); (*dev)->Release(dev); return 1; }

    usleep(200000);

    int useChunks = getenv("MF3110_CHUNK") ? atoi(getenv("MF3110_CHUNK")) : 1;

    unsigned char buf[65536]; ssize_t n; size_t total=0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (useChunks) {
            size_t off = 0;
            while (off < (size_t)n) {
                UInt32 w = (UInt32)(((size_t)n - off > 512) ? 512 : ((size_t)n - off));
                IOReturn r = (*intf)->WritePipe(intf, outPipe, buf + off, w);
                if (r != kIOReturnSuccess) {
                    fprintf(stderr, "ERROR: mf3110usb: WritePipe failed 0x%08x after %zu bytes (chunk=%u)\n", r, total, w);
                    return 1;
                }
                off += w; total += w;
            }
        } else {
            // Single WritePipe call with all available data
            UInt32 w = (UInt32)n;
            IOReturn r = (*intf)->WritePipe(intf, outPipe, buf, w);
            if (r != kIOReturnSuccess) {
                fprintf(stderr, "ERROR: mf3110usb: WritePipe failed 0x%08x after %zu bytes (single=%u)\n", r, total, w);
                return 1;
            }
            total += w;
        }
    }
    logmsg("wrote %zu bytes total (%s)", total, useChunks ? "512-byte chunks" : "single writes");

    // After writing all data, read from bulk-IN to let the device
    // process/acknowledge. Some devices won't print until the host
    // drains the status endpoint.
    if (inPipe) {
        logmsg("draining bulk-IN (pipe=%u) for up to 10s...", inPipe);
        unsigned char rb[512];
        int reads = 0;
        for (int i = 0; i < 20; i++) {
            UInt32 rlen = sizeof(rb);
            IOReturn rr = (*intf)->ReadPipeTO(intf, inPipe, rb, &rlen, 500, 500);
            logmsg("  ReadPipeTO[%d] -> 0x%08x len=%u", i, rr, (unsigned)rlen);
            reads++;
            if (rr != kIOReturnSuccess) break; // timeout or error
            if (rlen == 0) break;
        }
        logmsg("  completed %d read attempts", reads);
    }

    usleep(5000000); // 5s grace for printer to process
    logmsg("exiting without explicit USB close");
    return 0;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iface0") == 0) setenv("MF3110_IFACE", "0", 1);
        if (strcmp(argv[i], "--reset") == 0) setenv("MF3110_RESET", "1", 1);
    }
    const char *uri = getenv("DEVICE_URI");
    int isBackend = (uri && strstr(uri, "mf3110usb") != NULL) || strstr(argv[0], "mf3110usb") != NULL;

    if (argc >= 2 && strcmp(argv[argc-1], "probe") == 0) {
        IOUSBDeviceInterface **dev = find_device(); if(!dev){printf("printer not found\n"); return 1;}
        int rc = probe(dev); (*dev)->Release(dev); return rc;
    }

    if (isBackend) {
        int infd = 0;
        // CUPS mode: file at argv[6]
        if (argc >= 7 && argv[6] && argv[6][0]) {
            infd = open(argv[6], O_RDONLY);
        } else {
            // One-shot mode: file at last non-flag arg
            const char *fname = NULL;
            for (int i = 1; i < argc; i++) {
                if (argv[i][0] != '-') fname = argv[i];
            }
            if (fname) infd = open(fname, O_RDONLY);
        }
        if (infd < 0) { perror("open job file"); return 1; }
        int rc = send_job(infd);
        if (infd != 0) close(infd);
        return rc ? 1 : 0;
    }

    if (argc == 1) {
        printf("direct mf3110usb:/Canon/MF3110?serial=%s \"Canon MF3110\" \"Canon MF3110 (CARPS IOKit)\" \"MFG:Canon;MDL:MF3110;CMD:CARPS;CLS:PRINTER;DES:Canon MF3110;\"\n", WANT_SERIAL);
        return 0;
    }

    fprintf(stderr, "usage: %s [probe] [--iface0] [--reset] [file]\n", argv[0]); return 1;
}
