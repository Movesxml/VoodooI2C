# Case study: ELAN0001 touchpad on a Lenovo Legion Y7000P IRH8 (macOS 26)

This is the post-mortem for the issue that motivated this fork. It documents exactly where the
stock driver (and the ID-only Alder/Raptor Lake forks) fail on this firmware, and how the fallback
in this repository recovers the bus.

## Machine

| Item | Value |
|---|---|
| Model | Lenovo Legion Y7000P IRH8 (82YA), BIOS M2CN25WW |
| CPU | Intel Core i7-13700H (Raptor Lake-P) |
| OS | macOS 26.7.2 (25G332), Darwin 25.6.0, x86_64, OpenCore |
| Touchpad | `ELAN0001`, `_CID` `PNP0C50`, ACPI path `\_SB.PC00.I2C0.TPAD` |
| I2C controller | PCI `8086:51e8` (I2C0), slave address `0x15` |
| GPIO | `\_SB.GPI0` (`INTC1055`), interrupt pin `0xB0` |

## Symptom

The kext stack loads and the controller is fully initialised, but no HID device is ever created:

```
VoodooI2CPCIController            <registered, matched, active>
 └─ VoodooI2CControllerNub
     └─ VoodooI2CControllerDriver   "BusConfig" = {SS_HCNT=612, FS_HCNT=110, …}
         └─ VoodooI2CDeviceNub     <!registered, !matched>   <-- stuck here
```

`VoodooI2CDeviceNub::attach()` fails inside `getDeviceResources()`: neither `_CRS` nor `_DSM`
produced an I2C serial bus. Because the nub never reaches `registerService()`, `VoodooI2CHID`
never matches and the touchpad is absent — with no obvious error unless debug logging is enabled.

## Firmware layout

The DSDT keeps the resources in device-scope names and publishes them from a *method*:

```asl
Device (TPAD)
{
    Name (SBFB, ResourceTemplate () {          // I2C serial bus, address filled at run time
        I2cSerialBusV2 (0x0000, ControllerInitiated, 0x00061A80, AddressingMode7Bit,
                        "\\_SB.PC00.I2C0", 0x00, ResourceConsumer, , Exclusive)
    })
    Name (SBFG, ResourceTemplate () {          // GpioInt
        GpioInt (Level, ActiveLow, ExclusiveAndWake, PullUp, 0x0000,
                 "\\_SB.GPI0", 0x00, ResourceConsumer, ,) { 0x00B0 }
    })
    CreateWordField (SBFB, 0x10, ADR0)         // runtime slave address

    Method (_CRS, 0, Serialized) {
        ...
        ADR0 = DerefOf (DerefOf (TPID [Local0]) [One])   // 0x15
        HID2 = DerefOf (DerefOf (TPID [Local0]) [0x02])
        If ((OSYS < 0x07DC)) { Return (SBFI) }            // legacy IRQ template only
        If (Ones) { Return (ConcatenateResTemplate (SBFB, SBFG)) }
        Return (ConcatenateResTemplate (SBFB, SBFI))
    }
}
```

`SBFI` is the legacy interrupt template (IRQ `0x58`), which is useless to an I2C HID device.

### Why `_CRS` returns no bus on macOS

`OSYS` is set in `_INI` from a chain of `_OSI("Windows …")` probes. On Windows it ends up at
`0x07DF`+ (Windows 8.1/10), so `_CRS` takes the `ConcatenateResTemplate(SBFB, SBFG)` branch.

On macOS, unless the firmware's `_OSI` probes are spoofed (XOSI rename + `SSDT-XOSI`), every
`_OSI("Windows …")` returns false and `OSYS` stays at its default `0x03E8`, so `_CRS` stops at
the `OSYS < 0x07DC` branch and returns only `SBFI`.

### Why the `_DSM` fallback is dead

VoodooI2C normally falls back to the TP7G `_DSM` for resources. On this firmware that branch is
unreachable:

```asl
Method (_DSM, 4, NotSerialized) {
    If ((Arg0 == ToUUID ("3cdff6f7-4267-4555-ad05-b30a3d8938de"))) { … }
    Else { Return (Buffer (One) { 0x00 }) }   // any other GUID returns here
    If ((Arg0 == ToUUID ("ef87eb82-f951-46da-84ec-14871ac6f84b"))) { … }  // never reached
    …
}
```

At runtime the driver observes `"ACPI _DSM" = "unavailable"`.

So both standard sources fail and `VoodooI2CDeviceNub` bails out.

## The fix

`VoodooI2CDeviceNub::getDeviceResources()` now has a last-resort path: when neither `_CRS` nor
`_DSM` describes an I2C bus, it evaluates the device-scope names `SBFB` and `SBFG` and parses them
as resource templates.

This is sound because `_CRS` executes the `ADR0` assignment **before** the `OSYS` check — so by the
time the fallback reads `SBFB`, the buffer already carries the correct runtime address (`0x15`).

```cpp
if (!crs_parser.found_i2c && !dsm_parser.found_i2c) {
    VoodooI2CACPIResourcesParser scope_parser;
    OSObject* object = nullptr;

    if (acpi_device->evaluateObject("SBFB", &object) == kIOReturnSuccess)
        parseResourceObject(object, &scope_parser, "ACPI SBFB");
    OSSafeReleaseNULL(object);

    if (acpi_device->evaluateObject("SBFG", &object) == kIOReturnSuccess)
        parseResourceObject(object, &scope_parser, "ACPI SBFG");
    OSSafeReleaseNULL(object);

    if (scope_parser.found_i2c) {
        setProperty("ACPI Resource Source", "device scope SBFB/SBFG");
        crs_parser = scope_parser;
    }
}
```

`parseResourceObject()` accepts both `OSData` and `OSArray` results, so the same code covers
firmware that returns a package of resource buffers.

## Verified result

After the fix, on the same machine:

```
kextstat:
  com.alexandred.VoodooI2C (3.1.1)
  com.alexandred.VoodooI2CHID (1)
  org.coolstar.VoodooGPIO (1.1)
  com.alexandred.VoodooI2CServices (1)
  me.kishorprins.VoodooInput (1.1.7)

ioreg -c VoodooI2CDeviceNub:
  "ACPI Resource Source" = "device scope SBFB/SBFG"
  "ACPI _DSM"            = "unavailable"
  "i2cAddress"           = 21        (0x15)
  "Interrupt Mode"       = "Polling"
  "gpioPin"              = 176       (0xB0)

ioreg classes:
  VoodooI2CHIDDevice = 1
  VoodooI2CPrecisionTouchpadHIDEventDriver = 1
  AppleMultitouchTrackpadHIDEventDriver = 1
```

The touchpad then works with native gestures through VoodooInput / Apple's multitouch stack.

## Notes

* **Polling vs GPIO.** The example runs with `-vi2c-force-polling`. GPIO interrupts route through
  `VoodooGPIO`'s Tiger Lake-LP tables (`INTC1055`), which are shared with Alder Lake-P in Linux;
  polling is the safe default and can be dropped once GPIO is verified on the specific board.
* **The satellite is mandatory.** A common failure mode after fixing the core is forgetting
  `VoodooI2CHID.kext` (and `VoodooInput`) in `Kernel → Add` — the core then looks healthy while no
  HID device exists.
* **Alternative fix.** Publishing an SSDT that overrides `_CRS` to always return
  `ConcatenateResTemplate(SBFB, SBFG)` also solves it, but requires renaming the original method.
  The driver-side fallback avoids touching the firmware tables.
