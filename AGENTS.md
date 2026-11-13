# Canon MF3110 macOS Driver — Project Notes

This directory is the working clone of `ondrej-zary/carps-cups` (the Linux CARPS driver). All project work happens here.

## Objective
Develop a native macOS (direct USB, print-only) driver for a Canon imageCLASS/LaserBase MF3110 on macOS 26 (Apple Silicon), reusing this repository's CARPS implementation.

## Printer & Platform
- **Printer:** Canon MF3110 (USB `usb://Canon/MF3110?serial=235AT2134669`), print-only (no scanning).
- **Platform:** macOS 26.7.3, arm64, CUPS 2.3.4.
- **CUPS paths:** `cups-config --serverbin=/usr/libexec/cups`, `--datadir=/usr/share/cups`, `ppdc` at `/usr/bin/ppdc`.
- **USB device:** VID `0x04a9`, PID `0x2660`, serial `235AT2134669`.

## Working Directory
- **Repo root:** `/Users/brunoorsier/dev/carps-cups` (this directory).
- **Temp files:** use `./tmp/` (this repo's `tmp/` directory) instead of `/tmp`.
- **Build artifacts:** `./rastertocarps` (filter binary), `./ppd/mf3110.ppd` (generated PPD).

## Decisions
- Use this repo's `carps-cups` (CARPS, lists MF3110 as works, GPL-3.0) as the protocol/rendering base: `rastertocarps` filter + `ppd/mf3110.ppd` from `carps.drv`.
- Install as a legacy CUPS PPD+filter queue (prototype) with filter/PPD in `/Library/Printers/CARPS` (avoid SIP/system paths) and absolute `*cupsFilter` path in PPD.
- On macOS, guard local `fls(unsigned int)` in `rastertocarps.c` with `#if !defined(__APPLE__)` to avoid conflict with system `fls(int)` in `_strings.h`. (Applied as uncommitted change to `./rastertocarps.c`.)
- Link `rastertocarps` with no Homebrew dylibs (CUPS filter sandbox blocked `/opt/homebrew/...`): static-link `/opt/homebrew/opt/libtiff/lib/libtiff.a`, `.../zstd/lib/libzstd.a`, `.../xz/lib/liblzma.a`, `.../jpeg-turbo/lib/libjpeg.a`, plus `-lpthread -lz -lcupsimage -lcups`.
- **Not interested in a Linux print-server workaround.** Direct native macOS driver only.
- Custom USB backend (`mf3110usb`) modeled after `Kuberwastaken/hp-laser-1008a-macos` `daemon/hpl1008-usbd.c`: root-owned CUPS backend using IOKit/IOUSBLib to seize interface and bulk-write job data. Source in `./tmp/mf3110usb.c`, installed at `/usr/libexec/cups/backend/mf3110usb`.

## Pipeline

```
App prints (text/PDF)
    ↓
CUPS (macOS 26, CUPS 2.3.4)
    ↓
cgtexttopdf  (text/plain → application/pdf)
    ↓
cgpdftoraster (application/pdf → application/vnd.cups-raster)
    ↓
/Library/Printers/CARPS/rastertocarps   (CUPS filter → CARPS format)
    ↓  produces raw CARPS raster data (e.g. ~30–37 KB for a text page, A4 595×842 @600dpi)
    ↓
Custom CUPS backend: /usr/libexec/cups/backend/mf3110usb
    ↓  root-owned, 0700, IOKit/IOUSBLib
    ↓  finds device 0x04a9:0x2660, serial 235AT2134669
    ↓  opens/seizes interface 1 (Printer class 7/1/2, alt 0)
    ↓  bulk-OUT endpoint 0x04 (512-byte max packet)
    ↓  WritePipe → device
    ↓
Canon MF3110 printer
```

## USB Device Layout

| Interface | Class/Sub/Proto | Alt | Endpoints |
|-----------|----------------|-----|-----------|
| 0 | 255/0/255 (vendor) | 0 | `0x01` OUT bulk 512, `0x82` IN bulk 512, `0x83` IN intr 64 |
| 1 | 7/1/2 (Printer) | 0 | `0x04` OUT bulk 512, `0x85` IN bulk 512 |

- Interface 1 is the classic printer interface. No IPP-over-USB alternate setting (7/1/4) — single-mode device.

## What Works
- `rastertocarps` filter (rebuilt, no Homebrew dylibs) — exits with no errors in CUPS logs.
- PPD (`mf3110.ppd`) — `*cupsFilter` points to absolute path; A4 default set.
- Filter chain — `cgtexttopdf → cgpdftoraster → rastertocarps` all complete; A4 raster generated correctly.
- Custom USB backend (IOKit) — finds device, opens, seizes interface 1, writes 30–37 KB to bulk-OUT ep 0x04; exits with no errors.
- CUPS queue — Device URI `mf3110usb:/Canon/MF3110?serial=235AT2134669`; jobs marked completed.

## What Doesn't Work (the blocker)
The printer receives data but produces no output. No panel errors, no paper movement, no physical reaction.

## Key Discoveries
1. **CARPS quirk:** MF3110 (0x04a9:0x2660) on Linux requires a `no-reattach` quirk — printer is sensitive to USB session attach/detach/reset behavior.
2. **Custom backend diagnostics:** `USBDeviceOpenSeize` → success, `USBInterfaceOpenSeize` → success (no exclusive-access conflict), `WritePipe` → all bytes written. Bulk-IN reads → all timeout (no device response/ack).
3. **SetAlternateInterface(0)** → `0xe0005000` (failed, but interface already on alt 0 — harmless).
4. **CARPS payload looks valid** — ~100 KB, proper PostScript header, A4 595×842, `%%EOF` terminator.
5. **Tried without explicit close** (2s grace then process exit) — still no print.

## What's Been Ruled Out
- Filter build/sandbox issues (rebuilt with no Homebrew dylibs)
- PPD/filter path issues (absolute path, correct permissions)
- Wrong paper size (A4 throughout, printer panel also A4)
- Wrong USB interface (correctly targeting 7/1/2 alt 0, ep 0x04)
- Exclusive access / `ippusbd` conflict (seize succeeds)
- Basic write failure (all bytes written at API level)
- Missing data (filter produces correct CARPS output)

## Likely Cause
The printer receives data but ignores it — likely because:
1. **USB session teardown behavior** — device may require a specific close/reset sequence (or none at all).
2. **Missing device handshake** — device may expect initialization/status exchange before/after data stream.
3. **Bulk-IN protocol** — device has bulk-IN endpoint (0x85) that may carry status/ack; reads timeout.

## Next Steps (most promising)
1. Try 512-byte chunked writes — some devices are sensitive to transfer chunk sizes.
2. Try writing to interface 0's bulk-OUT (0x01) — unlikely but easy to test.
3. Try a soft reset before writing — may prepare the device to accept data.
4. Capture a USB trace on a working Linux print and compare to macOS — definitive way to identify transaction/handshake difference.
5. Try sending a known-good CARPS stream from Linux through the macOS backend — isolates transport vs. data.

## Project Setup
- `./Makefile.macos`: macOS build/install Makefile. Targets: `build`, `install`, `uninstall`, `probe`, `status`, `print-test`, `revert-stock`, `clean`, `log`.
- `./install-macos.sh`: one-command build + install script.
- `./.gitignore`: ignores build artifacts (`rastertocarps`, `mf3110usb`, `carps-decode`, `ppd/*.ppd`).
- Quick commands:
  - `make -f Makefile.macos build` — rebuild filter + backend + PPD
  - `make -f Makefile.macos install` — install everything + set up queue
  - `make -f Makefile.macos status` — show build/install/queue status
  - `make -f Makefile.macos probe` — probe USB device endpoints
  - `make -f Makefile.macos print-test` — print a test page
  - `make -f Makefile.macos log` — capture last job's log
  - `./install-macos.sh` — full build + install in one step

## Relevant Files (in this repo)
- `./rastertocarps.c`: filter source (modified: `fls` guard for macOS). Uncommitted change.
- `./rastertocarps`: built filter binary (1.4 MB).
- `./carps.drv`: driver definition (generates PPD).
- `./ppd/mf3110.ppd`: generated PPD.
- `./carps.usb-quirks`: USB quirks (lists MF3110 0x04a9:0x2660 `no-reattach`).
- `./tmp/`: temp folder for project files (use instead of `/tmp`).
- `./tmp/mf3110usb.c`: custom USB backend source.

## Installed Files (system locations)
- `/Library/Printers/CARPS/rastertocarps`: installed filter (copy of `./rastertocarps`).
- `/Library/Printers/CARPS/mf3110.ppd`: installed PPD.
- `/usr/libexec/cups/backend/mf3110usb`: custom backend (0700, root:wheel).
- `/private/etc/cups/ppd/Canon_MF3110.ppd`: CUPS' active PPD copy.

## Important Context
- **Key errors resolved:** initial `dyld ... Library not loaded: /opt/homebrew/opt/libtiff/lib/libtiff.6.dylib ... (blocked by sandbox)` causing `rastertocarps` crash/signal 6 and "Filter failed"; fixed by static linking deps.
- **Current symptom:** pipeline completes, backend reports successful writes, printer does nothing.
- **Useful refs:** `ondrej-zary/carps-cups` (this repo), `Kuberwastaken/hp-laser-1008a-macos` (native macOS CUPS IOKit backend template), `OpenPrinting/cups` usb-libusb.c (quirks/reset/altset reference).

## Notes
- Linux option explicitly rejected by user — direct native macOS driver only.
- User asked to search the web for information about how to handle USB under macOS and similar driver projects on GitHub — research completed, documented above.
- Changes are local/uncommitted (`./rastertocarps.c` modified, `./ppd/`, `./rastertocarps`, `./tmp/` untracked). No commits/pushes.