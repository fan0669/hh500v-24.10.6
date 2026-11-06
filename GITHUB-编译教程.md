# 用 GitHub 编译 HH500V 固件（2026-11-06 最新版）

> 目标：**ImmortalWrt 24.10.6（git tag `v24.10.6`）+ 全插件 + 我们这一路修好的所有东西**，编出一份可直接刷的 `factory.bin`。
> 全程网页操作，不用装 Linux、不用会 git 命令。
> 本目录 = 上传包 `hh500v-v23-upload.zip` 解压后的内容（72 个文件）。

---

## 一、包里有什么（一眼看清）

| 路径 | 作用 |
|---|---|
| `.github/workflows/build.yml` | 编译流水线（**必须上传，Windows 默认隐藏 `.github`**） |
| `configs/hh500v-24.10.config` | 24.10 的 .config（含我们所有 CONFIG_PACKAGE_*） |
| `configs/hh500v-basic.config` / `hh500v-master.config` | 精简档 / master 档 |
| `patches/706-v7.2-...-mhi...patch` | 6.6 内核的 MHI IRQ 修复（5G 稳定性） |
| `kernel/pinctrl-aw9523.c` | 6.6 没有的 AW9523 LED 驱动（板上的 5G/4G/WiFi/POWER 灯） |
| `port/apply-port.py` | 自动把设备定义/caldata 补丁塞进 24.10 源码 |
| `custom-packages/` | 自写 LuCI 包（ZeroTier GUI 等） |
| `files/` | 板级 overlay：`/etc/config/*`、init 脚本、`usr/bin` 下的自制工具（atgw / mbimprox / 各种 .sh / `hh500v-ca.uc`）、LuCI 页面 |
| `proxy/*.c` | AT 网关 + MBIM 代理的源码（仅存档，编译不用） |
| `当前状态与下次继续.md` | **完整技术档案**（每个 bug 的证据与修法） |
| `救砖操作单.md` / `救砖卡片.md` | 变砖后的 WPS+TFTP 救砖流程（务必先看一遍） |

---

## 二、创建仓库并上传

1. 登录 GitHub → 右上角 **+ → New repository**
   - 名字随意（例：`hh500v-firmware`）
   - **Public**（Actions 免费不限分钟；私有也只有 2000 分钟/月）
   - 勾 **Add a README file** → **Create repository**
2. **Add file → Upload files**，把解压出来的**所有文件和文件夹**拖进去
   - ⚠️ `.github` 必须一起传（资源管理器：查看 → 显示 → 隐藏的项目）
   - ⚠️ `files/usr/bin/` 里的二进制（`hh500v-atgw`、`hh500v-mbimprox`，各约 1.2MB）也要传，否则固件里没有它们
3. 填提交信息 → **Commit changes**

> `.github` 传不上去时，用 **GitHub Desktop**（File → Add local repository → Publish repository），最稳。

---

## 三、启用 Actions 并开始编译

1. 仓库顶部 **Actions** → 若提示，点 **I understand my workflows, go ahead and enable them**
2. 左侧选 **Build HH500V ImmortalWrt** → 右侧 **Run workflow**
3. 两个下拉：

   | 输入 | 选什么 | 说明 |
   |---|---|---|
   | `firmware version` | **`24.10.6`** | 走 git tag `v24.10.6`（commit `5fa11a1b`，2026-04-27），`/etc/openwrt_release` 会显示 **24.10.6**，opkg 源指向 `releases/24.10.6` |
   | `plugin set` | **`full`** | openclash / passwall2 / qmodem / tailscale / zerotier / 我们的 LuCI 页 |

   其它可选项：`v24.10.5`（老 tag）、`24.10-SNAPSHOT`（24.10 分支头）、`master`（6.18 内核、apk、原生支持这块板子但**没有**我们的 AT 网关/短信/CA 页那一套）。
4. 点绿色 **Run workflow**

---

## 四、等多久、每一步在干什么

**一次约 3.5–4 小时**（Build 占 2.5~3 小时）。日志里对照：

| 步骤 | 内容 | 正常耗时 |
|---|---|---|
| Free disk / Install deps | 腾空间、装编译依赖 | 1~3 分 |
| Clone ImmortalWrt | `git clone --depth 1 -b v24.10.6` | 1~3 分 |
| Backport HH500V device | 打 3 个补丁：`ac8b5526bc`（设备）、`12fd85eb79`（ipq-wifi BDF）、`30378ffea4`（sysupgrade bootconfig） | 1 分 |
| Backport MHI IRQ fix to 6.6 | 706 补丁 | 10 秒 |
| Backport AW9523 pinctrl | 拷 `kernel/pinctrl-aw9523.c` + 注册 Kconfig/Makefile | 10 秒 |
| Install custom packages | 拷 `custom-packages/` + `files/` overlay + `chmod 755` | 10 秒 |
| Write feeds.conf | 官方 6 个 feed + `FUjr/QModem` + `kenzok8/small-package` | 5~15 分 |
| Configure | `make defconfig` | 1~2 分 |
| Download | 下载源码包 | 10~30 分 |
| **Build** | **编译** | **2.5~3 小时** |
| Collect / Upload | 收集 `*hh500v*` 并上传 artifact | 1~2 分 |

