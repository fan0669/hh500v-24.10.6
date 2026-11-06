#!/bin/sh
# Patch QModem's at() helper so an AT exchange can never hang forever.
#
# Why: /usr/share/qmodem/modem_util.sh defines at() as the single entry point
# for every AT command QModem sends. It takes an flock on the tty and then
# calls either
#     sms_tool_q -d <port> at "<cmd>"
# or
#     tom_modem ... -d <port> -o a -c "<cmd>"
# with **no timeout at all** (only fastat() passes -t 1). The GM800 answers
# some commands with "+CME ERROR: memory failure" and then never sends OK
# (AT+CGSN is the known one), so sms_tool_q blocks forever. QModem retries,
# every retry leaves another child holding the tty, and after a few minutes
# 40+ processes own /dev/ttyUSB2 - at which point no AT works at all and only
# a power cycle brings it back.
#
# With -t 8 each call fails fast and the pile-up becomes impossible. The edit
# is idempotent and skips fastat() (which already passes -t 1).
#
# Called from hh500v-at-port.sh on every boot, so a QModem package upgrade
# cannot silently un-patch it.

F=/usr/share/qmodem/modem_util.sh
LOG=/tmp/hh500v-atfix.log
say() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >>"$LOG"; }

[ -f "$F" ] || {
	say "no $F, nothing to do"
	exit 0
}

if grep -q 'sms_tool_q -t 8' "$F" && grep -q 'flock -w 5 6' "$F" &&
	grep -q 'HH500V-CGSN2' /usr/share/qmodem/cmds/gosuncn.sh 2>/dev/null; then
	say "already patched"
	exit 0
fi

cp -f "$F" "$F.orig" 2>/dev/null

# The two patterns only appear in at(); fastat() already carries -t 1 and is
# left untouched. \$ matches a literal $ inside sed patterns.
sed -i \
	-e 's/sms_tool_q -d \$at_port/sms_tool_q -t 8 -d \$at_port/g' \
	-e 's/tom_modem \$use_ubus_flag/tom_modem -t 8 \$use_ubus_flag/g' \
	-e 's/flock -x 6 || exit 1/flock -w 5 6 || exit 1/g' \
	"$F" 2>>"$LOG"

n=$(grep -c 'sms_tool_q -t 8' "$F" 2>/dev/null)
m=$(grep -c 'tom_modem -t 8' "$F" 2>/dev/null)
k=$(grep -c 'flock -w 5 6' "$F" 2>/dev/null)
if [ "${n:-0}" -gt 0 ] && [ "${m:-0}" -gt 0 ] && [ "${k:-0}" -gt 0 ]; then
	say "patched at(): sms_tool_q -t 8 x$n, tom_modem -t 8 x$m, flock -w 5 x$k"
else
	say "PATCH FAILED (sms=$n tom=$m flock=$k) - restoring backup"
	cp -f "$F.orig" "$F" 2>/dev/null
	exit 1
fi

# --- 2) never send AT+CGSN to this module --------------------------------
# The GM800 firmware answers AT+CGSN with "+CME ERROR: memory failure" and
# then stops accepting writes on the USB AT interface altogether - that is the
# single command that kills the port. QModem calls it on every sim_info /
# base_info (i.e. every few seconds while the LuCI modem page is open).
#
# The IMEI is cosmetic, so serve it from a file and never touch the modem:
# QModem gets a value, the port stays alive. /etc/hh500v-imei is also what the
# AT gateway answers AT+CGSN / AT+EGMR=0,7 from, so "设置IMEI" and this cache
# always agree.
G=/usr/share/qmodem/cmds/gosuncn.sh
if [ -f "$G" ] && ! grep -q 'HH500V-CGSN2' "$G"; then
	# an older (cache-only) patch may be in place - start from the backup
	if grep -q 'HH500V-CGSN' "$G" && [ -f "$G.orig" ]; then
		cp -f "$G.orig" "$G" 2>/dev/null
		say "restored $G from backup before re-patching"
	fi
	cp -f "$G" "$G.orig" 2>/dev/null
	awk '
		/^[ \t]*at "[$]1" "AT\+CGSN"[ \t]*$/ {
			print "\t# HH500V-CGSN2: AT+CGSN wedges this module, answer from /etc/hh500v-imei"
			print "\t_cg=/etc/hh500v-imei"
			print "\t[ -s \"$_cg\" ] && { cat \"$_cg\"; return 0; }"
			print "\treturn 0"
			next
		}
		{ print }
	' "$G" >"$G.new" && mv "$G.new" "$G"
	if grep -q 'HH500V-CGSN2' "$G"; then
		say "patched gosuncn.sh: cmd_cgsn answers from /etc/hh500v-imei (backup $G.orig)"
	else
		say "gosuncn.sh patch FAILED, restoring"
		cp -f "$G.orig" "$G" 2>/dev/null
	fi
fi
exit 0
