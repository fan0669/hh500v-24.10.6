# 让 HH500V 走 wukongdaily 那个工作流

## 结论：可以，但只能用 **snapshot** 的 ImageBuilder

那个工作流的 ipq807x 分支写死了：

```yaml
case_tag="qualcommax-ipq807x-openwrt-24.10.6"
```

而 **24.10.6 的 ImageBuilder 里没有 `tcl_linkhub-hh500v` 这个 profile**（我验证过 24.10.5/24.10-SNAPSHOT 的 profiles.json 都不含它）。
所以：**不能把机型直接加进 24.10.6 那套**，必须走 **snapshot 的 ImageBuilder**（里面有 HH500V）。

## 做法 A（推荐）：加一个新 workflow，不动他的文件

把本目录的 `build-hh500v-imagebuilder.yml` 复制到你 fork 的仓库：

```bash
mkdir -p .github/workflows
cp build-hh500v-imagebuilder.yml .github/workflows/
git add -A && git commit -m "add hh500v imagebuilder build" && git push
```

然后 Actions → **Build HH500V via ImageBuilder** → Run workflow → 10~20 分钟 → 下载 artifact。

它做的事和那个工作流**完全同构**：

```
下载 ImageBuilder tar（snapshot）
  → make image PROFILE=tcl_linkhub-hh500v PACKAGES="..."
  → 上传 factory.bin / sysupgrade.bin
```

## 做法 B：给原工作流打 3 处补丁

### 1) 新建 `model/ipq807x-snapshot.txt`（内容一行）

```
tcl_linkhub-hh500v
```

### 2) 在 `build-wireless-router.yml` 里，`case "$profile" in` **之前**插入：

```bash
          ipq807x_snapshot_models=$(cat model/ipq807x-snapshot.txt | tr '\n' '|')
          ipq807x_snapshot_models=${ipq807x_snapshot_models%|}
          if [[ "$profile" =~ ^($ipq807x_snapshot_models)$ ]]; then
            case_tag="qualcommax-ipq807x-snapshot"
          fi
```

### 3) 在 `on.workflow_dispatch.inputs.profile.options` 里加一行

```yaml
      - tcl_linkhub-hh500v
```

> ⚠️ 风险：docker tag `qualcommax-ipq807x-snapshot` 是否存在于 Docker Hub **我无法确认**（Docker Hub API 在我这边超时/502）。
> 该工作流里已有 `qualcommax-ipq60xx-snapshot`，说明 **snapshot 类 tag 存在**，ipq807x 大概率也有；若没有，用**做法 A**（直接下 tar，100% 可行）。

## 两种做法的能力边界（重要）

| | 做法 A / B（ImageBuilder） | 源码编译（hh500v-firmware/） |
|---|---|---|
| 耗时 | **10~20 分钟** | 1~2 小时 |
| 加新设备 | ❌（只用现成 profile） | ✅（backport DTS） |
| 插件来源 | **只能是 ImageBuilder 里的包**（snapshot feed） | 任意源码 feed |
| 有 openclash/passwall/daed/sing-box/tailscale/zerotier | ✅ | ✅ |
| 有 **mihomo / UU / QModem / daed-GUI** | ❌ | ✅ |
| 版本 | `SNAPSHOT`（内核 6.18，**含全部 HH500V 修复**） | 可选 24.10 / master |

## 所以怎么选

| 你要什么 | 用哪个 |
|---|---|
| **10 分钟出个能用的固件**（官方插件） | ✅ **做法 A** |
| 要 QModem / UU / mihomo | ✅ 源码编译 |
| 要 24.10 版本号 | ✅ 源码编译（+backport） |
| 只想验证硬件（5G/WiFi/SIM） | ✅ 做法 A 最快 |

**建议顺序**：先用 **做法 A** 出一个 snapshot 固件，刷上去验证 HH500V 的 5G/WiFi/SIM（这一步最便宜）；
验证通过后，再决定要不要为"第三方插件"去跑源码编译。
