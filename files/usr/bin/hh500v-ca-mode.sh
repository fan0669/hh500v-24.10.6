#!/bin/sh
# hh500v-ca-mode.sh - tiny, allow-listed wrapper used by the LuCI "HH500V 载波"
# page. It only accepts the four modes we support and forwards to
# /usr/bin/hh500v-netmode.sh, which writes the value into
# /etc/hh500v-modem-cfg. The value is pushed to the modem by hh500v-at-port.sh
# during the *next* boot AT window (AT dies as soon as the MBIM session is up).
#
# Keeping the allow-list here means the rpcd ACL only has to grant exec on this
# one script instead of the whole netmode helper.

set -e

usage() {
	echo "usage: hh500v-ca-mode.sh {ca|nronly|auto|lte5g}" >&2
	exit 1
}

[ $# -eq 1 ] || usage

case "$1" in
	ca)
		exec /usr/bin/hh500v-netmode.sh ca
		;;
	nronly)
		exec /usr/bin/hh500v-netmode.sh nronly
		;;
	auto)
		exec /usr/bin/hh500v-netmode.sh auto
		;;
	lte5g)
		exec /usr/bin/hh500v-netmode.sh lte5g
		;;
	*)
		usage
		;;
esac
