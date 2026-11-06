#!/bin/sh
# hh500v-netmode.sh - pick which radio access technologies the GM800 may use.
#
# The module speaks the ZTE/GosunCn AT+ZSNT command:
#
#   AT+ZSNT=<cm_mode>,<net_sel>,<pref_acq>
#
#     cm_mode 0 = automatic (3G + 4G + 5G)
#             2 = WCDMA only
#             6 = LTE only
#             7 = NR5G only        <- the "NR only" a phone can do
#             8 = LTE + NR5G
#
# This is the same switch the QModem "网络优先级" page drives, but that page can
# only express 3G/4G/5G combinations and it never reaches the module: once a data
# session is up every AT port of this modem is silent, so the value can only be
# handed over during the boot AT window. This script records the choice in
# /etc/hh500v-modem-cfg ("zsnt"), which the gateway reports back for AT+ZSNT?,
# and /usr/bin/hh500v-at-port.sh pushes it to the module before the first
# ifup, with an automatic fallback when registration does not come back.
#
#   hh500v-netmode.sh status
#   hh500v-netmode.sh auto       0,0,0   3G + 4G + 5G
#   hh500v-netmode.sh lte5g      8,0,0   LTE + NR5G   (default)
#   hh500v-netmode.sh nronly     7,0,0   NR5G only
#   hh500v-netmode.sh lteonly    6,0,0   LTE only
#
# Takes effect on the next boot (or: /usr/bin/hh500v-reset.sh).

CFG=/etc/hh500v-modem-cfg
PUSHED=/etc/hh500v-netmode-pushed

kvread() { [ -f "$1" ] || return 0; sed -n "s/^$2=//p" "$1" | tail -1; }
kvset() {
	key="$1"; val="$2"
	touch "$CFG"
	sed -i "/^$key=/d" "$CFG" 2>/dev/null
	printf '%s=%s\n' "$key" "$val" >>"$CFG"
}

name_of() {
	case "$1" in
	0,0,0 | 0 | '') echo "自动 (3G+4G+5G)" ;;
	2,0,0 | 2) echo "仅 3G (WCDMA)" ;;
	6,0,0 | 6) echo "仅 4G (LTE)" ;;
	7,0,0 | 7) echo "仅 5G (NR only)" ;;
	8,0,0 | 8 | 8,1,1) echo "4G+5G (LTE+NR)" ;;
	*) echo "未知 ($1)" ;;
	esac
}

case "$1" in
status)
	want=$(kvread "$CFG" zsnt)
	have=$(kvread "$PUSHED" zsnt)
	echo "配置里的网络模式 : ${want:-8,0,0}  ->  $(name_of "${want:-8,0,0}")"
	echo "已下发给模组的   : ${have:-（从未下发，下次开机会发）}  ->  $(name_of "$have")"
	if [ -f /tmp/at-window.out ] && grep -q 'net mode' /tmp/at-window.out 2>/dev/null; then
		echo "上次开机窗口     :"
		grep 'net mode' /tmp/at-window.out | tail -3 | sed 's/^/  /'
	fi
	;;
auto | 0) kvset zsnt 0,0,0 && echo "已设为 自动 (3G+4G+5G)，下次开机会下发给模组" ;;
lte5g | 8) kvset zsnt 8,0,0 && echo "已设为 4G+5G (LTE+NR)，下次开机会下发给模组" ;;
nronly | 7 | nr) kvset zsnt 7,0,0 && echo "已设为 仅 5G (NR only)，下次开机会下发给模组" ;;
lteonly | 6) kvset zsnt 6,0,0 && echo "已设为 仅 4G (LTE)，下次开机会下发给模组" ;;
ca)
	# Widest setting that still keeps 5G first: LTE + NR allows EN-DC (LTE
	# anchor + NR) *and* plain LTE carrier aggregation, which "NR only"
	# forbids. Use this when you carry the box to a place that aggregates.
	kvset zsnt 8,0,0
	echo "已设为 4G+5G (LTE+NR) —— 载波聚合机会最大的一档"
	echo "  在只开 SA 的小区仍然是 5G；在 NSA / LTE 载波聚合的小区也能吃上"
	;;
*)
	cat <<'EOF'
用法: hh500v-netmode.sh <命令>

  status      显示配置值与已下发值
  auto        自动 3G+4G+5G       (AT+ZSNT=0,0,0)
  lte5g       LTE + NR5G          (AT+ZSNT=8,0,0)  ← 默认
  ca          同 lte5g，为"载波聚合"场景准备的别名
  nronly      仅 5G，即 NR only    (AT+ZSNT=7,0,0)
  lteonly     仅 4G               (AT+ZSNT=6,0,0)

  「仅 5G」也能用 SA 的 NR 载波聚合，但它禁掉了 LTE 锚点，
  所以 NSA（EN-DC）和 LTE CA 都用不上；要"到处都能聚合"就选 ca。

改完要重启才生效：/usr/bin/hh500v-reset.sh
开机下发时会检查注册，若 40 秒内注不上会自动回退到 LTE+NR5G，
不会把你自己锁在没网的状态。
EOF
	;;
esac
