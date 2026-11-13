# Canon MF3110 Native macOS Driver (CARPS)

Native direct-USB CUPS driver for the **Canon imageCLASS / LaserBase MF3110** (`0x04a9:0x2660`, print-only) on **macOS** (tested on macOS 26.7.3, Apple Silicon `arm64`, CUPS 2.3.4), built on [ondrej-zary/carps-cups](https://github.com/ondrej-zary/carps-cups).

## Architecture

1. **CUPS Filter (`rastertocarps`):** Static-linked against Homebrew's `libtiff`, `zstd`, `xz`, and `jpeg-turbo` so it runs cleanly inside the macOS CUPS filter sandbox without dynamic library load errors.
2. **PPD (`mf3110.ppd`):** Generated from `carps.drv` with `ppdc` and patched for `/Library/Printers/CARPS/rastertocarps`, `14.25pt` hardware margins (`4721px` printable width at 600 dpi), and A4 defaults.
3. **Custom IOKit USB Backend & LaunchDaemon (`macos/mf3110usb.c`):** A persistent root LaunchDaemon (`com.carps.mf3110usb.plist`) listens on `/var/run/mf3110usb.sock` and keeps USB interface 1 (`7/1/2`) open across jobs (avoiding the MF3110 `no-reattach` USB wedge), issuing `GET_DEVICE_ID`, `GET_PORT_STATUS`, and `ClearPipeStallBothEnds` before each job.

## Prerequisites

- Xcode Command Line Tools (`xcode-select --install`)
- [Homebrew](https://brew.sh) static libraries (used at build time only):
  ```sh
  brew install libtiff zstd xz jpeg-turbo
  ```

## Build & Install

```sh
./install-macos.sh
```

Or run the targets individually:

```sh
make -f Makefile.macos build
make -f Makefile.macos install
make -f Makefile.macos print-test
```

## Uninstall

```sh
make -f Makefile.macos uninstall
```

## Useful Targets

- `make -f Makefile.macos status` — show build artifacts, installed files, and CUPS queue status
- `make -f Makefile.macos probe` — probe USB interfaces and endpoints on the connected MF3110
- `make -f Makefile.macos log` — capture the last CUPS job log for `Canon_MF3110`

## Credits & License

- **CARPS filter & protocol:** [ondrej-zary/carps-cups](https://github.com/ondrej-zary/carps-cups) (GPL-3.0)
- **IOKit CUPS backend pattern:** inspired by [Kuberwastaken/hp-laser-1008a-macos](https://github.com/Kuberwastaken/hp-laser-1008a-macos)
- Licensed under **GPL-3.0** — see [LICENSE](LICENSE).
