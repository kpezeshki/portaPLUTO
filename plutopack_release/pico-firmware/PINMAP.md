# PlutoPack — authoritative pin map (Zynq ↔ display ↔ Pico)

**Device:** `xc7z020-2clg400i` (fishball7020 / PlutoSky). All Zynq balls, banks and pin
names verified against the Vivado 2025.2 device database (`link_design -part xc7z020clg400-1`,
package-identical to `-2clg400i`). Pico side = Raspberry Pi Pico (RP2040). Board connectivity
from the plutopack V6 review netlist (parser cross-checked 372/372 vs the KiCad PCB).

Last updated 2026-07-25.

---

## 1. Display SPI — Zynq **bank 13 @ 3.3 V** (LVCMOS33), wired directly to the 3.3 V panel

| Top-level port | Signal (panel) | Zynq ball | Zynq pin name (device DB) | Dir (Zynq) | FFC J2 | Header J1 |
|---|---|---|---|---|---|---|
| `disp_sdo` | SDO (panel→Zynq, MISO) | **V10** | `IO_L21N_T3_DQS_13` | in  | 33 | 7 |
| `disp_sdi` | SDI (Zynq→panel, MOSI) | **U9**  | `IO_L17P_T2_13`      | out | 34 | 9 |
| `disp_csn` | LCD_RD                 | **U10** | `IO_L12N_T1_MRCC_13` | out | 35 | 11 |
| `disp_scl` | LCD_WR (SPI SCLK)      | **T9**  | `IO_L12P_T1_MRCC_13` | out | 36 | 13 |

