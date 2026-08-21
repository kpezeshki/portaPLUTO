#!/usr/bin/env bash
# Fetch the upstream maia-sdr HDL tree (F5OEO fork, 'sweep' branch) plus its
# adi-hdl / XilinxUnisimLibrary submodules, then overlay the PlutoPack FPGA
# project sources from this repo on top. After this runs, ./build.sh can build
# the bitstream + XSA.
#
# Nothing here is machine-specific; tool paths live in env.sh (see env.sh.example).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UPSTREAM_DIR="${SCRIPT_DIR}/maia-sdr"

# --- pinned upstream revisions (the exact versions this release was built from) ---
MAIA_SDR_URL="https://github.com/F5OEO/maia-sdr"
MAIA_SDR_COMMIT="2637b59891bdfe38d8c5bf52c84984b3da68064e"   # branch: sweep
ADI_HDL_COMMIT="cf81ab15cdb1376982b53827a9aa3db01297eecc"    # analogdevicesinc/hdl

if [ ! -d "${UPSTREAM_DIR}/.git" ]; then
    echo "== Cloning maia-sdr (${MAIA_SDR_URL}) =="
    git clone "${MAIA_SDR_URL}" "${UPSTREAM_DIR}"
fi

echo "== Checking out pinned maia-sdr revision =="
git -C "${UPSTREAM_DIR}" fetch --all --tags
git -C "${UPSTREAM_DIR}" checkout "${MAIA_SDR_COMMIT}" \
    || { echo "WARN: pinned commit not on remote; staying on 'sweep' tip"; git -C "${UPSTREAM_DIR}" checkout sweep; }

echo "== Initialising submodules (adi-hdl, XilinxUnisimLibrary) =="
git -C "${UPSTREAM_DIR}" submodule update --init --recursive
# Pin adi-hdl to the revision used for this release (best-effort).
if [ -d "${UPSTREAM_DIR}/maia-hdl/adi-hdl/.git" ] || [ -f "${UPSTREAM_DIR}/maia-hdl/adi-hdl/.git" ]; then
    git -C "${UPSTREAM_DIR}/maia-hdl/adi-hdl" checkout "${ADI_HDL_COMMIT}" 2>/dev/null \
        || echo "WARN: could not pin adi-hdl to ${ADI_HDL_COMMIT}; using submodule default"
fi

echo "== Overlaying PlutoPack FPGA project sources =="
for P in fishball7020_iio fishball_iio; do
    dst="${UPSTREAM_DIR}/maia-hdl/projects/${P}"
    mkdir -p "${dst}"
    cp -f "${SCRIPT_DIR}/projects/${P}/"* "${dst}/"
    echo "   -> maia-hdl/projects/${P}"
done

echo ""
echo "Done. Next:  cp env.sh.example env.sh  (edit tool paths)  &&  ./build.sh"
