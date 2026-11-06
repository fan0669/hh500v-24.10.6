#!/bin/sh
# hh500v-3ginfo-fix.sh - make 3ginfo-lite behave on this box.
#
# Two independent patches, both idempotent:
#
# 1) timeouts. 3ginfo shells out to gcom / sms_tool without any timeout. The
#    module answers AT only until the MBIM data session opens, so those calls
#    never return and the LuCI page spins on "loading view..." forever. Wrapping
#    every call in `timeout` degrades the page to dashes instead of hanging
#    (2s per port keeps detect.sh's 5-port sweep under ~10s).
#
# 2) the MODE table stops at LTE. 3ginfo maps the <AcT> it gets from
#    +CREG/+COPS with: 2* UMTS, 3* EDGE, 4* HSDPA, 5* HSUPA, 6* HSPA, 7* LTE,
#    anything else -> "-". A 5G SA attach (AcT 10/11/13/19, EN-DC = 9) is
#    therefore shown as "-". hh500v-atgw reports the real AcT it cached during
#    the boot AT window, so extend the table with the NR values.
S=/usr/share/3ginfo-lite/3ginfo.sh
D=/usr/share/3ginfo-lite/detect.sh

[ -f "$S" ] || exit 0
[ -f "$D" ] || exit 0

# ---- 1) timeouts -----------------------------------------------------------
if grep -q 'timeout 2 gcom' "$S" && grep -q 'timeout 2 gcom' "$D"; then
	echo "3ginfo timeouts already patched"
else
	[ -f "$S.orig" ] || cp "$S" "$S.orig"
	[ -f "$D.orig" ] || cp "$D" "$D.orig"

	sed -i \
		-e 's/\$(sms_tool /$(timeout 3 sms_tool /g' \
		-e 's/`sms_tool /`timeout 3 sms_tool /g' \
		-e 's/\$(gcom /$(timeout 2 gcom /g' \
		-e 's/`gcom /`timeout 2 gcom /g' \
		"$S"

	sed -i \
		-e 's/^\(\s*\)gcom /\1timeout 2 gcom /' \
		-e 's/\$(gcom /$(timeout 2 gcom /g' \
		-e 's/`gcom /`timeout 2 gcom /g' \
		"$D"

	echo "3ginfo timeouts patched: $S ($(grep -c 'timeout ' "$S") calls) $D ($(grep -c 'timeout ' "$D") calls)"
fi

# ---- 2) NR5G in the MODE table --------------------------------------------
if grep -q 'HH500V-NR-MODE' "$S"; then
	echo "3ginfo mode table already extended"
else
	[ -f "$S.orig" ] || cp "$S" "$S.orig"
	sed -i \
		-e 's|^\(\s*\)7\*) MODE="LTE";;|\1 7*) MODE="LTE";;\n\t# HH500V-NR-MODE: NR5G / EN-DC, which stock 3ginfo does not know\n\t 9*) MODE="5G NSA";;\n\t1[0139]*) MODE="5G SA";;|' \
		"$S"
	if grep -q 'HH500V-NR-MODE' "$S"; then
		echo "3ginfo mode table extended with NR5G"
	else
		echo "!! 3ginfo mode table not found (no 7* LTE case) - left untouched"
	fi
fi

# ---- 3) RSSI + traffic section ---------------------------------------------
# Two more gaps that only show up with a MBIM modem behind a pty:
#
# a) RSSI. 3ginfo prints $RSSI but never assigns it unless it went through the
#    gcom/info.gcom code path. We have CSQ, and CSQ->dBm is the standard
#    3GPP mapping: -113 + 2*csq.
#
# b) SEC (the netifd section it uses for RX/TX/uptime/temperature). It guesses
#    it by walking /sys/class/tty|usbmisc and grepping `uci show network` for
#    the node name. Our modem is `option device '/dev/wwan0mbim0'`, which never
#    matches the tty nodes it walks, so SEC stays empty -> the page says
#    "waiting for connection data" and RX/TX/uptime are all "-".
if grep -q 'HH500V-RSSI' "$S"; then
	echo "3ginfo rssi/section already patched"
else
	[ -f "$S.orig" ] || cp "$S" "$S.orig"
	sed -i \
		-e 's|^# COPS numeric$|# HH500V-RSSI: derive RSSI from CSQ (3GPP mapping)\nif [ -z "$RSSI" ] \&\& [ -n "$CSQ" ]; then\n\tRSSI=$(( -113 + 2 * CSQ ))\nfi\n\n# COPS numeric|' \
		"$S"
	sed -i \
		-e 's|^NETUP=\$(ifstatus \$SEC|# HH500V-NETIF: the mbim device never matches the tty walk above\n[ -z "$SEC" ] \&\& SEC=wwan\nNETUP=$(ifstatus $SEC|' \
		"$S"
	if grep -q 'HH500V-RSSI' "$S" && grep -q 'HH500V-NETIF' "$S"; then
		echo "3ginfo rssi/section patched (RSSI from CSQ, SEC fallback = wwan)"
	else
		echo "!! 3ginfo rssi/section patch anchor missing - left partly untouched"
	fi
fi

# ---- 4) SCC1..SCC4: do not print a bare " | - -" ---------------------------
# The view fills each CA row with `sNband + ' | ' + sNpci + ' ' + sNearfcn`
# unconditionally. When the modem has no secondary carrier we write a short
# note into s1band instead of leaving "-", so the row would read
# "未聚合（基站只给 1 个载波） |  - -". Trim the suffix when both extra fields
# are "-". Same 4 replacements as flash/patch_3gdetail.py (verified identical).
JS=/www/luci-static/resources/view/modem/3gdetail.js
if [ -f "$JS" ]; then
	if grep -q "s1pci=='-'" "$JS"; then
		echo "3ginfo SCC rows already trimmed"
	else
		[ -f "$JS.orig-hh500v" ] || cp "$JS" "$JS.orig-hh500v"
		for n in 1 2 3 4; do
			sed -i "s#json.s${n}band+' | '+json.s${n}pci+' '+json.s${n}earfcn;#json.s${n}band+((json.s${n}pci=='-'\&\&json.s${n}earfcn=='-')?'':' | '+json.s${n}pci+' '+json.s${n}earfcn);#g" "$JS"
		done
		if [ "$(grep -c "pci=='-'" "$JS")" = "4" ]; then
			echo "3ginfo SCC rows trimmed (4 spots)"
		else
			echo "!! 3ginfo SCC trim incomplete - restoring backup"
			[ -f "$JS.orig-hh500v" ] && cp -f "$JS.orig-hh500v" "$JS"
		fi
	fi
fi