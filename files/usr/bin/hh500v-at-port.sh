#!/bin/sh
# Find the GM800's real AT port and point QModem at it.
#
# The ttyUSB numbering depends on enumeration order, so we never trust a
# fixed name: every candidate is asked "AT" and the first one that answers
# OK is used. The result is written to uci and to /tmp for other scripts.

LOG=/tmp/hh500v-at-port.log
say() { echo "$(date '+%H:%M:%S') $*" >>"$LOG"; }

# Pin the QModem settings netifd/AT dependencies need. /etc/uci-defaults runs
# once, so anything that was changed later (or lost to a config restore) would
# stay wrong forever. Cheap enough to redo every boot, and it only commits when
# a value actually differs.
qset() { # qset <uci path> <value>
	[ "$(uci -q get "$1")" = "$2" ] && return 0
	uci -q set "$1=$2" && say "uci: $1 <- $2"
	QMODEM_DIRTY=1
}
qset qmodem.main.at_tool '1'		# sms_tool_q keeps AT on stdout; tom_modem uses stderr
qset qmodem.gm800.use_ubus '0'		# atgw serves the AT port itself, no ubus-at-daemon
qset qmodem.gm800.sms_mode 'direct'
qset qmodem.gm800.platform 'qualcomm'	# needed for the 5G choices + ZSNT paths
qset qmodem.gm800.at_port '/dev/ttyUSB2'
# All four gateway ptys must be declared, otherwise QModem's port-ownership
# check rejects the extra ports and its RPC handlers exit 2 ("No response").
qset qmodem.gm800.valid_at_ports '/dev/ttyUSB0 /dev/ttyUSB1 /dev/ttyUSB2 /dev/ttyUSB3'
# smsd reads on at_port and writes on sms_at_port: keep them on different ptys
# or its reader swallows the gateway's replies ("direct modem read failed").
qset qmodem.gm800.sms_at_port '/dev/ttyUSB3'
[ -n "$QMODEM_DIRTY" ] && uci -q commit qmodem && say "uci: qmodem committed"

# Keep QModem's un-timed AT calls from piling up on the tty (see the header of
# hh500v-qmodem-atfix.sh). Re-applied every boot so a package upgrade cannot
# undo it.
[ -x /usr/bin/hh500v-qmodem-atfix.sh ] && /usr/bin/hh500v-qmodem-atfix.sh >/dev/null 2>&1

# 3ginfo-lite calls gcom/sms_tool without a timeout, so its LuCI page spins
# forever once the data session has silenced AT. Re-apply the timeout patch on
# every boot in case the package got upgraded.
[ -x /usr/bin/hh500v-3ginfo-fix.sh ] && /usr/bin/hh500v-3ginfo-fix.sh >/dev/null 2>&1

# Already done once for this boot (procd may start us again): nothing to do,
# and don't touch the tty again - the LED sampler and QModem share it.
if [ -c "$(cat /tmp/hh500v.at_port 2>/dev/null)" ] && [ -s /tmp/hh500v.rat ]; then
	say "already configured ($(cat /tmp/hh500v.at_port), rat=$(cat /tmp/hh500v.rat))"
	exit 0
fi

# Every AT user must take QModem's own lock, otherwise two writers interleave
# and gcom fails with "Could not write to COM device".
atrun() { # atrun <dev> <script> <timeout>
	lockf="/var/lock/qmodem-at-$(printf '%s' "$1" | tr -c 'A-Za-z0-9_' '_').lock"
	rm -f /tmp/_hh_at.out
	( flock -w 5 9 || exit 0; timeout "${3:-6}" gcom -d "$1" -s "$2" >/tmp/_hh_at.out 2>/dev/null ) 9>"$lockf"
	cat /tmp/_hh_at.out 2>/dev/null
}

# Send a single AT command and print whatever came back (same lock as atrun).
atone() { # atone <dev> <cmd> <timeout> [lines]
	printf 'opengt\n\tset com 115200n81\n\tset senddelay 0.05\n\twaitquiet 1 0.2\n\tsend "%s\\r"\n\tget %s "" $o\n\tprint $o\n\texit\n.\n' \
		"$2" "${4:-4}" >/tmp/_hh_one.gcom
	atrun "$1" /tmp/_hh_one.gcom "${3:-10}"
	rm -f /tmp/_hh_one.gcom
}

