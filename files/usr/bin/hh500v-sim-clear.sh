#!/bin/sh
# hh500v-sim-clear.sh - queue / inspect the "empty the SIM message store" job.
#
# Why it is a two-step thing: the GM800 stops answering AT the moment the MBIM
# data session opens, so AT+CMGD can only be sent in the boot AT window (before
# dialling). This script just drops a flag; hh500v-at-port.sh picks it up in
# that window, deletes the messages and writes /etc/hh500v-sim-clear.log.
#
# Usage:
#   hh500v-sim-clear.sh status     # queued? what happened last time?
#   hh500v-sim-clear.sh request    # queue it for the next boot
#   hh500v-sim-clear.sh cancel     # drop the request
#   hh500v-sim-clear.sh log        # full log

FLAG=/etc/hh500v-sim-clear
LOG=/etc/hh500v-sim-clear.log

case "$1" in
	request)
		: >"$FLAG"
		# keep the flag across reboots (JFFS2 tmpfs? no: /etc is on the overlay)
		sync
		echo 'queued: AT+CMGD=1,4 will run in the next boot AT window'
		echo 'restart with: /usr/bin/hh500v-reset.sh   (plain reboot hangs on this board)'
		;;
	cancel)
		rm -f "$FLAG"
		echo 'cancelled'
		;;
	status)
		if [ -f "$FLAG" ]; then
			echo 'state: queued (waiting for the next boot)'
		else
			echo 'state: idle'
		fi
		if [ -f "$LOG" ]; then
			echo
			echo 'last run:'
			tail -n 8 "$LOG"
		else
			echo
			echo 'last run: never'
		fi
		;;
	log)
		cat "$LOG" 2>/dev/null || echo '(no log yet)'
		;;
	*)
		echo 'usage: hh500v-sim-clear.sh status|request|cancel|log' >&2
		exit 1
		;;
esac
