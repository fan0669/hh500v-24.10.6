#!/bin/sh
# hh500v-reset.sh - reset this box in the only way that survives.
#
# Measured on 2026-11-05, three tries each:
#   `reboot`      -> powered, no ping/ARP/WiFi, needs a power cycle
#   `reboot -f`   -> same
#   SysRq-b       -> same (and that one skips device_shutdown() entirely)
# So the stall is not our teardown and not a driver shutdown hook: the reset
# itself never gets the SoC running again. A pulled plug always recovers, i.e.
# only a hardware reset works.
#
# The hardware reset we can trigger from software is the APSS watchdog
# (qcom_wdt, /dev/watchdog, 30 s) - but procd owns it and keeps feeding it.
# procd stops being able to feed it the moment the kernel panics:
#   1. /proc/sys/kernel/panic = 0, so a panic does NOT try the (broken)
#      software reboot path and just sits there;
#   2. crash the kernel with SysRq-c;
#   3. nobody feeds the watchdog any more, so 30 s later qcom_wdt resets the
#      SoC in hardware - the same thing the power switch does.
#
# Usage: hh500v-reset.sh [--now]
#   --now   skip the teardown/grace period, panic immediately

GRACE=3
[ "$1" = "--now" ] && GRACE=0

log() { echo "$*"; logger -t hh500v-reset "$*" 2>/dev/null; }

log "resetting: panic -> watchdog bite (hardware reset)"

rm -f /tmp/wan-watch.cnt 2>/dev/null

if [ "$GRACE" -gt 0 ]; then
	( ifdown wwan ) >/dev/null 2>&1 &
	sleep 3
	for s in hh500v-atgw hh500v-mbimprox; do
		[ -x /etc/init.d/$s ] && /etc/init.d/$s stop >/dev/null 2>&1
	done
	killall -q hh500v-atgw hh500v-mbimprox umbim 2>/dev/null
	sleep 1
	for m in /dev/ttyUSB1 /dev/ttyUSB2 /dev/ttyUSB3 /dev/wwan0mbim0; do
		grep -q " $m " /proc/mounts 2>/dev/null && umount "$m" 2>/dev/null
	done
	sync
	sleep 1
fi

# no software reboot on panic - let the hardware watchdog do it
echo 0 >/proc/sys/kernel/panic 2>/dev/null
echo 1 >/proc/sys/kernel/sysrq 2>/dev/null
sync
log "panicking now; expect the watchdog to reset the SoC in ~30 s"
echo c >/proc/sysrq-trigger 2>/dev/null

# only reached if SysRq-c was rejected
sleep 5
log "panic trigger failed, arming watchdog directly"
exec 3>/dev/watchdog 2>/dev/null && while :; do sleep 60; done
reboot -f