probe() { # probe <dev> <timeout>
	[ -c "$1" ] || return 1
	if command -v gcom >/dev/null 2>&1; then
		cat >/tmp/_hh_probe.gcom <<-'EOF'
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
		r=$(atrun "$1" /tmp/_hh_probe.gcom "${2:-6}")
		rm -f /tmp/_hh_probe.gcom
		case "$r" in
		*OK*) return 0 ;;
		esac
		return 1
	fi
	exec 9<>"$1" 2>/dev/null || return 1
	printf 'AT\r\n' >&9
	r=$(timeout "${2:-3}" cat <&9 2>/dev/null | tr -d '\r')
	exec 9<&-
	case "$r" in
	*OK*) return 0 ;;
	esac
	return 1
}

# GM800 takes a long time to boot: its USB device only shows up ~160s after
# the board powers on, and the AT interface can come even later. So keep
# trying for 5 minutes instead of giving up early.
NEWID=/sys/bus/usb-serial/drivers/option1/new_id

AT=""
i=0
while [ $i -lt 60 ]; do
	i=$((i + 1))

	# Re-assert the usb-serial id in case the hotplug hook ran before the
	# option module was loaded (then no ttyUSB would ever be created).
	if [ -w "$NEWID" ] && [ ! -c /dev/ttyUSB0 ]; then
		echo "305a 1421" >"$NEWID" 2>/dev/null
	fi

	# Do not poke the serial ports while the modem is not up on PCIe. In that
	# state its USB stack enumerates but its firmware never serves AT, and
	# opening the tty just makes it reset again (observed: devnum 3..10 loop).
	if [ ! -d /sys/bus/pci/devices/0000:01:00.0 ]; then
		say "waiting for modem on pcie (try $i)"
		sleep 5
		continue
	fi

	for p in /dev/ttyUSB2 /dev/ttyUSB1 /dev/ttyUSB4 /dev/ttyUSB3 /dev/ttyUSB0 /dev/ttyUSB5; do
		if probe "$p" 6; then
			AT="$p"
			break
		fi
	done
	[ -n "$AT" ] && break
	say "no AT port yet (try $i)"
	sleep 5
done

if [ -z "$AT" ]; then
	say "gave up: no port answered AT (modem firmware may be stuck; power-cycle the router)"
	exit 1
fi

echo "$AT" >/tmp/hh500v.at_port
# 3ginfo-lite's detect.sh reads /tmp/modem first; seeding it skips its
# 5-port sweep and saves ~10s on every page load.  It gets its own pty too:
# qmodem-smsd parks a blocking reader on its at_port, and any second client
# on the same pty makes both of them lose the gateway's replies.
AUX=/dev/ttyUSB0
echo "$AUX" >/tmp/modem
say "AT port = $AT  (aux clients on $AUX)"

# Remember which radio we are on while AT still answers. Once the MBIM data
# session is up this module stops serving AT on USB, so the LED script has to
# rely on this cached value (act 7 = LTE, 11/13 = NR).
cat >/tmp/_hh_rat.gcom <<-'EOF'
	opengt
		set com 115200n81
		set senddelay 0.05
		waitquiet 1 0.2
		send "AT+COPS?\r"
		get 3 "" $c
		print $c
		exit
	.
EOF
rat=""
i=0
while [ $i -lt 3 ]; do
	i=$((i + 1))
	r=$(atrun "$AT" /tmp/_hh_rat.gcom 8 | tr -d '\r')
	act=$(echo "$r" | sed -n 's/.*+COPS: *[0-9]*,[0-9]*,"[^"]*",\([0-9][0-9]*\).*/\1/p' | head -n 1)
	case "$act" in
	7) rat=4g ;;
	11 | 13) rat=5g ;;
	esac
	[ -n "$rat" ] && break
	sleep 10
done
rm -f /tmp/_hh_rat.gcom
if [ -n "$rat" ]; then
	echo "$rat" >/tmp/hh500v.rat
	say "RAT = $rat (act=$act)"
else
	say "RAT unknown, LED will assume 5g"
fi

