# portaPLUTO
### LINQS Hardware Hack Week 2026

This is a handheld microwave analyzer based on the PlutoSky, a variant of the ADALM-PLUTO SDR development platform. At a hardware level, it consists of a PlutoSky RF PCB, a system board, and user IO. The software integrates with Maia-SDR and iio to deliver basic microwave characterization functionality.

## Hardware

To build a portaPLUTO, you will need the following:

- a PlutoSky board. Buy one without the GPIO header, as you'll want pins of a different height
- a mainboard PCB. JLCPCB stocks most of the components, but minor SMT hand assembly is required
- an aluminum midplate. The Fusion file includes toolpaths for a Makera Carvera, but you can get this manufactured externally. This needs to be aluminum, not plastic, as it is used as a heatsink.
- an 18560 cell. Unprotected is OK. The max discharge you should expect is 1.5 A
- a CH320QV15A-T 3.2" TFT display. Adafruit resells these
- Assorted 3D printed components (backplate, display mount, knob). Some of these have versions intended for machining with toolpaths

## Software

The main software package is built around tezuka and maia-sdr, though rtl_433 and dump1090 are exposed in applications. The software consists of an FPGA image, a Zynq firmware image, and a Pi Pico firmware image: these vary from the tezuka defaults, so ensure you flash both.

Importantly, *the system must be powered before you plug the Pico in to a computer! Otherwise you may damage the touchscreen controller*.
