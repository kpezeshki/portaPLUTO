# PlutoPack hardware architecture

PlutoPack (fishball7020 / PlutoSky) is a portable AD9361 SDR with a front panel.

## Components

- **Zynq-7020** (`xc7z020clg400-1`) — runs Linux + the maia-sdr FPGA image.
  Hosts the front-panel app suite and drives the display data bus and the RF.
- **AD9361** — the RF transceiver, driven by the FPGA (`axi_ad9361`) and
  configured through the `ad9361-phy` IIO driver.
- **ILI9341 320×240 panel** — SPI display on the Zynq's PS SPI1 + an EMIO GPIO
  for D/C, rendered via the fbtft framebuffer driver (`/dev/fb0`).
- **RP2040 Pico** — front-panel controller / co-processor: reads buttons, the
  rotary encoder, the resistive touch panel (TSC2007) and the fuel gauge
  (BQ27441), owns the display RESET/CS lines, and exposes it all to the Zynq
  over an SPI link.

## The Zynq ↔ Pico link

The Zynq talks to the Pico over a **bit-banged `spi-gpio`** bus on EMIO GPIO
(SCK / MOSI / MISO / CS + an attention/INT line), exposed to Linux as
`/dev/spidev*`. The Pico is the SPI **slave** and implements the framing with an
RP2040 **PIO** state machine so the link re-synchronises on every chip-select
edge. Each poll returns a status snapshot (buttons, encoder, touch X/Y/Z + pen
state, gauge, battery, user-GPIO) and a queue of UI events; commands let the
Zynq pulse display reset, set CS, and drive user GPIO.

## Full pin map

The authoritative, verified pin assignment — every Zynq ball (with device-DB pin
names), FFC/header pins, and the Pico GPIO assignments — is in
[`../pico-firmware/PINMAP.md`](../pico-firmware/PINMAP.md).

## Software map

| Layer | Where |
|---|---|
| FPGA (maia-sdr + AD9361 + GPIO/SPI to Pico) | [`../fpga`](../fpga) |
| Linux image + board support (DTS, overlay, U-Boot env) | [`../zynq-firmware`](../zynq-firmware) |
| Front-panel app suite + UI framework | `../zynq-firmware/overlay/package/plutopack-panel` |
| Host UI simulator | [`../simulator`](../simulator) |
| Pico controller firmware | [`../pico-firmware`](../pico-firmware) |
