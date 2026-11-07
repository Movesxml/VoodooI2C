# VoodooI2C — macOS 26 / Alder Lake & Raptor Lake fork

[![CI](https://github.com/Movesxml/VoodooI2C/actions/workflows/main.yml/badge.svg)](https://github.com/Movesxml/VoodooI2C/actions/workflows/main.yml)

A fork of [VoodooI2C](https://github.com/VoodooI2C/VoodooI2C) focused on **macOS 26 (Tahoe)** and
**Intel Alder Lake-P / Raptor Lake-P** laptops, plus a fix for a class of firmware that hides the
I2C bus behind an `OSYS`-gated `_CRS` method.

This is the driver stack that finally brought up the **ELAN0001 precision touchpad on a Lenovo
Legion Y7000P IRH8 (82YA, i7-13700H)** under macOS 26.7.2 — a machine on which the stock driver and
the ID-only "new platform" forks both silently failed.

* 中文说明：[README.zh-CN.md](README.zh-CN.md)
* Case study (full post-mortem): [docs/lenovo-elan0001.md](docs/lenovo-elan0001.md)
* Satellite kext fork: [Movesxml/VoodooI2CHID](https://github.com/Movesxml/VoodooI2CHID)

---

## Why this fork exists

Upstream VoodooI2C is effectively unmaintained. A 2026 fork added Alder/Raptor Lake *PCI IDs*
(`0x51e8`, `0x7acx`, `0x7a4c`…) to `Info.plist`, but **no platform code**, and it does not handle
firmware that builds the I2C resource at run time.

On the affected Lenovo machines the driver loads and the controller initialises fine, yet the
touchpad never appears:

```
VoodooI2CPCIController → VoodooI2CControllerNub → VoodooI2CControllerDriver   [OK, BusConfig computed]
  └── VoodooI2CDeviceNub                                      [!registered, !matched]
```

`VoodooI2CDeviceNub::attach()` bails out in `getDeviceResources()` because neither `_CRS` nor
`_DSM` yields an I2C serial bus. No device nub → no `VoodooI2CHIDDevice` → no touchpad, with
nothing obviously wrong in the logs.

## Root cause

The DSDT describes the touchpad as `\_SB.PC00.I2C0.TPAD` (`ELAN0001`, `PNP0C50`, address `0x15`)
and keeps the resources in two device-scope names, `SBFB` (I2C serial bus) and `SBFG` (GPIO
interrupt). `_CRS` is supposed to publish them:

```asl
Method (_CRS, 0, Serialized) {
    ...
    ADR0 = DerefOf (DerefOf (TPID [Local0]) [One])   // runtime slave address -> SBFB
    HID2 = DerefOf (DerefOf (TPID [Local0]) [0x02])
    If ((OSYS < 0x07DC)) { Return (SBFI) }            // <-- legacy IRQ template only
    If (Ones) { Return (ConcatenateResTemplate (SBFB, SBFG)) }
    Return (ConcatenateResTemplate (SBFB, SBFI))
}
```

macOS is not Windows: unless the firmware's `_OSI("Windows …")` probes are spoofed, `OSYS` stays
below `0x07DC` and `_CRS` returns **only the legacy interrupt template**. The `_DSM` fallback that
VoodooI2C normally uses is also dead on this firmware, because the method returns early for any
GUID other than HIDG:

```asl
If ((Arg0 == ToUUID ("3cdff6f7-…"))) { ... }
Else { Return (Buffer (One) { 0x00 }) }   // TP7G branch below is unreachable
```

So: `_CRS` → no I2C, `_DSM` → no I2C, and the driver gave up.

## The fix

When both `_CRS` and `_DSM` fail to describe a bus, the driver now evaluates the device-scope
names **`SBFB`** and **`SBFG`** directly and parses them as resource templates.

This works because `_CRS` writes the runtime slave address into `SBFB` **before** the `OSYS`
check, so the buffer already contains the correct address even when `_CRS` returns only `SBFI`.

The result is visible in the registry:

```
"ACPI Resource Source" = "device scope SBFB/SBFG"
"ACPI _DSM"            = "unavailable"
"i2cAddress"           = 21            (0x15)
"Interrupt Mode"       = "Polling"
```

The fallback only runs when the normal paths fail, so no existing machine changes behaviour.

## What else is in this fork

Full audit of the driver stack (23 issues), including:

**Core / bus**

* GPIO controller missing no longer kills the device — degrades to polling.
* `_CRS` bus parameters are preferred over `_DSM` (dynamic address tables).
* `i2c_address` widened to `UInt16` (10-bit addressing was truncated).
* `PMstop()` on every `start()` failure path; no leaked power tree.
* `publishNubs()` no longer stops enumerating on a non-`IOService` child; null-safe names.
* `getGPIOController()` retains the service instead of keeping a dangling pointer.
* Null-safe `IOName` construction from the 4-byte, non-NUL-terminated ACPI name.
* `VoodooI2CControllerNub::attach()` returns `false` when `super::attach()` fails.

**ACPI parser**

* Iterative descriptor walk with bounds checks (no recursion, no OOB reads, no infinite loop).
* Handles both `OSData` and `OSArray` resource returns.

**HID satellite** (`VoodooI2CHID` fork)

* HID descriptor read retried for ~1 s (was a single attempt that could doom the whole boot).
* Falls back to register `0x0001` when the HIDG `_DSM` is missing.
* Explicit polling when VoodooI2C reports `Interrupt Mode = Polling` (an interrupt event source
  that never fires used to leave the device silent).
* Host-initiated reset no longer blocks the work loop for 12 s in polling mode.
* Allocation/return-value checks in report paths; division-by-zero guards; PTP mode switch retried.

**Multitouch injection**

* Contact count clamped to `VOODOO_INPUT_MAX_TRANSDUCERS` (out-of-bounds write).
* `VoodooI2CMultitouchInterface::setProperty()` no longer dereferences a NULL engine set.

## Supported controllers

All IDs from upstream, plus the Alder/Raptor Lake controllers:

| Generation | PCI IDs |
|---|---|
| Haswell / Broadwell | `INT33C2`, `INT33C3`, `INT3432`, `INT3433` |
| Skylake … Coffee Lake | `9d60-9d63`, `a160-a163`, `9de8-9deb`, `a368-a36b` |
| Comet Lake | `02e8-02eb`, `06e8-06eb` |
| Ice Lake | `34e8-34eb` |
| Tiger Lake | `a0e8-a0eb`, `43e8-43eb` |
| Alder Lake | `51e8-51eb`, `7acc-7acf`, `54e8-54eb` |
| Raptor Lake | `7a4c-7a4f` |

Satellites (install only what you need): `VoodooI2CHID`, `VoodooI2CELAN`, `VoodooI2CSynaptics`,
`VoodooI2CFTE`, `VoodooI2CAtmelMXT`.

## Installation

1. Build or download `VoodooI2C-*.zip` (CI artifacts / releases).
2. Copy `VoodooI2C.kext` **and the satellite you need** (e.g. `VoodooI2CHID.kext`) into
   `EFI/OC/Kexts/`.
3. Add them to `config.plist → Kernel → Add` **in this order**:

   ```
   VoodooI2C.kext/Contents/PlugIns/VoodooI2CServices.kext
   VoodooI2C.kext/Contents/PlugIns/VoodooGPIO.kext
   VoodooI2C.kext/Contents/PlugIns/VoodooInput.kext
   VoodooI2C.kext
   VoodooI2CHID.kext
   ```

4. Reboot. `kextstat | grep -i voodoo` must list **all** of them — a missing satellite is the most
   common reason a “working” core still produces no touchpad.

Polling mode can be forced with `-vi2c-force-polling` if GPIO interrupts misbehave on your PCH.

## Building

The repository builds with Xcode and [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK).
CI (`.github/workflows/main.yml`, `macos-latest`) produces Debug and Release zips:

```bash
xcodebuild -workspace VoodooI2C.xcworkspace -scheme VoodooI2C \
           -derivedDataPath build -configuration Release
```

Submodules: `Dependencies/VoodooGPIO`, the satellites, and `VoodooInput` (bootstrapped by CI).

## Diagnostics

Registry properties exposed by the device nub:

| Property | Meaning |
|---|---|
| `ACPI Resource Source` | `_CRS`, `_DSM` or `device scope SBFB/SBFG` |
| `ACPI _CRS` / `ACPI _DSM` | bytes/type returned by each method |
| `i2cAddress`, `addrWidth`, `sclHz` | parsed bus parameters |
| `Interrupt Mode` | `GPIO`, `APIC` or `Polling` |
| `gpioPin`, `gpioIRQ` | GPIO interrupt pin (when used) |

```bash
ioreg -l -w 0 -c VoodooI2CDeviceNub | grep -E 'ACPI|i2cAddress|Interrupt'
ioreg -c VoodooI2CHIDDevice | grep -c VoodooI2CHIDDevice   # 1 = HID device attached
```

Kernel messages to look for:

```
VoodooI2C 3.1.1 starting (ELAN0001 bring-up build)
… I2C resources from _CRS: address 0x15, bus speed 400000 Hz
… Recovered I2C resources from device scope names (SBFB/SBFG)
… Interrupt mode: Polling (i2c address 0x15, gpio pin 0xb0)
… HID descriptor: VID 0x04f3 PID … report descriptor … bytes
… Multitouch elements: N finger collection(s) … input mode yes
```

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `VoodooI2CDeviceNub` is `!registered` | ACPI resources not parsed — check `ACPI Resource Source` |
| No `VoodooI2CHID` in `kextstat` | satellite kext missing from EFI or not enabled |
| Device present, no cursor | satellite not in PTP mode / VoodooInput missing |
| Works only with `-vi2c-force-polling` | GPIO mapping for your PCH; polling is a safe fallback |

## Credits

* [Alexandre Daoud / coolstar](https://github.com/VoodooI2C/VoodooI2C) — original VoodooI2C
* [lzhoang2801](https://github.com/lzhoang2801/VoodooI2C) — Alder/Raptor Lake PCI IDs
* [1Revenger1](https://github.com/1Revenger1), [kprinssu](https://github.com/kprinssu) — VoodooI2CHID
* [acidanthera](https://github.com/acidanthera) — MacKernelSDK, VoodooInput, OpenCore

## License

GPL-3.0, inherited from upstream VoodooI2C. See [LICENSE.txt](LICENSE.txt).
