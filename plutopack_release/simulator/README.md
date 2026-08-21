# PlutoPack front-panel simulator

A host web-app that runs the **real** front-panel app sources compiled with
`-DPP_SIM`, so what the browser renders is exactly what the panel draws — same
UI framework (`pp_ui.h`), same layout, same fonts — with no hardware.

The simulator is part of the `plutopack-panel` package (it compiles the app
sources from the package's `src/`), so it lives alongside them at:

```
../zynq-firmware/overlay/package/plutopack-panel/sim/
```

## Build & run

```bash
cd ../zynq-firmware/overlay/package/plutopack-panel/sim
make            # needs cc + pthreads; no other deps
./ppsim         # serves http://localhost:8080   ( ./ppsim 9000 to pick a port )
```

Open the URL. You get the 320×240 panel plus on-screen controls (D-pad, BTN7/8,
encoder), keyboard shortcuts, and — **click or drag directly on the display
image to inject a touch** at that pixel, driving the real firmware touch path.
This lets you exercise TOUCH CAL, the app markers, the on-screen keyboard, and
the maia-sdr 2D waterfall marker without hardware.

There is also a small scripted HTTP API (`/fb`, `/in?...`) for automated UI
tests. Full details, including the touch/marker walkthrough, are in that
directory's own `README.md`.

## How the seam works

`pp_common.h` / `pp_maia.h` have `#ifdef PP_SIM` branches that are inert in the
firmware build and active only here: framebuffer writes go to an in-memory
panel, Pico status polls come from web-injected buttons/encoder/touch, and the
maia radio is a synthetic spectrum model. See the in-package README for the
mapping table.