if [ -f /etc/config/qmodem ]; then
	uci -q set qmodem.gm800.at_port="$AT"
	# qmodem-smsd runs its URC reader on override_at_port and its command
	# batch on sms_at_port.  All three on one pty made the reader swallow the
	# gateway's answers, so the page failed with "direct modem read failed"
	# roughly one call in three (measured: 2/8 before, 0/8 after the split).
	uci -q set qmodem.gm800.sms_at_port='/dev/ttyUSB3'
	uci -q set qmodem.gm800.override_at_port='/dev/ttyUSB1'
	uci -q commit qmodem
	say "uci qmodem ports: at=$AT sms=/dev/ttyUSB3 reader=/dev/ttyUSB1"
fi

# sms-tool-js and modemband both need the port spelled out, otherwise their
# pages only show "requires user configuration" / "unsupported modem".
# They use $AUX, not $AT: qmodem-smsd keeps a blocking reader on $AT and the
# three clients would otherwise steal each other's replies.
if [ -f /etc/config/sms_tool_js ]; then
	for opt in readport sendport ussdport atport; do
		uci -q set "sms_tool_js.@sms_tool_js[0].$opt=$AUX"
	done
	uci -q set sms_tool_js.@sms_tool_js[0].storage='SM'
	uci -q set sms_tool_js.@sms_tool_js[0].pnumber='86'
	uci -q set sms_tool_js.@sms_tool_js[0].prefix='1'
	uci -q commit sms_tool_js
	say "uci sms_tool_js ports = $AUX"
fi
if [ -f /etc/config/modemband ]; then
	uci -q set modemband.@modemband[0].set_port="$AUX"
	uci -q set modemband.@modemband[0].get_port="$AUX"
	uci -q commit modemband
	say "uci modemband ports = $AUX"
fi

# Pull the SIM's SMS into the local database while AT still answers. smsd can
# only talk to the module before the data session starts, but the LuCI pages
# read the database afterwards - this is what makes the SMS list usable at all.
if [ -x /etc/init.d/qmodem-smsd ]; then
	uci -q set qmodem.gm800.sms_mode='database_poll'
	uci -q set qmodem.gm800.sms_poll_interval='600'
	# smsd's database mode goes through ubus-at-daemon ("database mode requires
	# use_ubus"), so bring the daemon up for the sync and take it down again:
	# leaving it running would have it hold the tty open for the rest of the day.
	uci -q set qmodem.gm800.use_ubus='1'
	uci -q commit qmodem
	[ -x /etc/init.d/ubus-at-daemon ] && /etc/init.d/ubus-at-daemon start >/dev/null 2>&1
	sleep 2
	/etc/init.d/qmodem-smsd start >/dev/null 2>&1
	sleep 3
	syncres=$(ubus call qmodem.sms sync '{"modem_id":"gm800"}' 2>&1 | tr -d '\n\t')
	say "sms sync: $syncres"
	# Keep use_ubus=1: the LuCI SMS pages refuse to read the database when it is
	# 0, even though only the sync needs the daemon. Shut the daemon down so it
	# cannot scratch the tty node while the data session is running.
	[ -x /etc/init.d/ubus-at-daemon ] && /etc/init.d/ubus-at-daemon stop >/dev/null 2>&1
	say "ubus-at-daemon stopped after sync"
fi

