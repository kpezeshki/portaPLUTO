# Licensing overview

This repository combines **PlutoPack-original code** with **fetched upstream
components**. Sort out both before publishing.

## Upstream (fetched at build time, not vendored here)

These keep their own upstream licenses; nothing in this repo relicenses them:

| Component | Project | License (verify upstream) |
|---|---|---|
| Buildroot | buildroot.org | GPL-2.0-or-later (build system); packages keep their own |
| Linux kernel | analogdevicesinc/linux | GPL-2.0 |
| maia-sdr (HDL/httpd/etc.) | F5OEO/maia-sdr ← maia-sdr/maia-sdr | see upstream (mix incl. Apache-2.0) |
| ADI HDL library | analogdevicesinc/hdl | ADI's HDL license (see `adi-hdl/LICENSE_*`) |
| tezuka_fw external tree | F5OEO/tezuka_fw | see upstream repo |

When you distribute a built SD image you are redistributing GPL/other-licensed
binaries — follow those licenses (offer of source, etc.). The `setup.sh` scripts
record the exact pinned revisions so the corresponding source is identifiable.

## PlutoPack-original code (in this repo)

You hold the copyright on this code and should pick a license for it:

- `pico-firmware/plutopack_mcu.c`, `CMakeLists.txt`
- `zynq-firmware/overlay/package/plutopack-panel/` (the app suite, UI framework,
  and the host simulator `sim/`)
- `fpga/projects/fishball_iio/`, `fpga/projects/fishball7020_iio/` (the project
  glue, `system_top.v`, constraints)
- the board support in `zynq-firmware/overlay/board/tezuka/fishball7020/`

### Suggested next step

Add a top-level `LICENSE` file with your chosen license and a copyright line,
e.g. `Copyright (c) 2026 <your name or organisation>`. Common choices for
firmware like this are **MIT**, **BSD-3-Clause**, or **GPL-2.0** (the last keeps
it compatible with the GPL parts you build against). Because the front-panel
apps link only against their own sources and standard system libraries at
runtime (they talk to maia over HTTP and to the Pico over spidev), a permissive
license for your originals is generally fine — but confirm for your situation.
