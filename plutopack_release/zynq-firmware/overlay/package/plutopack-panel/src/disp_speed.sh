#!/bin/sh
# Set the ILI9341 display SPI clock by editing the boot devicetree IN PLACE, then
# reboot. Always consistent with the deployed DT (edits only spi-max-frequency).
# usage: disp_speed <250k|1|2|5|10|20|30|HZ>
DTB=/boot/devicetree.dtb
NODE=/amba/spi@e0007000/display@0
case "$1" in
  250k) HZ=250000 ;;
  1)    HZ=1000000 ;;
  2)    HZ=2000000 ;;
  5)    HZ=5000000 ;;
  10)   HZ=10000000 ;;
  20)   HZ=20000000 ;;
  30)   HZ=30000000 ;;
  *)    case "$1" in ''|*[!0-9]*) HZ="" ;; *) HZ="$1" ;; esac ;;
esac
if [ -z "$HZ" ]; then
  echo "usage: disp_speed <250k|1|2|5|10|20|30|HZ>   (display SPI clock)"
  command -v fdtget >/dev/null && echo "current: $(fdtget "$DTB" "$NODE" spi-max-frequency 2>/dev/null) Hz"
  exit 1
fi
command -v fdtput >/dev/null || { echo "fdtput missing (needs BR2_PACKAGE_DTC)"; exit 1; }
fdtput -t i "$DTB" "$NODE" spi-max-frequency "$HZ" || { echo "fdtput failed ($NODE not found?)"; exit 1; }
echo "$HZ" > /boot/pp_spi_hz; sync
echo "display SPI clock -> $HZ Hz; rebooting in 2s..."; sleep 2; reboot
