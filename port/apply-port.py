#!/usr/bin/env python3
"""
把 HH500V 的支持移植到 ImmortalWrt 24.10（内核 6.6）

思路：不使用 git apply / patch（补丁上下文对不上），而是对每个文件做
"定位锚点 + 插入/追加"，因此不依赖上游文件的具体行号与上下文。

覆盖：
  1. DTS（从 openwrt main 下载）
  2. ipq807x.mk 设备条目
  3. 01_leds / 02_network / 11-ath11k-caldata 的 case 块
  4. uboot-envtools 的 APPSBLENV 处理
  5. ipq-wifi：加板子 + 版本升级到含 HH500V BDF 的提交
  6. 内核 config-6.6：补 PINCTRL_AW9523 / POWER_RESET_GPIO

说明：platform.sh + tcl.sh（sysupgrade 双分区切换）不在本脚本内，
      首刷后若要在线升级，需要第二轮补齐。
"""
import os
import re
import subprocess
import sys
import urllib.request

ROOT = sys.argv[1] if len(sys.argv) > 1 else "openwrt"
RAW_MAIN = "https://raw.githubusercontent.com/openwrt/openwrt/main/"
GH = "https://github.com/openwrt/openwrt/commit/"

TABBED = "\t"


def log(msg):
    print(f"[port] {msg}", flush=True)


def fetch(url, binary=False):
    """下载；直连失败时回退到 gh-proxy 镜像（本地/受限网络用）"""
    last = None
    for u in (url, "https://gh-proxy.com/" + url):
        try:
            req = urllib.request.Request(u, headers={"User-Agent": "port-script"})
            data = urllib.request.urlopen(req, timeout=120).read()
            return data if binary else data.decode("utf-8", "replace")
        except Exception as exc:  # noqa: BLE001
            last = exc
    raise last


def read(rel):
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8") as f:
        return f.read()


def write(rel, text):
    path = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def insert_before_last(text, needle, block):
    """在最后一个 needle 之前插入 block"""
    idx = text.rfind(needle)
    if idx < 0:
        raise SystemExit(f"anchor not found: {needle!r}")
    return text[:idx] + block + text[idx:]


# ---------------------------------------------------------------- 1. DTS
# 重要：必须用【原版提交 ac8b5526bc】里的 DTS。
# main 的 DTS 已改成新 DSA/uniphy 绑定（uniphy0 / pcs-handle），6.6 内核没有这些节点；
# 原版用 dp4/dp6_syn + qcom,port_phyinfo，与 24.10（nss-dp）匹配。
DTS_REL = "target/linux/qualcommax/files/arch/arm64/boot/dts/qcom/ipq8072-linkhub-hh500v.dts"
DTS_PINNED = ("https://raw.githubusercontent.com/openwrt/openwrt/ac8b5526bc/"
              + DTS_REL)
log("download DTS (pinned to ac8b5526bc, old ESS binding)")
if os.path.exists(os.path.join(ROOT, DTS_REL)):
    log("DTS already present, skip download")
else:
    write(DTS_REL, fetch(DTS_PINNED))

# ------------------------------------------------------- 2. ipq807x.mk 条目
MK_REL = "target/linux/qualcommax/image/ipq807x.mk"
mk = read(MK_REL)
if "tcl_linkhub-hh500v" not in mk:
    block = (
        "\n"
        "define Device/tcl_linkhub-hh500v\n"
        f"{TABBED}$(call Device/FitImage)\n"
        f"{TABBED}$(call Device/UbiFit)\n"
        f"{TABBED}DEVICE_VENDOR := TCL\n"
        f"{TABBED}DEVICE_MODEL := LINKHUB HH500V\n"
        f"{TABBED}BLOCKSIZE := 128k\n"
        f"{TABBED}PAGESIZE := 2048\n"
        f"{TABBED}DEVICE_DTS_CONFIG := config@hk09\n"
        f"{TABBED}SOC := ipq8072\n"
        f"{TABBED}IMAGES += factory.bin\n"
        f"{TABBED}IMAGE/factory.bin := append-ubi | qsdk-ipq-factory-nand\n"
        f"{TABBED}DEVICE_PACKAGES := ipq-wifi-tcl_linkhub-hh500v kmod-mhi-pci-generic \\\n"
        f"{TABBED}{TABBED}kmod-mhi-wwan-ctrl kmod-mhi-wwan-mbim\n"
        "endef\n"
        "TARGET_DEVICES += tcl_linkhub-hh500v\n"
    )
    write(MK_REL, mk + block)
    log("ipq807x.mk: device added")
