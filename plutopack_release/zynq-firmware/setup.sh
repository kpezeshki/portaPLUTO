#!/usr/bin/env bash
# Fetch the upstream tezuka_fw Buildroot external tree (F5OEO fork) + Buildroot
# itself, then overlay the PlutoPack deltas from this repo (fishball7020 board,
# defconfig, and the plutopack-panel package). After this, ./build.sh produces
# the SD-card image.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEZUKA_DIR="${SCRIPT_DIR}/tezuka_fw"

# --- pinned upstream revision (what this release was built from) ---
TEZUKA_URL="https://github.com/F5OEO/tezuka_fw"
TEZUKA_COMMIT="c1e8f574148e71e7537077fff63c6213c29257d4"   # branch: main

if [ ! -d "${TEZUKA_DIR}/.git" ]; then
    echo "== Cloning tezuka_fw (${TEZUKA_URL}) =="
    git clone "${TEZUKA_URL}" "${TEZUKA_DIR}"
fi

echo "== Checking out pinned tezuka_fw revision =="
git -C "${TEZUKA_DIR}" fetch --all --tags
git -C "${TEZUKA_DIR}" checkout "${TEZUKA_COMMIT}" \
    || { echo "WARN: pinned commit not on remote; staying on 'main' tip"; git -C "${TEZUKA_DIR}" checkout main; }

echo "== Downloading Buildroot (version pinned in tezuka_fw/buildroot.version) =="
( cd "${TEZUKA_DIR}" && ./getbuildroot.sh )

echo "== Overlaying PlutoPack deltas =="
# board + defconfig + package are purely additive on top of upstream.
cp -rf "${SCRIPT_DIR}/overlay/board/tezuka/fishball7020" "${TEZUKA_DIR}/board/tezuka/"
cp -f  "${SCRIPT_DIR}/overlay/configs/"*                 "${TEZUKA_DIR}/configs/"
cp -rf "${SCRIPT_DIR}/overlay/package/plutopack-panel"   "${TEZUKA_DIR}/package/"
echo "   -> board/tezuka/fishball7020, configs/, package/plutopack-panel"

# Ensure the board->defconfig mapping exists in boards.json (idempotent).
BJ="${TEZUKA_DIR}/boards.json"
if ! grep -q '"fishball7020"' "${BJ}" 2>/dev/null; then
    echo "== Registering fishball7020 in boards.json =="
    if command -v jq >/dev/null 2>&1; then
        tmp="$(mktemp)"
        jq '. + [{"board":"fishball7020","defconfig":"fishball_maiasdr_7020_defconfig"}]' "${BJ}" > "${tmp}" && mv "${tmp}" "${BJ}"
    else
        echo "WARN: jq not found and boards.json has no fishball7020 entry; add it manually:"
        echo '   {"board":"fishball7020","defconfig":"fishball_maiasdr_7020_defconfig"}'
    fi
fi

echo ""
echo "Done. Next:  ./build.sh"
echo "(The FPGA XSA already ships in the overlay; rebuild it with ../fpga if desired.)"
