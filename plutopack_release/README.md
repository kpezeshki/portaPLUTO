# PlutoPack

PlutoPack (a.k.a. *fishball7020* / *PlutoSky*) is a portable AD9361 software-defined
radio with a touchscreen front panel: a **Zynq-7020** running Linux + a
[maia-sdr](https://github.com/maia-sdr/maia-sdr) FPGA image, a **320×240 ILI9341**
panel, and a **Raspberry Pi Pico** front-panel controller. The panel runs a suite
of self-contained apps (waterfall, spectrum analyzer, scalar/vector network
analyzer, oscilloscope, tone generators, RTL_433, ADS-B, RF-IC status &
calibration, a root terminal, and diagnostics) sharing one UI framework.

This repository contains everything needed to build the software and bring up a
board, organised into four components:

| Directory | What it is | Build guide |
|---|---|---|
| [`fpga/`](fpga) | FPGA project sources (maia-sdr + AD9361 for the fishball7020) | [docs/BUILD_SD_IMAGE.md](docs/BUILD_SD_IMAGE.md#1-optional-build-the-fpga-bitstream) |
| [`zynq-firmware/`](zynq-firmware) | Buildroot deltas: board support + the `plutopack-panel` app suite | [docs/BUILD_SD_IMAGE.md](docs/BUILD_SD_IMAGE.md) |
| [`pico-firmware/`](pico-firmware) | RP2040 controller firmware (`plutopack_mcu`) | [docs/BUILD_PICO_UF2.md](docs/BUILD_PICO_UF2.md) |
| [`simulator/`](simulator) | Host web-app that runs the real panel UI with no hardware | [simulator/README.md](simulator/README.md) |
| [`prebuilt/`](prebuilt) | Flash-ready UF2 + SD-card image | — |
| [`docs/`](docs) | Build & hardware documentation | [docs/HARDWARE.md](docs/HARDWARE.md) |

## Design in one picture

```
        +-------------------+        SPI (spi-gpio, Pico=slave)     +-----------+
        |     Zynq-7020     | <-----------------------------------> |  RP2040   |
        |  Linux + maia FPGA|   buttons / encoder / touch / gauge   |   Pico    |
        |                   |   display RESET+CS / user GPIO        +-----------+
        |  AD9361 <--FPGA-->|                                        (front panel
        |  ILI9341 panel    |  <-- PS SPI1 + EMIO (display data bus)  controller)
        +-------------------+
```

More detail: [`docs/HARDWARE.md`](docs/HARDWARE.md) and the verified pin map in
[`pico-firmware/PINMAP.md`](pico-firmware/PINMAP.md).

## Quick start

**Just flash a board (no build):**

1. Pico: hold BOOTSEL, plug in USB, drop [`prebuilt/pico/plutopack_mcu.uf2`](prebuilt/pico) on the `RPI-RP2` drive.
2. SD card: write [`prebuilt/sd-image/`](prebuilt/sd-image) to a FAT32 boot partition — see [docs/BUILD_SD_IMAGE.md](docs/BUILD_SD_IMAGE.md).

**Try the UI with no hardware:**

```bash
cd zynq-firmware/overlay/package/plutopack-panel/sim && make && ./ppsim
# open http://localhost:8080  — click on the screen to simulate a touch
```

**Build from source:** FPGA → `fpga/` (Vivado 2025.2), Linux image →
`zynq-firmware/` (Buildroot), Pico → `pico-firmware/` (Pico SDK). Each has a
`README.md`; start with [`docs/BUILD_SD_IMAGE.md`](docs/BUILD_SD_IMAGE.md).

## Upstream & self-containment

`fpga/` and `zynq-firmware/` are **deltas** on top of public upstreams — the
large HDL/Buildroot trees are fetched (not vendored) by each component's
`setup.sh`, at pinned revisions:

| Upstream | Repo | Pinned |
|---|---|---|
| maia-sdr HDL | `github.com/F5OEO/maia-sdr` (`sweep`) | `2637b59` |
| adi-hdl | `github.com/analogdevicesinc/hdl` | `cf81ab1` |
| tezuka_fw (Buildroot ext) | `github.com/F5OEO/tezuka_fw` (`main`) | `c1e8f57` |
| Buildroot | `buildroot.org` | `2026.02` |

## Licensing

The fetched upstreams keep their own licenses (Buildroot, the Linux kernel,
maia-sdr, the ADI HDL library, etc.). The PlutoPack-original code in this repo
(`pico-firmware/`, `zynq-firmware/overlay/package/plutopack-panel/`, the
simulator, and the `fpga/projects/` glue) needs a license of **your** choosing —
add a `LICENSE` file and a copyright line before publishing. See
[`docs/LICENSES.md`](docs/LICENSES.md).
