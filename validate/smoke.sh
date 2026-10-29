#!/bin/sh
# Quick look at what the card brought up.
for d in /sys/bus/pci/devices/*; do
  [ "$(cat $d/vendor)" = 0x1234 ] && [ "$(cat $d/device)" = 0x7acc ] && echo "@@PCI $(basename $d) class $(cat $d/class)"
done
ls /dev/lacc* 2>&1
cat /proc/mtd
for h in /sys/class/hwmon/hwmon*; do echo "@@HWMON $(cat $h/name) $(ls $h | tr '\n' ' ')"; done
cat /sys/bus/pci/drivers/lacc/*/lacc/stats
cat /sys/bus/pci/drivers/lacc/*/lacc/fw_version /sys/bus/pci/drivers/lacc/*/lacc/booted_slot
dmesg | grep -iE "lacc|spi-nor|pmbus|at24|BUG|WARN|KASAN" | head -40