---

## 五、下载固件

编译成功（绿勾）后，在该次运行页面**最下方 Artifacts**：

- 名字：**`hh500v-24.10.6-full`**（点了会下 zip）
- 解压后三个文件：

| 文件 | 用途 |
|---|---|
| `...-squashfs-factory.bin` | **首刷/从原厂刷用这个** |
| `...-squashfs-sysupgrade.bin` | 已经是本固件，以后升级用 |
| `...-initramfs-uImage.itb` | 只跑内存、不写 flash，先验证用 |

---

## 六、刷机（务必先读 `救砖操作单.md`）

### 方式 1：原厂固件 SSH（最常用）

```bash
ssh -p 42000 root@192.168.1.1          # 原厂：端口 42000，无密码
scp -O -P 42000 ...-squashfs-factory.bin root@192.168.1.1:/tmp/factory.bin
ssh -p 42000 root@192.168.1.1 "sysupgrade -n /tmp/factory.bin"
```

### 方式 2：先跑 initramfs（不写 flash，最安全）

`WPS 键按住插电 15 秒` 进 U-Boot TFTP → 用 `flash/tftp_server.py` 喂 `norplusnand-ipq807x_64-single.img` → 起 initramfs 验证 5G/网口/短信，再刷 factory。

> 刷完第一次启动会跑 `uci-defaults`（建 QModem 段、校准 uci、装 3ginfo 补丁、改 LuCI 页），需要 2~3 分钟，别急着断电。

---

## 七、刷完自检（5 分钟）

| 检查 | 位置 / 命令 |
|---|---|
| 5G 上网 | 状态 → 概览拿到 IP；`ping 223.5.5.5` |
| 短信（不断网） | LuCI **模块 → HH500V 短信**（或 短信工具 / QModem → SIM卡短信） |
| 载波/小区 | LuCI **模块 → HH500V 载波**（服务小区、CA 就绪清单、切 ca/仅5G） |
| 信号详情 | **模块 → 3G/4G/5G 连接信息**（RSRP/RSRQ/SINR/PCI/ARFCN/TAC + SCC 行） |
| 模组概览 | **模块 → QModem**（连接状态应为 Yes，拨号模式 mbim） |
| 2.5G 口 | 它现在是 **LAN**（和三个 1G 口同一个网桥、同一个 192.168.1.0/24）；对端要 2.5G 网卡才协商到 2500 |
| WiFi | `iwinfo` 看 Tx-Power >20dBm；5GHz 是 HE160，开机要等 60 秒 DFS/CAC |
| 安全重启 | 用 `/usr/bin/hh500v-reset.sh`（panic + 硬件看门狗）；**普通 `reboot` 在这块板上会挂死，只能拔电** |

---

## 八、可能踩的坑（我这一路真踩过的）

| 现象 | 原因 | 处理 |
|---|---|---|
| Actions 里没有 workflow | `.github` 没传上去 | 用 GitHub Desktop 重传，确认网页能看到 `.github/workflows/build.yml` |
| `Backport ... failed` / `does not apply` | 基座变动导致补丁冲突 | 把日志最后 30 行发我；或改用 `24.10-SNAPSHOT` 档 |
| `MISS daed` / `MISS mihomo` | 第三方 feed 某天缺包 | 无害（`missing<=3` 放行）或从 config 里删掉该包 |
| `package/feeds/qmodem/qmodem_sipd failed to build` | QModem 上游偶发（sipd 依赖） | 重跑一次；或在 config 里去掉 `CONFIG_PACKAGE_qmodem_sipd` |
| 编译 6 小时超时 | 某包装死 | 看日志最后一个 `make[3] -C` 是谁；删掉它重跑 |
| artifact 是空的 | 编译失败但没报错 | 看 `Collect` 步骤日志 |
| 刷完没网、灯亮 | 首次 uci-defaults 还没跑完 / 模组冷启动没抢到 PCIe | 等 3 分钟；`hh500v-coldboot` 会自动用 `hh500v-reset.sh` 复位一次（约 100~330 秒回来） |
| 后台打不开 | 远程 `reboot` 挂死（本板已知硬件问题） | **拔电**重插；以后一律用 `hh500v-reset.sh` |

---

## 九、费用

| 项 | 说明 |
|---|---|
| 公开仓库 | Actions 免费、不限分钟 |
| 私有仓库 | 2000 分钟/月（一次 3.5~4h ≈ 220 分钟） |
| Artifacts | 保留 90 天，过期重编 |
| 并发 | 免费账号同时 1~2 个任务 |
