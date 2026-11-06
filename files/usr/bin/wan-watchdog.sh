#!/bin/sh
# HH500V: auto-reconnect the MBIM data session when the internet is dead.
# Runs from cron every 2 minutes; needs 3 consecutive failures (~6 min) before acting.
#
# Special case handled here: after a crash/reset the modem's PCIe link often does
# not train during boot ("qcom-pcie: Phy link never came up"), so /dev/wwan*mbim0
# never appears and ifup can never succeed. Upstream ships /etc/init.d/modem with
# a rescan loop for exactly this; we repeat that before giving up.
CNT=/tmp/wan-watch.cnt
LOG=/tmp/wan-watch.log

if ping -c 1 -W 4 223.5.5.5 >/dev/null 2>&1 || ping -c 1 -W 4 119.29.29.29 >/dev/null 2>&1; then
	echo 0 > "$CNT"
	exit 0
fi

n=$(cat "$CNT" 2>/dev/null)
case "$n" in ''|*[!0-9]*) n=0 ;; esac
n=$((n + 1))
echo "$n" > "$CNT"

[ "$n" -lt 3 ] && exit 0

echo "$(date '+%F %T') no internet after $n checks -> repairing modem" >> "$LOG"
echo 0 > "$CNT"

# 1) make sure the modem exists on PCIe at all
if [ ! -d /sys/bus/pci/devices/0000:01:00.0 ]; then
	echo "$(date '+%F %T') modem missing on pci, rescanning" >> "$LOG"
	for i in 0 1 2 3 4 5 6 7 8 9 10; do
		[ -d /sys/bus/pci/devices/0000:01:00.0 ] && break
		echo 1 > /sys/bus/pci/rescan
		sleep 2
	done
	sleep 3
fi

# 1b) A cold boot cannot train the PCIe link: the modem's endpoint only comes up
# after its firmware has booted (~2.5 min), while qcom-pcie gives up at ~1.2 s and
# has no .remove() so it cannot be re-probed. A warm reboot fixes it because the
# module keeps its 12 V rail (this was verified). Just record the situation.
if [ ! -d /sys/bus/pci/devices/0000:01:00.0 ]; then
	if [ "$(cat /proc/uptime | cut -d. -f1)" -lt 900 ]; then
		echo "$(date '+%F %T') modem not on pcie after cold boot; one warm reboot is needed" >> "$LOG"
	else
		echo "$(date '+%F %T') modem still not on pcie; interface will stay down" >> "$LOG"
	fi
fi

# 2) make sure network.wwan points at an existing wwan mbim port
DEV=""
for p in /dev/wwan*mbim0; do
	[ -e "$p" ] && DEV="$p"
done
if [ -n "$DEV" ] && [ "$(uci -q get network.wwan.device)" != "$DEV" ]; then
	echo "$(date '+%F %T') wwan device -> $DEV" >> "$LOG"
	uci set network.wwan.device="$DEV"
	uci commit network
fi

# 3) bounce the interface
ifdown wwan 2>/dev/null
sleep 4
ifup wwan 2>/dev/null
sleep 10
if ping -c 2 -W 4 223.5.5.5 >/dev/null 2>&1; then
	echo "$(date '+%F %T') reconnect OK (dev=${DEV:-none})" >> "$LOG"
else
	echo "$(date '+%F %T') reconnect FAILED (dev=${DEV:-none})" >> "$LOG"
fi
