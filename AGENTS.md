# Canon MF3110 macOS Driver — Project Notes

This repository is a macOS-enabled fork of `ondrej-zary/carps-cups` (the Linux CARPS driver).

## Objective
Provide a native macOS (direct USB, print-only) driver for the Canon imageCLASS/LaserBase MF3110 on macOS (tested on macOS 26.7.3 Apple Silicon, CUPS 2.3.4), reusing `carps-cups`'s CARPS implementation.

## Printer & Platform
- **Printer:** Canon MF3110 (USB VID `0x04a9`, PID `0x2660`), print-only (no scanning).
- **Platform:** macOS 26.7.3, arm64, CUPS 2.3.4.
- **CUPS paths:** `cups-config --serverbin=/usr/libexec/cups`, `--datadir=/usr/share/cups`, `ppdc` at `/usr/bin/ppdc`.

## Working Directory
- **Temp files:** use `./tmp/` (ignored by `.gitignore`) instead of `/tmp`.
- **Build artifacts (in `./build/` and `./ppd/`):**
  - `./build/rastertocarps` (filter binary)
  - `./build/mf3110usb` (optional IOKit USB backend/daemon)
  - `./ppd/mf3110.ppd` (generated and patched PPD)

## Decisions & Current Architecture
- **Filter (`rastertocarps`):**
  - Renamed upstream's `0`-indexed `fls(unsigned int)` to `static int carps_fls(unsigned int)` in `./rastertocarps.c` to avoid colliding with macOS/BSD `<strings.h>` `1`-indexed `fls(int)` (which otherwise corrupted `encode_number()` bitstreams on GUI print jobs).
  - Static-linked against Homebrew's `libtiff.a`, `libzstd.a`, `liblzma.a`, `libjpeg.a`, and (when present) `libwebp.a`/`libsharpyuv.a` (`WEBP_LIBS`), plus `-lpthread -lz -lcupsimage -lcups`, so no Homebrew dylibs are loaded inside the CUPS filter sandbox.
- **PPD (`ppd/mf3110.ppd`):**
  - Generated from `./carps.drv` with `ppdc` and patched by `Makefile.macos` for absolute `*cupsFilter` (`/Library/Printers/CARPS/rastertocarps`), `14.25pt` `HWMargins` / A4 `ImageableArea` (`4721px` wide instead of full-bleed `4958px`), and A4 defaults.
- **USB Backend:**
  - **Default (`make -f Makefile.macos install` / `./install-macos.sh`):** Uses macOS's stock CUPS `usb` backend (`usb://Canon/MF3110...`), which works cleanly for consecutive CLI and GUI jobs once `carps_fls()` and PPD margins are fixed.
  - **Optional fallback (`make -f Makefile.macos install-iokit`):** Installs `./macos/mf3110usb.c` as `/usr/libexec/cups/backend/mf3110usb` + `LaunchDaemon` (`./macos/com.carps.mf3110usb.plist`) for setups that hit `no-reattach` USB lockups.

## USB Device Layout

| Interface | Class/Sub/Proto | Alt | Endpoints |
|-----------|----------------|-----|-----------|
| 0 | 255/0/255 (vendor) | 0 | `0x01` OUT bulk 512, `0x82` IN bulk 512, `0x83` IN intr 64 |
| 1 | 7/1/2 (Printer) | 0 | `0x04` OUT bulk 512, `0x85` IN bulk 512 |

- Interface 1 is the classic printer interface (single-mode device, no IPP-over-USB `7/1/4` alternate setting).

## Project Setup & Quick Commands
- `./Makefile.macos`: macOS build/install Makefile. Targets: `build`, `install`, `install-iokit`, `uninstall`, `probe`, `status`, `print-test`, `revert-stock`, `clean`, `log`.
- `./install-macos.sh`: one-command build + stock-USB install script.
- Quick commands:
  - `make -f Makefile.macos build` — rebuild `build/rastertocarps`, `build/mf3110usb`, and `ppd/mf3110.ppd`
  - `make -f Makefile.macos install` — install filter + PPD and set up queue on stock macOS `usb` backend
  - `make -f Makefile.macos install-iokit` — install optional `mf3110usb` IOKit backend + LaunchDaemon
  - `make -f Makefile.macos status` — show build/install/queue status
  - `make -f Makefile.macos probe` — probe USB device endpoints (requires `mf3110usb` installed)
  - `make -f Makefile.macos print-test` — print an A4 test page
  - `make -f Makefile.macos log` — capture last job's log into `./tmp/`

## Key Project Files
- `./rastertocarps.c`: CARPS filter source (`carps_fls()` fix for macOS/BSD).
- `./carps.drv`: driver definition (generates `ppd/*.ppd`).
- `./carps.usb-quirks`: upstream USB quirks (lists MF3110 `0x04a9:0x2660` `no-reattach`).
- `./macos/mf3110usb.c`: optional custom IOKit USB backend & LaunchDaemon source (`GPL-3.0-or-later`).
- `./macos/com.carps.mf3110usb.plist`: LaunchDaemon plist for `mf3110usb --daemon`.
- `./macos/clean-slate.sh`: queue/cache/daemon reset script.
- `./macos/ARCHITECTURE.md`: pipeline diagram and design decisions.
- `./macos/STATUS.md`: resolved root causes and pre-fix experiment log.
- `./.github/workflows/macos.yml`: CI build + `carps-decode` round-trip verification.

## Installed Files (system locations)
- `/Library/Printers/CARPS/rastertocarps`: installed filter.
- `/Library/Printers/CARPS/mf3110.ppd`: installed PPD.
- `/private/etc/cups/ppd/Canon_MF3110.ppd`: CUPS' active PPD copy.
- `/usr/libexec/cups/backend/mf3110usb` & `/Library/LaunchDaemons/com.carps.mf3110usb.plist`: optional (`install-iokit` only).