else:
    log("ipq807x.mk: already present")

# -------------------------------------------------- 3. base-files 的 case 块
LEDS = (
    "tcl,linkhub-hh500v)\n"
    f"{TABBED}ucidef_set_led_wlan \"wlan5g\" \"WiFi 5GHz\" \"blue:wlan\" \"phy0radio\"\n"
    "\t;;\n"
)
p = "target/linux/qualcommax/ipq807x/base-files/etc/board.d/01_leds"
t = read(p)
if "tcl,linkhub-hh500v" not in t:
    write(p, insert_before_last(t, "esac", LEDS))
    log("01_leds: added")

NET_IF = (
    "tcl,linkhub-hh500v)\n"
    f"{TABBED}ucidef_set_interfaces_lan_wan \"lan\" \"wan\"\n"
    "\t;;\n"
)
NET_MAC = (
    "tcl,linkhub-hh500v)\n"
    f"{TABBED}wan_mac=$(mtd_get_mac_binary \"0:ART\" 0x5e290)\n"
    f"{TABBED}lan_mac=$(mtd_get_mac_binary \"0:ART\" 0x5e296)\n"
    "\t;;\n"
)
p = "target/linux/qualcommax/ipq807x/base-files/etc/board.d/02_network"
t = read(p)
if "tcl,linkhub-hh500v" not in t:
    head, sep, tail = t.partition("ipq807x_setup_macs()")
    head = insert_before_last(head, "esac", NET_IF)
    tail = insert_before_last(tail, "esac", NET_MAC)
    write(p, head + sep + tail)
    log("02_network: added (if + mac)")

# 注意：11-ath11k-caldata 的结构是
#   case "$FIRMWARE" in
#   "ath11k/IPQ8074/hw2.0/cal-ahb-c000000.wifi.bin")
#           case "$board" in
#           ...
#           esac
# 所以必须插到**内层** case "$board" 里；插到文件最后一个 esac 之前会落到
# 外层 case 的兜底分支之后，永远不会被执行（这是 v1 固件 WiFi 起不来的原因）。
CAL = (
    "tcl,linkhub-hh500v)\n"
    f"{TABBED}caldata_extract \"0:ART\" 0x1000 0x20000\n"
    "\t;;\n"
)
p = "target/linux/qualcommax/ipq807x/base-files/etc/hotplug.d/firmware/11-ath11k-caldata"
t = read(p)
if "tcl,linkhub-hh500v" not in t:
    fw_anchor = '"ath11k/IPQ8074/hw2.0/cal-ahb-c000000.wifi.bin")'
    board_anchor = 'case "$board" in'
    i = t.find(fw_anchor)
    j = t.find(board_anchor, i) if i >= 0 else -1
    if j >= 0:
        k = j + len(board_anchor)
        t = t[:k] + "\n" + CAL + t[k:]
        write(p, t)
        log("11-ath11k-caldata: added (inner case)")
    else:
        log("11-ath11k-caldata: anchors not found (skipped)")

# --------------------------------------------- 4. uboot-envtools：APPSBLENV
# 24.10 的路径是 package/boot/uboot-envtools/...，main 改名成 uboot-tools/...，两者都试
UBOOT_CANDIDATES = [
    "package/boot/uboot-envtools/files/qualcommax_ipq807x",
    "package/boot/uboot-tools/uboot-envtools/files/qualcommax_ipq807x",
]
p = next((x for x in UBOOT_CANDIDATES if os.path.exists(os.path.join(ROOT, x))), None)
if p is None:
    log("uboot-envtools: file not found (skip)")
else:
    t = read(p)
