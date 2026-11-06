#!/bin/sh
# hh500v-reboot.sh - reboot this box in the only way that actually comes back.
#
# Measured on 2026-11-05 (three tries each, all left the box powered but dead -
# no ping, no ARP, no WiFi - until the plug was pulled):
#     `reboot`          hangs
#     `reboot -f`       hangs
#     SysRq-b           hangs (and that skips device_shutdown() entirely)
# The one thing that works is a real hardware reset: panic the kernel with
# /proc/sys/kernel/panic = 0 and let the APSS watchdog (qcom_wdt, 30 s) reset the
# SoC, because nothing can feed it once the kernel is down. Verified: the box is
# back on the LAN ~100 s later, with the modem still enumerated on PCIe (the 12 V
# rail was never cut), which is exactly what this board needs.
#
# So this script is a thin wrapper around hh500v-reset.sh, which does:
#   data down -> proxies stopped -> bind mounts gone -> sync -> panic -> wdt.
#
# Usage: hh500v-reboot.sh [--now]
#   --now   skip the teardown/grace period

exec /usr/bin/hh500v-reset.sh "$@"
