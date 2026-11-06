#!/bin/sh
# HH500V status LEDs.
#
# The four indicators are driven by an AW9523 I2C GPIO expander at 0x5b on
# i2c-0 (orange is not a chip colour: "orange" = red + green lit together).
# Every LED is wired ACTIVE LOW, so writing 0 to a pin turns that LED on.
#
#   Port0 (reg 0x02): bit0 4G green, bit1 4G blue, bit3 WiFi blue,
#                     bit5 POWER blue, bit6 POWER red
#   Port1 (reg 0x03): bit0 5G red, bit1 5G green, bit2 5G blue, bit3 4G red
#
# If a kernel driver ever exposes /sys/class/leds entries (v2 with
# PINCTRL_AW9523), that path is preferred; otherwise we speak I2C directly.
#
# Meaning, following the TCL manual (blue = good signal, yellow = weak,
# off = not working) and "light the window of whichever radio is carrying
# the data":
#   POWER  blue, always on while the box is running
#   WIFI   blue while at least one AP is up
#   5G / 4G  only one of the two windows is lit at a time:
#            blue   = internet up, signal >= -95 dBm
#            yellow = internet up, signal -105..-95 dBm (red+green)
#            red    = modem present but no internet, or signal < -105 dBm
#            off    = that radio is not the one in use
# Both windows off = no modem at all. RAT comes from AT+COPS? (act 7 = LTE,
# 11/13 = NR); anything else defaults to the 5G window.

BUS=0
ADDR=0x5b
LEDS=/sys/class/leds
CSQ_FILE=/tmp/led.csq
ACT_FILE=/tmp/led.act
RAT_FILE=/tmp/hh500v.rat
STAT_TS=/tmp/led.csq.ts

have_i2c() { command -v i2cset >/dev/null 2>&1; }
i2cw() { i2cset -y -f "$BUS" "$ADDR" "$1" "$2" >/dev/null 2>&1; }

# clear bit $2 in variable $1 (shell has no references)
clr() {
	eval "v=\$$1"
	if [ $((v & $2)) -ne 0 ]; then
		v=$((v - $2))
	fi
	eval "$1=\$v"
}

leds_ready() {
	[ -d "$LEDS" ] || return 1
	[ -e "$LEDS/blue:5g" ] || [ -e "$LEDS/red:4g" ] || return 1
	return 0
}

set_led() { # set_led <name> <0|1>
	[ -e "$LEDS/$1" ] || return 0
	echo none >"$LEDS/$1/trigger" 2>/dev/null
	if [ "$2" = "1" ]; then
		echo 255 >"$LEDS/$1/brightness" 2>/dev/null
	else
		echo 0 >"$LEDS/$1/brightness" 2>/dev/null
	fi
}

# Prints "<csq> <act>" where csq is 0..31 (99 = unknown) and act is the
# AcT field of AT+COPS? (2/4/5/6/7 = 2G/3G, 7 = LTE, 11/13 = NR).
# Cached for 5 minutes. Only ever talks to the port that hh500v-at-port.sh
# confirmed, and never when the modem is not on PCIe: opening a tty that the
# module's own firmware has already given up on makes the module reset again.
get_at_stats() {
	now=$(date +%s)
	last=$(cat "$STAT_TS" 2>/dev/null)
	[ -n "$last" ] || last=0
	[ -d /sys/bus/pci/devices/0000:01:00.0 ] || return 0
	atport=$(cat /tmp/hh500v.at_port 2>/dev/null)
	[ -c "$atport" ] || return 0
	if [ $((now - last)) -ge 300 ] && command -v gcom >/dev/null 2>&1; then
		s=/tmp/led_at.gcom
		cat >"$s" <<-'EOF'
			opengt
				set com 115200n81
				set senddelay 0.05
				waitquiet 1 0.2
				send "AT+CSQ\r"
				get 3 "" $r
				print "CSQEOL $r"
				waitquiet 1 0.2
				send "AT+COPS?\r"
				get 3 "" $c
				print "COPSEOL $c"
				exit
			.
		EOF
		# Serialise with QModem: it flocks /var/lock/qmodem-at-<port>.lock
		# around every AT exchange. Two writers on the same tty is what makes
		# gcom报 "Could not write to COM device".
		lockf="/var/lock/qmodem-at-$(printf '%s' "$atport" | tr -c 'A-Za-z0-9_' '_').lock"
		(
			flock -w 5 9 || exit 0
			timeout 8 gcom -d "$atport" -s "$s" >/tmp/led_at.out 2>/dev/null
		) 9>"$lockf"
		out=$(tr -d '\r' </tmp/led_at.out 2>/dev/null)
		rm -f "$s" /tmp/led_at.out
		n=$(echo "$out" | sed -n 's/.*+CSQ: *\([0-9][0-9]*\),.*/\1/p' | head -n 1)
		a=$(echo "$out" | sed -n 's/.*+COPS: *[0-9]*,[0-9]*,"[^"]*",\([0-9][0-9]*\).*/\1/p' | head -n 1)
		if [ -n "$n" ] || [ -n "$a" ]; then
			[ -n "$n" ] && echo "$n" >"$CSQ_FILE"
			[ -n "$a" ] && echo "$a" >"$ACT_FILE"
			echo "$now" >"$STAT_TS"
		fi
	fi
	echo "$(cat "$CSQ_FILE" 2>/dev/null) $(cat "$ACT_FILE" 2>/dev/null)"
}

