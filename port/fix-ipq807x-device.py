#!/usr/bin/env python3
"""把 HH500V 的 device 块放到 ipq807x.mk 里正确的位置。

背景
----
24.10.6 的树里没有这块板子，构建时用 `patch -p1 --fuzz=3` 兜底把
openwrt commit ac8b5526bc 打上去。但那个 hunk 的上下文在 24.10 的
ipq807x.mk 里正好模糊匹配到了 `define Device/tplink_eap660hd-v1` 里面，
结果变成：

    define Device/tplink_eap660hd-v1
    ...
    TPLINK_SUPPORT_STRING := ...
    define Device/tcl_linkhub-hh500v      <- 被塞进来了
    ...
    endef
    TARGET_DEVICES += tcl_linkhub-hh500v
    endef                                 <- 这是 eap660hd 真正的 endef
    TARGET_DEVICES += tplink_eap660hd-v1

`define` 里面套 `define`，Make 会把里面整段当成外层宏的正文 ——
`TARGET_DEVICES += tcl_linkhub-hh500v` 因此**从来没有被执行**，
这块板子压根没注册进构建系统。表现就是：
  - make 一路绿（工具链/内核/包都编了，image 阶段跑了）
  - bin/targets 里一台设备都没有 → Collect 的 out/ 是空的
  - .config 里 CONFIG_TARGET_qualcommax_ipq807x_DEVICE_tcl_linkhub-hh500v 被丢掉

这个脚本做两件事：
  1. 把散落在任何位置的 hh500v 块（define ... endef）和
     `TARGET_DEVICES += tcl_linkhub-hh500v` 行全部删掉；
  2. 在文件末尾追加一份正确的块。
因为删除的是"我们插进去的那几行"，eap660hd 的定义会原样恢复。

用法
----
    python3 port/fix-ipq807x-device.py target/linux/qualcommax/image/ipq807x.mk

退出码
------
    0 = 文件已修好（含"本来就没问题、只补了末尾块"）
    2 = 目标文件不存在
    3 = 修完自检失败（define/endef 不平衡、出现重复）
"""

import re
import sys

NAME = "tcl_linkhub-hh500v"

BLOCK = """define Device/%(n)s
\t$(call Device/FitImage)
\t$(call Device/UbiFit)
\tDEVICE_VENDOR := TCL
\tDEVICE_MODEL := LINKHUB HH500V
\tBLOCKSIZE := 128k
\tPAGESIZE := 2048
\tDEVICE_DTS_CONFIG := config@hk09
\tSOC := ipq8072
\tIMAGES += factory.bin
\tIMAGE/factory.bin := append-ubi | qsdk-ipq-factory-nand
\tDEVICE_PACKAGES := ipq-wifi-%(n)s kmod-mhi-pci-generic \\
\t\tkmod-mhi-wwan-ctrl kmod-mhi-wwan-mbim
endef
TARGET_DEVICES += %(n)s
""" % {"n": NAME}

RE_DEFINE = re.compile(r"^define\s+Device/" + re.escape(NAME) + r"\s*$")
RE_TARGET = re.compile(r"^TARGET_DEVICES\s*\+=\s*" + re.escape(NAME) + r"\s*$")
RE_ENDEF = re.compile(r"^endef\s*$")
RE_ANY_DEFINE = re.compile(r"^define\s+Device/")
RE_ANY_ENDEF = re.compile(r"^endef\s*$")


def strip_bad_blocks(lines):
    """删掉任何位置的 hh500v define 块 + 它的 TARGET_DEVICES 行。"""
    out = []
    i = 0
    removed = 0
    while i < len(lines):
        if RE_DEFINE.match(lines[i]):
            j = i + 1
            while j < len(lines) and not RE_ENDEF.match(lines[j]):
                j += 1
            removed += 1
            i = j + 1                      # 连同 endef 一起跳掉
            continue
        if RE_TARGET.match(lines[i]):
            i += 1                         # 单独出现的 TARGET_DEVICES 行也删
            continue
        out.append(lines[i])
        i += 1
    return out, removed


def main(argv):
    if len(argv) != 2:
        print("usage: fix-ipq807x-device.py <path/to/ipq807x.mk>", file=sys.stderr)
        return 2
    path = argv[1]
    try:
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
    except FileNotFoundError:
        print("!! %s not found" % path, file=sys.stderr)
        return 2

    lines, removed = strip_bad_blocks(text.split("\n"))

    # 去掉末尾多余空行，再追加标准块
    while lines and lines[-1].strip() == "":
        lines.pop()
    lines.append("")                       # define 之前空一行
    body = BLOCK.rstrip("\n").split("\n")
    lines.extend(body)
    lines.append("")

    new_text = "\n".join(lines)

    # ---- 自检 ----------------------------------------------------------
    check = new_text.split("\n")
    n_define = sum(1 for l in check if RE_ANY_DEFINE.match(l))
    n_endef = sum(1 for l in check if RE_ANY_ENDEF.match(l))
    n_ours = sum(1 for l in check if RE_DEFINE.match(l))
    n_tgt = sum(1 for l in check if RE_TARGET.match(l))

    ok = True
    if n_define != n_endef:
        print("!! define(%d) != endef(%d) —— 文件结构还是坏的" % (n_define, n_endef), file=sys.stderr)
        ok = False
    if n_ours != 1:
        print("!! Device/%s 出现了 %d 次（应该 1 次）" % (NAME, n_ours), file=sys.stderr)
        ok = False
    if n_tgt != 1:
        print("!! TARGET_DEVICES += %s 出现了 %d 次（应该 1 次）" % (NAME, n_tgt), file=sys.stderr)
        ok = False

    # 我们这块必须是文件里最后一个 device（后面不再跟别的 device）
    our_line = next(i for i, l in enumerate(check) if RE_DEFINE.match(l))
    if any(RE_ANY_DEFINE.match(l) for l in check[our_line + 1:]):
        print("!! 我们的块后面还有别的 define，位置不对", file=sys.stderr)
        ok = False

    if not ok:
        # 自检失败就别写坏文件
        return 3

    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(new_text)

    print("ipq807x.mk: removed %d misplaced block(s); appended canonical Device/%s at EOF"
          % (removed, NAME))
    print("check: define=%d endef=%d our_block=%d target_line=%d  -> OK"
          % (n_define, n_endef, n_ours, n_tgt))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
