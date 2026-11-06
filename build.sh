#!/usr/bin/env bash
# HH500V 一键编译脚本（在 Ubuntu/WSL2 里跑）
#
# 用法：
#   ./build.sh 24.10.6    full    # 推荐：24.10.6（r33869，内核 6.6.133）+ 全插件
#   ./build.sh v24.10.5   full    # 固定 tag（6.6.122）
#   ./build.sh 24.10-SNAPSHOT basic
#   ./build.sh master     full    # 6.18 内核，原生支持 HH500V
#
# 产物：openwrt/bin/targets/qualcommax/ipq807x/*hh500v*

set -euo pipefail

VERSION="${1:-24.10.6}"
PLUGINS="${2:-full}"
JOBS="$(nproc)"

echo "==> 版本: $VERSION   插件集: $PLUGINS   并行: $JOBS"

# ---------- 1. 依赖 ----------
need_pkgs="build-essential ccache ecj fastjar file g++ gawk gettext git libelf-dev \
libncurses5-dev libssl-dev python3 python3-distutils rsync swig unzip zlib1g-dev \
zip binutils bzip2 perl diffstat patch device-tree-compiler"
if command -v apt-get >/dev/null; then
  sudo apt-get update -qq
  sudo apt-get install -y $need_pkgs
fi

# ---------- 2. 拉源码 ----------
rm -rf openwrt
case "$VERSION" in
  master)          git clone --depth 50 -b master       https://github.com/immortalwrt/immortalwrt openwrt ;;
  24.10-SNAPSHOT)  git clone --depth 50 -b openwrt-24.10 https://github.com/immortalwrt/immortalwrt openwrt ;;
  24.10.6)         git clone --filter=blob:none --no-checkout https://github.com/immortalwrt/immortalwrt openwrt || \
                   git clone --no-checkout https://github.com/immortalwrt/immortalwrt openwrt; \
                   git -C openwrt checkout -q cf234f8de6d5 ;;
  *)               git clone --depth 1  -b "$VERSION"   https://github.com/immortalwrt/immortalwrt openwrt \
                     || git clone --depth 50 -b openwrt-24.10 https://github.com/immortalwrt/immortalwrt openwrt ;;
esac
cd openwrt

# ---------- 3. 非 master 需要 backport HH500V 设备 ----------
if [ "$VERSION" != "master" ]; then
  git config user.email ci@local; git config user.name ci
  for sha in ac8b5526bc 12fd85eb79 30378ffea4; do
    echo "==> backport $sha"
    curl -sSL "https://github.com/openwrt/openwrt/commit/$sha.patch" -o "/tmp/$sha.patch"
    if ! git am -3 "/tmp/$sha.patch"; then
      echo "!! git am failed, fallback to git apply -3"
      git am --abort 2>/dev/null || true
      git apply -3 "/tmp/$sha.patch"
      git add -A && git commit -m "backport $sha" || true
    fi
  done
  grep -q tcl_linkhub-hh500v target/linux/qualcommax/image/ipq807x.mk && echo "==> 设备已加入"
  echo "!!! 注意：24.10 是内核 6.6，缺 MHI IRQ 修复，5G 可能不稳"
fi

# ---------- 4. feeds ----------
mkdir -p package/custom
cp -r ../custom-packages/* package/custom/ 2>/dev/null || true
if [ "$VERSION" = "master" ]; then BR=master; else BR=openwrt-24.10; fi
cat > feeds.conf.default <<EOF
src-git packages https://github.com/immortalwrt/packages.git;$BR
src-git luci https://github.com/immortalwrt/luci.git;$BR
src-git routing https://github.com/immortalwrt/routing.git;$BR
src-git telephony https://github.com/immortalwrt/telephony.git;$BR
src-git video https://github.com/immortalwrt/video.git;$BR
src-git qmodem https://github.com/FUjr/QModem.git;main
EOF
if [ "$VERSION" != "master" ]; then
cat >> feeds.conf.default <<EOF
src-git small https://github.com/kenzok8/small-package.git;main
src-git kenzok8 https://github.com/kenzok8/openwrt-packages.git;master
EOF
fi
./scripts/feeds update -a
./scripts/feeds install -a || true

# ---------- 5. 配置 ----------
if [ "$VERSION" = "master" ]; then
  cp ../configs/hh500v-master.config .config
elif [ "$PLUGINS" = "basic" ]; then
  cp ../configs/hh500v-24.10-basic.config .config
else
  cp ../configs/hh500v-24.10.config .config
fi
make defconfig

# ---------- 6. 编译 ----------
make download -j8 || make download -j1 V=s
make -j"$JOBS" || make -j1 V=s

echo ""
echo "==================== 完成 ===================="
ls -lh bin/targets/qualcommax/ipq807x/*hh500v* 2>/dev/null || echo "（没找到产物，检查上面日志）"
echo "刷机：ssh root@<原厂IP> -p 42000 → scp factory.bin /tmp → sysupgrade -n /tmp/factory.bin"
