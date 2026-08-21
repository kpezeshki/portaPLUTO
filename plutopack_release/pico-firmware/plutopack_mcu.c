/*
 * PlutoPack — Raspberry Pi Pico "MCU interface" firmware
 * ============================================================================
 * Turns the on-board Pico into the PlutoPack front-panel controller and a
 * peripheral of the PlutoSky/Zynq. It:
 *   1. Reads the local UI (buttons SW1/3-8, encoder SW2) + sensors (BQ27441
 *      fuel gauge, TSC2007 touch) on I2C1 — same as the demo firmware.
 *   2. Serves a Zynq<->Pico SPI link (Pico = SPI0 SLAVE) that lets the Zynq
 *      (Linux, via spi-gpio) poll a queue of UI events + a live status
 *      snapshot, and drive the display control lines (LCD_RST / LCD_CS) that
 *      are physically wired to the Pico.
 *   3. Mirrors everything to USB-CDC serial for debugging (optional).
 *
 * ---------------------------------------------------------------------------
 * Zynq<->Pico SPI bus  (Pico hardware SPI0, SLAVE, mode 0, 8-bit, MSB-first)
 *   GPIO16 = SPI0 RX  = MOSI (Zynq -> Pico)   [Zynq gpio0 line 79]
 *   GPIO17 = SPI0 CSn = CS   (Zynq -> Pico)   [Zynq gpio0 line 78]
 *   GPIO18 = SPI0 SCK = SCLK (Zynq -> Pico)   [Zynq gpio0 line 77]
 *   GPIO19 = SPI0 TX  = MISO (Pico -> Zynq)   [Zynq gpio0 line 76]
 *   GPIO20 = INT/attention (Pico -> Zynq, active-low: low = events queued)
 *                                             [Zynq gpio0 line 75]
 * These 7 lines run through the TXS0108 (U7) level shifter; GPIO16-19 are the
 * RP2040's native SPI0 pinout, so the board was laid out for exactly this.
 *
 * Display control lines the Pico owns (level-shifted to the panel FFC):
 *   GPIO0 = LCD_RST (panel RESET, active-low)   [FFC pin 10]
 *   GPIO1 = LCD_CS  (panel chip-select, active-low) [FFC pin 38]
 * The panel's SCLK/MOSI/MISO/D-C come from the Zynq (PS SPI1 + an EMIO GPIO);
 * only RESET and CS are on the Pico. Default after boot: panel released from
 * reset and selected (CS low) so the Zynq's fbtft driver can bring it up with
 * no handshake. The Zynq can re-pulse reset / change CS via SPI commands.
 *
 * Local I2C1 (unchanged from the demo): GPIO2=SDA, GPIO3=SCL;
 *   BQ27441-G1 @0x55, TSC2007 @0x48, TSC2007 /PENIRQ on GPIO28.
 * UI: encoder A/B = GPIO11/12; buttons SW1=4 SW5=5 SW4=6 SW3=7 SW6=8 SW7=9 SW8=10.
 * User GPIO (level-shifted to the header): GPIO13/14 = digital outputs, GPIO15 =
 * digital input (no pull). Read/driven from the Zynq via CMD_SET_GPIO + the
 * STAT_GPIO_* status bits; surfaced in the FRONTEND STAT app.
 * Battery divider on ADC0/GPIO26 (+BATT / 2). On-board LED = GPIO25.
 * ---------------------------------------------------------------------------
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/sync.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "hardware/irq.h"

/* SPI-slave backend: 1 = PIO slave (self-resyncs on CS every frame — robust to the
 * bit-banged Zynq master); 0 = legacy PL022 hardware-SPI slave (could slip a byte
 * boundary and stay desynced until a Pico power-cycle). */
#define USE_PIO_SPI 1
#if USE_PIO_SPI
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#define PICO_PIO pio0
#define PICO_SM  0
#endif

/* ------------------------------- local UI/I2C ----------------------------- */
#define PIN_SDA        2
#define PIN_SCL        3
#define PIN_PENIRQ     28
#define PIN_ENC_A      11
#define PIN_ENC_B      12
#define PIN_LED        PICO_DEFAULT_LED_PIN   /* GPIO25 */
#define PIN_BATT_ADC   26
#define BATT_ADC_INPUT 0

/* display control lines (Pico-owned) */
#define PIN_LCD_RST    0
#define PIN_LCD_CS     1

/* board user-GPIO (level-shifted to the header). 13 & 14 are digital outputs,
 * 15 is a digital input. Read/driven over the SPI link (see CMD_SET_GPIO and the
 * STAT_GPIO_* status bits). */
#define PIN_GPIO_O0    13   /* user output 0 */
#define PIN_GPIO_O1    14   /* user output 1 */
#define PIN_GPIO_IN    15   /* user input (no internal pull) */

/* Zynq<->Pico SPI (SPI0 slave) + INT */
#define PIN_SPI_MOSI   16   /* SPI0 RX  */
#define PIN_SPI_CSN    17   /* SPI0 CSn */
#define PIN_SPI_SCK    18   /* SPI0 SCK */
#define PIN_SPI_MISO   19   /* SPI0 TX  */
#define PIN_INT        20   /* attention to Zynq, active-low */
#define SPI_PORT       spi0

typedef struct { uint gpio; const char *name; uint8_t id; } button_t;
static const button_t BUTTONS[] = {
    { 4,  "SW1", 1 }, { 7, "SW3", 3 }, { 6, "SW4", 4 }, { 5, "SW5", 5 },
    { 8,  "SW6", 6 }, { 9, "SW7", 7 }, { 10, "SW8", 8 },
};
#define N_BUTTONS (sizeof(BUTTONS) / sizeof(BUTTONS[0]))

#define I2C_PORT       i2c1
#define I2C_HZ         100000   /* 100 kHz standard-mode: max setup/hold margin for a
                                   marginal TSC2007 I2C connection (was 400 kHz) */
#define BQ27441_ADDR   0x55
#define TSC2007_ADDR   0x48
#define BATT_DIV_RATIO 2.0f
#define ADC_VREF       3.3f
#define TSC_Z_MIN      10
#define LED_BLINK_MS   40

/* ============================ shared UI state ============================= */
/* Written by core0 main loop, snapshotted by the SPI ISR. Scalars are word
 * sized so reads/writes are atomic on Cortex-M0+. */
static volatile uint8_t  g_btn_bitmap = 0;      /* bit (id-1) set = pressed  */
static volatile int32_t  g_enc_pos    = 0;
static volatile uint8_t  g_touch_down = 0;
static volatile uint16_t g_touch_x = 0, g_touch_y = 0, g_touch_z = 0;
static volatile uint16_t g_batt_mV = 0;
static volatile uint8_t  g_soc = 0;
static volatile int16_t  g_current_mA = 0;
static volatile uint8_t  g_status = 0;          /* see STAT_* below */
#define STAT_GAUGE_OK  (1<<0)
#define STAT_TOUCH_OK  (1<<1)
#define STAT_TOUCH_DN  (1<<2)
#define STAT_CHARGING  (1<<3)
#define STAT_GPIO_IN   (1<<4)   /* GPIO15 input level (1=high)       */
#define STAT_GPIO_O0   (1<<5)   /* GPIO13 output level, echoed back  */
#define STAT_GPIO_O1   (1<<6)   /* GPIO14 output level, echoed back  */
#define STAT_PENIRQ    (1<<7)   /* raw PENIRQ line level (1=high/idle, 0=pen line asserted low) */

/* current display-control levels (as driven) */
static volatile uint8_t  g_lcd_rst = 1;   /* 1 = released (high) */
static volatile uint8_t  g_lcd_cs  = 0;   /* 0 = selected (low)  */

