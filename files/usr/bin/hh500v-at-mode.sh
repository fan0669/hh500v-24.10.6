#!/bin/sh
# AT mode switch for the GM800.
#
# Hard fact on this hardware: the module serves AT only while no MBIM/PDP data
# session is open. As soon as netifd brings wwan up (MHI MBIM), the USB AT
# interface either answers nothing (ttyUSB1, /dev/wwan0at0) or refuses writes
# outright ("Could not write to COM device", ttyUSB2), and it stays that way
# until the module is reset. Tested both ways: taking the data session down
# afterwards does NOT bring AT back.
#
# So AT and 5G data are scheduled instead of shared:
#
#   hh500v-at-mode.sh data              normal operation (default): 5G up, no AT
#   hh500v-at-mode.sh at                reboot into AT mode: no data, AT stays
#                                       usable until you switch back
#   hh500v-at-mode.sh window "AT+CSQ"   reboot, run that command (several allowed)
#                                       before the data session starts, store the
#                                       answers in /tmp/at-window.out, bring 5G up
#   hh500v-at-mode.sh report            same, but queues a standard full report
#                                       (ATI/CPIN/CSQ/CREG/CGREG/COPS/CGATT/
#                                       CGDCONT/CGPADDR/CIMI/time zone)
#   hh500v-at-mode.sh status            what is armed right now + last output
#
# The actual work happens in hh500v-at-port.sh (runs early, while AT is alive)
# and hh500v-wwan-up (skips the bring-up when AT mode is armed).

FLAG=/etc/hh500v.at-mode
CMD=/etc/hh500v-at-window.cmd
OUT=/tmp/at-window.out

case "$1" in
data)
	rm -f "$FLAG" "$CMD"
	echo "normal mode armed: 5G data will come up, AT will not be usable"
	echo "(reboot for this to take effect, or just leave it - it is the default)"
	;;
at)
	echo at >"$FLAG"
	rm -f "$CMD"
	echo "AT mode armed: after the next reboot there will be NO 5G data and AT stays usable"
	echo "  then:  hh500v-at-mode.sh window is not needed, just use"
	echo "         gcom -d /dev/ttyUSB2 -s <script>   or   tom_modem -d /dev/ttyUSB2 -o a -c ATI"
	echo "  back:  hh500v-at-mode.sh data && reboot"
	;;
window)
	shift
	[ -n "$*" ] || { echo "usage: $0 window \"AT+CSQ\""; exit 1; }
	for c in "$@"; do printf '%s\n' "$c"; done >"$CMD"
	echo at >"$FLAG"
	echo "armed: at the next boot these run before the data session, result in $OUT:"
	cat "$CMD"
	echo "now reboot:  reboot"
	;;
report)
	cat >"$CMD" <<-'EOF'
		ATI
		AT+CPIN?
		AT+CSQ
		AT+CREG?
		AT+CGREG?
		AT+COPS?
		AT+CGATT?
		AT+CGDCONT?
		AT+CGPADDR
		AT+CIMI
		AT+CTZU=3
		AT+CTZR?
	EOF
	echo at >"$FLAG"
	echo "armed: a full AT report runs at the next boot, then 5G comes up automatically"
	echo "result: $OUT"
	echo "now reboot:  reboot"
	;;
status)
	printf 'mode file : '; [ -f "$FLAG" ] && cat "$FLAG" || echo "(none, normal)"
	printf 'pending   : \n'; [ -f "$CMD" ] && sed 's/^/    /' "$CMD" || echo "    (none)"
	printf 'last out  : '; [ -f "$OUT" ] && head -20 "$OUT" || echo "(none)"
	;;
*)
	sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
	;;
esac
