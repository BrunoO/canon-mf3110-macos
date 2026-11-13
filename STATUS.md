# Canon MF3110 macOS Driver — Status & Findings

## Current State
- **Filter pipeline (`rastertocarps`):** ✅ Fixed & working. `cgtexttopdf → cgpdftoraster → rastertocarps` produces valid CARPS data (verified with `carps-decode`, 0 underflows/errors). Fixed a critical bug where `#if !defined(__APPLE__)` around `fls()` caused `encode_number()` to call macOS `<strings.h>` `fls(int)` (1-indexed) instead of `rastertocarps.c`'s 0-indexed `fls(unsigned int)`, corrupting run-lengths `>= 2` on non-trivial pages (e.g., Notes.app); renamed to `static int carps_fls(unsigned int)`.
- **PPD margins (`mf3110.ppd`):** ✅ Fixed. `ppdc` generated `*HWMargins: -1 -1 -1 -1` and full-bleed `"0 0 595 842"` (`4958px` wide), which caused an LCD paper-format error (`status=0x10`). Patched to `14.25pt` margins (`4721px` wide) and A4 default.
- **Custom USB backend + LaunchDaemon (`mf3110usb`):** ✅ Working. Persistent daemon (`com.carps.mf3110usb.plist`, socket `/var/run/mf3110usb.sock`) keeps USB interface 1 open across jobs, runs `GET_DEVICE_ID` + `GET_PORT_STATUS`, calls `ClearPipeStallBothEnds` before each job, writes in `4096`-byte chunks, reads filtered CARPS from `stdin` (`argc >= 6`), and syncs an 8s post-job cooldown via `shutdown(SHUT_WR)` + EOF.
- **Printer output:** ✅ **Working (CLI `lp`, consecutive jobs, and GUI apps such as Notes.app verified).**

## What Was Tried

### 1. Filter Build & Installation
- Rebuilt `rastertocarps` with no Homebrew dylibs (static-linked libtiff, zstd, lzma, jpeg). Resolved sandbox crash (`dyld: Library not loaded: libtiff.6.dylib`).
- Initially guarded `fls(unsigned int)` with `#if !defined(__APPLE__)`, which exposed a 0-indexed vs 1-indexed mismatch with `<strings.h>` `fls(int)` (1 extra bit per `encode_number(num >= 2)`). Replaced with `static int carps_fls(unsigned int)`.
- Installed filter + PPD in `/Library/Printers/CARPS` (SIP-safe). Absolute `*cupsFilter` path in PPD.
- **Result:** Filter pipeline works. No CUPS errors.

### 2. Stock CUPS USB Backend
- Used macOS stock `/usr/libexec/cups/backend/usb` with `usb://Canon/MF3110?serial=...`.
- Job 20: sent 30704 bytes, backend exited no errors, job completed.
- **Result:** Printer didn't print. Same symptom.

### 3. Custom IOKit USB Backend (mf3110usb)
- Modeled after `Kuberwastaken/hp-laser-1008a-macos` `hpl1008-usbd.c`.
- Root-owned (0700) CUPS backend in `/usr/libexec/cups/backend/mf3110usb`.
- Finds device 0x04a9:0x2660, opens/seizes interface 1 (7/1/2 alt 0), bulk-OUT ep 0x04.
- **Result:** All writes succeed (30-105 KB), backend exits 0, job completes. Printer doesn't print.

### 4. Experiments Tested
| # | Experiment | Result |
|---|-----------|--------|
| 4a | 512-byte chunked writes (vs single large writes) | No difference. Both complete, no print. |
| 4b | Interface 0 bulk-OUT (ep 0x01) instead of interface 1 | `WritePipe` fails with `0xe00002ed` after 3072 bytes. Wrong interface. |
| 4c | `USBDeviceReEnumerate` before write | **Printer physically reacted** (parts moving) on first attempt. But device not found in time (2s). With 12s wait, device found but no physical reaction on subsequent attempts. |
| 4d | `ResetDevice` before write | Returns `0xe00002cd` (no device). No physical reaction. |
| 4e | `SetConfiguration(0)` → `SetConfiguration(1)` cycle | Both return `0xe00002cd`. No physical reaction. |
| 4f | No explicit close (let process exit clean up) | No difference. |
| 4g | Bulk-IN drain after write (read ep 0x05) | ReadPipeTO times out immediately (`0xe0004051`). Device sends no data. |
| 4h | Single large WritePipe (no chunking) | No difference. |

