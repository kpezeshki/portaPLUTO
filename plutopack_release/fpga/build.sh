#!/usr/bin/env bash
# Build the PlutoPack (fishball7020) FPGA bitstream and export the XSA, then
# copy the XSA into the Zynq firmware tree so the SD-image build can pick it up.
#
# Runs the full from-scratch flow:
#   1. maia-sdr IP cores   (Amaranth -> Verilog -> Vivado package_ip)
#   2. ADI library IP       (axi_ad9361, ...)
#   3. Vivado synth -> impl -> bitstream -> XSA export
#
# It invokes the ADI '<project>.sdk/system_top.xsa' target directly rather than
# the Makefile 'all' target, because 'all' also runs fsbl (needs XSCT/Vitis) and
# an 'install' step hardcoded to the wrong path. A prebuilt FSBL already ships in
# the firmware tree, so we skip it here.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --- machine-specific tool paths (never committed) ---
if [ ! -f "${SCRIPT_DIR}/env.sh" ]; then
    echo "ERROR: ${SCRIPT_DIR}/env.sh not found. Run: cp env.sh.example env.sh  and edit it." >&2
    exit 1
fi
# shellcheck source=/dev/null
source "${SCRIPT_DIR}/env.sh"

[ -f "${VIVADO_SETTINGS}" ] || { echo "ERROR: VIVADO_SETTINGS not found: ${VIVADO_SETTINGS}" >&2; exit 1; }
# shellcheck source=/dev/null
source "${VIVADO_SETTINGS}"
# Put the maia-hdl venv's python first so the IP-core Makefiles' `python3 -m
# maia_hdl.maia_sdr` sees Amaranth (Vivado's bundled python stays on PATH but
# loses the `python3` name race, which is what we want).
export PATH="${MAIA_HDL_VENV}/bin:${PATH}"

PROJECT_DIR="${SCRIPT_DIR}/maia-sdr/maia-hdl/projects/fishball7020_iio"
XSA_SRC="${PROJECT_DIR}/fishball.sdk/system_top.xsa"
XSA_DEST="${SCRIPT_DIR}/../zynq-firmware/overlay/board/tezuka/fishball7020/bitstream/maia-iio/system_top.xsa"

[ -d "${PROJECT_DIR}" ] || { echo "ERROR: ${PROJECT_DIR} missing. Run ./setup.sh first." >&2; exit 1; }

echo "Vivado:  $(command -v vivado)"
echo "python3: $(command -v python3)  ($(python3 -c 'import amaranth; print("amaranth", amaranth.__version__)' 2>/dev/null || echo 'amaranth NOT found in venv'))"
echo ""

cd "${PROJECT_DIR}"
make fishball.sdk/system_top.xsa

[ -f "${XSA_SRC}" ] || { echo "ERROR: XSA not produced at ${XSA_SRC}" >&2; exit 1; }
mkdir -p "$(dirname "${XSA_DEST}")"
cp -f "${XSA_SRC}" "${XSA_DEST}"

echo ""
echo "=========================================="
echo "Bitstream build complete."
echo "XSA exported:  ${XSA_SRC}"
echo "XSA installed: ${XSA_DEST}"
echo ""
echo "Next: build the SD image  ->  ../zynq-firmware/build.sh"
echo "=========================================="
