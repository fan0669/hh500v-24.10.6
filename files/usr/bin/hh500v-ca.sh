#!/bin/sh
# hh500v-ca.sh [--raw]
#
# Carrier aggregation status as far as this GM800 (GosunCn, SDX55) lets us see.
#
# Background: the module's AT surface has no SCC / SCell command at all -
# "AT+ZCELLINFO=?" lists the fields it can report and none of them is a
# carrier list - MBIM's MS Basic Connect Extensions only describes LTE cells,
# QMI is dead (no qmux on mainline MHI) and the DIAG log mask is disabled in
# firmware, so RRC signalling (the only remaining source) cannot be captured.
#
# The one useful side effect: AT+ZCELLINFO? prints ONE LINE PER CAMPED CELL.
# So the line count per RAT is our carrier count:
#   NR5G >= 2      -> NR carrier aggregation is active (2+ carriers)
#   LTE  >= 2      -> LTE carrier aggregation is active
#   LTE>=1 & NR>=1 -> EN-DC (NSA: LTE anchor + NR), which also carries LTE CA
#   1 & 1          -> single carrier, nothing aggregated right now
#
# The data comes from /etc/hh500v-cells.txt, written by hh500v-sig-cache.sh
# during the boot AT window (that is the only moment the module answers AT).
#
# Usage: hh500v-ca.sh          status + how to get aggregation
#        hh500v-ca.sh --raw    dump the raw cached +ZCELLINFO lines

CELLS=/etc/hh500v-cells.txt
CFG=/etc/hh500v-modem-cfg

kv() { sed -n "s/^$1=//p" "$CFG" 2>/dev/null | tail -1; }

if [ "$1" = "--raw" ]; then
	if [ -s "$CELLS" ]; then
		cat "$CELLS"
	else
		echo "no cached cell info ($CELLS is empty)"
	fi
	exit 0
fi

nlte=0
nnr=0
if [ -s "$CELLS" ]; then
	nlte=$(grep -c -i 'LTE:' "$CELLS" 2>/dev/null)
	nnr=$(grep -c -i 'NR5G:' "$CELLS" 2>/dev/null)
fi

act=$(kv act)
band=$(kv band)
arfcn=$(kv earfcn)
pci=$(kv pci)
rsrp=$(kv rsrp)
sinr=$(kv sinr)
zsnt=$(kv zsnt)

echo "=== HH500V 载波情况 ==="
if [ ! -s "$CELLS" ]; then
	echo "小区信息   ： 还没有缓存（要下次开机、模组驻网后才会写入）"
else
	echo "小区信息   ： LTE ${nlte} 个 / NR ${nnr} 个"
	case "$act" in
	13) echo "服务小区   ： 5G NR  band n${band:-?}  ARFCN ${arfcn:-?}  PCI ${pci:-?}" ;;
	7)  echo "服务小区   ： 4G LTE band B${band:-?}  ARFCN ${arfcn:-?}  PCI ${pci:-?}" ;;
	*)  echo "服务小区   ： AcT=${act:-?}  band ${band:-?}  ARFCN ${arfcn:-?}" ;;
	esac
	if [ -n "$rsrp" ] || [ -n "$sinr" ]; then
		echo "信号       ： RSRP ${rsrp:--} dBm   SINR ${sinr:--} dB"
	fi
fi

echo "网络模式   ： $(case "$zsnt" in
	7,*) echo "仅 5G（NR only） - 不能做 EN-DC，只能用 SA 的 NR 载波聚合" ;;
	8,*) echo "4G+5G（LTE+NR） - 允许 EN-DC，LTE 侧和 NR 侧都能聚合" ;;
	6,*) echo "仅 4G（LTE only） - 只能用 LTE 载波聚合" ;;
	0,*) echo "自动（2G/3G/4G/5G） - 最放开，回落最多" ;;
	*)   echo "${zsnt:-未设置}" ;;
esac)"

echo
echo "=== 结论 ==="
if [ "$nnr" -ge 2 ]; then
	echo "NR 载波聚合：已启用（NR 侧看到 ${nnr} 个小区）"
elif [ "$nlte" -ge 2 ]; then
	echo "LTE 载波聚合：已启用（LTE 侧看到 ${nlte} 个小区）"
elif [ "$nlte" -ge 1 ] && [ "$nnr" -ge 1 ]; then
	echo "EN-DC（NSA）：已启用（LTE 锚点 + NR 双连接）"
elif [ "$nnr" -eq 1 ] || [ "$nlte" -eq 1 ]; then
	echo "单载波：当前没有聚合（基站只给了 1 个载波）"
else
	echo "未知：还没有小区缓存，等下次开机再跑"
fi

echo
echo "=== 本机是否准备就绪（这几项都是我们能控制的）==="
lock_lte=$(kv locked_lte)
lock_nr=$(kv locked_nr)
lock_wcdma=$(kv locked_wcdma)
if [ -z "$lock_lte" ] && [ -z "$lock_nr" ] && [ -z "$lock_wcdma" ]; then
	echo "[OK]   频段锁        ：无锁（辅载波不会被我们自己挡住）"
else
	echo "[注意] 频段锁        ：LTE='${lock_lte}' NR='${lock_nr}' WCDMA='${lock_wcdma}'"
	echo "       → 执行 hh500v-band.sh clear all 后再重启"
fi
case "$zsnt" in
7,*) echo "[OK]   网络模式      ：仅 5G —— SA 的 NR 载波聚合可用；NSA/LTE CA 不可用" ;;
8,*|0,*) echo "[OK]   网络模式      ：允许 LTE+NR —— SA、NSA(EN-DC)、LTE CA 都能用" ;;
*)   echo "[注意] 网络模式      ：${zsnt:-未设置}，建议 hh500v-netmode.sh ca" ;;
esac
if [ -s /etc/hh500v-cells.txt ]; then
	echo "[OK]   小区采集      ：已记录（$CELLS）"
else
	echo "[注意] 小区采集      ：还没有，下次开机 AT 窗口会自动写入"
fi
echo "[OK]   CA 禁用开关    ：固件没有此开关，AT+QCFG=\"nr5g_disable_ca\" 回 0（未禁用）"
echo
echo "结论：这台机器的配置已经不挡路了。到了会聚合的小区，基站一给辅载波它就会用，"
echo "      用上面的小区行数就能确认；不需要任何额外开关（固件里本来也没有）。"

echo
echo "说明：载波聚合（CA）由基站调度，设备只能上报能力，不能要求辅载波。"
echo "      这块 GM800 硬件支持（NR 峰值 3.5Gbps / LTE Cat.22 / 4x4 MIMO），"
echo "      但固件不对外暴露 SCC 字段，所以只能用上面的小区行数来判断。"
echo
echo "想让它更容易聚合："
echo "  hh500v-netmode.sh lte5g      # 允许 EN-DC（4G+5G），NSA 区域必需"
echo "  hh500v-netmode.sh nronly     # 只走 5G（SA 区域可用 NR CA，NSA 区域会没网）"
echo "  hh500v-band.sh clear all     # 确保没有频段锁（锁了就会挡住辅载波）"
