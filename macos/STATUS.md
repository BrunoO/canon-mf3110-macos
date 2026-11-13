# Canon MF3110 macOS Driver — Status & Findings

## Current State
- **Filter pipeline (`rastertocarps`):** ✅ Fixed & working. `cgtexttopdf → cgpdftoraster → rastertocarps` produces valid CARPS data (verified with `carps-decode` in CI, `0` underflows/errors). Renamed `fls()` to `static int carps_fls(unsigned int)` so it does not collide with macOS `<strings.h>` `1`-indexed `fls(int)`.
- **PPD margins (`ppd/mf3110.ppd`):** ✅ Fixed. Patched `ppdc` output from `-1` `HWMargins` / full-bleed `"0 0 595 842"` (`4958px` wide, which caused LCD error `status=0x10`) to `14.25pt` margins (`4721px` wide) and A4 defaults (`status=0x18`).
- **Custom USB backend + LaunchDaemon (`macos/mf3110usb.c`):** ✅ Working. Persistent root LaunchDaemon (`com.carps.mf3110usb.plist`, socket `/var/run/mf3110usb.sock` `0600`) keeps USB interface 1 open across jobs, runs `GET_DEVICE_ID` + `GET_PORT_STATUS`, calls `ClearPipeStallBothEnds` before each job, writes `4096`-byte chunks, reads `stdin` when `argc >= 6`, and enforces an 8s post-job cooldown via `shutdown(SHUT_WR)` + EOF.
- **Printer output:** ✅ **Working for CLI (`lp`), consecutive jobs, and macOS GUI apps (such as Notes.app).**

## Root Causes Resolved

| Issue | Symptom | Root Cause & Fix |
|-------|---------|------------------|
| CUPS filter sandbox | `rastertocarps` crashed (`dyld: Library not loaded: libtiff.6.dylib`) | Static-linked `libtiff`, `zstd`, `xz`, `jpeg-turbo`; installed filter/PPD in `/Library/Printers/CARPS/` with absolute `*cupsFilter`. |
| First-job silence | All bytes written to ep `0x04`, no physical reaction | Switched to `IOUSBInterfaceInterface220`, added `GET_DEVICE_ID` (`bRequest=0`), `GET_PORT_STATUS` (`bRequest=1`), `ClearPipeStallBothEnds`, and `4096`-byte chunks. |
| LCD paper-format error | `GET_PORT_STATUS` `status=0x10` (`not_error=0`) | `ppdc` emitted `-1` margins (`4958px` wide); patched `mf3110.ppd` to `14.25pt` margins (`4721px` wide) and A4 defaults. |
| 2nd-job lockup (`no-reattach`) | 2nd job wedged printer after 1st print | Kept USB interface 1 open in a persistent `LaunchDaemon`, called `ClearPipeStallBothEnds` before each job, and added an 8s cooldown sync. |
| Power-cycle stale handle | Jobs failed with `0xe00002c0` after printer power-cycle | Pre-checked handle with `GET_PORT_STATUS` and released dead handles without `Close` before re-opening. |
| GUI app (`Notes.app`) failure | Simple `lp` text printed, larger GUI jobs wedged printer | `#if !defined(__APPLE__)` around `fls()` caused `encode_number()` to call macOS `1`-indexed `fls(int)`, corrupting run-lengths `>= 2`; renamed to `carps_fls()`. |

## Historical Experiments (Pre-Fix Reference)

- **Stock CUPS `usb` backend (`usb://Canon/MF3110`):** Completed jobs with 0 errors, but printer produced no output.
- **Interface 0 bulk-OUT (`0x01`):** `WritePipe` failed with `0xe00002ed` after 3072 bytes (vendor interface, wrong endpoint for printing).
- **USB resets (`ResetDevice`, `SetConfiguration(0/1)`):** Returned `0xe00002cd` (`kIOReturnNoDevice`); `USBDeviceReEnumerate` triggered a physical reaction only on the first attempt.
- **Bulk-IN (`0x85`) reads:** `ReadPipeTO` timed out (`0xe0004051`); the MF3110 sends no status/ack payload on bulk-IN during normal printing.

## Project Files

- [Makefile.macos](../Makefile.macos) — macOS build/install/probe/status/clean automation (`./build/` output)
- [install-macos.sh](../install-macos.sh) — one-command build and install
- [README.macos.md](../README.macos.md) — setup, usage, and troubleshooting
- [ARCHITECTURE.md](ARCHITECTURE.md) — pipeline diagram, USB layout, and design details
- [mf3110usb.c](mf3110usb.c) — custom IOKit USB backend & LaunchDaemon (`GPL-3.0-or-later`)
- [com.carps.mf3110usb.plist](com.carps.mf3110usb.plist) — LaunchDaemon definition
- [clean-slate.sh](clean-slate.sh) — queue/daemon reset helper
- [rastertocarps.c](../rastertocarps.c) — CARPS filter source (`carps_fls()` fix for macOS)
- [.github/workflows/macos.yml](../.github/workflows/macos.yml) — macOS CI build and CARPS round-trip test
