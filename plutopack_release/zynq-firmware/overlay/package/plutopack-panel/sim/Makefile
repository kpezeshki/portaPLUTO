# PlutoPack front-panel simulator — builds the REAL app sources with -DPP_SIM
# and links them against the embedded web-server backend (sim_main.c).
#   make        -> ./ppsim
#   ./ppsim     -> open http://localhost:8080
CC     ?= cc
SRCDIR  = ../src
CFLAGS ?= -O2 -Wall -DPP_SIM -I$(SRCDIR)
LDLIBS  = -lm -lpthread

OBJS = sim_main.o pp_font.o pp_launcher.o pp_sdr.o pp_tonegen.o pp_spectrum.o pp_sna.o pp_scope.o pp_iqrec.o pp_rtl433.o pp_adsb.o pp_vna.o pp_twotone.o pp_ampgain.o pp_atten.o pp_manual.o pp_tetris.o pp_acks.o pp_test.o pp_term.o pp_rficstat.o pp_touchcal.o
HDRS = $(SRCDIR)/pp_common.h $(SRCDIR)/pp_ui.h $(SRCDIR)/pp_maia.h \
       $(SRCDIR)/pp_iio.h $(SRCDIR)/pp_cap.h $(SRCDIR)/pp_screen.h $(SRCDIR)/pp_waterfall.h \
       $(SRCDIR)/pp_iq.h $(SRCDIR)/pp_fft.h

ppsim: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

sim_main.o: sim_main.c $(SRCDIR)/pp_common.h $(SRCDIR)/pp_screen.h
	$(CC) $(CFLAGS) -c sim_main.c -o $@
pp_font.o: $(SRCDIR)/pp_font.c $(SRCDIR)/pp_screen.h
	$(CC) $(CFLAGS) -c $(SRCDIR)/pp_font.c -o $@
pp_launcher.o: $(SRCDIR)/pp_launcher.c $(HDRS)
	$(CC) $(CFLAGS) -Dmain=pp_launcher_main -c $(SRCDIR)/pp_launcher.c -o $@
pp_sdr.o: $(SRCDIR)/pp_sdr.c $(HDRS)
	$(CC) $(CFLAGS) -Dmain=pp_sdr_main -c $(SRCDIR)/pp_sdr.c -o $@
pp_tonegen.o: $(SRCDIR)/pp_tonegen.c $(HDRS)
	$(CC) $(CFLAGS) -Dmain=pp_tonegen_main -c $(SRCDIR)/pp_tonegen.c -o $@
pp_spectrum.o: $(SRCDIR)/pp_spectrum.c $(HDRS)
	$(CC) $(CFLAGS) -Dmain=pp_spectrum_main -c $(SRCDIR)/pp_spectrum.c -o $@
pp_sna.o: $(SRCDIR)/pp_sna.c $(HDRS)
	$(CC) $(CFLAGS) -Dmain=pp_sna_main -c $(SRCDIR)/pp_sna.c -o $@
pp_scope.o: $(SRCDIR)/pp_scope.c $(HDRS)
	$(CC) $(CFLAGS) -Dmain=pp_scope_main -c $(SRCDIR)/pp_scope.c -o $@
pp_iqrec.o: $(SRCDIR)/pp_iqrec.c $(HDRS)
	$(CC) $(CFLAGS) -Dmain=pp_iqrec_main -c $(SRCDIR)/pp_iqrec.c -o $@
pp_rtl433.o: $(SRCDIR)/pp_rtl433.c $(HDRS)
	$(CC) $(CFLAGS) -Dmain=pp_rtl433_main -c $(SRCDIR)/pp_rtl433.c -o $@
pp_adsb.o: $(SRCDIR)/pp_adsb.c $(HDRS)
	$(CC) $(CFLAGS) -Dmain=pp_adsb_main -c $(SRCDIR)/pp_adsb.c -o $@
pp_vna.o: $(SRCDIR)/pp_vna.c $(HDRS) $(SRCDIR)/pp_vna.h
	$(CC) $(CFLAGS) -Dmain=pp_vna_main -c $(SRCDIR)/pp_vna.c -o $@
pp_twotone.o: $(SRCDIR)/pp_twotone.c $(HDRS) $(SRCDIR)/pp_vna.h
	$(CC) $(CFLAGS) -Dmain=pp_twotone_main -c $(SRCDIR)/pp_twotone.c -o $@
pp_ampgain.o: $(SRCDIR)/pp_ampgain.c $(HDRS) $(SRCDIR)/pp_vna.h
	$(CC) $(CFLAGS) -Dmain=pp_ampgain_main -c $(SRCDIR)/pp_ampgain.c -o $@
pp_atten.o: $(SRCDIR)/pp_atten.c $(HDRS) $(SRCDIR)/pp_vna.h
	$(CC) $(CFLAGS) -Dmain=pp_atten_main -c $(SRCDIR)/pp_atten.c -o $@
pp_manual.o: $(SRCDIR)/pp_manual.c $(HDRS)
	$(CC) $(CFLAGS) -Dmain=pp_manual_main -c $(SRCDIR)/pp_manual.c -o $@
pp_tetris.o: $(SRCDIR)/pp_tetris.c $(HDRS)
	$(CC) $(CFLAGS) -Dmain=pp_tetris_main -c $(SRCDIR)/pp_tetris.c -o $@
pp_acks.o: $(SRCDIR)/pp_acks.c $(HDRS)
	$(CC) $(CFLAGS) -Dmain=pp_acks_main -c $(SRCDIR)/pp_acks.c -o $@
pp_test.o: $(SRCDIR)/pp_test.c $(HDRS)
	$(CC) $(CFLAGS) -Dmain=pp_test_main -c $(SRCDIR)/pp_test.c -o $@
pp_term.o: $(SRCDIR)/pp_term.c $(HDRS)
	$(CC) $(CFLAGS) -Dmain=pp_term_main -c $(SRCDIR)/pp_term.c -o $@
pp_rficstat.o: $(SRCDIR)/pp_rficstat.c $(HDRS)
	$(CC) $(CFLAGS) -Dmain=pp_rficstat_main -c $(SRCDIR)/pp_rficstat.c -o $@
pp_touchcal.o: $(SRCDIR)/pp_touchcal.c $(HDRS)
	$(CC) $(CFLAGS) -Dmain=pp_touchcal_main -c $(SRCDIR)/pp_touchcal.c -o $@

clean:
	rm -f $(OBJS) ppsim
.PHONY: clean
