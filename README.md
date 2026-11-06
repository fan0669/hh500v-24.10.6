# HH500V 固件构建工程（ImmortalWrt + 全插件）

> 目标：给 **TCL LINKHUB HH500V**（IPQ8072A / 1GB / 256MB NAND / GM800）编一版
> 带 **openclash / passwall / daed / zerotier / UU加速器 / tailscale / mihomo / qmodem** 且**都有图形界面**的 ImmortalWrt。
>
> ⚠️ 本仓库只提供**构建流水线**，不包含预编译固件（固件由 GitHub Actions 生成）。

## 〇、版本可选（workflow 里两个下拉）

| 版本 | 版本号 | 内核 | 包管理器 | HH500V 支持 | 插件 |
|---|---|---|---|---|---|
| **24.10.6** ⭐ | `24.10.6`（commit `cf234f8de6d5`） | **6.6.133** | opkg | ❌ 需 backport | ✅ **全家桶** |
| v24.10.5 | `24.10.5`（tag） | 6.6.122 | opkg | ❌ 需 backport | ✅ 全家桶 |
| 24.10-SNAPSHOT | `24.10-SNAPSHOT` | 6.6.x | opkg | ❌ 需 backport | ✅ 全家桶 |
| master | `SNAPSHOT` | 6.18.x | apk | ✅ 原生 | ⚠️ 仅官方 + 部分第三方 |

> 注意：**24.10.6 没有 git tag**（只有 `v24.10.5`），所以用官方构建的 revision
> `cf234f8de6d5` 固定（对应 `r33869`）。这是"最新稳定 + 可复现"的折中点。

插件集（`plugins` 下拉）：

| 值 | 含义 |
|---|---|
| `full` | 官方 + 第三方（openclash/passwall/daed/mihomo/UU/zerotier/tailscale，全带 GUI） |
| `basic` | 官方 feed（openclash/passwall/daed/sing-box/zerotier/tailscale）+ 模组管理 |

> **QModem 已加到所有组合**，并且是"功能最全且不冲突"的那一套（见下）。
> 它是源码编译，**apk/opkg 两种版本都能编**；GM800 在它支持列表里（`gm800 | qualcomm | ✔ pcie(mbim)`）。

### QModem 包含哪些包（功能最全组合）

| 包 | 提供的功能 |
|---|---|
| `qmodem` | 核心：模组发现、拨号、AT、短信、RAT/锁频/锁小区、SIM 切换 |
| `luci-app-qmodem-next` | **主 UI**（总览/移动数据/射频与小区/短信/模组与SIM/高级/AT 控制台） |
| `luci-app-qmodem-monitor` | 状态监控页 |
| `luci-app-qmodem-uplink` | 上行管理（多上行切换） |
| `luci-app-qmodem-voip` + `qmodem-voip` | **VoIP/语音**（这是 project-cpe 里"电话"功能的替代） |
| `luci-app-qmodem-ttlfw4` | TTL 伪装（防火墙 4 版） |
| `qmodem-seal` | 签名/校验工具（libsodium） |
| `qfirehose` | 高通模组刷机工具（救砖用） |
| `libqmodem-sms` | 短信库（next UI 依赖） |

### 更正："没有 apk" 的真正含义

之前说 UU/mihomo/daed-GUI "没有 apk" —— **不准确**。准确说法是：

| 说法 | 对错 |
|---|---|
| "官方 feed 里没有这些包的**预编译包**" | ✅ 对 |
| "所以 master 版装不了" | ❌ 错——**加源码 feed 后，master 构建会把它们编成 .apk** |

kenzok8/small-package 里实际存在（我核过）：

```
daed / daed-next / luci-app-daed / luci-app-daed-next
mihomo / nikki / luci-app-nikki
luci-app-UUGameAcc        ← UU 加速器
luci-app-tailscale / tailscale
luci-app-homeproxy / luci-app-passwall2
```

所以：**master + 加 small feed → 这些都会变成 .apk**，随固件一起产出。
唯一风险：这些源码按 24.10 维护，在 master（新内核/luci）上个别包可能编不过 → 把编不过的从 config 里去掉重试。

### 故意**没装**的三个（会拉旧 UI 或不适配）

| 包 | 为什么不装 |
|---|---|
| `luci-app-qmodem`（旧 UI） | README 明确：**新旧两套 UI 不能共存** |
| `luci-app-qmodem-sms` | 依赖 `luci-app-qmodem`（旧 UI）→ 会把旧 UI 拉进来 |
| `luci-app-qmodem-mwan` | 同上（依赖旧 UI + mwan3） |
| `luci-app-qmodem-ttl` | 同上（依赖旧 UI） |
| `luci-app-qmodem-hc` | 只适配 Mediatek `hc-g80` 设备，与 HH500V 无关 |

> 换句话说：**next UI 已经包含短信功能**，所以不需要 `-sms`；`-mwan`/`-ttl` 的等价能力，next UI 里也有对应页面。

