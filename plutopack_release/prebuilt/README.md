# Prebuilt binaries

Flash-ready artifacts so you can bring a board up without building from source.

```
pico/plutopack_mcu.uf2        Pico controller firmware (drag-and-drop in BOOTSEL mode)
sd-image/                     Zynq SD-card boot files
    BOOT.bin                  FSBL + FPGA bitstream + U-Boot
    uImage                    Linux kernel
    devicetree.dtb            device tree
    uEnv.txt                  U-Boot environment
    uramdisk.image.xz         root filesystem (initramfs)
    system_top.bin            raw bitstream (reference)
    tezuka.zip                the images/ set, zipped
```

- **Pico:** hold BOOTSEL, plug in USB, copy `pico/plutopack_mcu.uf2` to the
  `RPI-RP2` drive. See [`../docs/BUILD_PICO_UF2.md`](../docs/BUILD_PICO_UF2.md).
- **SD card:** write the `sd-image/` files to a FAT32 boot partition. See
  [`../docs/BUILD_SD_IMAGE.md`](../docs/BUILD_SD_IMAGE.md). Note the root FS here
  is `uramdisk.image.xz`; make sure `uEnv.txt`'s ramdisk filename matches what
  you copy (decompress/rename if your `uEnv.txt` expects `.gz`).

These match the source in this repo at release time. Rebuilding from source
(`fpga/` → `zynq-firmware/`, and `pico-firmware/`) reproduces them.
