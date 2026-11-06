#!/bin/sh
# Bring a stuck GM800 AT port back without touching the power cable.
#
# Failure modes handled here, in the order that actually works:
#   1. a hung child still holding the tty (QModem's un-timed sms_tool_q - see
#      hh500v-qmodem-atfix.sh). It also holds /var/lock/qmodem-at-<port>.lock,
#      so every other AT user blocks forever: reap these FIRST, before probing.
#   2. the option driver having lost the interface (node gone, or the node
#      exists but every write fails) - cured by unbind/bind of 3-1:1.1/.1.2/.1.4,
#      which makes the kernel re-run probe and recreate ttyUSB*, without
#      power-cycling the box (dmesg: "GSM modem (1-port) converter now attached
#      to ttyUSB1/2/3").
#   3. a tty that a previous command turned into a regular file (exec 3<> on a
#      stale node creates one) - only the option rebind removes that.
#
# Exits 0 when AT works, 1 when it still does not, and 0 without doing
# anything when the modem is not on PCIe at all (hh500v-coldboot owns that).

LOG=/tmp/hh500v-at-recover.log
say() { echo "$(date '+%H:%M:%S') $*" >>"$LOG"; }
PORTFILE=/tmp/hh500v.at_port
RATFILE=/tmp/hh500v.rat

[ -d /sys/bus/pci/devices/0000:01:00.0 ] || {
	say "modem not on pcie, skipping"
	exit 0
}

# While a data session is open the module does not serve AT at all - that is
# hardware behaviour, not a fault, and no amount of rebinding changes it. Only
# look for AT when no data session is up (AT mode / data down).
if ifstatus wwan 2>/dev/null | grep -q '"up": *true'; then
	say "data session up, AT is unavailable by design, skipping"
	exit 0
fi

# --- 1. reap anything stuck on a ttyUSB ------------------------------------
reaped=0
for pid in $(ls -l /proc/*/fd/* 2>/dev/null | grep -e ttyUSB | sed -n 's#.*/proc/\([0-9]*\)/fd/.*#\1#p' | sort -u); do
	[ "$pid" = "$$" ] && continue
	name=$(cat "/proc/$pid/comm" 2>/dev/null)
	case "$name" in
	sms_tool_q | gcom | tom_modem | flock | timeout)
		say "killing stuck $name (pid $pid)"
		kill -9 "$pid" 2>/dev/null
		reaped=1
		;;
	esac
done
[ "$reaped" = 1 ] && sleep 1

atrun() { # atrun <dev> <script> <timeout>
	lockf="/var/lock/qmodem-at-$(printf '%s' "$1" | tr -c 'A-Za-z0-9_' '_').lock"
	rm -f /tmp/_rc.out
	( flock -w 5 9 || exit 0; timeout "${3:-6}" gcom -d "$1" -s "$2" >/tmp/_rc.out 2>/dev/null ) 9>"$lockf"
	cat /tmp/_rc.out 2>/dev/null
}

cat >/tmp/_rc_probe.gcom <<-'EOF'
	opengt
		set com 115200n81
		set senddelay 0.05
		waitquiet 1 0.2
		send "AT\r"
		get 1 "" $s
		print $s
		exit
	.
EOF

probe() { # probe <dev> -> 0 when AT answers
	[ -c "$1" ] || return 1
	case "$(atrun "$1" /tmp/_rc_probe.gcom 6)" in
	*OK*) return 0 ;;
	esac
	return 1
}

PORT=$(cat "$PORTFILE" 2>/dev/null)
if [ -n "$PORT" ] && probe "$PORT"; then
	say "AT ok on $PORT"
	rm -f /tmp/_rc_probe.gcom
	exit 0
fi
say "AT dead on ${PORT:-<none>}, trying the other ports"

for p in /dev/ttyUSB2 /dev/ttyUSB1 /dev/ttyUSB3 /dev/ttyUSB0 /dev/ttyUSB4 /dev/ttyUSB5; do
	[ "$p" = "$PORT" ] && continue
	if probe "$p"; then
		echo "$p" >"$PORTFILE"
		rm -f "$RATFILE"
		uci -q set qmodem.gm800.at_port="$p"
		uci -q set qmodem.gm800.sms_at_port="$p"
		uci -q set qmodem.gm800.override_at_port="$p"
		uci -q commit qmodem
		say "AT is on $p instead, updated uci"
		rm -f /tmp/_rc_probe.gcom
		/usr/bin/hh500v-at-port.sh >/dev/null 2>&1 &
		exit 0
	fi
done

# --- 2. no port answers: recreate the tty nodes ----------------------------
say "no port answers, rebinding the option interfaces"
for i in 1.1 1.2 1.4; do
	echo "3-1:$i" >/sys/bus/usb/drivers/option/unbind 2>/dev/null
done
sleep 2
for i in 1.1 1.2 1.4; do
	echo "3-1:$i" >/sys/bus/usb/drivers/option/bind 2>/dev/null
done
sleep 3
[ -w /sys/bus/usb-serial/drivers/option1/new_id ] &&
	echo "305a 1421" >/sys/bus/usb-serial/drivers/option1/new_id 2>/dev/null
sleep 2

for p in /dev/ttyUSB2 /dev/ttyUSB1 /dev/ttyUSB3 /dev/ttyUSB0 /dev/ttyUSB4 /dev/ttyUSB5; do
	if probe "$p"; then
		echo "$p" >"$PORTFILE"
		rm -f "$RATFILE"
		uci -q set qmodem.gm800.at_port="$p"
		uci -q set qmodem.gm800.sms_at_port="$p"
		uci -q set qmodem.gm800.override_at_port="$p"
		uci -q commit qmodem
		say "recovered after rebind: AT ok on $p"
		rm -f /tmp/_rc_probe.gcom
		/usr/bin/hh500v-at-port.sh >/dev/null 2>&1 &
		exit 0
	fi
done

rm -f /tmp/_rc_probe.gcom
say "still no AT after rebind (power cycle needed)"
exit 1
