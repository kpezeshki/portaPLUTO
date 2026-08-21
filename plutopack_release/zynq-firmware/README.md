# PlutoPack Zynq firmware (fishball7020 SD image)

The Linux side of PlutoPack is a [Buildroot](https://buildroot.org/) image built
from the **tezuka_fw** external tree (a fork of the PlutoSDR/maia-sdr Buildroot
integration by [F5OEO](https://github.com/F5OEO/tezuka_fw)). This directory holds
**only the PlutoPack-specific deltas**; the upstream tree and Buildroot itself
are fetched by `setup.sh`.

## What's here (our deltas)

```
overlay/board/tezuka/fishball7020/     board support:
    dts/                                  device tree (display, spi-gpio to Pico, AD9361)
    overlay/etc/init.d/                   S60maia-httpd / S95bgcript / S99zzbootdone overrides
    uboot-env.txt                         U-Boot environment
    bitstream/maia-iio/system_top.xsa     prebuilt FPGA XSA (rebuild via ../fpga)
    bitstream/fsbl.elf, overclock/        prebuilt FSBL (+ overclock variants)
overlay/configs/fishball_maiasdr_7020_defconfig    the Buildroot defconfig
overlay/package/plutopack-panel/       the front-panel app suite (see below)
```

`setup.sh` copies these into the cloned `tezuka_fw` tree and registers the board
in `boards.json`. The defconfig reuses the upstream `fishball7010` kernel/U-Boot
config and adds our DTS, root-fs overlay, and the `plutopack-panel` package;
`BR2_PACKAGE_BOARD_FPGA_BOARD="fishball7020"` pulls in the XSA above.

## The plutopack-panel package

`overlay/package/plutopack-panel/` is the on-panel software: `pp-launcher` (the
app switcher that boots at S30) plus the app suite — WATERFALL (maia-sdr),
SPECTRUM, SNA, SCOPE, VNA, tone/two-tone generators, RTL_433, ADS-B, an RFIC
status/calibration screen, a root TERMINAL, TOUCH CAL, and diagnostics. All apps
share the `pp_ui.h` UI framework and talk to the Pico over `/dev/spidev*`
(`pp_common.h`) and to maia-httpd over local HTTP (`pp_maia.h`).

The same sources also build the **host simulator** — see [`../simulator`](../simulator).

## Build

```bash
./setup.sh     # clone tezuka_fw@main + download Buildroot, overlay our deltas
./build.sh     # make <defconfig> && make  (first build downloads/compiles a lot)
```

Output lands in `tezuka_fw/output/fishball7020/images/` and
`tezuka_fw/build/fishball7020.zip`. See [`../docs/BUILD_SD_IMAGE.md`](../docs/BUILD_SD_IMAGE.md)
for writing it to an SD card. A prebuilt image is in [`../prebuilt/sd-image`](../prebuilt/sd-image).

## Pinned upstream revisions

| Component | Repo | Revision |
|---|---|---|
| tezuka_fw | `github.com/F5OEO/tezuka_fw` (branch `main`) | `c1e8f57` |
| Buildroot | `buildroot.org` | `2026.02` (sha256 pinned in `tezuka_fw/buildroot.version`) |
| Linux kernel | `analogdevicesinc/linux` | tarball pinned in the defconfig |

## Requirements

A Linux host with a normal Buildroot toolchain prerequisite set (`make`, `gcc`,
`git`, `jq`, `wget`, `cpio`, `unzip`, `bc`, `rsync`, `file`, `python3`, ~20 GB
free). Buildroot builds its own cross-toolchain; no Vivado is needed for the
Linux image (the FPGA XSA is prebuilt and shipped).