# Band lock, for real this time. The QModem "閿侀娈? page only records the
# choice (the gateway answers AT+ZBAND? from /etc/hh500v-modem-cfg, because AT
# is dead once the data session is up). So the only moment the module can be
# told about it is here, before ifup. /etc/hh500v-band-pushed remembers what
# the module was last told, so a normal boot costs nothing.
CFG=/etc/hh500v-modem-cfg
PUSHED=/etc/hh500v-band-pushed
kvread() { # kvread <file> <key>
	[ -f "$1" ] || return 0
	sed -n "s/^$2=//p" "$1" | tail -1
}
if [ -s "$CFG" ] && [ -n "$AT" ]; then
	# The first AT command after a cold boot is often swallowed by the module
	# (empty answer even though it runs). Burn one on a warmup so the ZBAND
	# exchange below gets a real reply.
	case "$(kvread "$CFG" locked_wcdma)$(kvread "$CFG" locked_lte)$(kvread "$CFG" locked_nr)" in
	'') ;;
	*)
		printf 'opengt\n\tset com 115200n81\n\tset senddelay 0.05\n\twaitquiet 1 0.2\n\tsend "AT\\r"\n\tget 4 "" $o\n\texit\n.\n' >/tmp/_hh_warm.gcom
		atrun "$AT" /tmp/_hh_warm.gcom 10 >/dev/null 2>&1
		rm -f /tmp/_hh_warm.gcom
		say "band lock: warmup done"
		;;
	esac
	for rat in 3 1 5; do
		case "$rat" in
		3) cls=WCDMA; key=locked_wcdma ;;
		1) cls=LTE;   key=locked_lte ;;
		5) cls=NR5G;  key=locked_nr ;;
		esac
		want=$(kvread "$CFG" "$key")
		have=$(kvread "$PUSHED" "$key")
		[ "$want" = "$have" ] && continue
		if [ -n "$want" ]; then
			cnt=$(echo "$want" | tr ',' '\n' | grep -c .)
			cmd="AT+ZBAND=$rat,$cnt,$want"
		elif [ -n "$have" ]; then
			# Clearing a lock must NOT become "AT+ZBAND=1,0" (count 0 =
			# lock to zero bands) - the module's "unlock everything" form
			# prints in its own syntax help as rat 0, so send exactly that.
			cmd="AT+ZBAND=0,0"
		else
			continue
		fi
		say "band lock: $cls -> [$want] via [$cmd]"
		{
			echo 'opengt'
			echo '	set com 115200n81'
			echo '	set senddelay 0.05'
			echo '	waitquiet 1 0.2'
			printf '	send "%s\\r"\n' "$cmd"
			echo '	get 4 "" $o'
			echo '	print $o'
			echo '	exit'
			echo '.'
		} >/tmp/_hh_band.gcom
		{
			echo "=== $cmd ==="
			atrun "$AT" /tmp/_hh_band.gcom 20
			echo
		} >/tmp/_hh_band.out 2>&1
		cat /tmp/_hh_band.out >>/tmp/at-window.out
		rm -f /tmp/_hh_band.gcom
		# remember only once the module had its say, so a failed push retries
		if ! grep -qi 'ERROR' /tmp/_hh_band.out; then
			sed -i "/^$key=/d" "$PUSHED" 2>/dev/null
			touch "$PUSHED"
			echo "$key=$want" >>"$PUSHED"
		fi
	done
	rm -f /tmp/_hh_band.out
fi

