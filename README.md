# G814Keys

A Lilu plugin that makes the Fn brightness and keyboard-light keys of the ASUS ROG Strix G814JI work under macOS.

The internal keyboard (USB `0b05:19b6`, an ASUS "N-KEY" device) reports its Fn hotkeys in input report `0x5a` on the vendor usage
page `0xff31`, which macOS's HID driver does not read, so the keys do nothing. On Windows, ASUS's software hands each hotkey to the
firmware through the ATK WMI method (`\_SB.ATKD.WMNB`, method `DEVS`, device `0x00100021` → `\_SB.PC00.LPCB.EC0.SKEY`); for the
brightness keys the firmware then sends the standard ACPI video notification `Notify (\_SB.PC00.GFX0.DD1F, 0x86 / 0x87)`.

## What it does

- Reads report `0x5a` on its way through `AppleUserHIDDevice::handleReportWithTime` (a Lilu route in IOHIDFamily; Apple's driver
  still handles every report).
- **Brightness** (usages `0x10` / `0x20`): calls `\_SB.ATKD.WMNB` (`DEVS`, `0x00100021`, the usage), as the Windows software does;
  the firmware's `EC0.SKEY` does the rest. macOS evaluates only a four-character name on a published ACPI device, and `EC0`
  is not published (its `_STA` is Zero), so the WMI entry is the one way in.
  [BrightnessKeys](https://github.com/acidanthera/BrightnessKeys) turns the resulting notification into the brightness keys.
  The firmware's `BRTN` also notifies the dGPU's panel device (`\_SB.PC00.PEG1.PEGP.EDP1`), which the Optimus SSDT defines only
  when the panel is wired to the dGPU; in hybrid mode the method aborts there (`AE_NOT_FOUND`) after the iGPU notification has been
  delivered, so each press logs an ACPI error and the WMI call returns an error status (`WMNB 0xe00002bc` in the plugin's log). The
  keys work regardless; an SSDT that defines an empty `EDP1` under `PEGP` removes the error.
- **Keyboard light** (usages `0xc4` / `0xc5`): this model has no firmware path for it (its WMI device `0x00050021` answers
  unsupported), so the keys set the keyboard's level (0-3) with the feature report Linux's `hid-asus` sends. The level cannot be read
  back; the first key press starts from 2.
- **The power key while typing:** the keyboard also sends its Consumer Power usage (report `0x02`, usage `0x0030`) on some ordinary
  key presses, which macOS turns into the power key: one held past loginwindow's 0.35 s debounce sleeps the machine, the screen goes
  black for 2-3 s and the next key press wakes it. The plugin drops that report before Apple's driver sees it, as Linux's `hid-asus`
  does for the ROG N-KEY keyboards. The power button is the firmware's (ACPI `PWRB`) and is not affected.
- At start-up it sends the keyboard the handshake `hid-asus` sends (report IDs `0x5a` and `0x5d`) and asks which functions it
  has. The kernel-side read may complete with no data; the keyboard light is then assumed (the N-KEY answers the same query
  with its backlight bit set from user space).
- Logs to the kernel log with the prefix `g814keys` (`log show --predicate 'eventMessage CONTAINS "g814keys"' --last boot`).

## Requirements

- ASUS ROG Strix G814JI (the ACPI path and the keyboard are this laptop's).
- macOS 15 (Sequoia), x86_64. Lilu 1.2.0 or later; BrightnessKeys for the brightness keys.
- `_OSI ("Windows 2015")` answered (e.g. `SSDT-XOSI`): the firmware's brightness branch needs `OSYS >= 0x07DC`.

## Install (OpenCore)

Copy `G814Keys.kext` and `BrightnessKeys.kext` to `EFI/OC/Kexts/` and add both to `Kernel/Add` after Lilu:

| key | value |
|---|---|
| `BundlePath` | `G814Keys.kext` |
| `ExecutablePath` | `Contents/MacOS/G814Keys` |
| `PlistPath` | `Contents/Info.plist` |
| `Arch` | `x86_64` |
| `Enabled` | `true` |

`-g814keysoff` disables it.

## Build

```
git clone https://github.com/acidanthera/MacKernelSDK
# Lilu.kext: a DEBUG build of https://github.com/acidanthera/Lilu (its Contents/Resources holds the plugin SDK)
make
```

builds `build/G814Keys.kext` with clang (Xcode or the Command Line Tools) and prints the binary's SHA-256. `make MKSDK=… LILU=…` points
at other copies.

## License

BSD-3-Clause (`LICENSE`). The report format, the handshake and the keyboard-light command follow Linux's `drivers/hid/hid-asus.c`.
