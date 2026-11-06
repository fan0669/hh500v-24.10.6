#!/bin/sh
# hh500v-band.sh - set the band lock that the module is told about at boot.
#
# The QModem page (模块 -> 高级调制解调器配置 -> 锁频段) can only *show* the
# current lock on this firmware: its Apply button goes through the QModem ubus
# object, which rejects the call ("Invalid argument") before it ever reaches the
# vendor script. This CLI writes the same state file the page reads, and the
# boot-time AT window pushes it to the module while AT is still alive.
#
#   hh500v-band.sh show                 what is stored / what was pushed
#   hh500v-band.sh set lte 1,3          lock LTE to B1+B3
#   hh500v-band.sh set nr 78            lock 5G to n78
#   hh500v-band.sh set wcdma 1          lock WCDMA to B1
#   hh500v-band.sh clear lte|nr|wcdma|all
#
# The module only takes AT before the data session starts, so a change needs a
# reboot. After the first boot the choice is remembered by the module itself
# (ZBAND survives resets), and /etc/hh500v-band-pushed stops us from re-sending
# it on every boot.

CFG=/etc/hh500v-modem-cfg
PUSHED=/etc/hh500v-band-pushed

kvread() { [ -f "$1" ] && sed -n "s/^$2=//p" "$1" | tail -1; }
kvwrite() { # kvwrite <file> <key> <value>
	touch "$1"
	sed -i "/^$2=/d" "$1"
	[ -n "$3" ] && echo "$2=$3" >>"$1"
	return 0
}

keyof() {
	case "$1" in
	lte|LTE) echo locked_lte ;;
	nr|NR|5g|5G) echo locked_nr ;;
	wcdma|WCDMA|3g|3G) echo locked_wcdma ;;
	*) echo "" ;;
	esac
}

case "$1" in
show)
	printf 'stored : lte=[%s] nr=[%s] wcdma=[%s]\n' \
		"$(kvread "$CFG" locked_lte)" "$(kvread "$CFG" locked_nr)" "$(kvread "$CFG" locked_wcdma)"
	printf 'pushed : lte=[%s] nr=[%s] wcdma=[%s]\n' \
		"$(kvread "$PUSHED" locked_lte)" "$(kvread "$PUSHED" locked_nr)" "$(kvread "$PUSHED" locked_wcdma)"
	if [ -f /tmp/at-window.out ]; then
		printf 'last AT window:\n'
		sed -n '1,12p' /tmp/at-window.out
	fi
	;;
set)
	k=$(keyof "$2")
	[ -n "$k" ] || { echo "usage: $0 set <lte|nr|wcdma> <band[,band...]>"; exit 1; }
	list=$(echo "$3" | tr -d ' ')
	case "$list" in
	''|*[!0-9,]*)
		echo "bad band list: $3"; exit 1 ;;
	esac
	n=$(echo "$list" | tr ',' '\n' | grep -c .)
	[ "$n" -ge 1 ] && [ "$n" -le 10 ] || { echo "1-10 bands per class (got $n)"; exit 1; }
	kvwrite "$CFG" "$k" "$list"
	echo "stored $k=$list"
	echo "reboot to apply (the module only accepts AT before the data session)"
	;;
clear)
	what="$2"
	[ -n "$what" ] || { echo "usage: $0 clear <lte|nr|wcdma|all>"; exit 1; }
	case "$what" in
	all)
		kvwrite "$CFG" locked_lte ""
		kvwrite "$CFG" locked_nr ""
		kvwrite "$CFG" locked_wcdma ""
		echo "all band locks cleared" ;;
	*)
		k=$(keyof "$what")
		[ -n "$k" ] || { echo "usage: $0 clear <lte|nr|wcdma|all>"; exit 1; }
		kvwrite "$CFG" "$k" ""
		echo "cleared $k" ;;
	esac
	echo "reboot to apply"
	;;
*)
	sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
	;;
esac
