PlutoPack front-panel demo & debug suite  (/usr/bin)
=====================================================
Display demos (render to /dev/fb0):
  pp-demo waterfall     SDR waterfall, synthesized 2.4 GHz spectrum
  pp-demo gui           animated status GUI (fake sensors, no Pico)
  pp-demo tune          SDR waterfall + LO tuner: ENCODER=digit value, SW3/SW4=move digit
  pp-demo live          live Pico sensor data -> status GUI
Headless / debug:
  pp-status [spidev]    poll Pico, print decoded status as text
  pp-probe [spidev] [mode] [hz] [n]   raw 32-byte SPI frame dump (default mode 1)
  pp-fbnoise [fb] [n]   write noise frames to the panel (display sanity)
  pp-fbrebind           unbind/rebind fbtft (re-init the panel; fixes flaky white)

Display SPI clock (higher = faster refresh; needs the fast level-shifter path):
  disp_speed <250k|1|2|5|10|HZ>  edit boot devicetree in place (fdtput) + reboot
  Demos auto-pace to it (marker /boot/pp_spi_hz; or set PP_SPI_HZ=<hz>).

Notes:
  * Pico link is SPI mode 1 (CPHA=1) — the RP2040 PL022 slave requires it.
  * The Pico bus enumerates as /dev/spidev2.0 (fbtft owns spi1, no spidev1.0).
