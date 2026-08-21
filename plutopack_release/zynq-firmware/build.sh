#!/usr/bin/env bash
# Build the fishball7020 SD-card image with Buildroot.
#
# Wraps tezuka_fw's own board build (make <defconfig> && make) via its build.sh.
# Requires ./setup.sh to have been run first (clones tezuka_fw + Buildroot and
# overlays our deltas).
#
# Output:
#   tezuka_fw/output/fishball7020/images/   (BOOT.bin, uImage, devicetree.dtb,
#                                             uramdisk.image.*, uEnv.txt, ...)
#   tezuka_fw/build/fishball7020.zip        (packaged archive)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEZUKA_DIR="${SCRIPT_DIR}/tezuka_fw"

[ -d "${TEZUKA_DIR}/buildroot" ] || { echo "ERROR: run ./setup.sh first (no tezuka_fw/buildroot)." >&2; exit 1; }

XSA="${TEZUKA_DIR}/board/tezuka/fishball7020/bitstream/maia-iio/system_top.xsa"
[ -f "${XSA}" ] || { echo "ERROR: FPGA XSA missing at ${XSA}. Build it in ../fpga or restore the shipped one." >&2; exit 1; }

# Buildroot compiles thousands of small files; if this repo lives on a network
# share, point the build at local disk with BUILDROOT_O=... (optional).
JOBS="${JOBS:-$(nproc)}"

echo "== Building fishball7020 (this takes a while on a first run) =="
( cd "${TEZUKA_DIR}" && ./build.sh -j"${JOBS}" fishball7020 )

echo ""
echo "=========================================="
echo "SD-image build complete."
echo "Images:  ${TEZUKA_DIR}/output/fishball7020/images/"
echo "Archive: ${TEZUKA_DIR}/build/fishball7020.zip"
echo "See ../docs/BUILD_SD_IMAGE.md for how to write these to an SD card."
echo "=========================================="