/* user-GPIO output levels (bit0=GPIO13, bit1=GPIO14), set via CMD_SET_GPIO */
static volatile uint8_t  g_gpio_out = 0;

/* ------------------------- event queue (SPSC ring) ------------------------ */
/* 1 producer (main loop), 1 consumer (SPI ISR). 2-byte events {type,value}. */
#define EVT_TYPE_BTN_DOWN  1
#define EVT_TYPE_BTN_UP    2
#define EVT_TYPE_ENC       3   /* value = int8 step (+/-)                  */
#define EVT_TYPE_TOUCH_DN  4
#define EVT_TYPE_TOUCH_UP  5
typedef struct { uint8_t type; uint8_t value; } evt_t;
#define EVT_RING 128
static volatile evt_t   evt_ring[EVT_RING];
static volatile uint16_t evt_head = 0;   /* consumer */
static volatile uint16_t evt_tail = 0;   /* producer */

static inline uint16_t evt_count(void) {
    return (uint16_t)(evt_tail - evt_head);
}
static void evt_push(uint8_t type, uint8_t value) {
    if (evt_count() >= EVT_RING - 1) { evt_head++; }   /* drop oldest */
    uint16_t i = evt_tail % EVT_RING;
    evt_ring[i].type = type; evt_ring[i].value = value;
    __dmb(); evt_tail++;
}

/* ============================= I2C sensors ================================ */
static bool i2c_read16(uint8_t addr, uint8_t reg, uint16_t *out) {
    if (i2c_write_timeout_us(I2C_PORT, addr, &reg, 1, true, 2000) != 1) return false;
    uint8_t b[2];
    if (i2c_read_timeout_us(I2C_PORT, addr, b, 2, false, 2000) != 2) return false;
    *out = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return true;
}
#define BQ_VOLTAGE 0x04
#define BQ_FLAGS   0x06
#define BQ_NAC     0x08   /* NominalAvailableCapacity */
#define BQ_FAC     0x0A   /* FullAvailableCapacity    */
#define BQ_REMCAP  0x0C
#define BQ_FCC     0x0E
#define BQ_AVG_CUR 0x10
#define BQ_TEMP    0x02   /* 0.1 K */
#define BQ_SOC     0x1C
#define BQ_FLAG_DSG (1<<0)
/* cached gauge registers (updated in the 200 ms loop / at boot) for the heartbeat
 * and the SPI "gauge detail" frame the FRONTEND STAT app requests */
static volatile uint16_t g_bq_fcc=0, g_bq_rm=0, g_bq_nac=0, g_bq_fac=0;
static volatile uint16_t g_bq_temp=0, g_bq_flags=0, g_bq_ctrl=0;
static volatile uint16_t g_bq_dcap=0, g_bq_qmax=0, g_bq_termv=0, g_bq_chem=0;

/* ---------------- BQ27441-G1 gauge configuration (Design Capacity) ---------
 * The gauge ships configured for a ~1340 mAh cell; our pack is a single 18650
 * of 3000 mAh. Reprogram the data-memory Design Capacity / Design Energy /
 * Taper Rate via the CONFIG_UPDATE flow (unseal -> SET_CFGUPDATE -> edit the
 * State block -> checksum -> soft reset). Only commits a flash write when the
 * stored capacity differs from target, so re-running each boot costs one block
 * read and no wear once programmed. Terminate Voltage and the chemistry profile
 * are left at their defaults (fine for a standard 4.2 V 18650). */
#define BATT_DESIGN_MAH   3000
#define BATT_DESIGN_MWH   ((BATT_DESIGN_MAH * 37) / 10)   /* cap x 3.7 V nominal */
#define BATT_TAPER_RATE   ((BATT_DESIGN_MAH * 176) / 1340) /* keep default taper current */
#define BATT_TERM_MV      3200                            /* Terminate Voltage (empty point) */
#define R_INT_MOHM        120     /* cell+FETs+wiring, for OCV-based SoC IR compensation */
#define FCC_SANE_LO       1500    /* trust the gauge's SoC only when FCC is in this range; */
#define FCC_SANE_HI       4000    /* the impedance-track FCC is garbage under heavy load   */
/* BQ27441 Control() subcommands */
#define BQ_CTRL           0x00
#define BQ_SUB_STATUS     0x0000  /* CONTROL_STATUS */
#define BQ_SUB_DEVTYPE    0x0001
#define BQ_SUB_FWVER      0x0002
#define BQ_SUB_CHEMID     0x0008
#define BQ_SUB_BAT_INSERT 0x000C
#define BQ_SUB_CFGUPDATE  0x0013
#define BQ_SUB_SOFT_RESET 0x0042
#define BQ_SUB_SEALED     0x0020
#define BQ_SUB_UNSEAL     0x8000
/* extended-data / block registers */
#define BQ_BLOCKDATACTRL  0x61
#define BQ_DATACLASS      0x3E
#define BQ_DATABLOCK      0x3F
#define BQ_BLOCKDATA      0x40   /* 32-byte window */
#define BQ_BLOCKCHECKSUM  0x60
#define BQ_STATE_SUBCLASS 0x52   /* 82 = State */
/* Flags() 0x06 bits */
#define BQ_FLAG_DSG_      0x0001
#define BQ_FLAG_SOCF      0x0002  /* SOC-final (near empty) threshold */
#define BQ_FLAG_SOC1      0x0004
#define BQ_FLAG_BATDET    0x0008  /* battery detected (0 => gauge inactive -> SOC 0) */
#define BQ_FLAG_CFGUPMODE 0x0010
#define BQ_FLAG_ITPOR     0x0020  /* POR/RESET occurred -> config must be re-sent */
#define BQ_FLAG_OCVTAKEN  0x0080
#define BQ_FLAG_FC        0x0200  /* full charged */

static uint8_t bq_read8(uint8_t reg){
    uint8_t v = 0;
    if (i2c_write_timeout_us(I2C_PORT, BQ27441_ADDR, &reg, 1, true, 3000) != 1) return 0;
    i2c_read_timeout_us(I2C_PORT, BQ27441_ADDR, &v, 1, false, 3000);
    return v;
}
static void bq_write8(uint8_t reg, uint8_t val){
    uint8_t b[2] = { reg, val };
    i2c_write_timeout_us(I2C_PORT, BQ27441_ADDR, b, 2, false, 3000);
}
static void bq_ctrl(uint16_t sub){          /* write a Control() subcommand */
    uint8_t b[3] = { BQ_CTRL, (uint8_t)(sub & 0xFF), (uint8_t)(sub >> 8) };
    i2c_write_timeout_us(I2C_PORT, BQ27441_ADDR, b, 3, false, 3000);
}
static bool bq_wait_flag(uint16_t mask, bool set){
    for (int i = 0; i < 100; i++) {
        uint16_t fl;
        if (i2c_read16(BQ27441_ADDR, BQ_FLAGS, &fl) && (!!(fl & mask) == set)) return true;
        sleep_ms(10);
    }
    return false;
}

/* Ensure the gauge's Design Capacity matches the 18650, WITHOUT disturbing a gauge
 * that's already programmed AND running. Reading data memory only needs UNSEAL (not
 * CONFIG_UPDATE) and does not perturb gauging. We enter CONFIG_UPDATE + SOFT_RESET
 * (which restart gauging and re-init the state) ONLY when needed:
 *   (a) the capacity isn't programmed yet (first boot after flashing), OR
 *   (b) the gauge's ITPOR flag is set — it POR'd (e.g. the cell dipped below the
 *       gauge's ~2.4 V floor during a deep discharge) and is running on invalid data,
 *       which it signals until the host re-sends config. In that state SoC is garbage
 *       (reads 0%), and it will NOT clear on its own — the host must do a CONFIG_UPDATE
 *       cycle. (An earlier version reset on EVERY boot, wiping learning each power-cycle;
 *       this version resets only when the gauge itself asks for it.)
 * Returns true if the gauge was reachable. */
