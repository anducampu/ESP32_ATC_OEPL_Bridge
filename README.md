# ESP32_ATC_OEPL_Bridge

An ESP32 sketch that sends pictures, text and **self-updating stats screens** to **electronic shelf
labels (ESL)**, also known as e-paper price tags,
running [atc1441](https://github.com/atc1441)'s **ATC_BLE_OEPL** firmware (Bluetooth name `ATC_xxxxxx`).

It does two jobs:

1. **Bridge for the web page.** Plugged into a computer by USB, it carries uploads from the
   companion web page, [ESL Uploader](https://andu.ro/esl-uploader/) (picture, formatted text, stats screen) to the tag over Bluetooth. Handy when
   the computer has no Bluetooth, or its Bluetooth is unreliable.
2. **Automatic stats refresh.** Plugged into any USB charger, it keeps a saved list of tags and every
   1–60 minutes reads each tag's battery and temperatures, draws its stats screen and uploads it. No
   computer needed. The ESLs can't redraw on their own, so something has to send the new screen.

The stats screen shows the tag's Bluetooth name (so you can find it), screen and board temperature,
battery level and the time of the last update. The layout adapts to 2.13″ (250×122) and 4.2″
(400×300) tags, landscape or portrait.

## What you need

- **An ESP32 board with Bluetooth LE**: ESP32, ESP32-S3, ESP32-C3 or ESP32-C6. The **ESP32-S2
  won't work** (no Bluetooth). Tested on a Seeed **XIAO ESP32-S3**.
- **ESLs running ATC_BLE_OEPL.** For Hanshow ESLs with a Telink chip, atc1441's
  [web tool](https://atc1441.github.io/ATC_BLE_OEPL_Image_Upload.html) flashes it (UART Flasher tab).
  Tested on Hanshow 2.13″ and 4.2″ black/white/red ESLs.
- A **data** USB cable (some cables only charge).

## Installing

1. **Arduino IDE** (2.x) → *Boards Manager* → install **esp32** by Espressif.
2. *Library Manager* → install **NimBLE-Arduino** (2.x) and **Adafruit GFX Library**.
3. Download this repository and open `ESP32_ATC_OEPL_Bridge.ino`. The folder must be named
   `ESP32_ATC_OEPL_Bridge` (a GitHub ZIP unpacks as `ESP32_ATC_OEPL_Bridge-main`: rename it, or let
   the IDE move the sketch). `render.h` and `fonts.h` must stay next to the `.ino`.
4. *Tools* menu:
   - **Board**: yours, e.g. *XIAO_ESP32S3*, *ESP32 Dev Module*, *ESP32C3 Dev Module*.
   - **Partition Scheme → Huge APP**. The sketch is ~1.4 MB, too big for the default 1.2 MB on most
     boards. The XIAO ESP32-S3 profile already has enough room.
   - **USB CDC On Boot**: *Enabled* if the board's USB socket is wired straight to the ESP32 chip
     (XIAO ESP32-S3/C3/C6, "SuperMini" boards, the USB port of an S3/C3 DevKit). *Disabled* if it
     goes through a USB-serial chip (CP2102, CH340: most classic ESP32 boards, the UART port of
     DevKits). If unsure, try one: with the wrong setting the serial monitor stays silent.
5. Upload. The serial monitor (115200 baud) should print `ESP32_ATC_OEPL_Bridge ready.`

With `arduino-cli`:

```sh
arduino-cli compile --upload -p /dev/ttyACM0 --fqbn esp32:esp32:XIAO_ESP32S3 .
arduino-cli compile --upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32:PartitionScheme=huge_app .
```

## Using it with the web page

The companion web page is **[ESL Uploader](https://andu.ro/esl-uploader/)**. Open it in
Chrome, Edge, Brave (or Firefox 151+ for USB) and choose **Via ESP32 (USB)** → **Connect** → pick the
board's port → **Scan**. Then:

- **Picture / Text**: the page renders the image and sends it through the ESP32 (`img` command).
- **Stats screen**: the ESP32 reads the tag and draws the screen itself (`stats` command). The same
  drawing code runs in the page, so the preview matches the tag pixel for pixel.
- **Automatic refresh**: tick *Keep this tag's stats screen up to date* and pick the interval. The
  list and interval are saved on the ESP32 and survive a power cut. Then move it to any USB charger
  within a few metres of the tags.

Every refresh redraws the whole screen (~15 s on black/white/red tags), and that is what uses the
tag's battery. 10 minutes or more is a good balance.

### The clock

The board has no battery-backed clock. The web page sets it (and the time zone) every time it
connects. After a power cut the stats screens show `Updated --:--` until the page connects again,
unless you give the board WiFi once:

```
wifi YourNetworkName YourPassword
```

It then syncs over NTP at start-up and every 6 hours, and switches WiFi off in between.

## Serial commands

One per line, 115200 baud. The web page uses the same commands.

| Command | What it does |
|---|---|
| `scan` | List ATC tags nearby (name, MAC, RSSI) |
| `target <name\|mac>` | Default tag for `send` (saved) |
| `info [name\|mac]` | Connect and print the tag's display buffer size and colours |
| `img <name\|mac\|-> <len>` | Receive `<len>` raw buffer bytes (prints `READY`, then `ACK <n>` per 1 KB) and upload them |
| `rot <name> [0-3]` | Per-tag rotation of drawn text/stats, clockwise quarter turns (saved) |
| `send <spec>` / `to <name> <spec>` | Draw formatted text (see below) and upload it |
| `stats <name\|mac\|->` | Read battery + temperatures, draw the stats screen, upload; prints `STATS name mV board screen time` |
| `auto` | Show the auto-refresh list, interval and last refresh |
| `auto <min> <tag,tag\|all>` | Refresh those tags every `<min>` minutes (saved; first refresh right away) |
| `auto add <tag>` / `auto del <tag>` / `auto off` | Edit / stop the list |
| `time <unix-utc>` | Set the clock |
| `tz <POSIX TZ>` | Time zone (default Romania: `EET-2EEST,M3.5.0/3,M10.5.0/4`) |
| `wifi <ssid> <password>` / `wifi off` | Optional NTP time sync |

### Formatted text (`<spec>`)

Lines separated by `|`; each may start with `{flags}`:

- `1`…`9`: size. 7, 9 pt regular, then 12, 18, 24, 32, 40, 52, 64 pt bold (FreeSans, including
  Romanian ăâîșț, °, ℃ and €)
- `r` red, `b` black, `i` white on a full-width bar
- `<` `=` `>`: align left / centre / right
- `@n` / `@-n`: move the line n pixels down / up

Every line always stays on the screen: a line too wide drops to a smaller size, a stack too tall
shrinks its biggest lines, a moved line stops at the edge.

```
send {4}Mere Golden|{7r}4,99 lei|{2}preț / kg
```

## How it talks to the tags

Service and characteristic `0x1337`, the same protocol as atc1441's
[upload page](https://atc1441.github.io/ATC_BLE_OEPL_Image_Upload.html):

- `0005` → display info (buffer size, colours)
- `0064` + data info → the tag asks for 4 KB blocks (`00C6`) → `0002`, then `0065` parts of 230 bytes,
  acked with `00C5`/`00C4` → `00C7`/`00C8` done → `0003` draw
- `0016 0000` → `00D1` + battery mV, raw ADC, chip temperature, panel temperature

Image buffer: black plane, then red plane (1 = ink), row-major, MSB first. The 2.13″ panel shows its
128×250 buffer turned 90° clockwise and hides 6 of the 128 rows: the bottom 6 of the landscape view
(measured on a black/white/red tag; `panelBaseHidden()` in the sketch). Text and stats are drawn
only on the visible 250×122.

## Troubleshooting

- **Browser says “The device has been lost” right after Connect (Linux)**: the port was left in a
  mode some Chromium versions misread (by the Arduino serial monitor or pyserial). Unplug and replug
  the board, or run `stty -F /dev/ttyACM0 min 1` (native USB) / `stty -F /dev/ttyUSB0 min 1`
  (CP2102/CH340 boards).
- **The board doesn't show up in the browser's port list (Linux)**: add yourself to the `dialout`
  group (`sudo usermod -aG dialout $USER`, then log out and back in). Close the Arduino serial monitor.
- **Connect works but the page says the board doesn't answer**: many boards with a CP2102/CH340
  restart when the port opens; the page waits for them, so give it a few seconds. Still nothing:
  check the *USB CDC On Boot* setting above and that this sketch is on the board.
- **A tag isn't found**: tags don't advertise while they redraw (~15 s), so scan again. Keep the tag
  within a few metres; small boards have short-range antennas. A tag takes one connection at a time.
- **Sketch too big**: set *Partition Scheme → Huge APP*.

## Files

- `ESP32_ATC_OEPL_Bridge.ino`: Bluetooth, upload protocol, serial commands, auto refresh, clock
- `render.h`: formatted text and stats-screen drawing. The web page has a line-by-line JavaScript
  port (`render.js`); `tools/render_test.cpp` and `tools/render_test.mjs` check they produce identical
  pixels. The JS test needs the page's `render.js` and `fonts.js`: `RENDER_JS=/path/to/render.js
  node tools/render_test.mjs stats 250 128`.
- `fonts.h`: bitmap fonts, generated by `tools/gen_fonts.py` from GNU FreeFont. Don't edit it by hand.
- `tools/fonts_to_js.py`: exports `fonts.h` for the web page.

## Credits and licence

- [atc1441 (Aaron Christophel)](https://github.com/atc1441) for the ATC_BLE_OEPL firmware and its
  upload protocol.
- Code: MIT licence, see `LICENSE`. Fonts in `fonts.h`: GNU FreeFont, GPLv3 with font exception.
