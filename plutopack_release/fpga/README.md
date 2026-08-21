# PlutoPack FPGA (fishball7020 / PlutoSky, Zynq-7020)

The FPGA image is a [maia-sdr](https://github.com/maia-sdr/maia-sdr) design for
the AD9361 on a Zynq-7020 (`xc7z020clg400-1`). maia-sdr provides the on-FPGA
spectrometer/waterfall; the ADI HDL library provides `axi_ad9361` and the
DMA/interconnect. This directory contains **only the PlutoPack-specific project
sources** — the large upstream HDL trees and all Vivado build artifacts are
fetched/generated, not vendored.

## What's here (our sources)

```
projects/fishball_iio/        the real HDL:
    system_top.v                 top-level wrapper (I/O, GPIO, SPI/EMIO to the Pico)
    system_constr.xdc            pin/timing constraints for the fishball7020 board
    system_bd.tcl                block-design switches (layers onto ../pluto)
    system_project.tcl           Vivado project recipe (device, sources, strategy)
    Makefile, *overclock.sh, create_fsbl_project.tcl
projects/fishball7020_iio/    the 7020 build variant that wraps the above
```

The projects reference the base `pluto` block design and `../../adi-hdl`,
`../../ip` (generated maia IP) from the upstream tree — that's why `setup.sh`
fetches maia-sdr and overlays these directories into it.

## Prerequisites

- **Xilinx Vivado 2025.2** (this is what the design was built and pinned against).
- A **Python virtualenv with Amaranth HDL + maia-hdl** installed, used to
  generate the maia-sdr IP cores. Create one, e.g.:
  ```bash
  python3 -m venv ~/.venvs/maia-hdl
  ~/.venvs/maia-hdl/bin/pip install amaranth
  # after setup.sh has fetched the tree, install maia-hdl itself:
  ~/.venvs/maia-hdl/bin/pip install -e fpga/maia-sdr/maia-hdl
  ```
  (See the upstream `maia-sdr/maia-hdl` README for the authoritative dependency
  list; the version pinned here is recorded in `setup.sh`.)
- `git`, `make`.

## Build

```bash
./setup.sh                 # clone maia-sdr@sweep + submodules, overlay our projects
cp env.sh.example env.sh   # then edit VIVADO_SETTINGS / MAIA_HDL_VENV for your box
./build.sh                 # IP cores -> synth -> impl -> bitstream -> XSA
```

`build.sh` exports `system_top.xsa` and copies it to
`../zynq-firmware/overlay/board/tezuka/fishball7020/bitstream/maia-iio/`, which is
where the SD-image build consumes it. A prebuilt XSA already ships there, so you
can skip the FPGA build entirely if you only want to rebuild the Linux image.

## Pinned upstream revisions

| Component | Repo | Revision |
|---|---|---|
| maia-sdr (HDL) | `github.com/F5OEO/maia-sdr` (branch `sweep`) | `2637b59` |
| adi-hdl | `github.com/analogdevicesinc/hdl` | `cf81ab1` |
| XilinxUnisimLibrary | `github.com/Xilinx/XilinxUnisimLibrary` | submodule default |

## Notes

- The FSBL is **not** rebuilt here (it needs XSCT/Vitis). A prebuilt
  `fsbl.elf` ships in the firmware tree; regenerate it with
  `make fsbl` inside the project only if you change the PS7 configuration.
- `overclock.sh` / `all_overclock.sh` patch the FSBL's PS7 init to raise the
  CPU/DDR PLL multipliers; the shipped image uses the stock (non-overclocked)
  FSBL.
