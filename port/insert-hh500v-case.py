#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 HH500V 的 A/B 升级分支插进 platform.sh（幂等）。

上游 ac8b5526bc 在 platform.sh 里加了这 4 行：

    tcl,linkhub-hh500v)
        tcl_upgrade_prepare
        nand_do_upgrade "$1"
        ;;

但这一步是 `git am` 打进源码树才有的；如果基座（比如 24.10.6）里没有这
块板子、或者 cherry-pick 只成功了一部分，platform.sh 就会退回到通用
`nand_do_upgrade`，sysupgrade 直接写当前分区 —— 没有 A/B 回退。

用法：
    python3 insert-hh500v-case.py <platform.sh>

退出码：0 已插入/已存在；2 找不到 platform_do_upgrade()；3 找不到 case。
"""
import io
import re
import sys

SNIPPET = (
    "\ttcl,linkhub-hh500v)\n"
    "\t\ttcl_upgrade_prepare\n"
    "\t\tnand_do_upgrade \"$1\"\n"
    "\t\t;;\n"
)


def main(path):
    with io.open(path, "r", encoding="utf-8", newline="") as fh:
        lines = fh.readlines()

    if any("linkhub-hh500v" in line for line in lines):
        print("platform.sh: hh500v 分支已存在，跳过")
        return 0

    start = None
    for idx, line in enumerate(lines):
        if line.startswith("platform_do_upgrade()"):
            start = idx
            break
    if start is None:
        print("!! 找不到 platform_do_upgrade()", file=sys.stderr)
        return 2

    for idx in range(start, len(lines)):
        if re.match(r"^\s*case\s", lines[idx]):
            lines.insert(idx + 1, SNIPPET)
            break
    else:
        print("!! platform_do_upgrade() 里找不到 case", file=sys.stderr)
        return 3

    with io.open(path, "w", encoding="utf-8", newline="") as fh:
        fh.writelines(lines)
    print("platform.sh: 已在第 %d 行插入 hh500v 分支" % (idx + 2))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("用法: insert-hh500v-case.py <platform.sh>", file=sys.stderr)
        sys.exit(1)
    sys.exit(main(sys.argv[1]))
