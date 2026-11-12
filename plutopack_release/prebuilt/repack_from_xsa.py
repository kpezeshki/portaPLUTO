#!/usr/bin/env python3
"""Repack the SD boot artifacts with a rebuilt PlutoPack FPGA XSA.

Keeps the existing U-Boot payload and the three matching FSBL variants.
Requires Xilinx bootgen on PATH or --bootgen. Run after fpga/build.sh.
"""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
SD = ROOT / "prebuilt" / "sd-image"
BOARD = ROOT / "zynq-firmware" / "overlay" / "board" / "tezuka" / "fishball7020"
DEFAULT_XSA = BOARD / "bitstream" / "maia-iio" / "system_top.xsa"


def partition(image: bytes, index: int) -> tuple[bytes, int, int]:
    if struct.unpack_from("<I", image, 0x20)[0] != 0xAA995566:
        raise ValueError("not a Zynq BOOT.bin")
    pht = struct.unpack_from("<I", image, 0x9C)[0]
    length, _, _, load, entry, offset = struct.unpack_from(
        "<6I", image, pht + index * 64
    )
    start, end = offset * 4, (offset + length) * 4
    if not length or end > len(image):
        raise ValueError(f"invalid partition {index}")
    return image[start:end], load, entry


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--xsa", type=Path, default=DEFAULT_XSA)
    ap.add_argument("--bootgen", default=shutil.which("bootgen"))
    args = ap.parse_args()
    if not args.bootgen:
        ap.error("bootgen not found; pass --bootgen /path/to/bootgen")

    old_boot = (SD / "BOOT.bin").read_bytes()
    uboot, load, entry = partition(old_boot, 2)
    if load != 0x04000000 or entry != load:
        raise ValueError("unexpected U-Boot load/entry address")

    fsbls = {
        "BOOT.bin": BOARD / "bitstream" / "fsbl.elf",
        "overclock/BOOT_fsbl_CPU950_DDR600":
            BOARD / "bitstream" / "overclock" / "fsbl_CPU950_DDR600.elf",
        "overclock/BOOT_fsbl_CPU1100_DDR750":
            BOARD / "bitstream" / "overclock" / "fsbl_CPU1100_DDR750.elf",
    }
    with tempfile.TemporaryDirectory(prefix="pp-repack-") as td:
        tmp = Path(td)
        bit = tmp / "system_top.bit"
        with ZipFile(args.xsa) as xsa:
            bit.write_bytes(xsa.read("system_top.bit"))
        raw_uboot = tmp / "u-boot.bin"
        raw_uboot.write_bytes(uboot)
        staged = {}
        raw_bit = None
        for name, fsbl in fsbls.items():
            bif = tmp / (name.replace("/", "_") + ".bif")
            boot = tmp / (name.replace("/", "_") + ".bin")
            bif.write_text(
                "the_ROM_image:\n{\n"
                f"  [bootloader] {fsbl}\n"
                f"  {bit}\n"
                f"  [load=0x04000000, startup=0x04000000] {raw_uboot}\n"
                "}\n"
            )
            subprocess.run(
                [args.bootgen, "-arch", "zynq", "-image", str(bif),
                 "-o", str(boot), "-w", "on"],
                check=True, stdout=subprocess.DEVNULL,
            )
            data = boot.read_bytes()
            this_bit, _, _ = partition(data, 1)
            this_uboot, _, _ = partition(data, 2)
            if this_uboot != uboot:
                raise ValueError(f"U-Boot changed in {name}")
            if raw_bit is None:
                raw_bit = this_bit
            elif this_bit != raw_bit:
                raise ValueError("bitstream differs across BOOT variants")
            staged[name] = data
        staged["system_top.bin"] = raw_bit

        archive = tmp / "tezuka.zip"
        with ZipFile(SD / "tezuka.zip") as old, ZipFile(archive, "w") as new:
            for info in old.infolist():
                if info.is_dir():
                    new.writestr(info, b"")
                    continue
                rel = Path(info.filename).relative_to("sdimg")
                data = staged.get(str(rel))
                if data is None:
                    data = (SD / rel).read_bytes()
                new.writestr(info, data)
        with ZipFile(archive) as z:
            for info in z.infolist():
                if info.is_dir():
                    continue
                rel = str(Path(info.filename).relative_to("sdimg"))
                data = staged.get(rel)
                if data is None:
                    data = (SD / rel).read_bytes()
                if sha(z.read(info)) != sha(data):
                    raise ValueError(f"ZIP mismatch: {rel}")

        for name, data in staged.items():
            target = SD / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            print(f"{name}: {sha(data)}")
        staged_zip = SD / "tezuka.zip.tmp"
        shutil.copy2(archive, staged_zip)
        os.replace(staged_zip, SD / "tezuka.zip")
        print(f"tezuka.zip: {sha((SD / 'tezuka.zip').read_bytes())}")


if __name__ == "__main__":
    main()
