# PlutoPack — Raspberry Pi Pico "MCU interface" firmware

The Pico (RP2040) is the PlutoPack front-panel controller and a peripheral of
the PlutoSky/Zynq. `plutopack_mcu.c`:

1. Reads the local UI — buttons SW1/SW3–SW8, the SW2 quadrature encoder — and
   the on-board sensors: **BQ27441-G1** fuel gauge and **TSC2007** resistive
   touch controller (both on I²C1).
2. Serves the **Zynq↔Pico SPI link** (Pico = SPI **slave**). Linux on the Zynq
   polls, over a bit-banged `spi-gpio` bus, a queue of UI events plus a live
   status snapshot (buttons, encoder, touch X/Y/Z + pen state, gauge, battery,
   user-GPIO), and drives the panel control lines wired to the Pico.
3. Owns the display **RESET** and **CS** lines (the rest of the panel SPI comes
   from the Zynq), and drives two user GPIO outputs + reads one input on behalf
   of the Zynq.
4. Mirrors a human-readable status report to **USB-CDC serial** for debugging.

The full pin assignment and the Zynq-side wiring are documented in
[`PINMAP.md`](PINMAP.md); the SPI frame layout and command set are described in
the header comment of [`plutopack_mcu.c`](plutopack_mcu.c). The current build
uses an RP2040 **PIO** state machine for the SPI-slave framing so the link
re-synchronises on every chip-select edge (robust against the Zynq's software
`spi-gpio` timing).

## Flash the prebuilt UF2

1. Hold **BOOTSEL** while plugging the Pico into USB → it mounts as `RPI-RP2`.
2. Copy [`plutopack_mcu.uf2`](plutopack_mcu.uf2) onto that drive. The Pico
   reboots and runs.

## Build from source

Requires the **Raspberry Pi Pico SDK**, `arm-none-eabi-gcc`, CMake and Ninja.

```bash
export PICO_SDK_PATH=/path/to/pico-sdk          # your Pico SDK checkout (2.1.1 used here)
export PICO_TOOLCHAIN_PATH=/usr                 # if arm-none-eabi-gcc is in /usr/bin
mkdir build && cd build
cmake -G Ninja -DPICO_BOARD=pico -DCMAKE_BUILD_TYPE=Release ..
ninja
# -> build/plutopack_mcu.uf2
```

Built and verified against Pico SDK 2.1.1 (RP2040, board `pico`).

## View the serial report

The Pico enumerates as a USB-CDC device (`/dev/ttyACM0`); baud is irrelevant:

```bash
screen /dev/ttyACM0 115200     # or minicom -D /dev/ttyACM0 / picocom /dev/ttyACM0
```

It prints a periodic line group with the gauge reading, an independent
ADC battery-divider voltage, encoder position, button states, touch state, the
SPI link status (frames/s, sequence), and the I²C health of the gauge and touch
controller.

## Notes

- `linux/` contains host-side helpers used during panel-UI development
  (`gen_font.py`, a framebuffer mock `pp_mock.c`, and a render harness) — not
  needed to build or flash the Pico firmware.
- The BQ27441 is (re)configured at runtime for the pack's design capacity and
  chemistry; see the gauge section of the source.
- Touch coordinates are delivered to the Zynq already rotated to the display
  orientation; residual scale/offset is corrected by the on-panel TOUCH CAL app.