**推荐组合**：
- **首选** → `version=24.10.6` + `plugins=full`（最新稳定 + 全插件 + 可复现）
- 求稳 → `version=master` + `plugins=basic`（原生支持、修复最全，但插件少）
- 想退一步 → `version=v24.10.5` + `plugins=full`（有 tag，但内核旧 11 个版本）

> ⚠️ 注意：HH500V 2025-06 进 master，但 **WiFi BDF 到 2026-01、sysupgrade 到 2026-01、MHI 5G 修复到 2026-04** 才补齐。
> 24.10（内核 6.6）**拿不到 MHI 的 6.6 版修复** → 5G 可能不稳；master 才是"修完"的版本。

## 一、为什么选 24.10 分支（而不是 snapshot）

| 项 | 24.10 分支 | snapshot |
|---|---|---|
| 包管理器 | **opkg** | apk |
| 第三方插件（UU/mihomo/daed-GUI/LuCI 版 zerotier/tailscale） | ✅ **生态齐全** | ❌ 大量第三方 ipk 不兼容 |
| HH500V 设备支持 | ❌ 需要 backport | ✅ 原生有 |
| 内核 | 6.6 | 6.18 |

**结论**：要"插件全 + 都有 GUI"，走 **24.10 + backport HH500V** 是唯一现实路径。

## 二、官方 feed 已确认有 / 没有（我实测 24.10.5 feed）

| 插件 | 24.10 官方 feed | 需要的来源 |
|---|---|---|
| openclash | ✅ `luci-app-openclash_0.47.156_all.ipk` | 官方 |
| passwall | ✅ `luci-app-passwall_25.12.16-r1_all.ipk` | 官方 |
| daed（核心） | ✅ `daed-geoip_1.24.0` | 官方 |
| **daed（GUI）** | ❌ | **第三方**（kenzok8） |
| zerotier（核心） | ✅ `zerotier_1.14.1` | 官方 |
| **zerotier（GUI）** | ❌ | **第三方** `luci-app-zerotier` |
| tailscale（核心） | ✅ `tailscale_1.80.3` | 官方 |
| **tailscale（GUI）** | ❌ | **第三方** `luci-app-tailscale` |
| sing-box | ✅ `sing-box-tiny` | 官方 |
| **mihomo / mihomo GUI** | ❌ | **第三方** `luci-app-mihomo` |
| **UU 加速器** | ❌ | **第三方** `luci-app-uugamebooster` |
| **QModem** | ❌ | **第三方** `FUjr/QModem` |
| modemband | ✅ | 官方 |

## 三、目录结构

```
.
├─ .github/workflows/build.yml     # CI：拉源码 → 打补丁 → 编 → 出固件
├─ feeds.conf.default              # 官方 + 第三方 feed
├─ configs/hh500v-24.10.config     # 插件/功能选择
├─ patches/
│   └─ README.md                   # HH500V 设备支持的 cherry-pick 清单与风险
└─ README.md
```

## 四、怎么用

```bash
# 1) 新建 GitHub 仓库，把本目录推上去
git init && git add -A && git commit -m "hh500v build"
git remote add origin git@github.com:<你>/hh500v-firmware.git
git push -u origin main

# 2) 打开 Actions → Build HH500V ImmortalWrt → Run workflow
#    约 1~2 小时后，在 Artifacts 下载固件
```

产物：
- `immortalwrt-qualcommax-ipq807x-tcl_linkhub-hh500v-squashfs-factory.bin`（首刷）
- `...-sysupgrade.bin`（升级）
- `...-initramfs-uImage.itb`（先点亮用）

## 五、刷机

```text
1. 原厂固件：root ssh（端口 42000，无密码）
2. scp factory.bin 到 /tmp
3. sysupgrade -n /tmp/factory.bin
4. 有串口就先跑 initramfs 验证
```

## 六、⚠️ 必须知道的三个坑

### 1) 代理插件会互相打架
`openclash / passwall / daed / mihomo / UU` **都改 DNS、防火墙、TPROXY**：
- **同时只能启用一个**，其余必须停用；
- 建议默认只开一个（比如 passwall），其他按需切换。

### 2) UU 加速器需要账号/授权
`luci-app-uugamebooster` 只是客户端，**加速需要 UU 账号/会员**；不登录时界面在但不工作。

### 3) 第三方 feed 可能编不过
- kenzok8 的包按 23.05/24.10 维护，**24.10 上大概率能编**，但不是 100%；
- 失败时看日志里哪个包报错，把它从 `configs/hh500v-24.10.config` 里去掉再试。

## 七、体积与内存

| 项 | 情况 |
|---|---|
| NAND | 256MB（够装全部，剩 ~100MB） |
| RAM | 1GB（同时开 2 个代理会紧张，建议只开 1 个） |
| 启动时间 | 插件多，开机约 40~60 秒 |
