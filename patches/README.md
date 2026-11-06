# HH500V 设备支持补丁（用于 24.10 backport）

## ⚠️ 先说最重要的事：这个设备"进主线"和"能用"差了 10 个月

| 时间 | 提交 | 意义 |
|---|---|---|
| **2025-06-13** | `ac8b5526bc` | 设备进 master —— **但 WiFi 没有 BDF、sysupgrade 是坏的** |
| **2026-01-25** | `12fd85eb79` | ipq-wifi 补上 **HH500V 的 BDF**（在此之前 WiFi 没有校准数据） |
| **2026-01-26** | `30378ffea4` | 修 **sysupgrade**（bootconfig.sh 引用，之前升级会失败） |
| **2026-04-09** | `8dd0872659` | **6.12** 内核的 MHI IRQ 修复（5G 模组稳定性） |
| **2026-04-09** | `862a4edfa6` | **6.18** 内核的 MHI IRQ 修复 |

**结论**：2025-06 ~ 2026-01 之间刷 HH500V 的 OP，**WiFi 基本是废的**；2026-04 之前，**5G 可能不稳**。

## 对 24.10 backport 的影响（关键）

24.10 = **内核 6.6**，而 MHI IRQ 修复只有 **6.12 / 6.18** 两个版本 —— **6.6 没有上游修复**。

| 修复 | 24.10 能不能拿到 |
|---|---|
| BDF（WiFi） | ✅ 可以（cherry-pick `12fd85eb79` 或提升 ipq-wifi 包版本） |
| sysupgrade | ✅ 可以（`30378ffea4`，低风险） |
| **MHI IRQ（5G 稳定性）** | ✅ **已提供 6.6 backport 补丁**：`706-v7.2-bus-mhi-host-pci_generic-round-up-nr_irqs-to-a-power-of-two.patch`（见下） |

## 706 号补丁：给 6.6 内核补 MHI IRQ 修复

上游修复（Linux `86f6dc05ea05`）只有 6.12/6.18 的 backport，但改动本身是**一行**：

```c
- nr_vectors = pci_alloc_irq_vectors(pdev, 1, mhi_cntrl->nr_irqs, PCI_IRQ_MSI);
+ nr_vectors = pci_alloc_irq_vectors(pdev, 1, roundup_pow_of_two(mhi_cntrl->nr_irqs),
+                                    PCI_IRQ_MSI);
```

所以可以直接放到 6.6 的 `target/linux/generic/backport-6.6/` 下（本目录已提供该 patch 文件）。
CI 会在非 master 版本时自动复制进去。

> 若 patch 因行号偏移打不上（`patch` 失败），改用 sed 兜底：
> ```sh
> sed -i 's/pci_alloc_irq_vectors(pdev, 1, mhi_cntrl->nr_irqs, PCI_IRQ_MSI)/pci_alloc_irq_vectors(pdev, 1, roundup_pow_of_two(mhi_cntrl->nr_irqs), PCI_IRQ_MSI)/' \
>   build_dir/target-*/linux-*/drivers/bus/mhi/host/pci_generic.c
> ```


## 所以推荐顺序变了

| 方案 | 风险 |
|---|---|
| **master / snapshot（6.18）** | ✅ **最稳**：BDF + sysupgrade + MHI 修复全都有 |
| **24.10 + 3 个补丁** | ⚠️ WiFi/升级没问题，但 **5G 的 MHI 修复缺 6.6 版** |
| **24.10 只打 ac8b5526bc** | ❌ **别这样**：WiFi 无 BDF、sysupgrade 坏、5G 不稳 |



24.10 分支**没有** HH500V（设备支持 2025-06 才进 master），需要把下面这些提交 cherry-pick 到 `openwrt-24.10`。

## 需要 cherry-pick 的提交（全部来自 openwrt/openwrt）

| SHA | 内容 | 24.10 注意事项 |
|---|---|---|
| `ac8b5526bc` | **加设备支持**：`ipq8072-linkhub-hh500v.dts` + `ipq807x.mk` 设备条目 | 核心；DTS 可能引用 master 新属性，需改 |
| `12fd85eb79` | ipq-wifi 加 **HH500V BDF**（WiFi 校准） | 若 24.10 的 ipq-wifi 版本较旧需同步 |
| `30378ffea4` | 修 **sysupgrade**（bootconfig.sh 引用） | 低风险 |
| `8dd0872659` | generic **6.12** MHI IRQ 修复 | 24.10 是 **6.6**，可能不需要/不兼容 |

## 手动操作（如果 CI 里 cherry-pick 冲突）

```bash
git clone -b openwrt-24.10 https://github.com/immortalwrt/immortalwrt
cd immortalwrt

# 逐个打，冲突时手工解
git fetch https://github.com/openwrt/openwrt.git ac8b5526bc
git cherry-pick -3 ac8b5526bc     # 冲突 → 手工合并 → git add → git cherry-pick --continue

git fetch https://github.com/openwrt/openwrt.git 12fd85eb79
git cherry-pick -3 12fd85eb79

git fetch https://github.com/openwrt/openwrt.git 30378ffea4
git cherry-pick -3 30378ffea4
```

## 冲突时重点检查

1. **`target/linux/qualcommax/image/ipq807x.mk`**：设备条目用的 `$(call Device/FitImage)`、`Device/UbiFit` 在 24.10 是否存在；
2. **DTS 里的节点**：`&qpic_nand`、`phy-mode = "2500base-x"`、`status = "okay"` 等属性是否 24.10 支持；
3. **`kmod-mhi-*` 包名**：24.10 里可能叫法不同（`kmod-mhi-pci-generic` 等），要在 `DEVICE_PACKAGES` 里对齐；
4. **sysupgrade 脚本**：`bootconfig.sh` 路径/名称在 24.10 可能有差异。

## 备选方案（更省事）

如果 24.10 backport 一直失败：**改用 master/snapshot**（设备原生支持），代价是：
- 包管理器变 **apk** → 第三方 ipk（UU/mihomo/daed-GUI 等）基本装不上；
- 只能得到官方 feed 里的 openclash / passwall / daed(核心) / zerotier(核心) / tailscale(核心) / sing-box。