static bool bq_configure(void){
    bq_ctrl(BQ_SUB_UNSEAL); bq_ctrl(BQ_SUB_UNSEAL);   /* two writes = unseal (reads allowed) */
    sleep_ms(5);

    /* read current Design Capacity + the POR flag without entering CONFIG_UPDATE */
    bq_write8(BQ_BLOCKDATACTRL, 0x00);
    bq_write8(BQ_DATACLASS, BQ_STATE_SUBCLASS);
    bq_write8(BQ_DATABLOCK, 0x00);
    sleep_ms(5);
    uint16_t cur_cap = ((uint16_t)bq_read8(BQ_BLOCKDATA + 10) << 8) | bq_read8(BQ_BLOCKDATA + 11);
    uint16_t cur_tv  = ((uint16_t)bq_read8(BQ_BLOCKDATA + 16) << 8) | bq_read8(BQ_BLOCKDATA + 17);
    uint16_t fl = 0; i2c_read16(BQ27441_ADDR, BQ_FLAGS, &fl);
    bool itpor = (fl & BQ_FLAG_ITPOR) != 0;
    bool tv_bad = (cur_tv < 2800 || cur_tv > 3600);   /* corrupted Terminate Voltage -> tiny FCC */
    if (cur_cap == BATT_DESIGN_MAH && !tv_bad && !itpor) { bq_ctrl(BQ_SUB_SEALED); return true; } /* healthy */

    /* needs (re)init: capacity wrong or POR flagged -> CONFIG_UPDATE. The flash write
     * (checksum) only happens if the capacity is actually wrong; if we're here only to
     * clear ITPOR, the SOFT_RESET below reinitializes gauging without a flash write. */
    bq_ctrl(BQ_SUB_CFGUPDATE);
    if (!bq_wait_flag(BQ_FLAG_CFGUPMODE, true)) { bq_ctrl(BQ_SUB_SEALED); return false; }
    if (cur_cap != BATT_DESIGN_MAH || tv_bad) {   /* capacity or Terminate Voltage wrong -> rewrite */
        bq_write8(BQ_BLOCKDATACTRL, 0x00);
        bq_write8(BQ_DATACLASS, BQ_STATE_SUBCLASS);
        bq_write8(BQ_DATABLOCK, 0x00);
        sleep_ms(5);
        uint8_t blk[32];
        for (int i = 0; i < 32; i++) blk[i] = bq_read8(BQ_BLOCKDATA + i);
        blk[10] = (uint8_t)(BATT_DESIGN_MAH >> 8); blk[11] = (uint8_t)(BATT_DESIGN_MAH & 0xFF);
        blk[12] = (uint8_t)(BATT_DESIGN_MWH >> 8); blk[13] = (uint8_t)(BATT_DESIGN_MWH & 0xFF);
        blk[16] = (uint8_t)(BATT_TERM_MV   >> 8); blk[17] = (uint8_t)(BATT_TERM_MV   & 0xFF);
        blk[21] = (uint8_t)(BATT_TAPER_RATE >> 8); blk[22] = (uint8_t)(BATT_TAPER_RATE & 0xFF);
        bq_write8(BQ_BLOCKDATA + 10, blk[10]); bq_write8(BQ_BLOCKDATA + 11, blk[11]);
        bq_write8(BQ_BLOCKDATA + 12, blk[12]); bq_write8(BQ_BLOCKDATA + 13, blk[13]);
        bq_write8(BQ_BLOCKDATA + 16, blk[16]); bq_write8(BQ_BLOCKDATA + 17, blk[17]);
        bq_write8(BQ_BLOCKDATA + 21, blk[21]); bq_write8(BQ_BLOCKDATA + 22, blk[22]);
        uint8_t sum = 0; for (int i = 0; i < 32; i++) sum += blk[i];
        bq_write8(BQ_BLOCKCHECKSUM, (uint8_t)(255 - sum));   /* commit block -> flash */
        sleep_ms(5);
    }
    bq_ctrl(BQ_SUB_SOFT_RESET);               /* exit CONFIG_UPDATE: clears ITPOR, restarts gauging */
    bq_wait_flag(BQ_FLAG_CFGUPMODE, false);
    bq_ctrl(BQ_SUB_SEALED);
    return true;
}

/* read a Control() subcommand result: write the subcommand to 0x00, then read 0x00 */
static uint16_t bq_ctrl_read(uint16_t sub){
    bq_ctrl(sub); sleep_ms(2);
    uint16_t v=0; i2c_read16(BQ27441_ADDR, BQ_CTRL, &v); return v;
}
/* Dump gauge diagnostics to USB serial. verbose=1 also unseals to read
 * DeviceType/FW/ChemID/DesignCap/Qmax (use once at boot). The decoded flags explain a
 * stuck SoC:
 *   ITPOR       gauge POR'd, running on invalid data until the host re-sends config
 *   !BATDET(!)  gauge doesn't think a cell is present -> reports 0%
 *   CFGUP       stuck in config-update (gauging halted)
 *   SOCF        gauge believes the pack is at the final (empty) threshold
 *   !INITCOMP(!) initialisation never completed
 *   QMAXUP/RESUP whether Qmax / Ra (impedance) have been learned; VOK = OCV valid for Qmax
 * and the capacities: FCC (full-charge) and RM (remaining) — SOC = RM/FCC, so a tiny FCC
 * makes SOC collapse; ChemID should be 0x0128 (default 4.2 V profile) and DesignCap 3000. */
