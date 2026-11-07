#!/usr/bin/env python3
"""给 ipq-wifi 加上 HH500V 的 BDF 包（不依赖上游补丁能否落下）。

背景
----
HH500V 的 DEVICE_PACKAGES 里有 `ipq-wifi-tcl_linkhub-hh500v`。这个包由
`package/firmware/ipq-wifi/Makefile` 里的

    ALLWIFIBOARDS := ...
    $(eval $(call generate-ipq-wifi-package,<board>,<显示名>))

循环生成。24.10.6 的这份 Makefile 里没有这一项，所以：
  - 要么镜像阶段报 "unknown package ipq-wifi-tcl_linkhub-hh500v"
  - 要么（更隐蔽）设备被跳过

构建脚本本来靠 `patch -p1 --fuzz=3` 落 openwrt commit `12fd85eb79`，但那个 hunk 的
上下文（PKG_RELEASE / PKG_SOURCE_* 那几行）在 24.10 里对不上，日志里是：

    patching file package/firmware/ipq-wifi/Makefile
    Hunk #1 FAILED at 6.
    1 out of 1 hunk FAILED -- saving rejects to .../ipq-wifi/Makefile.rej

于是这个包压根没被定义。这个脚本用确定性的字符串替换把三件事做完：

  1. ALLWIFIBOARDS 里加 `tcl_linkhub-hh500v`（插在 spectrum_sax1v1k 后面，保持字母序）
  2. 对应的 `$(eval $(call generate-ipq-wifi-package,...))` 行也补上
  3. 版本升到含这块 BDF 的 qca-wireless 提交（PKG_SOURCE_DATE / VERSION 改写，
     PKG_MIRROR_HASH 删掉让它重新算 —— 旧 hash 对应旧 tarball，留着必然校验失败）

用法
----
    python3 port/fix-ipq-wifi.py package/firmware/ipq-wifi/Makefile

退出码
------
    0 = 已就绪（含"本来就有"的情况）
    2 = 文件不存在 / 找不到 INSERT 锚点
    3 = 写完自检失败
"""

import re
import sys

BOARD = "tcl_linkhub-hh500v"
DISPLAY = "TCL LINKHUB HH500V"

# openwrt commit 12fd85eb79 "ipq-wifi: add board file for TCL LINKHUB HH500V"
PKG_DATE = "2026-01-24"
PKG_VERSION = "8e1fb2f59893dc4ce3203b7649e61b30fde2d5a8"

ANCHOR_BOARD = "\tspectrum_sax1v1k \\\n"
ANCHOR_EVAL = "$(eval $(call generate-ipq-wifi-package,spectrum_sax1v1k,Spectrum SAX1V1K))\n"


def main(argv):
    if len(argv) != 2:
        print("usage: fix-ipq-wifi.py <path/to/ipq-wifi/Makefile>", file=sys.stderr)
        return 2
    path = argv[1]
    try:
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
    except FileNotFoundError:
        print("!! %s not found" % path, file=sys.stderr)
        return 2

    changed = []

    # ---- 1. ALLWIFIBOARDS 里加名字 -------------------------------------
    if "\t%s \\\n" % BOARD not in text:
        if ANCHOR_BOARD not in text:
            print("!! 在 ALLWIFIBOARDS 里找不到锚点 %r" % ANCHOR_BOARD, file=sys.stderr)
            return 2
        text = text.replace(
            ANCHOR_BOARD,
            ANCHOR_BOARD + "\t%s \\\n" % BOARD,
            1,
        )
        changed.append("ALLWIFIBOARDS += %s" % BOARD)

    # ---- 2. generate-ipq-wifi-package 行 -------------------------------
    if "generate-ipq-wifi-package,%s," % BOARD not in text:
        if ANCHOR_EVAL not in text:
            print("!! 找不到锚点 %r" % ANCHOR_EVAL, file=sys.stderr)
            return 2
        text = text.replace(
            ANCHOR_EVAL,
            ANCHOR_EVAL + "$(eval $(call generate-ipq-wifi-package,%s,%s))\n" % (BOARD, DISPLAY),
            1,
        )
        changed.append("generate-ipq-wifi-package,%s" % BOARD)

    # ---- 3. 版本升到带这块 BDF 的提交 ----------------------------------
    m_date = re.search(r"^PKG_SOURCE_DATE:=(.*)$", text, re.M)
    m_ver = re.search(r"^PKG_SOURCE_VERSION:=(.*)$", text, re.M)
    if not (m_date and m_ver):
        print("!! 这个 Makefile 里没有 PKG_SOURCE_DATE / PKG_SOURCE_VERSION", file=sys.stderr)
        return 2
    if m_ver.group(1).strip() != PKG_VERSION:
        text = re.sub(r"^PKG_SOURCE_VERSION:=.*$", "PKG_SOURCE_VERSION:=%s" % PKG_VERSION,
                      text, count=1, flags=re.M)
        changed.append("PKG_SOURCE_VERSION -> %s" % PKG_VERSION[:12])
    if m_date.group(1).strip() != PKG_DATE:
        text = re.sub(r"^PKG_SOURCE_DATE:=.*$", "PKG_SOURCE_DATE:=%s" % PKG_DATE,
                      text, count=1, flags=re.M)
        changed.append("PKG_SOURCE_DATE -> %s" % PKG_DATE)
    # 旧 hash 对应旧 tarball，必须删掉（OpenWrt 会自己算/跳过校验）
    if re.search(r"^PKG_MIRROR_HASH:=.*$\n?", text, re.M):
        text = re.sub(r"^PKG_MIRROR_HASH:=.*$\n?", "", text, count=1, flags=re.M)
        changed.append("PKG_MIRROR_HASH removed")

    # ---- 自检 ----------------------------------------------------------
    n_board = text.count("\t%s \\\n" % BOARD)
    n_eval = text.count("generate-ipq-wifi-package,%s," % BOARD)
    ok = True
    if n_board != 1:
        print("!! ALLWIFIBOARDS 里 %s 出现 %d 次（应为 1）" % (BOARD, n_board), file=sys.stderr)
        ok = False
    if n_eval != 1:
        print("!! generate-ipq-wifi-package 里 %s 出现 %d 次（应为 1）" % (BOARD, n_eval), file=sys.stderr)
        ok = False
    if PKG_VERSION not in text:
        print("!! PKG_SOURCE_VERSION 没写进去", file=sys.stderr)
        ok = False
    if re.search(r"^PKG_MIRROR_HASH:=", text, re.M):
        print("!! PKG_MIRROR_HASH 还在（旧 hash 会导致下载校验失败）", file=sys.stderr)
        ok = False
    if not ok:
        return 3

    if changed:
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        print("ipq-wifi: " + "; ".join(changed))
    else:
        print("ipq-wifi: already prepared (%s @ %s)" % (BOARD, PKG_VERSION[:12]))
    print("check: board=%d eval=%d version=%s  -> OK" % (n_board, n_eval, PKG_VERSION[:12]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
