# Building & flashing the Pi Pico UF2

The RP2040 Pico runs `plutopack_mcu` — the front-panel controller / Zynq
co-processor (buttons, encoder, fuel gauge, touch, display reset/CS, and the
SPI-slave link to the Zynq). A prebuilt UF2 ships in
[`../pico-firmware/plutopack_mcu.uf2`](../pico-firmware/plutopack_mcu.uf2) and
[`../prebuilt/pico/`](../prebuilt/pico).

## Flash the prebuilt UF2 (no toolchain)

1. Hold **BOOTSEL** on the Pico while plugging it into USB → it mounts as the
   `RPI-RP2` mass-storage drive.
2. Copy `plutopack_mcu.uf2` onto that drive. The Pico reboots into the firmware.

## Build from source

Requires the **Raspberry Pi Pico SDK** (2.1.1 used for this release),
`arm-none-eabi-gcc`, CMake and Ninja.

```bash
cd pico-firmware
export PICO_SDK_PATH=/path/to/pico-sdk      # your Pico SDK checkout
export PICO_TOOLCHAIN_PATH=/usr             # dir containing bin/arm-none-eabi-gcc
mkdir build && cd build
cmake -G Ninja -DPICO_BOARD=pico -DCMAKE_BUILD_TYPE=Release ..
ninja
# -> build/plutopack_mcu.uf2
```

`pico_sdk_import.cmake` is included in `pico-firmware/`; if `PICO_SDK_PATH` is
unset it will try `git clone` the SDK. Setting `PICO_SDK_PATH` to an existing
checkout is faster and offline-friendly.

## Pin map & protocol

The authoritative pin assignment (Zynq ↔ display ↔ Pico, with Zynq ball names
and FFC/header pins) is [`../pico-firmware/PINMAP.md`](../pico-firmware/PINMAP.md).
The SPI frame layout and command set are in the header comment of
`../pico-firmware/plutopack_mcu.c`.
