# Building & flashing the PlutoPack SD-card image

The PlutoPack boots a Zynq-7020 (fishball7020 / PlutoSky) from an SD card. This
walks through producing the image from source and writing it to a card. If you
just want to flash a working image, skip to [Flashing](#flashing) and use
`prebuilt/sd-image/`.

## 1. (Optional) Build the FPGA bitstream

A prebuilt FPGA XSA already ships in the firmware tree, so this step is only
needed if you changed the HDL. Requires Vivado 2025.2 + an Amaranth/maia-hdl
virtualenv.

```bash
cd fpga
./setup.sh                 # fetch maia-sdr@sweep + adi-hdl, overlay our projects
cp env.sh.example env.sh   # edit VIVADO_SETTINGS / MAIA_HDL_VENV
./build.sh                 # -> exports system_top.xsa into zynq-firmware/overlay/...
```

See [`../fpga/README.md`](../fpga/README.md).

## 2. Build the Linux image (Buildroot)

```bash
cd zynq-firmware
./setup.sh     # clone tezuka_fw@main, download Buildroot 2026.02, overlay our deltas
./build.sh     # first build compiles a full cross-toolchain + userspace — expect 1–3 h
```

Requirements: a Linux host with `make gcc git jq wget cpio unzip bc rsync file
python3` and ~20 GB free disk. No Vivado needed here — the FPGA XSA is prebuilt.

### Outputs

```
zynq-firmware/tezuka_fw/output/fishball7020/images/
    BOOT.bin              FSBL + bitstream + U-Boot
    uImage                Linux kernel
    devicetree.dtb        device tree
    uramdisk.image.gz     root filesystem (initramfs)
    uEnv.txt              U-Boot environment
zynq-firmware/tezuka_fw/build/fishball7020.zip   the above, zipped
```

## 3. Prepare the SD card

A single small FAT32 boot partition is all the Zynq needs (this build runs the
root filesystem from a RAM disk / initramfs, so there is no separate rootfs
partition to write).

```bash
# Identify your card first!  lsblk    (example uses /dev/sdX — DOUBLE-CHECK THIS)
sudo parted -s /dev/sdX mklabel msdos
sudo parted -s /dev/sdX mkpart primary fat32 1MiB 100%
sudo parted -s /dev/sdX set 1 boot on
sudo mkfs.vfat -F32 /dev/sdX1
```

## Flashing

Copy the boot files onto the FAT32 partition:

```bash
sudo mount /dev/sdX1 /mnt
sudo unzip -o fishball7020.zip -d /mnt        # or copy the individual images/ files
sudo cp BOOT.bin uImage devicetree.dtb uramdisk.image.gz uEnv.txt /mnt/   # if not using the zip
sudo umount /mnt
```

(The `prebuilt/sd-image/` directory ships `uramdisk.image.xz`; if your `uEnv.txt`
expects `uramdisk.image.gz`, decompress and recompress accordingly, or adjust
the `uenvcmd`/`ramdisk_image` name in `uEnv.txt` to match the file you place.)

Insert the card, power on. The front panel shows the boot splash, then the
launcher. The FPGA is loaded by the FSBL from `BOOT.bin`; maia-httpd starts
on demand when you open the WATERFALL app.

## Troubleshooting

- **Blank panel / no splash** — check the display FFC and that the Pico is
  running its firmware (it releases the panel from reset). See
  [`../pico-firmware`](../pico-firmware).
- **Panel up but battery reads 0 % / touch dead** — the Pico link or the I²C
  bus to the gauge/touch IC; the FRONTEND STAT app shows live link + I²C health.
- **No FPGA / AD9361 not found** — verify `BOOT.bin` contains the bitstream
  (it does when built via this flow) and that the XSA matched the board.