static void bq_dump(int verbose){    /* callers guard on bq_ok */
    uint16_t fl=0, cs, v=0, t=0, rm=0, fcc=0, soc=0, nac=0, fac=0; int16_t cur=0;
    i2c_read16(BQ27441_ADDR, BQ_FLAGS, &fl);
    cs = bq_ctrl_read(BQ_SUB_STATUS);
    i2c_read16(BQ27441_ADDR, BQ_VOLTAGE, &v);
    i2c_read16(BQ27441_ADDR, 0x02, &t);                 /* Temperature, 0.1 K */
    i2c_read16(BQ27441_ADDR, BQ_AVG_CUR, (uint16_t*)&cur);
    i2c_read16(BQ27441_ADDR, BQ_REMCAP, &rm);
    i2c_read16(BQ27441_ADDR, BQ_FCC, &fcc);
    i2c_read16(BQ27441_ADDR, BQ_SOC, &soc);
    i2c_read16(BQ27441_ADDR, 0x08, &nac);               /* NominalAvailableCapacity */
    i2c_read16(BQ27441_ADDR, 0x0A, &fac);               /* FullAvailableCapacity   */
    int tc10 = (int)t - 2731;                            /* 0.1 K -> 0.1 C */
    printf("[BQ] Flags=0x%04x %s%s%s%s%s%s| Ctrl=0x%04x %s%s%s%s%s| "
           "V=%umV I=%dmA T=%d.%dC SOC=%u%% RM=%umAh FCC=%umAh NAC=%u FAC=%u\n",
        fl,
        (fl&BQ_FLAG_ITPOR)?"ITPOR ":"",
        (fl&BQ_FLAG_BATDET)?"BATDET ":"!BATDET(!) ",
        (fl&BQ_FLAG_CFGUPMODE)?"CFGUP ":"",
        (fl&BQ_FLAG_SOCF)?"SOCF ":"",
        (fl&BQ_FLAG_OCVTAKEN)?"OCV ":"noOCV ",
        (fl&BQ_FLAG_FC)?"FC ":((fl&BQ_FLAG_DSG_)?"DSG ":"CHG "),
        cs,
        (cs&0x8000)?"SEALED ":"unsealed ",
        (cs&0x0200)?"INITCOMP ":"!INITCOMP(!) ",
        (cs&0x0800)?"QMAXUP ":"",
        (cs&0x0400)?"RESUP ":"",
        (cs&0x0008)?"VOK ":"",
        v, cur, tc10/10, (tc10<0?-tc10:tc10)%10, soc, rm, fcc, nac, fac);
    if(verbose){
        bq_ctrl(BQ_SUB_UNSEAL); bq_ctrl(BQ_SUB_UNSEAL); sleep_ms(3);
        uint16_t devt=bq_ctrl_read(BQ_SUB_DEVTYPE), fwv=bq_ctrl_read(BQ_SUB_FWVER), chem=bq_ctrl_read(BQ_SUB_CHEMID);
        bq_write8(BQ_BLOCKDATACTRL,0x00); bq_write8(BQ_DATACLASS,BQ_STATE_SUBCLASS); bq_write8(BQ_DATABLOCK,0x00); sleep_ms(5);
        uint16_t qmax=((uint16_t)bq_read8(BQ_BLOCKDATA+ 0)<<8)|bq_read8(BQ_BLOCKDATA+ 1);
        uint16_t dcap=((uint16_t)bq_read8(BQ_BLOCKDATA+10)<<8)|bq_read8(BQ_BLOCKDATA+11);
        uint16_t den =((uint16_t)bq_read8(BQ_BLOCKDATA+12)<<8)|bq_read8(BQ_BLOCKDATA+13);
        uint16_t tv  =((uint16_t)bq_read8(BQ_BLOCKDATA+16)<<8)|bq_read8(BQ_BLOCKDATA+17);
        uint16_t tap =((uint16_t)bq_read8(BQ_BLOCKDATA+21)<<8)|bq_read8(BQ_BLOCKDATA+22);
        bq_ctrl(BQ_SUB_SEALED);
        g_bq_dcap=dcap; g_bq_qmax=qmax; g_bq_termv=tv; g_bq_chem=chem;  /* cache for gauge frame */
        printf("[BQ] DevType=0x%04x FW=0x%04x ChemID=0x%04x(dflt0128) | "
               "DesignCap=%umAh Energy=%umWh TermV=%umV Taper=%u Qmax=%u\n",
               devt, fwv, chem, dcap, den, tv, tap, qmax);
    }
}

/* Open-circuit-voltage -> state-of-charge for a single Li-ion cell (piecewise
 * linear, mV -> %). Used when the gauge's load-compensated FCC is unusable; the
 * caller passes OCV = terminal voltage + IR-drop so it's load-corrected. */
static int soc_from_ocv(int mv){
    static const int P[][2] = {
        {4200,100},{4100,90},{4000,80},{3930,70},{3870,60},{3820,50},
        {3790,40},{3770,30},{3740,20},{3700,15},{3650,10},{3550,5},{3300,0}
    };
    const int n = (int)(sizeof(P)/sizeof(P[0]));
    if (mv >= P[0][0]) return 100;
    if (mv <= P[n-1][0]) return 0;
    for (int i = 1; i < n; i++)
        if (mv >= P[i][0])
            return P[i][1] + (P[i-1][1]-P[i][1]) * (mv-P[i][0]) / (P[i-1][0]-P[i][0]);
    return 0;
}

/* TSC2007 */
#define TSC_FN_X 12
#define TSC_FN_Y 13
#define TSC_FN_Z1 14
#define TSC_FN_TEMP0 0
#define TSC_PWR_ADON 1
#define TSC_PWR_PD_IRQON 0
static bool tsc_meas(uint8_t func, uint8_t pwr, uint16_t *out) {
    uint8_t cmd = (uint8_t)((func & 0xF) << 4) | ((pwr & 3) << 2);
    if (i2c_write_timeout_us(I2C_PORT, TSC2007_ADDR, &cmd, 1, true, 2000) != 1) return false;
    uint8_t b[2];
    if (i2c_read_timeout_us(I2C_PORT, TSC2007_ADDR, b, 2, false, 2000) != 2) return false;
    *out = ((uint16_t)b[0] << 4) | (b[1] >> 4);
    return true;
}
static bool bq_ok = false, tsc_present = false;
/* TSC2007 presence is tracked CONTINUOUSLY, not latched at boot: a single flaky
 * power-on I2C read must not disable touch for the whole power cycle. A good read
 * marks it present + resets the miss counter; only TSC_FAIL_MAX misses in a row
 * mark it absent (hysteresis so one glitch doesn't flip the UI to "n/a"). */
#define TSC_FAIL_MAX  6
static uint8_t  tsc_fail = 0;
static uint32_t tsc_probe_ms = 0;        /* last idle presence re-probe (ms) */
static inline void tsc_seen(void){ tsc_present = true; tsc_fail = 0; g_status |= STAT_TOUCH_OK; }
static inline void tsc_missed(void){
    if (tsc_fail < 255) tsc_fail++;
    if (tsc_fail >= TSC_FAIL_MAX) { tsc_present = false; g_status &= ~STAT_TOUCH_OK; }
}

