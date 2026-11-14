# Canon MF3110 macOS Driver — Architecture & Pipeline

This document describes how the native macOS CUPS driver (`rastertocarps` + stock macOS `usb` backend, with optional `mf3110usb` IOKit fallback) works and why each macOS-specific design decision is needed.

## 1. Print Pipeline

```text
macOS App (Notes.app, Preview, etc.) or CLI (`lp`)
  │
  ▼  PDF / text/plain
CUPS 2.3.4 (`cupsd`)
  │
  ├─► `cgtexttopdf`   (text/plain ──► application/pdf)
  ├─► `cgpdftoraster` (application/pdf ──► application/vnd.cups-raster, 4721×6779 @ 600 dpi for A4)
  │
  ▼  application/vnd.cups-raster (stdin)
`/Library/Printers/CARPS/rastertocarps`
  │  Static-linked CARPS encoder; produces CARPS blocks (`0xCD 0xCA 0x10 ...`)
  │
  ▼  CARPS byte stream (stdout ──► CUPS pipe)
  ├─► Default (`install`): stock macOS CUPS `usb` backend (`usb://Canon/MF3110...`)
  │
  └─► Optional fallback (`install-iokit`): `/usr/libexec/cups/backend/mf3110usb` (`argc >= 6`, reads `stdin`)
        │  Streams CARPS bytes to `/var/run/mf3110usb.sock`, calls `shutdown(SHUT_WR)`, waits for EOF
        ▼
      `mf3110usb --daemon` (`LaunchDaemon`: `/Library/LaunchDaemons/com.carps.mf3110usb.plist`)
        │  1. Validates existing handle via `GET_PORT_STATUS` (releases stale handles on power-cycle)
        │  2. Seizes device `0x04a9:0x2660` and interface 1 (`7/1/2`, alt 0)
        │  3. Runs `GET_DEVICE_ID` + `GET_PORT_STATUS`, then `ClearPipeStallBothEnds`
        │  4. Writes CARPS stream in `4096`-byte `WritePipe` transfers to bulk-OUT `0x04`
        │  5. Sleeps 8s cooldown before closing client socket
  │
  ▼
Canon MF3110 USB Printer (`0x04a9:0x2660`)
```

## 2. USB Device Layout

| Interface | Class / Sub / Proto | Alt | Endpoints | Role |
|-----------|---------------------|-----|-----------|------|
| 0 | `255 / 0 / 255` (Vendor) | 0 | `0x01` OUT bulk (512B), `0x82` IN bulk (512B), `0x83` IN intr (64B) | Scanner / vendor (unused) |
| 1 | `7 / 1 / 2` (Printer) | 0 | `0x04` OUT bulk (512B), `0x85` IN bulk (512B) | **Print interface used by `mf3110usb`** |

The Canon MF3110 is a single-mode USB printer (no IPP-over-USB `7/1/4` alternate setting).

## 3. Key macOS Design Decisions

### CUPS Filter Sandbox & Static Linking (`Makefile.macos`)
macOS CUPS runs filters inside a strict sandbox that blocks loading dynamic libraries from `/opt/homebrew/...` (`dyld: Library not loaded ...`). To avoid runtime sandbox failures:
- `rastertocarps` is static-linked against Homebrew's `libtiff.a`, `libzstd.a`, `liblzma.a`, `libjpeg.a`, and (when present) `libwebp.a` / `libsharpyuv.a` (verified with `otool -L`).
- Filter and PPD are installed in `/Library/Printers/CARPS/` (avoiding SIP-protected system directories), and `mf3110.ppd` uses an absolute `*cupsFilter` path.

### `carps_fls()` vs. macOS `<strings.h>` `fls()` (`rastertocarps.c`)
Upstream `rastertocarps.c` defines a `0`-indexed `fls(unsigned int)` (`floor(log2(n))`), whereas macOS `<strings.h>` declares a `1`-indexed `fls(int)` (`floor(log2(n)) + 1`). Guarding upstream `fls()` out on macOS caused `encode_number()` to emit 1 extra bit for every run-length `>= 2`, corrupting compressed strips on non-trivial pages (such as macOS GUI print jobs) and wedging the printer's CARPS decoder. Renaming the local helper to `carps_fls()` preserves the exact `0`-indexed behavior.

### PPD Margins (`ppd/mf3110.ppd`)
On CUPS 2.3.4, `ppdc carps.drv` emits `*HWMargins: -1 -1 -1 -1` and full-bleed A4 `*ImageableArea A4/A4: "0 0 595 842"` (`4958px` raster width), which triggers a paper-format error (`status=0x10`) on the MF3110. `Makefile.macos` patches `mf3110.ppd` to `14.25pt` margins (`4721px` width) and A4 defaults (`status=0x18`).

### Persistent LaunchDaemon & `no-reattach` Quirk (`macos/mf3110usb.c`)
The Canon MF3110 (`0x04a9:0x2660`) has the CARPS `no-reattach` quirk: closing and re-opening the USB printer interface between jobs wedges the device after the first print.
- **Persistent handle:** `mf3110usb --daemon` keeps interface 1 open across jobs and calls `ClearPipeStallBothEnds` before each job.
- **Power-cycle recovery:** Before reading job data, the daemon checks handle health with `ControlRequest` (`GET_PORT_STATUS`) and releases dead handles without calling `Close` on disconnected devices.
- **CUPS backend arguments & `stdin`:** When invoked by CUPS with `argc >= 6`, `mf3110usb` always reads the filtered CARPS stream from `stdin` (never treating `argv[5]` job options or `argv[6]` raw spool file as CARPS input).
- **Job serialization:** After streaming to the daemon, the client calls `shutdown(s, SHUT_WR)` and blocks on `read()` until the daemon closes the socket after its 8s post-job cooldown, preventing CUPS from starting a second job while the printer is still feeding/fusing.

## 4. Installed System Files

| Path | Mode / Owner | Purpose |
|------|--------------|---------|
| `/Library/Printers/CARPS/rastertocarps` | `0755` | Static-linked CARPS raster filter |
| `/Library/Printers/CARPS/mf3110.ppd` | `0644` | Patched PPD (A4 default, `14.25pt` margins) |
| `/usr/libexec/cups/backend/mf3110usb` | `0700 root:wheel` | Optional (`install-iokit`): custom IOKit USB backend & daemon |
| `/Library/LaunchDaemons/com.carps.mf3110usb.plist` | `0644 root:wheel` | Optional (`install-iokit`): LaunchDaemon (`KeepAlive`, `RunAtLoad`) |
| `/var/run/mf3110usb.sock` | `0600 root:wheel` | Optional (`install-iokit`): Unix socket between CUPS backend and daemon |
| `/var/log/mf3110usb-daemon.log` | — | Optional (`install-iokit`): daemon stderr log |