**Name/ball reconciliation — RESOLVED (2026-07-25):** the schematic labels `IO_L20N=V10`
and `IO_L16P=U9`, but the device DB says **V10 = `IO_L21N_T3_DQS_13`** and **U9 = `IO_L17P_T2_13`**
(Xilinx's `L16P`/`L20N` are actually W10/Y13). Cause: **CLG400 bank 13 is only partially bonded**
(25 I/O vs 50 in banks 34/35) — Xilinx's pair **L15 is unbonded**, so the bonded bank-13 pairs
carry non-contiguous official names (…L14, L16, L17…). The SDR schematic's FPGA symbol renumbered
the bonded pairs *contiguously*, so every pair above the L15 gap reads one lower than Xilinx
(symbol L16P = Xilinx L17P = U9; symbol L20N = Xilinx L21N = V10). Pairs ≤ L14 (e.g. L12 = T9/U10)
are below the gap and match. **No error:** the XDC constrains by ball (the copper), so SDI=U9 /
SDO=V10 are correct; only the schematic's cosmetic L-labels are offset. Banks 34/35 are fully
bonded → no offset there. `V10` is a `DQS` pin — fine as ordinary I/O.

These four replace the previous agent's guesses (SDO=Y13, SDI=W10); U10/T9 were already correct.

---

## 2. D/C + Zynq↔Pico bus — Zynq **bank 35 @ 1.8 V** (LVCMOS18), level-shifted to 3.3 V by U7 (TXS0108)

| Top-level port | Zynq ball | Zynq pin name (device DB) | EMIO `gpio0` line | Header J1 | Pico GPIO | Pico SPI0 function |
|---|---|---|---|---|---|---|
| `disp_rs` (LCD_RS / D-C) | **G19** | `IO_L18P_T2_AD13P_35`      | 72 | 4  | — (to panel) | — |
| `mcu_io[6]` | **L15** | `IO_L22N_T3_AD7N_35`      | 79 | 18 | **GPIO16** | RX  = MOSI (Zynq→Pico) |
| `mcu_io[5]` | **L14** | `IO_L22P_T3_AD7P_35`      | 78 | 16 | **GPIO17** | CSn = CS   (Zynq→Pico) |
| `mcu_io[4]` | **H17** | `IO_L13N_T2_MRCC_35`      | 77 | 14 | **GPIO18** | SCK        (Zynq→Pico) |
| `mcu_io[3]` | **H16** | `IO_L13P_T2_MRCC_35`      | 76 | 12 | **GPIO19** | TX  = MISO (Pico→Zynq) |
| `mcu_io[2]` | **H18** | `IO_L14N_T2_AD4N_SRCC_35` | 75 | 10 | **GPIO20** | INT / attention (Pico→Zynq, active-low) |
| `mcu_io[1]` | **J18** | `IO_L14P_T2_AD4P_SRCC_35` | 74 | 8  | **GPIO21** | spare |
| `mcu_io[0]` | **G20** | `IO_L18N_T2_AD13N_35`     | 73 | 6  | **GPIO22** | spare |

EMIO line = `54 + emio_bit`; the HDL breaks out `disp_mcu_gpio[0]`=D/C (bit18/line72) and
`disp_mcu_gpio[1..7]`=`mcu_io[0..6]` (bits19..25 / lines73..79). GPIO16–19 are the RP2040's
native hardware **SPI0** pins (RX/CSn/SCK/TX) — the board was laid out for the Pico as SPI slave.

### ⚠️ Two open items for bank 35 (must resolve before the Vivado rebuild)
1. **VCCO.** Bank 35 is currently constrained **LVCMOS25 (2.5 V)** on 13 pins
   (`J19 J20 K14 K19 L14 L19 L20 M14 M15 M19 M20 N15 N16` — AD9361 ctrl/GPIO + PL-SPI). A bank has
   one VCCO, so the 1.8 V group can only live here if the **whole bank is 1.8 V** (all those pins →
   LVCMOS18). The plutopack level-shifter A-side (VCCA = +1V8 from PlutoSky J1.5) implies bank 35 =
   1.8 V. **Confirm the PlutoSky bank-35 rail is 1.8 V** (and that the SDR control pins are 1.8 V).
2. **Pin collision.** Ball **L14** is `pl_spi_clk_o` in the current design but is mapped here to
   GPIO17/CS. The other 7 balls are free. **Confirm `pl_spi` is unused** so L14 can be reclaimed.

---

## 3. Display control lines the **Pico** owns (level-shifted to the panel FFC)

| Pico GPIO | Signal | Active | FFC J2 | Notes |
|---|---|---|---|---|
| **GPIO0** | LCD_RST (panel reset) | low | 10 | FW releases (high) ~20 ms after boot |
| **GPIO1** | LCD_CS (panel chip-select) | low | 38 | FW asserts (low) after reset so fbtft needs no handshake |

The panel's SCLK/MOSI/MISO/D-C come from the Zynq (sections 1–2); only RESET + CS are on the Pico.

---

## 4. Pico (RP2040) — complete pinout

| Pico GPIO | Function | Detail |
|---|---|---|
| GPIO0  | LCD_RST out | panel reset (see §3) |
| GPIO1  | LCD_CS out  | panel chip-select (see §3) |
| GPIO2  | I²C1 SDA | BQ27441-G1 @0x55, TSC2007 @0x48 |
| GPIO3  | I²C1 SCL | " |
| GPIO4  | SW1 in  | button, active-high (10k pull-down + 1k series) |
| GPIO5  | SW5 in  | button |
| GPIO6  | SW4 in  | button |
| GPIO7  | SW3 in  | button |
| GPIO8  | SW6 in  | button |
| GPIO9  | SW7 in  | button |
| GPIO10 | SW8 in  | button |
| GPIO11 | Encoder A (SW2) | quadrature, common→GND, ext 10k pull-ups, active-low |
| GPIO12 | Encoder B (SW2) | " |
| GPIO16 | SPI0 RX  = MOSI | Zynq bus (→ Zynq L15) |
| GPIO17 | SPI0 CSn = CS   | Zynq bus (→ Zynq L14) |
| GPIO18 | SPI0 SCK        | Zynq bus (→ Zynq H17) |
| GPIO19 | SPI0 TX  = MISO | Zynq bus (→ Zynq H16) |
| GPIO20 | INT (to Zynq)   | active-low: low = events queued (→ Zynq H18) |
| GPIO21 | spare           | (→ Zynq J18) |
| GPIO22 | spare           | (→ Zynq G20) |
| GPIO25 | on-board LED    | blinks on button press / encoder step |
| GPIO26 | ADC0 | battery voltage via +BATT ÷2 divider (R21/R22) |
| GPIO28 | TSC2007 /PENIRQ in | open-drain active-low, internal pull-up |

---

## 5. Data flow (the demo-serial content, on the panel)

```
 sensors + UI            SPI0 slave           spi-gpio (EMIO)        fbtft / PS-SPI1
 (BQ27441, TSC2007,  →   status frame    →    Linux on Zynq    →    /dev/fb0 (ILI9341
  buttons, encoder)      over Pico bus        polls + renders       320x240, rotate=90)
```
The Pico owns the sensors, so it serves the gauge/encoder/button/touch snapshot + event queue
over the SPI bus; Linux polls it and draws to the panel. Panel RESET/CS are driven by the Pico.

---

## 6. Corrected Zynq XDC (fishball_iio/system_constr.xdc) — pending the two §2 confirmations

```tcl
# --- PlutoSky front-panel display (PS SPI1) — BANK 13, 3.3 V, direct to panel ---
set_property -dict {PACKAGE_PIN V10 IOSTANDARD LVCMOS33} [get_ports disp_sdo]  ;# IO_L21N_T3_DQS_13
set_property -dict {PACKAGE_PIN U9  IOSTANDARD LVCMOS33} [get_ports disp_sdi]  ;# IO_L17P_T2_13
set_property -dict {PACKAGE_PIN U10 IOSTANDARD LVCMOS33} [get_ports disp_csn]  ;# IO_L12N_T1_MRCC_13 (LCD_RD)
set_property -dict {PACKAGE_PIN T9  IOSTANDARD LVCMOS33} [get_ports disp_scl]  ;# IO_L12P_T1_MRCC_13 (LCD_WR)

# --- D/C + MCU bus — BANK 35, keep the bank's existing LVCMOS25 (see decision note) ---
set_property -dict {PACKAGE_PIN G19 IOSTANDARD LVCMOS25} [get_ports disp_rs]      ;# IO_L18P_T2_AD13P_35 (LCD_RS/DC)
set_property -dict {PACKAGE_PIN G20 IOSTANDARD LVCMOS25} [get_ports {mcu_io[0]}]  ;# IO_L18N_T2_AD13N_35 (Pico GPIO22)
set_property -dict {PACKAGE_PIN J18 IOSTANDARD LVCMOS25} [get_ports {mcu_io[1]}]  ;# IO_L14P_T2_AD4P_SRCC_35 (GPIO21)
set_property -dict {PACKAGE_PIN H18 IOSTANDARD LVCMOS25} [get_ports {mcu_io[2]}]  ;# IO_L14N_T2_AD4N_SRCC_35 (GPIO20/INT)
set_property -dict {PACKAGE_PIN H16 IOSTANDARD LVCMOS25} [get_ports {mcu_io[3]}]  ;# IO_L13P_T2_MRCC_35 (GPIO19/MISO)
set_property -dict {PACKAGE_PIN H17 IOSTANDARD LVCMOS25} [get_ports {mcu_io[4]}]  ;# IO_L13N_T2_MRCC_35 (GPIO18/SCK)
set_property -dict {PACKAGE_PIN L14 IOSTANDARD LVCMOS25} [get_ports {mcu_io[5]}]  ;# IO_L22P_T3_AD7P_35 (GPIO17/CS)  ** reclaim from pl_spi_clk_o **
set_property -dict {PACKAGE_PIN L15 IOSTANDARD LVCMOS25} [get_ports {mcu_io[6]}]  ;# IO_L22N_T3_AD7N_35 (GPIO16/MOSI)
# DECISION (2026-07-25): keep bank 35 on its existing LVCMOS25 (no change to the working
# AD9361 pins, no VCCO conflict). Outputs cap at the bank rail and feed the tracking level
# shifter safely whether the rail is 1.8 or 2.5 V. Only risk: MISO/INT (Zynq inputs) are
# marginal IF the rail is truly 1.8 V — if MISO is flaky on the bench, flip the whole bank
# to LVCMOS18 (needs the rail to be 1.8 V) as a one-line follow-up. Still must reclaim L14
# from pl_spi_clk_o.
```

## 8. Zynq bank voltage map (verified 2026-07-25 against maia-hdl references)

| Bank | Board (user) | `fishball_iio` ref | `plutoplus` ref | Notes |
|---|---|---|---|---|
| 0  | 3.3 V | config bank | config bank | config voltage (CFGBVS/CONFIG_VOLTAGE); 3.3 V standard ✓ |
| 13 | 3.3 V | unused | unused | display SPI data group (LVCMOS33); no reference constraint ✓ |
| 34 | 1.8 V | **2.5 V** (LVDS_25 + LVCMOS25) | **1.8 V** (LVCMOS18) | AD9361 data bus |
| 35 | 1.8 V | **2.5 V** (LVCMOS25) | **1.8 V** (LVCMOS18) | AD9361 ctrl + our 1.8 V display/MCU group |

**The board's mapping matches `plutoplus` (AD9361 = 1.8 V CMOS), NOT `fishball_iio` (AD9361 =
2.5 V LVDS).** This confirms bank 35 = 1.8 V → the §2 group is correctly LVCMOS18 and the whole
bank normalizes to LVCMOS18. ⚠️ It also implies the design that matches this board uses the
**CMOS/1.8 V** AD9361 interface; if the deployed bitstream is built from `fishball_iio` (LVDS/2.5 V)
the RF interface is mismatched — and LVDS↔CMOS is a different interface, not just an IOSTANDARD
swap. **Confirm which maia-hdl project builds the working image before the display rebuild.**

## 7. Linux `spi-gpio` master (device tree) — uses the EMIO `gpio0` lines above

```dts
spi_pico: spi-pico {
    compatible = "spi-gpio";
    #address-cells = <1>; #size-cells = <0>;
    sck-gpios  = <&gpio0 77 GPIO_ACTIVE_HIGH>;   /* GPIO18 SCK  */
    mosi-gpios = <&gpio0 79 GPIO_ACTIVE_HIGH>;   /* GPIO16 MOSI */
    miso-gpios = <&gpio0 76 GPIO_ACTIVE_HIGH>;   /* GPIO19 MISO */
    cs-gpios   = <&gpio0 78 GPIO_ACTIVE_LOW>;    /* GPIO17 CS   */
    num-chipselects = <1>;
    pico@0 { compatible = "spidev"; reg = <0>; spi-max-frequency = <2000000>; };
};
/* INT from Pico GPIO20 = gpio0 line 75 (input); panel D/C = gpio0 line 72 (fbtft dc-gpios). */
```
