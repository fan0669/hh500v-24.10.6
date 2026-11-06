#!/bin/sh
# hh500v-sig-cache.sh <at-port>
#
# Ask the module for its radio state while AT still answers (i.e. during the
# boot AT window, before netifd opens the MBIM session) and cache the values in
# /etc/hh500v-modem-cfg. hh500v-atgw then serves them back to 3ginfo-lite /
# modemband / QModem, which otherwise show "-" or "LTE" for the whole day
# because the module refuses AT once the data session is up.
#
# Why every value is optional: this GM800 answers only a subset of the usual
# Qualcomm AT surface (AT+ZSWITCH?, AT+ZSNT?, AT+ZCELLINFO? answer nothing at
# all, AT+CGSN fails). So each probe is independent and only a parsed answer
# updates the cache - a missing answer leaves the previous value alone.
#
# Keys written (all plain numbers/dBm, the gateway does the AT formatting):
#   act   3GPP AcT of the serving cell (13 = NR5G, 7 = LTE, 2 = UMTS)
#   csq   AT+CSQ <rssi> (0..31)
#   reg   registration state of the matching +CxxxREG
#   rsrp  dBm (negative)      rsrq  dB
#   sinr  dB                  pci   physical cell id
#   band  band number         earfcn ARFCN
#
# Usage: hh500v-sig-cache.sh /dev/ttyUSB2
#        hh500v-sig-cache.sh            (auto: /tmp/modem, then ttyUSB2)

CFG=/etc/hh500v-modem-cfg
OUT=/tmp/sig-probe.out
LOCK=/var/lock/hh500v-at.lock
# raw AT+ZCELLINFO? reply, one line per camped cell - the only hint at carrier
# aggregation this firmware gives us (see hh500v-ca.sh)
CELLS=/etc/hh500v-cells.txt

say() { echo "$(date +%H:%M:%S) sig: $*" >>"$OUT"; }

AT="$1"
if [ -z "$AT" ]; then
	AT=$(cat /tmp/modem 2>/dev/null)
	[ -n "$AT" ] || AT=/dev/ttyUSB2
fi
[ -c "$AT" ] || { say "no such port: $AT"; exit 1; }

kvset() { # kvset <key> <value>
	[ -n "$2" ] || return 0
	touch "$CFG"
	sed -i "/^$1=/d" "$CFG" 2>/dev/null
	echo "$1=$2" >>"$CFG"
}

ask() { # ask <at-command>  -> raw reply on stdout
	cat >/tmp/_sig.gcom <<-EOF
		opengt
			set com 115200n81
			set senddelay 0.05
			waitquiet 1 0.2
			send "$1\\r"
			get 4 "" \$o
			print \$o
			exit
		.
	EOF
	( flock -w 8 9 || exit 0
	  timeout 12 gcom -d "$AT" -s /tmp/_sig.gcom 2>/dev/null ) 9>"$LOCK"
	rm -f /tmp/_sig.gcom
}

: >"$OUT"
say "probing $AT"

# ---- signal quality: the CESQ/C5GREG pair is the only place RSRP/RSRQ live --
r=$(ask 'AT+CSQ' | tr -d '\r')
csq=$(echo "$r" | sed -n 's/.*+CSQ: *\([0-9]*\),.*/\1/p' | head -1)
[ -n "$csq" ] && kvset csq "$csq"
say "CSQ -> ${csq:--}"

r=$(ask 'AT+CESQ' | tr -d '\r')
# +CESQ: <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>
# rsrq: 0..34 as (x-39)/2 dB ; rsrp: 0..97 as (x-140) dBm ; 255/99 = not known
rsrq=$(echo "$r" | sed -n 's/.*+CESQ: *[0-9]*,[0-9]*,[0-9]*,[0-9]*,\([0-9]*\),.*/\1/p' | head -1)
rsrp=$(echo "$r" | sed -n 's/.*+CESQ: *[0-9]*,[0-9]*,[0-9]*,[0-9]*,[0-9]*,\([0-9]*\).*/\1/p' | head -1)
if [ -n "$rsrq" ] && [ "$rsrq" -le 34 ] 2>/dev/null; then
	kvset rsrq $(( (rsrq - 39) / 2 ))
	say "CESQ rsrq raw=$rsrq -> $(( (rsrq - 39) / 2 )) dB"
