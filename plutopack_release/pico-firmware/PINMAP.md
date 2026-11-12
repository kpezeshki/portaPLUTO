# PlutoPack V1.1 pin map

On the top side (inductors visible), J1 pin 1 (+5 V) is bottom right;
pin 3 (+3.3 V) and pin 5 (+1.8 V) are above it. Display 3.3 V I/O is on
the right row at pins 7, 9, 11 and 13. Kaveh confirmed this as the V1.1
header orientation on 2026-11-11.

This map follows the checked-in V1.1 KiCad netlist. The older V6 review
map does not describe the V1.1 display wiring.

## Display and PlutoSky header

The panel specification calls J2.37 RS_SCL (serial clock) and J2.36 WR_RS
(data/command) in four-wire SPI mode. J2.35 LCD_RD/RDX is unused and must
remain high. Pico GPIO1 drives the panel chip select.

| Firmware signal | Zynq ball | Bank / I/O standard | J1 | Panel path |
|---|---|---|---:|---|
| disp_sdo (MISO) | V10 | 13 / LVCMOS33 | 7 | J2.33 SDO |
| disp_sdi (MOSI) | U9 | 13 / LVCMOS33 | 9 | J2.34 SDI |
| disp_scl (SCLK) | U10 | 13 / LVCMOS33 | 11 | J2.37 LCD_RS / RS_SCL |
| disp_rs (D/C) | T9 | 13 / LVCMOS33 | 13 | J2.36 LCD_WR / WR_RS |
| disp_csn (RDX idle high) | G19 | 35 / LVCMOS25 | 4 | U7 A1 to B1, J2.35 LCD_RD |
| Pico GPIO0 (RESET) | — | — | — | J2.10 |
| Pico GPIO1 (CS) | — | — | — | J2.38 |

The Zynq ball values are the PlutoSky-side assignments in the FPGA
constraints. J1 and J2 connections were cross-checked against the V1.1
KiCad netlist. The schematic's LCD_RS, LCD_WR and LCD_RD names are parallel
interface names; use the serial functions above when editing HDL.

## Zynq ↔ Pico bus

U7 shifts the PlutoSky header signals to the Pico side. EMIO line = 54 +
bit. Display D/C is EMIO bit 18 / line 72; bits 19–25 are mcu_io[0..6].

| Pico GPIO | Role | J1 | Zynq ball | EMIO line |
|---:|---|---:|---|---:|
| 16 | SPI0 RX / MOSI | 18 | L15 | 79 |
| 17 | SPI0 CSn | 16 | L14 | 78 |
| 18 | SPI0 SCK | 14 | H17 | 77 |
| 19 | SPI0 TX / MISO | 12 | H16 | 76 |
| 20 | attention, active low | 10 | H18 | 75 |
| 21 | spare | 8 | J18 | 74 |
| 22 | spare | 6 | G20 | 73 |

The Zynq runs a bit-banged spi-gpio master. The Pico runs an SPI0 slave.

## Other Pico pins

| Pico GPIO | Role |
|---:|---|
| 2 / 3 | I2C1 SDA / SCL: BQ27441-G1 at 0x55, TSC2007 at 0x48 |
| 4 / 5 / 6 / 7 / 8 / 9 / 10 | SW1 / SW5 / SW4 / SW3 / SW6 / SW7 / SW8 |
| 11 / 12 | SW2 encoder A / B |
| 13 / 14 | user GPIO outputs |
| 15 | user GPIO input |
| 25 | Pico onboard LED |
| 26 | battery ADC0 through R21/R22 divider |
| 28 | TSC2007 active-low PENIRQ |

## Source and verification

- Hardware: the checked-in V1.1 KiCad schematic and PCB.
- FPGA: fpga/projects/fishball_iio/system_constr.xdc and system_top.v.
- Linux: zynq-firmware/overlay/board/tezuka/fishball7020/dts/fishball.dtsi.
- Pico: pico-firmware/plutopack_mcu.c.

This is a source-level electrical cross-check. Assembled V1.1 hardware still
needs a flash and panel test of the rebuilt image.
