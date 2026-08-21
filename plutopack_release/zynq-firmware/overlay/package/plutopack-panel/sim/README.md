# PlutoPack front-panel simulator

A host web app for testing the front-panel UI without hardware. It compiles the
**real** app sources (`../src/pp_launcher.c`, `pp_sdr.c`, `pp_test.c`) with
`-DPP_SIM`, so what the browser renders is exactly what `pp_ui.h` / `pp_screen.h`
draw on the panel — same code, same layout, same fonts.

## Build & run

```sh
make            # -> ./ppsim   (needs cc + pthreads; no other deps)
./ppsim         # serves on http://localhost:8080
./ppsim 9000    # ...or pick a port
```

Open the URL. You get the 320×240 panel (scaled 2×) plus on-screen controls:

- **D-pad**: ▲ ▼ ◄ ► and ● (center)
- **BTN7** (back) and **BTN8** (settings)
- **Encoder**: − / + (one click = one detent)
- **Touch**: click (or drag) anywhere on the display image — it injects a touch
  at that panel pixel, driving the real firmware touch path (`touch_down` +
  raw coords → `pp_touch_map`). Exercise TOUCH CAL, the app markers, the terminal
  keyboard, and the WATERFALL 2D marker without hardware.
- **Keyboard**: arrows = D-pad, Enter = center, `7`/`8` = buttons, `,` / `.` = encoder

It boots into the app switcher, exactly like the panel. CENTER opens the
highlighted app; inside an app, BTN7 is layered-back (edit → browse → screen →
switcher). See `../../claude_notes/plutopack_review/plutopack_ui_style_spec.md`.

## How the seam works (`PP_SIM`)

`pp_common.h` and `pp_maia.h` have `#ifdef PP_SIM` branches — inert in the
firmware build, active only here:

| Real hardware                       | Simulator                                   |
|-------------------------------------|---------------------------------------------|
| `pp_blit` → `/dev/fb0` pwrite       | `sim_fb_write()` → in-memory 320×240 panel  |
| `pp_poll` → Pico SPI status frame   | `sim_input_get()` + `sim_touch_get()` → web-injected buttons/enc/touch |
| `pp_maia.h` → maia-httpd on :80     | synthetic spectrum + local radio model      |
| launcher `fork`/`execl` the app     | `sim_run_app()` calls the app in-process    |

`sim_main.c` owns the panel buffer, the injected input, and a tiny embedded HTTP
server (`/` page, `/fb` raw RGBA, `/in?b=…&e=…` input). Everything under one
mutex; the app loop runs in the main thread, the server in a pthread.

The synthetic maia model quantizes `sampling_frequency` to 100 kHz and rounds
gain to integer dB, so the read-back / sync behaviour (amber "unconfirmed" →
confirmed, `sync..` in the status bar) actually exercises in the browser.

## HTTP API (for scripted UI tests)

```
GET /            -> the control page
GET /fb?v=<ver>  -> raw RGB565 little-endian (320*240*2 bytes); X-Ver header is the
                    frame version, 204 if unchanged since <ver> (cheap polling)
GET /in?b=UP     -> tap a button (UP/DOWN/LEFT/RIGHT/CENTER/B7/B8)
GET /in?e=1      -> encoder +1 detent  (e=-1 for -1)
GET /in?bd=UP    -> hold down / GET /in?bu=UP -> release
GET /in?t=160,120 -> touch down / move at panel pixel (sx,sy)
GET /in?tu=1     -> touch release
```

> Note: because a same-button tap is OR'd into one status frame until the app
> polls, scripted button presses must be spaced apart (~1 poll, i.e. tens of ms)
> or they collapse into a single edge. Touch `t=` updates are level state, not
> edges, so a hold = repeat the same `t=` a few times, then `tu=1`.

Example smoke test: launch PP-SDR, confirm the waterfall animates, walk into
settings and back:

```sh
h(){ curl -s localhost:8080/fb | md5sum | cut -c1-8; }
s(){ curl -s "localhost:8080/in?$1" >/dev/null; }
s b=CENTER; sleep .4; h; sleep .3; h     # two differing hashes = animating
s b=B8; sleep .3; h                      # settings screen
s b=B7; s b=B7                           # back to data, then to switcher
```