else
	say "CESQ rsrq -> ${rsrq:-none} (not usable)"
fi
if [ -n "$rsrp" ] && [ "$rsrp" -le 97 ] 2>/dev/null; then
	kvset rsrp $(( rsrp - 140 ))
	say "CESQ rsrp raw=$rsrp -> $(( rsrp - 140 )) dBm"
else
	say "CESQ rsrp -> ${rsrp:-none} (not usable)"
fi

# ---- serving cell: PCI / ARFCN / band / SINR / RSRP / RSRQ / TAC -----------
# The only command this GM800 actually answers is AT+ZCELLINFO?, and it does
# so in one long line:
#   +ZCELLINFO: NR5G: tac: 0X835500,pci: 948,band : 78,cell_id: 0,
#               freq: 633984,mcc: 460,mnc: 01,imsi: ...,rsrp: -70,
#               rsrq: -11,sinr: 27
# AT+QENG="servingcell" / AT+CPSI? / AT+CGCELLINFO? stay in the list for other
# modems, but ZCELLINFO goes first.  Everything is retried: early in the boot
# window the module has often not camped yet and answers with nothing, which is
# exactly why the first runs cached no cell info at all.
cell_parse() {
	r=$(printf '%s' "$1" | tr -d '\r')
	case "$r" in
	*PCI*|*pci*|*NR5G*|*LTE*)	;;
	*) return 1 ;;
	esac
	say "cell -> $(echo "$r" | tr -d '\n' | cut -c1-200)"
	pci=$(echo "$r" | sed -n 's/.*[Pp][Cc][Ii][ ":=,]*\([0-9]\{1,4\}\).*/\1/p' | head -1)
	[ -n "$pci" ] && kvset pci "$pci"
	# freq: 633984 (NR-ARFCN) or earfcn: 1850
	arfcntmp=$(echo "$r" | grep -o -i '\(e\)\{0,1\}arfcn[ ":=,]*[0-9]*' | head -1)
	[ -z "$arfcntmp" ] && arfcntmp=$(echo "$r" | grep -o -i '[a-z]*freq[ ":=,]*[0-9]*' | head -1)
	arfcn=$(echo "$arfcntmp" | grep -o '[0-9]*$')
	[ -n "$arfcn" ] && [ "$arfcn" != "0" ] && kvset earfcn "$arfcn"
	# band : 78 / BAND: 78 / band: 3
	bandnum=$(echo "$r" | sed -n 's/.*[Bb][Aa][Nn][Dd][ ":=,]*\([0-9]\{1,3\}\).*/\1/p' | head -1)
	[ -n "$bandnum" ] && [ "$bandnum" != "0" ] && kvset band "$bandnum"
	# rsrp: -70   rsrq: -11   sinr: 27
	rsrp=$(echo "$r" | sed -n 's/.*[Rr][Ss][Rr][Pp][ ":=,]*\(-\{0,1\}[0-9]\{1,3\}\).*/\1/p' | head -1)
	[ -n "$rsrp" ] && kvset rsrp "$rsrp"
	rsrq=$(echo "$r" | sed -n 's/.*[Rr][Ss][Rr][Qq][ ":=,]*\(-\{0,1\}[0-9]\{1,3\}\).*/\1/p' | head -1)
	[ -n "$rsrq" ] && kvset rsrq "$rsrq"
	sinr=$(echo "$r" | sed -n 's/.*[Ss][Ii][Nn][Rr][ ":=,]*\(-\{0,1\}[0-9]\{1,3\}\).*/\1/p' | head -1)
	[ -n "$sinr" ] && kvset sinr "$sinr"
	# tac: 0X835500 (hex, keep it without the prefix)
	tac=$(echo "$r" | sed -n 's/.*[Tt][Aa][Cc][ ":=,]*[0O]*[Xx]*\([0-9A-Fa-f]\{4,8\}\).*/\1/p' | head -1 | tr 'a-f' 'A-F')
	[ -n "$tac" ] && kvset tac "$tac"
	# Carrier aggregation: AT+ZCELLINFO? prints one line per camped cell, so the
	# number of lines per RAT is the only CA visibility this firmware offers.
	# 1 line = single carrier, 2+ lines = secondary carriers were added.
	nlte=$(printf '%s\n' "$r" | grep -c -i 'LTE:')
	nnr=$(printf '%s\n' "$r" | grep -c -i 'NR5G:')
	kvset cells_lte "$nlte"
	kvset cells_nr "$nnr"
	printf '%s\n' "$r" >"$CELLS"
	say "cells: lte=$nlte nr=$nnr"
	return 0
}

