# Prebuilt binaries

Flash-ready artifacts so you can bring a board up without building from source.

```
pico/plutopack_mcu.uf2        Pico controller firmware (drag-and-drop in BOOTSEL mode)
sd-image/                     Zynq SD-card boot files
    BOOT.bin                  FSBL + FPGA bitstream + U-Boot
    uImage                    Linux kernel
    devicetree.dtb            device tree
    uEnv.txt                  U-Boot environment
    uramdisk.image.xz         U-Boot-wrapped root filesystem (historical filename)
    system_top.bin            raw bitstream (reference)
    tezuka.zip                the images/ set, zipped
```

- **Pico:** hold BOOTSEL, plug in USB, copy `pico/plutopack_mcu.uf2` to the
  `RPI-RP2` drive. See [`../docs/BUILD_PICO_UF2.md`](../docs/BUILD_PICO_UF2.md).
- **SD card:** copy the `sd-image/` boot files to a FAT32 boot partition. See
  [`../docs/BUILD_SD_IMAGE.md`](../docs/BUILD_SD_IMAGE.md). This release's
  `uEnv.txt` expects the exact filename `uramdisk.image.xz`; despite the suffix,
  the file is a U-Boot ramdisk image and must be copied unchanged.

The prebuilt SD image is a coordinated set. Keep the loose boot files and
`tezuka.zip` from the same release when copying or archiving them.
