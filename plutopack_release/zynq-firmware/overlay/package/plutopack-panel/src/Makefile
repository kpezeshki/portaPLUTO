CC ?= cc
CFLAGS ?= -O2 -Wall
COMMON = pp_common.h pp_screen.h
BINS = plutopack-panel spitest_zynq pp-launcher pp-test pp-touchcal pp-gui pp-sdr pp-tonegen pp-spectrum pp-sna pp-scope pp-iqrec pp-iqfeed pp-rtl433 pp-adsb pp-vna pp-twotone pp-ampgain pp-atten pp-manual pp-tetris pp-acks pp-rficstat pp-term pp-status pp-probe pp-fbnoise

all: $(BINS)

plutopack-panel: plutopack_panel.c pp_font.c pp_screen.h
	$(CC) $(CFLAGS) -o $@ plutopack_panel.c pp_font.c
spitest_zynq: spitest_zynq.c
	$(CC) $(CFLAGS) -o $@ spitest_zynq.c
pp-launcher: pp_launcher.c pp_font.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ pp_launcher.c pp_font.c
pp-test: pp_test.c pp_font.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ pp_test.c pp_font.c
pp-touchcal: pp_touchcal.c pp_font.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ pp_touchcal.c pp_font.c -lm
pp-gui: pp_gui_demo.c pp_font.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ pp_gui_demo.c pp_font.c
pp-sdr: pp_sdr.c pp_font.c $(COMMON) pp_ui.h pp_maia.h pp_waterfall.h
	$(CC) $(CFLAGS) -o $@ pp_sdr.c pp_font.c -lm
pp-tonegen: pp_tonegen.c pp_font.c $(COMMON) pp_ui.h pp_iio.h pp_cap.h
	$(CC) $(CFLAGS) -o $@ pp_tonegen.c pp_font.c -lm -liio
pp-spectrum: pp_spectrum.c pp_font.c $(COMMON) pp_ui.h pp_iq.h pp_fft.h
	$(CC) $(CFLAGS) -o $@ pp_spectrum.c pp_font.c -lm -liio
pp-sna: pp_sna.c pp_font.c $(COMMON) pp_ui.h pp_vna.h pp_iio.h pp_iq.h
	$(CC) $(CFLAGS) -o $@ pp_sna.c pp_font.c -lm -liio
pp-scope: pp_scope.c pp_font.c $(COMMON) pp_ui.h pp_cap.h
	$(CC) $(CFLAGS) -o $@ pp_scope.c pp_font.c -lm -liio
pp-iqrec: pp_iqrec.c pp_font.c $(COMMON) pp_ui.h pp_iq.h pp_fft.h pp_waterfall.h
	$(CC) $(CFLAGS) -o $@ pp_iqrec.c pp_font.c -lm -liio
pp-iqfeed: pp_iqfeed.c
	$(CC) $(CFLAGS) -o $@ pp_iqfeed.c -lm -liio
pp-rtl433: pp_rtl433.c pp_font.c $(COMMON) pp_ui.h
	$(CC) $(CFLAGS) -o $@ pp_rtl433.c pp_font.c -lm
pp-adsb: pp_adsb.c pp_font.c $(COMMON) pp_ui.h
	$(CC) $(CFLAGS) -o $@ pp_adsb.c pp_font.c -lm
pp-vna: pp_vna.c pp_font.c $(COMMON) pp_ui.h pp_vna.h pp_iio.h pp_iq.h
	$(CC) $(CFLAGS) -o $@ pp_vna.c pp_font.c -lm -liio
pp-twotone: pp_twotone.c pp_font.c $(COMMON) pp_ui.h pp_vna.h pp_iio.h pp_iq.h pp_waterfall.h
	$(CC) $(CFLAGS) -o $@ pp_twotone.c pp_font.c -lm -liio
pp-ampgain: pp_ampgain.c pp_font.c $(COMMON) pp_ui.h pp_vna.h pp_iio.h pp_iq.h
	$(CC) $(CFLAGS) -o $@ pp_ampgain.c pp_font.c -lm -liio
pp-atten: pp_atten.c pp_font.c $(COMMON) pp_ui.h pp_vna.h pp_iio.h pp_iq.h
	$(CC) $(CFLAGS) -o $@ pp_atten.c pp_font.c -lm -liio
pp-manual: pp_manual.c pp_font.c $(COMMON) pp_ui.h
	$(CC) $(CFLAGS) -o $@ pp_manual.c pp_font.c -lm
pp-tetris: pp_tetris.c pp_font.c $(COMMON) pp_ui.h
	$(CC) $(CFLAGS) -o $@ pp_tetris.c pp_font.c -lm
pp-acks: pp_acks.c pp_font.c $(COMMON) pp_ui.h
	$(CC) $(CFLAGS) -o $@ pp_acks.c pp_font.c -lm
pp-rficstat: pp_rficstat.c pp_font.c $(COMMON) pp_ui.h
	$(CC) $(CFLAGS) -o $@ pp_rficstat.c pp_font.c -lm
pp-term: pp_term.c pp_font.c $(COMMON) pp_ui.h
	$(CC) $(CFLAGS) -o $@ pp_term.c pp_font.c -lm
pp-status: pp_status.c pp_font.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ pp_status.c pp_font.c
pp-probe: pp_probe.c
	$(CC) $(CFLAGS) -o $@ pp_probe.c
pp-fbnoise: pp_fbnoise.c
	$(CC) $(CFLAGS) -o $@ pp_fbnoise.c
clean:
	rm -f $(BINS)
.PHONY: all clean