cell_done=""
for try in 1 2 3 4 5 6; do
	for cmd in 'AT+ZCELLINFO?' 'AT+QENG="servingcell"' 'AT+CPSI?' 'AT+CGCELLINFO?'; do
		r=$(ask "$cmd")
		if cell_parse "$r"; then
			cell_done="$cmd"
			break
		fi
	done
	[ -n "$cell_done" ] && break
	say "cell retry $try: nothing yet"
	# the module only answers once it has camped, so give it time
	[ "$try" -lt 6 ] && sleep 6
done
[ -n "$cell_done" ] || say "cell: no serving cell info after 6 tries"

# ---- which radio access technology are we actually on ----------------------
# AT+ZPAS? is the one vendor command this module does answer.
r=$(ask 'AT+ZPAS?' | tr -d '\r')
case "$r" in
*NGRAN*|*NR5G*)
	kvset act 13
	say "ZPAS -> $r => act=13 (NR5G)"
	;;
*LTE*|*EUTRAN*)
	kvset act 7
	say "ZPAS -> $r => act=7 (LTE)"
	;;
*)
	say "ZPAS -> ${r:-none}"
	;;
esac

r=$(ask 'AT+C5GREG?' | tr -d '\r')
reg=$(echo "$r" | sed -n 's/.*+C5GREG: *[0-9]*,\([0-9]*\).*/\1/p' | head -1)
if [ -n "$reg" ]; then
	kvset reg "$reg"
	[ "$reg" = "1" ] && kvset act 13
	say "C5GREG -> stat=$reg"
fi

r=$(ask 'AT+CEREG?' | tr -d '\r')
act=$(echo "$r" | sed -n 's/.*+CEREG: *[0-9]*,*[0-9]*,"[^"]*","[^"]*",\([0-9]*\).*/\1/p' | head -1)
if [ -n "$act" ]; then
	case "$act" in
	13|19) kvset act 13 ;;
	7|11) [ "$(sed -n 's/^act=//p' "$CFG" 2>/dev/null | tail -1)" = "13" ] || kvset act 7 ;;
	esac
	say "CEREG -> act=$act"
fi

# ---- TAC / LAC: 3ginfo prints "-" for 跟踪区域码 when it cannot read it ------
# The module only answers this before the data session comes up, so it has to
# be captured here and replayed by hh500v-atgw (AT+CEREG / AT+CREG?).
r=$(ask 'AT+CEREG=2' | tr -d '\r')
r=$(ask 'AT+CEREG?' | tr -d '\r')
tac=$(echo "$r" | sed -n 's/.*+CEREG: *[0-9]*,*[0-9]*,"\([^"]*\)".*/\1/p' | head -1 | tr -d ' \r')
lac=$(echo "$r" | sed -n 's/.*+CEREG: *[0-9]*,*[0-9]*,"\([^"]*\)".*/\1/p' | head -1 | tr -d ' \r')
if [ -n "$tac" ] && [ "$tac" != "-" ] && [ "$tac" != "0000" ]; then
	kvset tac "$tac"
	kvset lac "$tac"
	say "CEREG -> tac=$tac"
else
	# fall back to LTE CREG (this module answers it while camping on LTE)
	r=$(ask 'AT+CREG=2' | tr -d '\r')
	r=$(ask 'AT+CREG?' | tr -d '\r')
	lac=$(echo "$r" | sed -n 's/.*+CREG: *[0-9]*,*[0-9]*,"\([^"]*\)".*/\1/p' | head -1 | tr -d ' \r')
	if [ -n "$lac" ] && [ "$lac" != "-" ] && [ "$lac" != "0000" ]; then
		kvset lac "$lac"
		kvset tac "$lac"
		say "CREG -> lac=$lac (used as tac)"
	else
		say "CEREG/CREG -> no tac (${tac:-none}/${lac:-none})"
	fi
fi

say "cached: $(grep -E '^(act|csq|reg|rsrp|rsrq|sinr|pci|band|earfcn|lac|tac)=' "$CFG" 2>/dev/null | tr '\n' ' ')"
exit 0
