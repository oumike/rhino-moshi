# Flashing the Xteink X4

Custom firmware replaces the stock reader. Nothing here is irreversible **as
long as you do step 1**, which takes two minutes and is the only step that
cannot be done after the fact.

## 1. Find the port

The X4's ESP32-C3 uses native USB CDC, so it appears as `/dev/cu.usbmodem*`
(not `usbserial`). Power the device on, connect USB-C, then:

```bash
ls /dev/cu.usbmodem*
```

There is no boot button to hold: esptool resets the chip into download mode over
USB by itself. If nothing appears, the usual causes are a charge-only cable
(much the most common), a powered-off device, or a port that needs changing.

Confirm you are talking to the right chip before doing anything destructive:

```bash
esptool.py --port /dev/cu.usbmodemXXXX flash_id
```

Expect `Chip is ESP32-C3` and a detected flash size. **Note that size** — the
next step depends on it.

## 2. Back up the stock firmware first

Once you overwrite flash, the original reader is gone unless you have a copy.
Xteink does not publish stock images, so this backup is your only way back.

`tools/xteink-flash` does all of this -- detecting the port, sizing the chip,
checking the read came back complete, and writing a checksum beside it:

```bash
./tools/xteink-flash backup            # read the whole chip
./tools/xteink-flash verify FILE       # check an image (no device needed)
./tools/xteink-flash restore FILE      # write it back
```

It drives the ESP32-C3 boot ROM rather than any particular firmware, so it
works on stock, on this firmware, and on a device too broken to boot. The rest
of this section is what it does underneath, for when you want to do it by hand.

The X4 ships with 16 MB of flash (`0x1000000`). Use whatever `flash_id`
reported above rather than trusting that number — a backup that is short by a
single byte is not a backup:

```bash
esptool.py --chip esp32c3 --port /dev/cu.usbmodemXXXX --baud 921600 \
  read_flash 0x0 0x1000000 xteink-x4-stock-backup.bin
```

This takes a few minutes at 16 MB. Verify you got a complete image:

```bash
ls -l xteink-x4-stock-backup.bin      # expect 16777216 bytes for 16 MB
shasum -a 256 xteink-x4-stock-backup.bin > xteink-x4-stock-backup.sha256
```

If the size does not match what `flash_id` detected, **stop and redo it.** Do
not flash anything until this file is right.

Keep both files somewhere that is not this repo — `.gitignore` deliberately
excludes `*.bin` so a backup cannot be committed by accident.

To restore later:

```bash
esptool.py --chip esp32c3 --port /dev/cu.usbmodemXXXX --baud 921600 \
  write_flash 0x0 xteink-x4-stock-backup.bin
```

## 3. Build and flash

```bash
cd firmware
pio run                                       # build only
pio run --target upload                       # build and flash
pio run --target upload --upload-port /dev/cu.usbmodemXXXX   # if autodetect picks wrong
pio device monitor                            # watch serial output at 115200
```

The build uses the default 4 MB partition layout, which is valid on a larger
chip too — the app needs about 900 KB, so the remaining flash simply goes
unused. Nothing here depends on the device being 16 MB.

## 4. Join it to Wi-Fi

On first boot the device has no credentials and starts a captive portal:

1. The screen shows `Rhino-Companion-Setup`.
2. Join that Wi-Fi network from your phone or laptop.
3. The portal opens automatically; pick your network and enter the password.
4. The device reboots and shows its IP address.

Credentials persist in NVS. To clear them: `curl <device-ip>/reset-wifi`.

**2.4 GHz only** — the ESP32-C3 has no 5 GHz radio. If your network is a single
SSID spanning both bands this usually still works, but a 5 GHz-only SSID will
never appear in the portal list.

## 5. Fix the screen rotation

The panel is 800x480 landscape natively; the X4 is a portrait device. Rotation
defaults to 3, which is right way up on the hardware this was developed on. If
the screen is upside down for how you hold it:

```bash
rhino-companion rotate 1
```

Rotation is stored in NVS, so it survives reboots but not a flash erase.

## 6. Check the buttons

The ADC thresholds in `firmware/include/board.h` came from a known-good X4, but
resistor tolerances and board revisions shift them. Verify:

```bash
rhino-companion calibrate
```

Press each button in turn. You should see the decoded name change. Expected
readings, in 16-bit ADC space:

| Ladder | Idle   | Buttons                                             |
|--------|--------|-----------------------------------------------------|
| ADC1   | ~58107 | Back ~50972, Confirm ~39531, Left ~22083, Right ~63 |
| ADC2   | ~58107 | Up ~32985, Down ~61                                 |

If a button reads as the wrong one or as nothing, note the raw values and set
each threshold in `BUTTON_ADC_1_RANGES` / `BUTTON_ADC_2_RANGES` midway between
two adjacent readings, then reflash.

## Recovering from a bad flash

The ESP32-C3's boot ROM lives in mask ROM and cannot be overwritten, so the chip
can always be talked back into download mode no matter what you wrote to flash.
A bad application image is inconvenient, not fatal:

```bash
esptool.py --chip esp32c3 --port /dev/cu.usbmodemXXXX erase_flash
esptool.py --chip esp32c3 --port /dev/cu.usbmodemXXXX --baud 921600 \
  write_flash 0x0 xteink-x4-stock-backup.bin
```

If a bad image makes the device reboot too fast for esptool to catch it, hold
the power button to keep it off, start the esptool command, and connect USB
while it is waiting on `Connecting....`.

## Battery expectations

This firmware keeps Wi-Fi associated continuously so it can answer a long-poll
the instant a card arrives. That costs roughly an order of magnitude more power
than the stock reader's deep-sleep behaviour: expect it to want USB power for
all-day use rather than the stock firmware's multi-week standby. The MagSafe
back makes a charging stand on the desk a reasonable arrangement.

Deep sleep between cards would need the host to reach a sleeping device, which
means either the host waking it (it cannot -- there is no radio listening) or
the device polling the host on a timer, which trades battery for latency. That
tradeoff is not implemented; see the README's limitations section.