# Network mode (NR only and friends). Same story as the band lock above: the
# QModem "缃戠粶浼樺厛绾? page only records the choice, because AT+ZSNT can only
# reach the module while no data session is up. /etc/hh500v-netmode-pushed
# remembers what the module was told, so a normal boot costs nothing.
#
#   AT+ZSNT=7,0,0 -> NR5G only     (the "NR only" a phone can select)
#   AT+ZSNT=8,0,0 -> LTE + NR5G    (default)
#   AT+ZSNT=0,0,0 -> automatic 3G + 4G + 5G
#
# NR only can strand the box when the SA cell is gone, so the push is verified:
# if the module has not registered within 25 s we fall back to LTE+NR (and to
# fully automatic if that was already the request) and leave the choice marked
# as not pushed, so the next boot tries again.
NMPUSHED=/etc/hh500v-netmode-pushed
: >/tmp/netmode.out
nm_at() { # nm_at <AT command> <timeout seconds>
	{
		echo 'opengt'
		echo '	set com 115200n81'
		echo '	set senddelay 0.05'
		echo '	waitquiet 1 0.2'
		printf '	send "%s\\r"\n' "$1"
		echo '	get 4 "" $o'
		echo '	print $o'
		echo '	exit'
		echo '.'
	} >/tmp/_hh_nm.gcom
	echo "=== $1 ==="
	atrun "$AT" /tmp/_hh_nm.gcom "$2"
	echo
	rm -f /tmp/_hh_nm.gcom
}
nm_registered() {
	# +CEREG matters: in NR/NSA the module answers there, not on CREG, and a
	# false negative here used to revert a good "LTE+NR" push to automatic.
	out=$(nm_at "AT+C5GREG?" 10 2>&1
		nm_at "AT+CEREG?" 10 2>&1
		nm_at "AT+CREG?" 10 2>&1)
	echo "$out" | grep -Eq '^\+C(5G|E|)?REG: [0-9]+,(1|5)' && return 0
	echo "$out" | grep -Eq '^\+CREG: [0-9]+,(1|5)' && return 0
	return 1
}
if [ -n "$AT" ]; then
	want=$(kvread "$CFG" zsnt)
	[ -n "$want" ] || want=8,0,0
	# normalise to "<cm_mode>,0,0": the page may have left "8,1,1" behind, and the
	# module only cares about the first field.
	case "$want" in
	*,*) want="${want%%,*},0,0" ;;
	*) want="$want,0,0" ;;
	esac
	have=$(kvread "$NMPUSHED" zsnt)
	if [ "$want" != "$have" ]; then
		say "net mode: want [$want] was [$have]"
		nm_at "AT+ZSNT=$want" 15 >>/tmp/netmode.out 2>&1
		ok=0
		i=0
		# 40s: an EN-DC (LTE+NR) attach needs an LTE attach plus an NR
		# measurement report, which is much slower than a plain SA attach.
		while [ "$i" -lt 40 ]; do
			sleep 1
			i=$((i + 1))
			if nm_registered >>/tmp/netmode.out 2>&1; then
				ok=1
				break
			fi
		done
		if [ "$ok" = "1" ]; then
			say "net mode: [$want] applied, registered after ${i}s"
			sed -i "/^zsnt=/d" "$NMPUSHED" 2>/dev/null
			touch "$NMPUSHED"
			echo "zsnt=$want" >>"$NMPUSHED"
			# Safety net: registration is not the same thing as a working PDP,
			# and this mode can strand the box where no ssh can reach it. If the
			# mbim session has not come up 5 minutes from now, drop back to
			# LTE+NR and reset. Worst case the box is 5 minutes without internet
			# and heals itself instead of waiting for a plug pull.
			case "$want" in
			8,0,0 | 0,0,0) ;;
			*)
				(
					sleep 600
					if ! ip -4 addr show wwan0 2>/dev/null | grep -q 'inet '; then
						logger -t hh500v-netmode "no data 10 min after [$want], reverting to 8,0,0"
						echo "net mode: no data 10 min after [$want], reverting" >>/tmp/netmode.out
						/usr/bin/hh500v-netmode.sh lte5g >/dev/null 2>&1
						/usr/bin/hh500v-reset.sh --now
					fi
				) >/dev/null 2>&1 &
				;;
			esac
		else
			case "$want" in
			8,0,0 | 0,0,0) fb=0,0,0 ;; # already the widest we have
			*) fb=8,0,0 ;;
			esac
			say "net mode: [$want] not registered in 40s, falling back to [$fb]"
			nm_at "AT+ZSNT=$fb" 15 >>/tmp/netmode.out 2>&1
			echo "net mode: fallback to [$fb]" >>/tmp/netmode.out
			sed -i "/^zsnt=/d" "$NMPUSHED" 2>/dev/null
			touch "$NMPUSHED"
			echo "zsnt=$fb" >>"$NMPUSHED"
		fi
	fi
fi

# One-shot AT window (see /usr/bin/hh500v-at-mode.sh): run the queued command
# now, while the module still answers AT, then let the data session start.
# This works because the module serves AT only until the first PDP/MBIM session
# is opened - after that every AT port is silent until the module is reset.
# The window file may hold several commands, one per line ('#' lines ignored).
WC=/etc/hh500v-at-window.cmd
if [ -s "$WC" ]; then
	: >/tmp/at-window.out
	n=0
	while IFS= read -r atcmd; do
		[ -n "$atcmd" ] || continue
		case "$atcmd" in \#*) continue ;; esac
		n=$((n + 1))
		say "AT window: running [$atcmd]"
		# build the gcom script with the actual command inlined (gcom has no variables)
		{
			echo 'opengt'
			echo '	set com 115200n81'
			echo '	set senddelay 0.05'
			echo '	waitquiet 1 0.2'
			printf '	send "%s\\r"\n' "$atcmd"
			echo '	get 4 "" $o'
			echo '	print $o'
			echo '	exit'
			echo '.'
		} >/tmp/_hh_win.gcom
		{
			echo "=== $atcmd ==="
			atrun "$AT" /tmp/_hh_win.gcom 20
			echo
		} >>/tmp/at-window.out 2>&1
	done <"$WC"
	rm -f /tmp/_hh_win.gcom "$WC"
	# window done: hand the box back to data mode
	rm -f /etc/hh500v.at-mode
	say "AT window: done ($n commands), mode switched back to data"
