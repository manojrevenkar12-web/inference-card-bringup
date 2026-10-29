#!/bin/sh
# The bring-up run, inside the guest. Every result is one "@@R {json}" line on the console;
# the host turns the log into the bring-up report (tools/report.py).
#
#   1. lacc-validate: PCI, driver, DMA, compute, board, concurrency, perf, power, faults
#   2. lacc-mlp: a classifier's matmuls on the card in LNS8 and INT8
#   3. firmware: update slot B with laccctl, reboot into it, a corrupt image, a rollback
#   4. trace: a Perfetto trace of a short workload from the driver's tracepoints
#   5. module reload under load, then kmemleak and a scan of the kernel log
#
# Exit status is not used: the host reads the results.

dev=$(for d in /sys/bus/pci/devices/*; do
  [ "$(cat "$d/vendor")" = 0x1234 ] && [ "$(cat "$d/device")" = 0x7acc ] && echo "$d"
done)
attr=$dev/lacc

result() {  # group name pass detail
  d=$(printf '%s' "$4" | tr '"\\\n' "''  ")
  echo "@@R {\"group\":\"$1\",\"name\":\"$2\",\"pass\":$3,\"detail\":\"$d\",\"metrics\":{}}"
}

echo "@@STAGE validate"
/bin/lacc-validate

echo "@@STAGE inference"
/bin/lacc-mlp

echo "@@STAGE jtag"
# The host drives the card's JTAG port through OpenOCD: IDCODE, register reads, a write, and a
# link-down injected over JTAG. It answers on the console when it is done.
downs0=$(sed -n 's/^link_downs //p' "$attr/stats")
resets0=$(sed -n 's/^resets //p' "$attr/stats")
echo "@@JTAG ready"
if read -t 180 -r reply && [ "$reply" = "jtag done" ]; then
  # the driver sees the link drop and recovers
  # wait for the drop to be noticed, then for the reset that follows it to finish
  i=0
  while [ "$(sed -n 's/^link_downs //p' "$attr/stats")" = "$downs0" ] && [ $i -lt 50 ]; do usleep 100000; i=$((i+1)); done
  i=0
  while [ "$(sed -n 's/^resets //p' "$attr/stats")" = "$resets0" ] && [ $i -lt 50 ]; do usleep 100000; i=$((i+1)); done
  usleep 200000
  downs1=$(sed -n 's/^link_downs //p' "$attr/stats")
  out=$(/bin/lacc-validate driver)
  echo "$out" | grep -q '"pass":false' && ok=false || ok=true
  [ "$ok" = true ] || echo "$out" | grep '"pass":false' | sed 's/^@@R /@@INFO /'
  resets1=$(sed -n 's/^resets //p' "$attr/stats")
  [ "$downs1" = $((downs0 + 1)) ] && [ "$resets1" = $((resets0 + 1)) ] || ok=false
  result jtag "a link drop injected over JTAG: the driver notices and recovers" $ok "link_downs $downs0 -> $downs1, resets $resets0 -> $resets1; driver checks pass afterwards"
  scratch=$(/bin/laccctl regs | sed -n 's/^0x000c SCRATCH *\(0x[0-9a-f]*\).*/\1/p')
  [ "$scratch" = 0x0badcafe ] && ok=true || ok=false
  result jtag "a value written over JTAG reads back over PCIe" $ok "SCRATCH over BAR0: $scratch"
else
  result jtag "host JTAG session" false "no answer from the host (${reply:-timeout})"
fi

echo "@@STAGE firmware"
/bin/laccctl status --json | sed 's/^/@@STATUS /'
before=$(cat "$attr/fw_version")
if out=$(/bin/laccctl fw update /fw/fw-3.bin --slot b 2>&1); then
  after=$(cat "$attr/fw_version"); slot=$(cat "$attr/booted_slot")
  [ "$after" = 3 ] && [ "$slot" = b ] && ok=true || ok=false
  result firmware "update slot B to version 3 and boot it" $ok "version $before -> $after, booted slot $slot. $out"
else
  result firmware "update slot B to version 3 and boot it" false "$out"
fi
min=$(cat "$attr/fw_min_version")
[ "$min" = 2 ] && ok=true || ok=false
result firmware "booting version 3 raised the anti-rollback floor" $ok "floor is now $min (image rollback index 2)"

# a corrupt image is refused by laccctl before it reaches the flash ...
if out=$(/bin/laccctl fw update /fw/fw-bad.bin --slot a 2>&1); then
  result firmware "a corrupt image is refused before writing" false "laccctl accepted it: $out"
else
  result firmware "a corrupt image is refused before writing" true "$out"
fi
# ... and if one is written anyway, the boot ROM falls back to the other slot
if out=$(/bin/laccctl fw update /fw/fw-bad.bin --slot a --force --boot-slot a 2>&1); then
  slot=$(cat "$attr/booted_slot"); st=$(cat "$attr/fw_status")
  [ "$slot" = b ] && ok=true || ok=false
  result firmware "boot ROM rejects a corrupt slot and falls back" $ok "asked for slot A, booted $slot (status $st). $out"
else
  result firmware "boot ROM rejects a corrupt slot and falls back" false "$out"
fi
# an older image below the floor is refused by the tool and, if forced, by the boot ROM
if out=$(/bin/laccctl fw update /fw/fw-1.bin --slot a 2>&1); then
  result firmware "a rollback below the floor is refused" false "laccctl accepted it: $out"
else
  out2=$(/bin/laccctl fw update /fw/fw-1.bin --slot a --force --boot-slot a 2>&1)
  slot=$(cat "$attr/booted_slot"); ver=$(cat "$attr/fw_version")
  [ "$slot" = b ] && [ "$ver" = 3 ] && ok=true || ok=false
  result firmware "a rollback below the floor is refused" $ok "tool: $out; forced: boot ROM stayed on slot $slot, version $ver"
fi
# the card still computes after all the reboots
/bin/lacc-validate compute | grep -q '"pass":false' && ok=false || ok=true
result firmware "the card computes correctly after the firmware changes" $ok "compute checks re-run"

echo "@@STAGE trace"
if /bin/laccctl trace --seconds 2 --workload -o /tmp/trace.json >/tmp/trace.out 2>&1; then
  n=$(grep -o '"ph":"b"' /tmp/trace.json | wc -l)
  [ "$n" -gt 0 ] && ok=true || ok=false
  result trace "driver tracepoints to a Perfetto trace" $ok "$n job slices; $(cat /tmp/trace.out)"
  echo "@@FILE trace.json $(gzip -c /tmp/trace.json | base64 -w0)"
else
  result trace "driver tracepoints to a Perfetto trace" false "$(cat /tmp/trace.out)"
fi

echo "@@STAGE reload"
/bin/lacc-validate dma >/dev/null 2>&1 &
load=$!
sleep 1
if rmmod lacc 2>/tmp/rmmod.err; then
  result driver "remove the driver while a client has the device open" false "rmmod succeeded with an open file"
else
  result driver "remove the driver while a client has the device open" true "refused: $(cat /tmp/rmmod.err)"
fi
wait $load
ok=true
for i in 1 2 3; do
  rmmod lacc || ok=false
  insmod /lib/modules/lacc.ko || ok=false
done
[ -c /dev/lacc0 ] || ok=false
/bin/lacc-validate driver | grep -q '"pass":false' && ok=false
result driver "unload and reload three times; the card comes back" $ok "rmmod/insmod x3, then the driver checks"

echo "@@STAGE leaks"
if [ -w /sys/kernel/debug/kmemleak ]; then
  echo scan > /sys/kernel/debug/kmemleak; sleep 6; echo scan > /sys/kernel/debug/kmemleak
  leaks=$(grep -c "^unreferenced object" /sys/kernel/debug/kmemleak)
  lacc_leaks=$(grep -A20 "^unreferenced object" /sys/kernel/debug/kmemleak | grep -c "lacc")
  [ "$lacc_leaks" = 0 ] && ok=true || ok=false
  result kernel "kmemleak: no leaks from the driver" $ok "$leaks unreferenced objects in total, $lacc_leaks with lacc frames"
else
  result kernel "kmemleak: no leaks from the driver" false "kmemleak not available"
fi
# reports, not the subsystems' start-up banners ("DMA-API: debugging enabled ...")
splat='BUG:|WARNING:|KASAN:|possible circular locking|inconsistent lock state|possible recursive locking|DMA-API: [a-z]+ [0-9a-f]{4}:|UBSAN:|kernel BUG'
splats=$(dmesg | grep -cE "$splat")
dmesg | grep -E "$splat" | head -8 | sed 's/^/@@SPLAT /'
[ "$splats" = 0 ] && ok=true || ok=false
result kernel "no KASAN, lockdep, DMA-API or WARN reports in the kernel log" $ok "$splats reports"
dmesg | grep -iE "lacc" | tail -60 | sed 's/^/@@DMESG /'