modem_present() {
	[ -e /sys/bus/pci/devices/0000:01:00.0 ] && return 0
	ls /dev/wwan*at0 /dev/wwan*mbim0 >/dev/null 2>&1 && return 0
	return 1
}

wan_online() {
	ifstatus wwan 2>/dev/null | grep -q '"up": *true'
}

wifi_up() {
	for i in phy0-ap0 phy1-ap0; do
		[ "$(cat /sys/class/net/$i/operstate 2>/dev/null)" = "up" ] && return 0
	done
	return 1
}

# ---- decide ----
pow=1                       # power LED: always on
wifi=0
g4=0                        # 4G window: 0 off, 1 red, 2 yellow, 3 blue
g5=0                        # 5G window: same scale

if wifi_up; then wifi=1; fi

# Which radio is actually carrying the data?
# Best effort: the module stops answering AT on its USB ports once a data
# session is up, so we sample AT+COPS? while we can and remember the answer.
# act 7 = LTE, 11/13 = NR; anything else keeps the last known value, and the
# default is 5G because this module dials NR whenever it can.
rat=5g
[ -f "$RAT_FILE" ] && rat=$(cat "$RAT_FILE")
stats=$(get_at_stats)
csq=${stats%% *}
act=${stats##* }
case "$act" in
7)
	rat=4g
	echo "$rat" >"$RAT_FILE"
	;;
11 | 13)
	rat=5g
	echo "$rat" >"$RAT_FILE"
	;;
esac

level=0
if ! modem_present; then
	level=0
elif wan_online; then
	case "$csq" in
	'' | 99) level=3 ;;          # no reading yet -> assume fine
	*)
		dbm=$(( -113 + 2 * csq ))
		if [ "$dbm" -ge -95 ]; then
			level=3
		elif [ "$dbm" -ge -105 ]; then
			level=2
		else
			level=1
		fi
		;;
	esac
else
	level=1                      # modem there but no internet -> red
fi

if [ "$level" = 0 ]; then
	g4=0
	g5=0
elif [ "$rat" = 4g ]; then
	g4=$level                    # running on LTE -> light the 4G window
	g5=0
else
	g5=$level                    # running on NR  -> light the 5G window
	g4=0
fi

# ---- drive ----
if leds_ready; then
	set_led blue:power "$pow"
	set_led blue:wlan "$wifi"
	set_led blue:4g "$([ "$g4" = 3 ] && echo 1 || echo 0)"
	set_led green:4g "$([ "$g4" = 2 ] && echo 1 || echo 0)"
	set_led red:4g "$([ "$g4" = 1 ] && echo 1 || echo 0)"
	set_led blue:5g "$([ "$g5" = 3 ] && echo 1 || echo 0)"
	set_led green:5g "$([ "$g5" = 2 ] && echo 1 || echo 0)"
	set_led red:5g "$([ "$g5" = 1 ] && echo 1 || echo 0)"
	[ -n "$DEBUG" ] && echo "sysfs pow=$pow wifi=$wifi rat=$rat csq=$csq act=$act 4g=$g4 5g=$g5"
	exit 0
fi

if ! have_i2c; then
	[ -n "$DEBUG" ] && echo "no i2c-tools, nothing to do"
	exit 0
fi

p0=255   # all off
p1=255

[ "$pow" = 1 ] && clr p0 32      # POWER blue (bit5)
[ "$wifi" = 1 ] && clr p0 8      # WiFi blue  (bit3)
# 4G window: blue = P0 bit1, green = P0 bit0, red = P1 bit3
case "$g4" in
3) clr p0 2 ;;
2) clr p0 1; clr p1 8 ;;         # green + red = yellow
1) clr p1 8 ;;
esac
# 5G window: red = P1 bit0, green = P1 bit1, blue = P1 bit2
case "$g5" in
3) clr p1 4 ;;
2) clr p1 1; clr p1 2 ;;         # red + green = yellow
1) clr p1 1 ;;
esac

# make sure the chip is in plain GPIO mode with all pins as outputs
i2cw 0x12 0xFF
i2cw 0x13 0xFF
i2cw 0x04 0x00
i2cw 0x05 0x00

i2cw 0x02 "$(printf '0x%02X' "$p0")"
i2cw 0x03 "$(printf '0x%02X' "$p1")"

[ -n "$DEBUG" ] && echo "i2c pow=$pow wifi=$wifi rat=$rat csq=$csq act=$act 4g=$g4 5g=$g5 p0=$p0 p1=$p1"
exit 0