/* =============================== encoder ================================= */
static volatile uint8_t enc_prev = 0;
static const int8_t ENC_LUT[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
static void enc_irq(uint gpio, uint32_t events);   /* fwd */

/* ============================ SPI slave link ============================= */
#define FRAME_LEN 32
#define M_CMD_MAGIC   0xA5
#define S_STAT_MAGIC  0x5A
#define S_GAUGE_MAGIC 0x5B       /* gauge-detail response frame (see build_gauge_frame) */
/* commands (master -> pico), rx_buf[1] */
#define CMD_NOP           0x00
#define CMD_SET_DISP_CTRL 0x01   /* arg0 bit0=RST level, bit1=CS level        */
#define CMD_PULSE_RESET   0x02   /* arg0 = low-pulse ms (then release high)   */
#define CMD_SET_INT_MODE  0x03   /* arg0: 0=INT off, 1=assert when events>0   */
#define CMD_SET_GPIO      0x04   /* arg0 bit0=GPIO13 level, bit1=GPIO14 level  */
#define CMD_GET_GAUGE     0x05   /* make the NEXT frame a gauge-detail frame  */

static uint8_t rx_buf[FRAME_LEN];
static uint8_t tx_buf[FRAME_LEN];
static int     dma_rx, dma_tx;
static volatile uint8_t  g_seq = 0;
static volatile uint8_t  g_next_gauge = 0;   /* one-shot: next armed frame is a gauge frame */
static volatile uint32_t g_spi_frames = 0;   /* total status frames served (SPI activity) */
static volatile uint32_t g_spi_short  = 0;   /* frames whose RX DMA came up short (slip/desync) */
static volatile uint8_t g_int_mode = 1;
static volatile uint32_t g_reset_pulse_until = 0;   /* ms; !=0 => reset held low */

static uint8_t crc8(const uint8_t *p, int n) {
    uint8_t c = 0;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}

/* Build the outgoing status frame from live state + drain up to 5 events. */
static void build_status_frame(void) {
    memset(tx_buf, 0, FRAME_LEN);
    g_spi_frames++;
    tx_buf[0]  = S_STAT_MAGIC;
    tx_buf[1]  = g_seq++;
    tx_buf[2]  = g_status;
    tx_buf[3]  = g_btn_bitmap;
    int32_t pos = g_enc_pos;
    tx_buf[4]  = (uint8_t)(pos);       tx_buf[5] = (uint8_t)(pos >> 8);
    tx_buf[6]  = (uint8_t)(pos >> 16); tx_buf[7] = (uint8_t)(pos >> 24);
    tx_buf[8]  = g_touch_down;
    tx_buf[9]  = (uint8_t)g_touch_x;   tx_buf[10] = (uint8_t)(g_touch_x >> 8);
    tx_buf[11] = (uint8_t)g_touch_y;   tx_buf[12] = (uint8_t)(g_touch_y >> 8);
    tx_buf[13] = (uint8_t)g_touch_z;   tx_buf[14] = (uint8_t)(g_touch_z >> 8);
    tx_buf[15] = (uint8_t)g_batt_mV;   tx_buf[16] = (uint8_t)(g_batt_mV >> 8);
    tx_buf[17] = g_soc;
    tx_buf[18] = (uint8_t)g_current_mA; tx_buf[19] = (uint8_t)(g_current_mA >> 8);
    tx_buf[20] = (uint8_t)(g_lcd_rst | (g_lcd_cs << 1));
    /* event window: [21]=count, then up to 5 events of 2B at [22..31] before CRC */
    uint16_t total = evt_count();
    uint8_t n = total > 5 ? 5 : (uint8_t)total;
    tx_buf[21] = (uint8_t)(total > 255 ? 255 : total);
    for (uint8_t i = 0; i < n; i++) {
        uint16_t idx = (uint16_t)(evt_head + i) % EVT_RING;
        tx_buf[22 + i*2] = evt_ring[idx].type;
        tx_buf[23 + i*2] = evt_ring[idx].value;
    }
    tx_buf[FRAME_LEN - 1] = crc8(tx_buf, FRAME_LEN - 1);
}

/* Build the gauge-detail frame (magic 0x5B) from cached BQ27441 registers. Packed
 * little-endian 16-bit values; decoded by pp_get_gauge() on the Zynq. No I2C here
 * (ISR context) — everything is read from globals refreshed in the main loop. */
static void build_gauge_frame(void) {
    memset(tx_buf, 0, FRAME_LEN);
    tx_buf[0] = S_GAUGE_MAGIC;
    tx_buf[1] = g_seq++;
    #define PUT16(o,v) do { uint16_t _v=(uint16_t)(v); tx_buf[o]=(uint8_t)_v; tx_buf[(o)+1]=(uint8_t)(_v>>8); } while(0)
    PUT16(2,  g_batt_mV);
    PUT16(4,  (uint16_t)g_current_mA);
    PUT16(6,  g_bq_temp);      /* 0.1 K */
    PUT16(8,  g_soc);          /* %     */
    PUT16(10, g_bq_rm);
    PUT16(12, g_bq_fcc);
    PUT16(14, g_bq_nac);
    PUT16(16, g_bq_fac);
    PUT16(18, g_bq_dcap);
    PUT16(20, g_bq_qmax);
    PUT16(22, g_bq_termv);
    PUT16(24, g_bq_flags);
    PUT16(26, g_bq_ctrl);
    PUT16(28, g_bq_chem);
    #undef PUT16
    tx_buf[FRAME_LEN - 1] = crc8(tx_buf, FRAME_LEN - 1);
}

/* Apply a received command frame. */
static void handle_command(void) {
    if (rx_buf[0] != M_CMD_MAGIC) return;
    if (crc8(rx_buf, FRAME_LEN - 1) != rx_buf[FRAME_LEN - 1]) return;
    uint8_t cmd = rx_buf[1], arg0 = rx_buf[2];
    uint8_t ack = rx_buf[4];               /* events master consumed */
    /* pop acked events */
    uint16_t have = evt_count();
    if (ack > have) ack = (uint8_t)have;
    evt_head += ack;

    switch (cmd) {
    case CMD_SET_DISP_CTRL:
        g_lcd_rst = (arg0 & 1) ? 1 : 0;
        g_lcd_cs  = (arg0 & 2) ? 1 : 0;
        gpio_put(PIN_LCD_RST, g_lcd_rst);
        gpio_put(PIN_LCD_CS,  g_lcd_cs);
        break;
    case CMD_PULSE_RESET:
        g_lcd_rst = 0; gpio_put(PIN_LCD_RST, 0);
        g_reset_pulse_until = to_ms_since_boot(get_absolute_time()) + (arg0 ? arg0 : 20);
        break;
    case CMD_SET_INT_MODE:
        g_int_mode = arg0 ? 1 : 0;
        break;
    case CMD_SET_GPIO:
        g_gpio_out = arg0 & 0x03;
        gpio_put(PIN_GPIO_O0, g_gpio_out & 1);
        gpio_put(PIN_GPIO_O1, (g_gpio_out >> 1) & 1);
        break;
    case CMD_GET_GAUGE:
        g_next_gauge = 1;                 /* spi_arm_dma builds a gauge frame next */
        break;
    default: break;
    }
}

#if USE_PIO_SPI
/* ===================== PIO SPI slave (self-resync) ==========================
 * The Zynq master is a bit-banged spi-gpio bus; the PL022 slave (below, #else)
 * could slip a byte boundary and stay desynced until a Pico power-cycle. This PIO
 * slave frames the START of every 32-byte frame on CS in hardware (the SM waits
 * for CS low), and the CS-rise ISR fully restarts the SM (clearing the shift
 * state) + re-arms DMA — so a slip can never persist beyond one frame.
 *
 * Mode 1 (CPOL=0, CPHA=1), MSB-first: drive MISO on SCK rising, sample MOSI on
 * SCK falling. Program (5 insns):
 *   +0: wait 0 gpio CS      ; frame start
 *   +1: wait 1 gpio SCK     ; rising  <- wrap_target
 *   +2: out pins, 1         ; drive MISO
 *   +3: wait 0 gpio SCK     ; falling
 *   +4: in  pins, 1         ; sample MOSI  <- wrap (back to +1)
 */
static uint16_t g_pio_insns[5];
static const struct pio_program g_pio_prog = { .instructions = g_pio_insns, .length = 5, .origin = -1 };
static uint g_pio_off = 0;

static inline void pio_rearm(void) {      /* build next frame + reset DMA for a fresh aligned frame */
    if (g_next_gauge) { g_next_gauge = 0; build_gauge_frame(); }
    else              build_status_frame();
    dma_channel_set_read_addr (dma_tx, tx_buf, false);
    dma_channel_set_trans_count(dma_tx, FRAME_LEN, false);
    dma_channel_set_write_addr(dma_rx, rx_buf, false);
    dma_channel_set_trans_count(dma_rx, FRAME_LEN, false);
    dma_channel_start(dma_rx);
    dma_channel_start(dma_tx);
}

/* CS rising edge = end of transaction: process cmd, fully re-sync the SM, re-arm. */
static void cs_rise_isr(uint gpio, uint32_t events) {
    if (gpio == PIN_SPI_CSN && (events & GPIO_IRQ_EDGE_RISE)) {
        if (dma_channel_hw_addr(dma_rx)->transfer_count != 0) g_spi_short++;  /* short/slipped frame */
        pio_sm_set_enabled(PICO_PIO, PICO_SM, false);
        dma_channel_abort(dma_tx);
        dma_channel_abort(dma_rx);
        handle_command();
        pio_sm_clear_fifos(PICO_PIO, PICO_SM);
        pio_sm_restart(PICO_PIO, PICO_SM);                          /* clear ISR/OSR/shift counters */
        pio_sm_exec(PICO_PIO, PICO_SM, pio_encode_jmp(g_pio_off));  /* PC -> "wait CS low"          */
        pio_rearm();
        pio_sm_set_enabled(PICO_PIO, PICO_SM, true);
        if (g_int_mode && evt_count() > 0) gpio_put(PIN_INT, 0);
        else                               gpio_put(PIN_INT, 1);
    } else if (gpio == PIN_ENC_A || gpio == PIN_ENC_B) {
        enc_irq(gpio, events);
    }
}

static void spi_slave_init(void) {
    g_pio_insns[0] = pio_encode_wait_gpio(false, PIN_SPI_CSN);   /* wait CS low  */
    g_pio_insns[1] = pio_encode_wait_gpio(true,  PIN_SPI_SCK);   /* SCK rising   */
    g_pio_insns[2] = pio_encode_out(pio_pins, 1);                /* drive MISO   */
    g_pio_insns[3] = pio_encode_wait_gpio(false, PIN_SPI_SCK);   /* SCK falling  */
    g_pio_insns[4] = pio_encode_in(pio_pins, 1);                 /* sample MOSI  */

    pio_gpio_init(PICO_PIO, PIN_SPI_MISO);            /* MISO: PIO output */
    pio_gpio_init(PICO_PIO, PIN_SPI_MOSI);            /* MOSI: PIO input  */
    gpio_init(PIN_SPI_SCK); gpio_set_dir(PIN_SPI_SCK, GPIO_IN);   /* read via 'wait gpio' */
    gpio_init(PIN_SPI_CSN); gpio_set_dir(PIN_SPI_CSN, GPIO_IN);
    pio_sm_set_consecutive_pindirs(PICO_PIO, PICO_SM, PIN_SPI_MISO, 1, true);  /* MISO = output */

    g_pio_off = pio_add_program(PICO_PIO, &g_pio_prog);
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, g_pio_off + 1, g_pio_off + 4);
    sm_config_set_in_pins(&c, PIN_SPI_MOSI);
    sm_config_set_out_pins(&c, PIN_SPI_MISO, 1);
    sm_config_set_in_shift (&c, false, true, 8);      /* shift left, autopush, 8 (MSB-first) */
    sm_config_set_out_shift(&c, false, true, 8);      /* shift left, autopull, 8 (MSB-first) */
    sm_config_set_clkdiv(&c, 1.0f);                   /* full speed: react to SCK edges promptly */
    pio_sm_init(PICO_PIO, PICO_SM, g_pio_off, &c);    /* PC = +0 (wait CS low) */

    dma_rx = dma_claim_unused_channel(true);
    dma_tx = dma_claim_unused_channel(true);
    dma_channel_config dc;
    /* RX: PIO RX FIFO (low byte) -> rx_buf */
    dc = dma_channel_get_default_config(dma_rx);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_8);
    channel_config_set_dreq(&dc, pio_get_dreq(PICO_PIO, PICO_SM, false));
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    dma_channel_configure(dma_rx, &dc, rx_buf, &PICO_PIO->rxf[PICO_SM], FRAME_LEN, false);
    /* TX: tx_buf -> PIO TX FIFO HIGH byte (+3), left-justifying so shift-left OUT
     * emits the byte MSB-first. */
    dc = dma_channel_get_default_config(dma_tx);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_8);
    channel_config_set_dreq(&dc, pio_get_dreq(PICO_PIO, PICO_SM, true));
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    dma_channel_configure(dma_tx, &dc, ((uint8_t*)&PICO_PIO->txf[PICO_SM]) + 3, tx_buf, FRAME_LEN, false);

    gpio_set_irq_enabled_with_callback(PIN_SPI_CSN, GPIO_IRQ_EDGE_RISE, true, &cs_rise_isr);
    pio_rearm();                                      /* prime the first frame */
    pio_sm_set_enabled(PICO_PIO, PICO_SM, true);
}
#else  /* legacy PL022 hardware-SPI slave */
static void spi_arm_dma(void) {
    /* Flush the TX/RX FIFOs by cycling SSP enable, rebuild the frame, then arm.
     *
     * ORDER MATTERS: enable the SSP *before* triggering the DMA. The old code
     * called dma_channel_start() while SSE=0 (SSP disabled) and only enabled
     * the SSP afterwards. Triggering the TX DMA into a disabled SSP strands a
     * single byte in the shift register and stalls the FIFO fill, so the master
     * reads byte0 (0x5A) correctly and then all zeros. With the SSP enabled
     * first, the FIFO is empty + the TX DREQ is live, so the DMA paces the whole
     * 32-byte frame into the FIFO before the master clocks. */
    hw_clear_bits(&spi_get_hw(SPI_PORT)->cr1, SPI_SSPCR1_SSE_BITS);  /* flush FIFOs */
    if (g_next_gauge) { g_next_gauge = 0; build_gauge_frame(); }     /* one-shot gauge frame */
    else              build_status_frame();
    dma_channel_set_read_addr(dma_tx, tx_buf, false);
    dma_channel_set_write_addr(dma_rx, rx_buf, false);
    dma_channel_set_trans_count(dma_tx, FRAME_LEN, false);
    dma_channel_set_trans_count(dma_rx, FRAME_LEN, false);
    hw_set_bits(&spi_get_hw(SPI_PORT)->cr1, SPI_SSPCR1_SSE_BITS);    /* SSP on: FIFO empty, DREQ live */
    dma_channel_start(dma_rx);
    dma_channel_start(dma_tx);                                       /* now fill via DREQ pacing */
}

