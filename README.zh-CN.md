# VoodooI2C —— macOS 26 / Alder Lake & Raptor Lake 分支

[![CI](https://github.com/Movesxml/VoodooI2C/actions/workflows/main.yml/badge.svg)](https://github.com/Movesxml/VoodooI2C/actions/workflows/main.yml)

这是 [VoodooI2C](https://github.com/VoodooI2C/VoodooI2C) 的一个分支，面向 **macOS 26 (Tahoe)** 和
**Intel Alder Lake-P / Raptor Lake-P** 笔记本，并修复了一类「固件把 I2C 总线藏在 `OSYS` 门控的
`_CRS` 后面」的问题。

它最终在 **联想拯救者 Y7000P IRH8（82YA，i7-13700H）** 的 **ELAN0001 精密触控板** 上跑通
macOS 26.7.2 —— 在这台机器上，官方驱动和「只加 PCI ID」的新平台分支都会静默失败。

* English: [README.md](README.md)
* 完整复盘：[docs/lenovo-elan0001.md](docs/lenovo-elan0001.md)
* 卫星驱动分支：[Movesxml/VoodooI2CHID](https://github.com/Movesxml/VoodooI2CHID)

---

## 为什么需要这个分支

官方 VoodooI2C 早已停更。2026 年的一个新平台分支只在 `Info.plist` 里加了 Alder/Raptor Lake 的
**PCI ID**（`0x51e8`、`0x7acc`、`0x7a4c`…），**没有任何平台相关代码**，也没有处理运行时才生成
I2C 资源的固件。

在这类联想机器上，驱动看起来一切正常，触控板就是不出现：

```
VoodooI2CPCIController → VoodooI2CControllerNub → VoodooI2CControllerDriver   [正常，BusConfig 已算出]
  └── VoodooI2CDeviceNub                                      [!registered, !matched]
```

`VoodooI2CDeviceNub::attach()` 在 `getDeviceResources()` 就退出了：`_CRS` 和 `_DSM` 都没能给出
I2C Serial Bus。没有设备 nub → 没有 `VoodooI2CHIDDevice` → 没有触控板，而日志里看起来什么都没坏。

## 根因

DSDT 把触控板描述成 `\_SB.PC00.I2C0.TPAD`（`ELAN0001`，`PNP0C50`，地址 `0x15`），资源放在两个
设备作用域名字里：`SBFB`（I2C 总线）和 `SBFG`（GPIO 中断）。`_CRS` 负责发布它们：

```asl
Method (_CRS, 0, Serialized) {
    ...
    ADR0 = DerefOf (DerefOf (TPID [Local0]) [One])   // 运行时地址写入 SBFB
    HID2 = DerefOf (DerefOf (TPID [Local0]) [0x02])
    If ((OSYS < 0x07DC)) { Return (SBFI) }            // <-- 只返回老式中断模板
    If (Ones) { Return (ConcatenateResTemplate (SBFB, SBFG)) }
    Return (ConcatenateResTemplate (SBFB, SBFI))
}
```

macOS 不是 Windows：如果固件里的 `_OSI("Windows …")` 没有被 XOSI 之类的补丁顶成 true，`OSYS`
会停在 `0x07DC` 以下，`_CRS` 就**只返回老式中断模板**。而 VoodooI2C 平时依赖的 `_DSM` 回退在这份
固件上也是死路 —— 它在 GUID 不等于 HIDG 时直接提前返回：

```asl
If ((Arg0 == ToUUID ("3cdff6f7-…"))) { ... }
Else { Return (Buffer (One) { 0x00 }) }   // 下面的 TP7G 分支永远不可达
```

结果：`_CRS` 没有 I2C，`_DSM` 没有 I2C，驱动放弃。

## 修复方法

当 `_CRS` 和 `_DSM` 都拿不到总线时，驱动改为直接求值设备作用域里的 **`SBFB`** 和 **`SBFG`**，
按资源模板解析。

之所以可行：`_CRS` 在 `OSYS` 判断**之前**就已经把运行时地址写进了 `SBFB`，所以即使 `_CRS`
只返回 `SBFI`，`SBFB` 里也已经是正确的地址。

在 IORegistry 里可以直接看到：

```
"ACPI Resource Source" = "device scope SBFB/SBFG"
"ACPI _DSM"            = "unavailable"
"i2cAddress"           = 21            (0x15)
"Interrupt Mode"       = "Polling"
```

这条兜底只在正常路径失败时触发，不影响已有机器。

## 其他改动

对整条驱动栈做了完整审计（23 项），包括：

**总线 / 设备层**

* 找不到 GPIO 控制器时不再放弃设备，自动降级轮询。
* I2C 地址优先取 `_CRS`（动态地址表）。
* `i2c_address` 扩为 `UInt16`（10-bit 地址原先会被截断）。
* 所有 `start()` 失败路径补 `PMstop()`，不再泄漏电源树。
* `publishNubs()` 遇到非 `IOService` 子节点不再提前结束枚举；名字判空。
* `getGPIOController()` 改为持有引用，避免悬垂指针。
* 用 4 字节、无 NUL 的 ACPI 名构造 `IOName` 时不再越界读。
* `VoodooI2CControllerNub::attach()` 在 `super::attach()` 失败时返回 false。

**ACPI 解析**

* 改成迭代遍历 + 边界校验（不再递归、不会越界读、不会死循环）。
* 同时支持 `OSData` 和 `OSArray` 两种资源返回形式。

**HID 卫星**（`VoodooI2CHID` 分支）

* HID 描述符读取重试约 1 秒（原先只读一次，失败就整次开机没有触控板）。
* HIDG `_DSM` 缺失时回退到寄存器 `0x0001`。
* VoodooI2C 报告 `Interrupt Mode = Polling` 时显式使用定时器轮询（原先中断源可能注册成功但永不触发）。
* 轮询模式下 host reset 不再阻塞工作队列 12 秒。
* 报告路径补分配/返回值检查、除零保护；PTP 模式切换重试。

**多触点注入**

* 触点数量裁剪到 `VOODOO_INPUT_MAX_TRANSDUCERS`（原先会越界写）。
* `VoodooI2CMultitouchInterface::setProperty()` 不再解引用空的 engine 集合。

## 支持的控制器

包含上游全部 ID，并补齐 Alder/Raptor Lake：

| 代际 | PCI ID |
|---|---|
| Haswell / Broadwell | `INT33C2`, `INT33C3`, `INT3432`, `INT3433` |
| Skylake … Coffee Lake | `9d60-9d63`, `a160-a163`, `9de8-9deb`, `a368-a36b` |
| Comet Lake | `02e8-02eb`, `06e8-06eb` |
| Ice Lake | `34e8-34eb` |
| Tiger Lake | `a0e8-a0eb`, `43e8-43eb` |
| Alder Lake | `51e8-51eb`, `7acc-7acf`, `54e8-54eb` |
| Raptor Lake | `7a4c-7a4f` |

卫星驱动（按需安装）：`VoodooI2CHID`、`VoodooI2CELAN`、`VoodooI2CSynaptics`、`VoodooI2CFTE`、
`VoodooI2CAtmelMXT`。

## 安装

1. 编译或下载 `VoodooI2C-*.zip`（CI 产物或 Release）。
2. 把 `VoodooI2C.kext` 和你需要的卫星（如 `VoodooI2CHID.kext`）放进 `EFI/OC/Kexts/`。
3. 在 `config.plist → Kernel → Add` 中按顺序加入：

   ```
   VoodooI2C.kext/Contents/PlugIns/VoodooI2CServices.kext
   VoodooI2C.kext/Contents/PlugIns/VoodooGPIO.kext
   VoodooI2C.kext/Contents/PlugIns/VoodooInput.kext
   VoodooI2C.kext
   VoodooI2CHID.kext
   ```

4. 重启。`kextstat | grep -i voodoo` 必须**全部**出现 —— 少了卫星驱动是「核心正常但没有触控板」
   最常见的原因。

如果某颗 PCH 的 GPIO 中断有问题，可以用 `-vi2c-force-polling` 强制轮询。

## 编译

需要 Xcode 和 [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK)。CI
（`.github/workflows/main.yml`，`macos-latest`）会产出 Debug/Release 两个 zip：

```bash
xcodebuild -workspace VoodooI2C.xcworkspace -scheme VoodooI2C \
           -derivedDataPath build -configuration Release
```

子模块：`Dependencies/VoodooGPIO`、各卫星；`VoodooInput` 由 CI 引导脚本拉取。

## 诊断

设备 nub 会暴露以下属性：

| 属性 | 含义 |
|---|---|
| `ACPI Resource Source` | `_CRS`、`_DSM` 或 `device scope SBFB/SBFG` |
| `ACPI _CRS` / `ACPI _DSM` | 两个方法各自返回的类型/长度 |
| `i2cAddress`、`addrWidth`、`sclHz` | 解析出的总线参数 |
| `Interrupt Mode` | `GPIO`、`APIC` 或 `Polling` |
| `gpioPin`、`gpioIRQ` | GPIO 中断引脚（使用中断时） |

```bash
ioreg -l -w 0 -c VoodooI2CDeviceNub | grep -E 'ACPI|i2cAddress|Interrupt'
ioreg -c VoodooI2CHIDDevice | grep -c VoodooI2CHIDDevice   # 1 = HID 设备已挂上
```

内核日志关键行：

```
VoodooI2C 3.1.1 starting (ELAN0001 bring-up build)
… I2C resources from _CRS: address 0x15, bus speed 400000 Hz
… Recovered I2C resources from device scope names (SBFB/SBFG)
… Interrupt mode: Polling (i2c address 0x15, gpio pin 0xb0)
… HID descriptor: VID 0x04f3 PID … report descriptor … bytes
… Multitouch elements: N finger collection(s) … input mode yes
```

## 排错

| 现象 | 可能原因 |
|---|---|
| `VoodooI2CDeviceNub` 是 `!registered` | ACPI 资源没解析出来，看 `ACPI Resource Source` |
| `kextstat` 里没有 `VoodooI2CHID` | 卫星 kext 没放进 EFI 或没启用 |
| 设备在但光标不动 | 卫星没进 PTP 模式 / 缺 VoodooInput |
| 只有加 `-vi2c-force-polling` 才正常 | 该 PCH 的 GPIO 映射问题，轮询是安全回退 |

## 致谢

* [Alexandre Daoud / coolstar](https://github.com/VoodooI2C/VoodooI2C) —— 原始 VoodooI2C
* [lzhoang2801](https://github.com/lzhoang2801/VoodooI2C) —— Alder/Raptor Lake PCI ID
* [1Revenger1](https://github.com/1Revenger1)、[kprinssu](https://github.com/kprinssu) —— VoodooI2CHID
* [acidanthera](https://github.com/acidanthera) —— MacKernelSDK、VoodooInput、OpenCore

## 许可证

GPL-3.0，继承自上游 VoodooI2C。见 [LICENSE.txt](LICENSE.txt)。