fi

# Queued "empty the SIM message store" request. The LuCI page (模块 -> HH500V 短信)
# or hh500v-sim-clear.sh only drops a flag file, because AT is dead once the MBIM
# session is up - the actual delete has to happen here, in the boot window.
SCFLAG=/etc/hh500v-sim-clear
SCLOG=/etc/hh500v-sim-clear.log
if [ -f "$SCFLAG" ] && [ -n "$AT" ]; then
	say "SIM clear: request found, emptying the message store"
	{
		echo "=== SIM clear $(date '+%Y-%m-%d %H:%M:%S') on $AT"
		if [ -f /etc/hh500v-sim-clear-dry ]; then
			# dry run: ask the module which delete forms it knows, delete nothing
			echo "dry run, AT+CMGD=? -> $(atone "$AT" "AT+CMGD=?" 20 | tr '\r\n' '  ')"
			echo "dry run: nothing was deleted"
		else
			# This GM800 answers AT+CMGD=? with "+CMGD: (),(0-4)", i.e. it does
			# not advertise <index> at all, so try the 3GPP "delete all" forms
			# first and only then fall back to deleting index by index.
			deleted=0
			for c in "AT+CMGD=1,4" "AT+CMGD=0,4"; do
				o1=$(atone "$AT" "$c" 25)
				echo "$c -> $(printf '%s' "$o1" | tr '\r\n' '  ')"
				case "$o1" in *OK*) deleted=1; break ;; esac
			done
			if [ "$deleted" = 0 ]; then
				echo "delete-all forms rejected, deleting index by index"
				i=1; ok=0; bad=0
				while [ "$i" -le 50 ]; do
					o2=$(atone "$AT" "AT+CMGD=$i" 8)
					case "$o2" in
						*OK*) ok=$((ok + 1)) ;;
						*ERROR*) bad=$((bad + 1)) ;;
					esac
					i=$((i + 1))
				done
				echo "per-index: ok=$ok error=$bad"
			fi
		fi
		echo "store now: $(atone "$AT" "AT+CPMS?" 10 | tr '\r\n' '  ')"
		echo "=== done"
	} >>"$SCLOG" 2>&1
	rm -f "$SCFLAG"
	say "SIM clear: done (see $SCLOG)"
fi

# Cache the radio state (AcT / RSRP / RSRQ / PCI / band / ARFCN) while AT still
# answers. hh500v-atgw serves these back later, otherwise 3ginfo-lite reads
# "-" for every signal field and reports "LTE" for a 5G SA connection - the
# module refuses AT from the moment the MBIM session opens.
if [ -n "$AT" ] && [ -x /usr/bin/hh500v-sig-cache.sh ]; then
	/usr/bin/hh500v-sig-cache.sh "$AT" >>/tmp/at-window.out 2>&1
	say "signal cache done -> $(grep -cE '^(act|csq|reg|rsrp|rsrq|sinr|pci|band|earfcn)=' /etc/hh500v-modem-cfg 2>/dev/null) keys"
fi

# Optional DIAG probe: only runs when the flag file exists, because the DIAG
# port is the same tty this script just used and the gateway takes it over as
# soon as the data session is up. The window is the only moment it is free.
if [ -f /etc/hh500v-diag-probe ] && [ -x /usr/bin/hh500v-qcdm ]; then
	(
		for mask in 0x0 0xffff 0x1f 0x3ff; do
			echo "=== mask $mask ===" >>/tmp/diag.log
			/usr/bin/hh500v-qcdm -d "$AT" log 8 "$mask" >>/tmp/diag.log 2>&1
		done
		echo "=== logrange 0000 0fff ===" >>/tmp/diag.log
		/usr/bin/hh500v-qcdm -d "$AT" logrange 0000 0fff 8 >>/tmp/diag.log 2>&1
		echo "=== logrange b000 bfff ===" >>/tmp/diag.log
		/usr/bin/hh500v-qcdm -d "$AT" logrange b000 bfff 8 >>/tmp/diag.log 2>&1
	) >/dev/null 2>&1
	say "diag probe done -> $(wc -l </tmp/diag.log 2>/dev/null) lines"
fi
exit 0