if "tcl,linkhub-hh500v" not in t:
    helper = (
        "ubootenv_mtdinfo () {\n"
        f"{TABBED}UBOOTENV_PART=$(cat /proc/mtd | grep -i APPSBLENV)\n"
        f"{TABBED}mtd_dev=$(echo $UBOOTENV_PART | awk '{{print $1}}' | sed 's/:$//')\n"
        f"{TABBED}mtd_size=$(echo $UBOOTENV_PART | awk '{{print \"0x\"$2}}')\n"
        f"{TABBED}mtd_erase=$(echo $UBOOTENV_PART | awk '{{print \"0x\"$3}}')\n"
        f"{TABBED}nor_flash=$(find /sys/bus/spi/devices/*/mtd -name $mtd_dev)\n"
        "\n"
        f"{TABBED}if [ -n \"$nor_flash\" ]; then\n"
        f"{TABBED}{TABBED}ubootenv_size=$mtd_size\n"
        f"{TABBED}else\n"
        f"{TABBED}{TABBED}ubootenv_size=0x40000\n"
        f"{TABBED}fi\n"
        "\n"
        f"{TABBED}sectors=$(( $ubootenv_size / $mtd_erase ))\n"
        f"{TABBED}sectors=$(printf \"0x%x\" $sectors )\n"
        f"{TABBED}echo /dev/$mtd_dev 0x0 $ubootenv_size $mtd_erase $sectors\n"
        "}\n"
        "\n"
    )
    anchor = "board=$(board_name)\n"
    if anchor in t:
        t = t.replace(anchor, anchor + "\n" + helper, 1)
    else:
        t = t.replace('case "$board" in', helper + 'case "$board" in', 1)
    t = insert_before_last(
        t, "esac",
        "tcl,linkhub-hh500v)\n"
        f"{TABBED}ubootenv_add_uci_config $(ubootenv_mtdinfo)\n"
        "\t;;\n",
    )
    write(p, t)
    log("uboot-envtools: added")

# ---------------------------------------------------------------- 5. ipq-wifi
WIFI = "package/firmware/ipq-wifi/Makefile"
w = read(WIFI)
if "tcl_linkhub-hh500v" not in w:
    w = w.replace(
        f"{TABBED}spectrum_sax1v1k \\\n",
        f"{TABBED}spectrum_sax1v1k \\\n{TABBED}tcl_linkhub-hh500v \\\n",
        1,
    )
    w = w.replace(
        "$(eval $(call generate-ipq-wifi-package,spectrum_sax1v1k,Spectrum SAX1V1K))\n",
        "$(eval $(call generate-ipq-wifi-package,spectrum_sax1v1k,Spectrum SAX1V1K))\n"
        "$(eval $(call generate-ipq-wifi-package,tcl_linkhub-hh500v,TCL LINKHUB HH500V))\n",
        1,
    )
    # 版本升级到包含 HH500V BDF 的提交（从上游补丁读取，避免硬编码）
    try:
        patch = fetch(GH + "12fd85eb79.patch")
        m_date = re.search(r"\+PKG_SOURCE_DATE:=(\S+)", patch)
        m_ver = re.search(r"\+PKG_SOURCE_VERSION:=(\S+)", patch)
        if m_date and m_ver:
            w = re.sub(r"PKG_SOURCE_DATE:=.*", f"PKG_SOURCE_DATE:={m_date.group(1)}", w)
            w = re.sub(r"PKG_SOURCE_VERSION:=.*", f"PKG_SOURCE_VERSION:={m_ver.group(1)}", w)
            w = re.sub(r"PKG_MIRROR_HASH:=.*\n", "\n", w)
            log(f"ipq-wifi: bumped to {m_date.group(1)} / {m_ver.group(1)[:12]}")
        else:
            log("ipq-wifi: WARNING 未能解析上游版本，BDF 可能缺失")
    except Exception as exc:  # noqa: BLE001
        log(f"ipq-wifi: WARNING 版本升级失败 {exc}")
    write(WIFI, w)
    log("ipq-wifi: board added")
else:
    log("ipq-wifi: already present")

# --------------------------------------------------- 6. 内核 config-6.6 补符号
CFG = "target/linux/qualcommax/config-6.6"
if os.path.exists(os.path.join(ROOT, CFG)):
    c = read(CFG)
    add = []
    if "CONFIG_PINCTRL_AW9523" not in c:
        add.append("CONFIG_PINCTRL_AW9523=y\n")
    if "CONFIG_POWER_RESET_GPIO" not in c:
        add.append("CONFIG_POWER_RESET_GPIO=y\n")
    if add:
        write(CFG, c + "".join(add))
        log(f"config-6.6: added {len(add)} symbol(s)")
else:
    log(f"config-6.6: not found (skip)")

log("done")