### 5. CARPS Protocol Quirk (from carps.usb-quirks)
- MF3110 (0x04a9:0x2660) requires `no-reattach` quirk on Linux.
- "Seems that the printer gets confused by repeated (re)attaching of the usblp module caused by libusb using the same device."
- This suggests the printer is sensitive to USB session attach/detach/reset behavior.

## Key Findings
1. **Data transfer works:** All bytes are written successfully to the correct bulk-OUT endpoint.
2. **No device response:** Bulk-IN reads always time out. Device never sends status/ack data.
3. **Reset sensitivity:** `USBDeviceReEnumerate` caused a physical printer reaction (first attempt only), suggesting the device responds to USB-level resets.
4. **Interface 0 is wrong:** Writing to interface 0's bulk-OUT fails after a few KB.
5. **No exclusive-access conflict:** `USBDeviceOpenSeize` and `USBInterfaceOpenSeize` both succeed.

## Likely Cause
The printer receives data but ignores it. Possible reasons:
1. **Missing initialization/handshake:** The device may expect a specific control transfer or status exchange before accepting print data.
2. **USB session teardown:** The "no-reattach" quirk on Linux suggests the device is sensitive to how the USB session is closed. On macOS, the IOKit session teardown might differ.
3. **Data framing:** The device might expect specific packet boundaries, zero-length packets, or a different transfer mode.
4. **Missing "end of job" signal:** The device might need a specific control transfer or status read to trigger printing after data is sent.

## Next Ideas (most promising first)

### A. USB Packet Capture on Linux (highest value)
- Capture a successful print on the Linux machine using `usbmon`/Wireshark.
- Compare exact USB transactions (control transfers, endpoint usage, transfer sizes, timing, reset sequences) against macOS.
- This is the most reliable way to identify the missing transaction or handshake.
- Command on Linux: `sudo modprobe usbmon && sudo wireshark` (capture on usbmon0 or the specific bus).

### B. Send Known-Good CARPS Stream from Linux
- Generate a CARPS file on Linux that successfully prints.
- Send it through the macOS backend in one-shot mode: `sudo /usr/libexec/cups/backend/mf3110usb known_good.carps`
- If it still doesn't print → transport issue (not data).
- If it prints → data/content issue.

### C. Try Control Transfer Initialization
- Before sending bulk data, send USB control transfers that the Windows/Linux driver might use:
  - `GET_STATUS` (standard USB request)
  - Canon vendor-specific initialization
- Research the CARPS Windows driver USB protocol for init sequences.

### D. Try Keeping Session Open Longer
- After writing all data, keep the USB session open for 30-60s without closing.
- Some devices need time to process and the "print" trigger might be a timeout-based flush.

### E. Try Different Bulk Transfer Modes
- Use `WritePipeTO` with a timeout instead of `WritePipe`.
- Try asynchronous transfers with completion callbacks.
- Try adding a zero-length packet (ZLP) at the end of the transfer.

### F. Try Sending to Interface 0 with Vendor Protocol
- Interface 0 (255/0/255) has bulk-OUT (0x01) and bulk-IN (0x82).
- The Windows driver might use this interface for control/status and interface 1 for data.
- Try a bidirectional handshake on interface 0 before sending data on interface 1.

## Project Files
- `./Makefile.macos` — build/install automation
- `./install-macos.sh` — one-command install
- `./tmp/mf3110usb.c` — custom USB backend source
- `./rastertocarps.c` — CARPS filter source (modified for macOS)
- `./ppd/mf3110.ppd` — generated PPD (patched with absolute filter path)
- `./AGENTS.md` — project notes and decisions
- `./STATUS.md` — this file