/* CS rising edge = end of transaction: process cmd, re-arm for next frame. */
static void cs_rise_isr(uint gpio, uint32_t events) {
    if (gpio == PIN_SPI_CSN && (events & GPIO_IRQ_EDGE_RISE)) {
        dma_channel_abort(dma_tx);
        dma_channel_abort(dma_rx);
        handle_command();
        spi_arm_dma();
        /* update INT line for the *next* poll */
        if (g_int_mode && evt_count() > 0) gpio_put(PIN_INT, 0);
        else                               gpio_put(PIN_INT, 1);
    } else if (gpio == PIN_ENC_A || gpio == PIN_ENC_B) {
        enc_irq(gpio, events);
    }
}

static void spi_slave_init(void) {
    spi_init(SPI_PORT, 1000000);            /* baud ignored in slave mode */
    spi_set_slave(SPI_PORT, true);
    /* CPHA=1 (SPI mode 1): the PL022 in slave mode with CPHA=0 requires the CS
     * to pulse high between every byte; held low for a block transfer it shifts
     * exactly one byte then stalls. CPHA=1 permits continuous multi-byte
     * transfers with CS held low, so the master's 32-byte block frame works.
     * The Zynq master MUST also use SPI mode 1. */
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_CSN,  GPIO_FUNC_SPI);

    dma_rx = dma_claim_unused_channel(true);
    dma_tx = dma_claim_unused_channel(true);
    dma_channel_config c;
    /* RX: SPI DR -> rx_buf */
    c = dma_channel_get_default_config(dma_rx);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_dreq(&c, spi_get_dreq(SPI_PORT, false));
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    dma_channel_configure(dma_rx, &c, rx_buf, &spi_get_hw(SPI_PORT)->dr, FRAME_LEN, false);
    /* TX: tx_buf -> SPI DR */
    c = dma_channel_get_default_config(dma_tx);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_dreq(&c, spi_get_dreq(SPI_PORT, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(dma_tx, &c, &spi_get_hw(SPI_PORT)->dr, tx_buf, FRAME_LEN, false);

    /* CS rising-edge IRQ (works even though the pin is muxed to SPI0 CSn) */
    gpio_set_irq_enabled_with_callback(PIN_SPI_CSN, GPIO_IRQ_EDGE_RISE, true, &cs_rise_isr);
    spi_arm_dma();
}
#endif  /* USE_PIO_SPI */

static void enc_irq(uint gpio, uint32_t events) {
    (void)gpio; (void)events;
    uint8_t a = gpio_get(PIN_ENC_A) ? 1 : 0;
    uint8_t b = gpio_get(PIN_ENC_B) ? 1 : 0;
    uint8_t curr = (uint8_t)((a << 1) | b);
    int8_t d = ENC_LUT[(enc_prev << 2) | curr];
    enc_prev = curr;
    if (d) { g_enc_pos += d; evt_push(EVT_TYPE_ENC, (uint8_t)d); }
}

/* ================================= main ================================== */
static bool     btn_state[N_BUTTONS] = {false};
static uint8_t  btn_cnt[N_BUTTONS]   = {0};
#define DEBOUNCE_N 3

int main(void) {
    stdio_init_all();

    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT); gpio_put(PIN_LED, 0);

    /* display control lines: release reset (after a pulse) and select panel */
    gpio_init(PIN_LCD_RST); gpio_set_dir(PIN_LCD_RST, GPIO_OUT); gpio_put(PIN_LCD_RST, 0);
    gpio_init(PIN_LCD_CS);  gpio_set_dir(PIN_LCD_CS,  GPIO_OUT); gpio_put(PIN_LCD_CS, 0);

    gpio_init(PIN_INT); gpio_set_dir(PIN_INT, GPIO_OUT); gpio_put(PIN_INT, 1); /* idle high */

    /* user GPIO: 13/14 outputs (start low), 15 digital input (no internal pull) */
    gpio_init(PIN_GPIO_O0); gpio_set_dir(PIN_GPIO_O0, GPIO_OUT); gpio_put(PIN_GPIO_O0, 0);
    gpio_init(PIN_GPIO_O1); gpio_set_dir(PIN_GPIO_O1, GPIO_OUT); gpio_put(PIN_GPIO_O1, 0);
    gpio_init(PIN_GPIO_IN); gpio_set_dir(PIN_GPIO_IN, GPIO_IN); gpio_disable_pulls(PIN_GPIO_IN);

    for (uint i = 0; i < N_BUTTONS; i++) {
        gpio_init(BUTTONS[i].gpio); gpio_set_dir(BUTTONS[i].gpio, GPIO_IN);
        gpio_pull_down(BUTTONS[i].gpio);
    }
    gpio_init(PIN_ENC_A); gpio_set_dir(PIN_ENC_A, GPIO_IN); gpio_pull_up(PIN_ENC_A);
    gpio_init(PIN_ENC_B); gpio_set_dir(PIN_ENC_B, GPIO_IN); gpio_pull_up(PIN_ENC_B);
    enc_prev = (uint8_t)((gpio_get(PIN_ENC_A) << 1) | gpio_get(PIN_ENC_B));

    gpio_init(PIN_PENIRQ); gpio_set_dir(PIN_PENIRQ, GPIO_IN); gpio_pull_up(PIN_PENIRQ);

    adc_init(); adc_gpio_init(PIN_BATT_ADC);

    i2c_init(I2C_PORT, I2C_HZ);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA); gpio_pull_up(PIN_SCL);

    /* probe sensors */
    { uint16_t v; bq_ok = i2c_read16(BQ27441_ADDR, BQ_VOLTAGE, &v);
      uint16_t d; tsc_present = tsc_meas(TSC_FN_TEMP0, TSC_PWR_PD_IRQON, &d); }
    if (bq_ok) g_status |= STAT_GAUGE_OK;
    if (tsc_present) g_status |= STAT_TOUCH_OK;

    /* one-time gauge reconfiguration for the 3000 mAh 18650 (skips the flash
     * write once the stored Design Capacity already matches) */
    if (bq_ok) { bq_configure(); bq_dump(1); }   /* verbose gauge diagnostics at boot */

    /* release the panel from reset (20 ms low pulse) then select it */
    sleep_ms(20);
    gpio_put(PIN_LCD_RST, 1); g_lcd_rst = 1;
    gpio_put(PIN_LCD_CS, 0);  g_lcd_cs = 0;

    /* encoder + CS edge share the one GPIO IRQ callback */
    gpio_set_irq_enabled_with_callback(PIN_ENC_A, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, &cs_rise_isr);
    gpio_set_irq_enabled(PIN_ENC_B, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);

    spi_slave_init();

    printf("\n=== PlutoPack Pico MCU firmware ===\n");
    printf("SPI0 slave on GPIO16-19 (MOSI/CS/SCK/MISO), INT=GPIO20; frame=%dB\n", FRAME_LEN);
    printf("BQ27441 %s | TSC2007 %s | panel RST released, CS asserted\n",
           bq_ok ? "OK" : "--", tsc_present ? "OK" : "--");

    absolute_time_t led_off = get_absolute_time();
    bool led_on = false;
    uint32_t report_ms = 0;
    uint32_t hb_ms = 0;              /* heartbeat timer */
    uint32_t hb_count = 0;           /* heartbeat sequence */
    uint32_t last_frames = 0;        /* g_spi_frames at previous heartbeat */
    uint32_t bqdump_ms = 0;          /* periodic gauge diagnostic */

    while (true) {
        uint32_t now = to_ms_since_boot(get_absolute_time());

        /* finish a commanded reset pulse */
        if (g_reset_pulse_until && now >= g_reset_pulse_until) {
            gpio_put(PIN_LCD_RST, 1); g_lcd_rst = 1; g_reset_pulse_until = 0;
        }

        /* buttons (active-high) with debounce; emit events + blink LED */
        bool press_edge = false;
        uint8_t bitmap = g_btn_bitmap;
        for (uint i = 0; i < N_BUTTONS; i++) {
            bool raw = gpio_get(BUTTONS[i].gpio);
            if (raw != btn_state[i]) {
                if (++btn_cnt[i] >= DEBOUNCE_N) {
                    btn_state[i] = raw; btn_cnt[i] = 0;
                    if (raw) { bitmap |=  (1 << (BUTTONS[i].id - 1)); press_edge = true;
                               evt_push(EVT_TYPE_BTN_DOWN, BUTTONS[i].id); }
                    else     { bitmap &= ~(1 << (BUTTONS[i].id - 1));
                               evt_push(EVT_TYPE_BTN_UP, BUTTONS[i].id); }
                }
            } else btn_cnt[i] = 0;
        }
        g_btn_bitmap = bitmap;

        /* touch — report the raw PENIRQ line every loop (even if the TSC2007 wasn't
         * found at boot) so the panel can distinguish pen-detect from continuity. */
        if (gpio_get(PIN_PENIRQ)) g_status |= STAT_PENIRQ; else g_status &= ~STAT_PENIRQ;
        if (gpio_get(PIN_PENIRQ) == 0) {
            /* Pen line asserted: read the TSC2007 REGARDLESS of the presence latch,
             * so a failed boot probe never blocks touch — a good read here IS the
             * proof of presence and registers the touch. */
            uint16_t x=0, y=0, z1=0, dummy;
            bool ok = tsc_meas(TSC_FN_Z1, TSC_PWR_ADON, &z1)
                   && tsc_meas(TSC_FN_X,  TSC_PWR_ADON, &x)
                   && tsc_meas(TSC_FN_Y,  TSC_PWR_ADON, &y);
            tsc_meas(TSC_FN_TEMP0, TSC_PWR_PD_IRQON, &dummy);   /* restore PENIRQ mode */
            if (ok) {
                tsc_seen();
                g_touch_z = z1;   /* raw pressure ALWAYS reported while the pen line is low
                                     (a low z with PENIRQ low = touch panel continuity fault) */
                if (z1 > TSC_Z_MIN) {
                    if (!g_touch_down) evt_push(EVT_TYPE_TOUCH_DN, 0);
                    /* The touch panel is mounted rotated 90 deg vs the display, so
                     * raw axes don't line up. Rotate here so the reported (x,y) is
                     * display-aligned. (The Zynq touch-cal app refines scale/offset.) */
                    g_touch_x = (uint16_t)(4095 - y);   /* screen X <- flipped raw Y */
                    g_touch_y = x;                       /* screen Y <- raw X          */
                    g_touch_down = 1; g_status |= STAT_TOUCH_DN;
                } else if (g_touch_down) {
                    g_touch_down = 0; g_status &= ~STAT_TOUCH_DN;
                    evt_push(EVT_TYPE_TOUCH_UP, 0);
                }
            } else {
                tsc_missed();   /* transient read failure -> hysteresis, not an instant disable */
            }
        } else {
            if (g_touch_down) {
                uint16_t dummy; tsc_meas(TSC_FN_TEMP0, TSC_PWR_PD_IRQON, &dummy);
                g_touch_down = 0; g_status &= ~STAT_TOUCH_DN;
                evt_push(EVT_TYPE_TOUCH_UP, 0);
            }
            /* Idle re-probe ~1/s: recovers touch on its own after a flaky boot (or a
             * transient I2C glitch) instead of staying dead until the next power cycle. */
            if (now - tsc_probe_ms >= 1000) {
                tsc_probe_ms = now;
                uint16_t d;
                if (tsc_meas(TSC_FN_TEMP0, TSC_PWR_PD_IRQON, &d)) tsc_seen(); else tsc_missed();
            }
        }

        /* user GPIO: sample input 15, echo the two output levels into status */
        if (gpio_get(PIN_GPIO_IN)) g_status |=  STAT_GPIO_IN; else g_status &= ~STAT_GPIO_IN;
        if (g_gpio_out & 1)        g_status |=  STAT_GPIO_O0; else g_status &= ~STAT_GPIO_O0;
        if (g_gpio_out & 2)        g_status |=  STAT_GPIO_O1; else g_status &= ~STAT_GPIO_O1;

        /* LED on any local UI activity */
        if (press_edge) { led_off = make_timeout_time_ms(LED_BLINK_MS);
                          if (!led_on) { gpio_put(PIN_LED,1); led_on=true; } }
        if (led_on && absolute_time_diff_us(get_absolute_time(), led_off) <= 0) {
            gpio_put(PIN_LED,0); led_on=false;
        }

        /* refresh gauge/battery ~5 Hz */
        if (now - report_ms >= 200) {
            report_ms = now;
            adc_select_input(BATT_ADC_INPUT);
            uint16_t raw = adc_read();
            g_batt_mV = (uint16_t)(raw * (ADC_VREF/4095.0f) * BATT_DIV_RATIO * 1000.0f);
            if (bq_ok) {
                uint16_t v, soc=0, fl; int16_t cur=0;
                if (i2c_read16(BQ27441_ADDR, BQ_VOLTAGE, &v)) g_batt_mV = v;
                i2c_read16(BQ27441_ADDR, BQ_SOC, &soc);
                if (i2c_read16(BQ27441_ADDR, BQ_AVG_CUR, (uint16_t*)&cur)) g_current_mA = cur;
                if (i2c_read16(BQ27441_ADDR, BQ_FLAGS, &fl)) {
                    g_bq_flags = fl;
                    if (fl & BQ_FLAG_DSG) g_status &= ~STAT_CHARGING;
                    else                  g_status |= STAT_CHARGING;
                }
                uint16_t fcc, rm, t, nac, fac;
                if (i2c_read16(BQ27441_ADDR, BQ_FCC, &fcc)) g_bq_fcc = fcc;
                if (i2c_read16(BQ27441_ADDR, BQ_REMCAP, &rm)) g_bq_rm = rm;
                if (i2c_read16(BQ27441_ADDR, BQ_TEMP, &t))   g_bq_temp = t;
                if (i2c_read16(BQ27441_ADDR, BQ_NAC, &nac))  g_bq_nac = nac;
                if (i2c_read16(BQ27441_ADDR, BQ_FAC, &fac))  g_bq_fac = fac;
                g_bq_ctrl = bq_ctrl_read(BQ_SUB_STATUS);     /* CONTROL_STATUS for the gauge frame */
                /* SoC: the impedance-track FCC is unreliable under this heavy/variable
                 * load (swings 1..112 mAh), so trust the gauge SoC only when FCC is in a
                 * sane range; otherwise derive SoC from the load-compensated OCV. */
                if (g_bq_fcc >= FCC_SANE_LO && g_bq_fcc <= FCC_SANE_HI) {
                    g_soc = (uint8_t)soc;
                } else {
                    int ocv = (int)g_batt_mV - ((int)g_current_mA * R_INT_MOHM)/1000; /* I<0 -> OCV higher */
                    g_soc = (uint8_t)soc_from_ocv(ocv);
                }
            }
        }

        /* periodic gauge diagnostics over USB serial (~every 5 s; verbose every ~30 s
         * so DesignCap/TermV/Qmax appear in the running stream, not just at boot) */
        if (bq_ok && now - bqdump_ms >= 5000) { static uint32_t dn=0; bqdump_ms = now; bq_dump((dn++ % 6)==0); }

        /* keep INT fresh (also refreshed in the CS ISR) */
        if (g_int_mode && evt_count() > 0) gpio_put(PIN_INT, 0);
        else                               gpio_put(PIN_INT, 1);

        /* --- 1 Hz USB-serial heartbeat: alive/dead + link + sensor status --- */
        if (now - hb_ms >= 1000) {
            hb_ms = now;
            uint32_t frames = g_spi_frames;
            uint32_t fps = frames - last_frames;   /* status frames served in the last second */
            last_frames = frames;
            printf("[HB %lu] up=%lus  ALIVE  | SPI:%s (%lu fps, %lu total, %lu short, seq=%u)  "
                   "| BQ27441:%s TSC2007:%s  | Vbat=%umV SoC=%u%% %c%dmA  "
                   "| enc=%ld btn=0x%02x touch=%s z=%u pirq=%d  | RST=%d CS=%d  "
                   "| GPIO o13=%d o14=%d in15=%d  | FCC=%umAh RM=%umAh\n",
                   (unsigned long)hb_count++, (unsigned long)(now/1000),
                   fps ? "UP" : "idle", (unsigned long)fps, (unsigned long)frames, (unsigned long)g_spi_short, g_seq,
                   bq_ok ? "ok" : "--", tsc_present ? "ok" : "--",
                   g_batt_mV, g_soc, g_current_mA < 0 ? '-' : '+',
                   g_current_mA < 0 ? -g_current_mA : g_current_mA,
                   (long)g_enc_pos, g_btn_bitmap, g_touch_down ? "DOWN" : "up",
                   g_touch_z, !!(g_status & STAT_PENIRQ),
                   g_lcd_rst, g_lcd_cs,
                   (g_gpio_out & 1), (g_gpio_out >> 1) & 1, !!(g_status & STAT_GPIO_IN),
                   g_bq_fcc, g_bq_rm);
        }

        sleep_ms(2);
    }
}
