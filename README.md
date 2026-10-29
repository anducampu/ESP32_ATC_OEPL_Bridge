# ESP32_ATC_OEPL_Bridge

An ESP32 sketch that sends pictures, text and **self-updating stats screens** to **electronic shelf
labels (ESL)**, also known as e-paper price tags,
running [atc1441](https://github.com/atc1441)'s **ATC_BLE_OEPL** firmware (Bluetooth name `ATC_xxxxxx`).

It does two jobs:

1. **Bridge for the web page.** Plugged into a computer by USB, it carries uploads from the
   companion web page, [ESL Uploader](https://andu.ro/esl-uploader/) (picture, formatted text, stats screen) to the tag over Bluetooth. Handy when
   the computer has no Bluetooth, or its Bluetooth is unreliable.
2. **Automatic stats refresh.** It keeps a saved list of tags and every 1–60 minutes reads each tag's
   battery and temperatures, draws its stats screen and uploads it, even with the page closed, as long
   as it stays plugged into the computer (which sets its clock). The ESLs can't redraw on their own,
   so something has to send the new screen.

The stats screen shows the tag's Bluetooth name (so you can find it), screen and board temperature,
battery level and the time of the last update. The layout adapts to 2.13″ (250×122), 4.2″
(400×300) and 5.83″ (648×480) tags, landscape or portrait.

## What you need

- **An ESP32 board with Bluetooth LE**: ESP32, ESP32-S3, ESP32-C3 or ESP32-C6. The **ESP32-S2
  won't work** (no Bluetooth). Tested on a Seeed **XIAO ESP32-C3** (plenty for
  this job, and the cheapest) and a **XIAO ESP32-S3**. The XIAO ESP32-C3 has **no antenna on the board**:
  attach the small U.FL antenna that comes with it, or Bluetooth reaches only a few centimetres.
- **ESLs running ATC_BLE_OEPL.** For Hanshow ESLs with a Telink chip, atc1441's
  [web tool](https://atc1441.github.io/ATC_BLE_OEPL_Image_Upload.html) flashes it (UART Flasher tab).
  Tested on Hanshow 2.13″ (black/white and black/white/red), 4.2″ and 5.83″ black/white/red ESLs.
- A **data** USB cable (some cables only charge).

## Installing

1. **Arduino IDE** (2.x) → *Boards Manager* → install **esp32** by Espressif.
2. *Library Manager* → install **NimBLE-Arduino** (2.x) and **Adafruit GFX Library**.
3. Download this repository and open `ESP32_ATC_OEPL_Bridge.ino`. The folder must be named
   `ESP32_ATC_OEPL_Bridge` (a GitHub ZIP unpacks as `ESP32_ATC_OEPL_Bridge-main`: rename it, or let
   the IDE move the sketch). `render.h` and `fonts.h` must stay next to the `.ino`.
4. *Tools* menu:
   - **Board**: yours, e.g. *XIAO_ESP32C3*, *XIAO_ESP32S3*, *ESP32 Dev Module*, *ESP32C3 Dev Module*.
   - **Partition Scheme**: leave the default. The sketch is ~850 KB and fits every board's 1.2 MB.
   - **USB CDC On Boot**: *Enabled* if the board's USB socket is wired straight to the ESP32 chip
     (XIAO ESP32-S3/C3/C6, "SuperMini" boards, the USB port of an S3/C3 DevKit). *Disabled* if it
     goes through a USB-serial chip (CP2102, CH340: most classic ESP32 boards, the UART port of
     DevKits). If unsure, try one: with the wrong setting the serial monitor stays silent.
5. Upload. The serial monitor (115200 baud) should print `ESP32_ATC_OEPL_Bridge ready.`

With `arduino-cli`:

```sh
arduino-cli compile --upload -p /dev/ttyACM0 --fqbn esp32:esp32:XIAO_ESP32C3 .
arduino-cli compile --upload -p /dev/ttyACM0 --fqbn esp32:esp32:XIAO_ESP32S3 .
arduino-cli compile --upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 .
```

## Using it with the web page

The companion web page is **[ESL Uploader](https://andu.ro/esl-uploader/)**. Open it in
Chrome, Edge, Brave (or Firefox 151+ for USB) and choose **Via ESP32 (USB)** → **Connect** → pick the
board's port → **Scan**. Then:

- **Picture / Text**: the page renders the image and sends it through the ESP32 (`img` command).
- **Stats screen**: the ESP32 reads the tag and draws the screen itself (`stats` command). The same
  drawing code runs in the page, so the preview matches the tag pixel for pixel.
- **Automatic refresh**: tick *Keep this tag's stats screen up to date* and pick the interval. The
  list and interval are saved on the ESP32 and survive a power cut. You can close the page: the ESP32
  keeps refreshing while it stays plugged into the computer.

Every refresh redraws the whole screen (~15 s on black/white/red tags), and that is what uses the
tag's battery. 10 minutes or more is a good balance.

### The clock

The board has no battery-backed clock and never uses WiFi: it only bridges USB to Bluetooth. The
computer it's plugged into sets the clock with `time` (and `tz`): the web page does it every time it
connects; a script driving the board should do it at start-up and every few hours. After a power cut
the stats screens show `Updated --:--` until the computer sets the clock again.

## Serial commands

One per line, 115200 baud. The web page uses the same commands.

| Command | What it does |
|---|---|
| `scan` | List ATC tags nearby (name, MAC, RSSI) |
| `target <name\|mac>` | Default tag for `send` (saved) |
| `info [name\|mac]` | Connect and print the tag's display buffer size and colours |
| `img <name\|mac\|-> <len>` | Receive `<len>` raw buffer bytes (prints `READY`, then `ACK <n>` per 1 KB) and upload them |
| `imgz <name\|mac\|-> <len>` | Same, for a payload that is already compressed (data type `0x30`; the web page sends this) |
| `rot <name> [0-3]` | Per-tag rotation of drawn text/stats, clockwise quarter turns (saved) |
| `send <spec>` / `to <name> <spec>` | Draw formatted text (see below) and upload it |
| `stats <name\|mac\|->` | Read battery + temperatures, draw the stats screen, upload; prints `STATS name mV board screen time` |
| `auto` | Show the auto-refresh list, interval and last refresh |
| `auto <min> <tag,tag\|all>` | Refresh those tags every `<min>` minutes (saved; first refresh right away) |
| `auto add <tag>` / `auto del <tag>` / `auto off` | Edit / stop the list |
| `time <unix-utc>` | Set the clock |
| `tz <POSIX TZ>` | Time zone (default Romania: `EET-2EEST,M3.5.0/3,M10.5.0/4`) |

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
- Uploads are compressed (data type `0x30`, `deflate.h`): raw size (u32 LE) + zlib with a 4 KB window
  of `06`, width, height (u16 LE), planes (1 or 2), then the buffer. A 5.83″ stats screen shrinks from
  76 KB to ~5 KB. The tags' decoder needs a single dynamic-Huffman deflate block (fixed Huffman codes
  hang it, a second block is ignored), so the encoder makes two passes and writes one block. The raw
  types (`0x20` BW / `0x21` BWR) are only used when compression doesn't make an image smaller.
- The image version in the `0064` data info is CRC32 over [size u16 LE, byte sum u16 LE] + data, as
  atc1441's uploader computes it. The tags check it on uploads longer than one block: with a plain
  CRC32 they stall or redraw an older image.
- After `0003` the sketch stays connected until the tag acks it (`0063`, ~4 s on a 5.83″). Hanging up
  sooner makes the tag draw the previous image.
- `0016 0000` → `00D1` + battery mV, raw ADC, chip temperature, panel temperature. The stats command
  reads it on its own connection, then reconnects for the upload: an upload on the same connection
  after `0016` is not displayed.
- Images over 64 KB (the 5.83″): before asking for the first block the tag erases flash for the whole
  image (~2.3 s). Now and then it asks after ~0.2 s instead; that image would be lost. The sketch
  notices, sends nothing, and tries again on a new connection after 5, 20 and 45 s.

Image buffer: black plane, then red plane (1 = ink), row-major, MSB first. The 2.13″ panel shows its
buffer turned 90° clockwise. With the UC controller (screen type 19) the buffer is 128×250 and the
bottom 6 rows of the landscape view are hidden; with the SSD controller (screen type 16) it is 128×256
and the left 6 columns and bottom 6 rows are hidden (measured; `panelBaseHidden()` in the sketch).
Both show 250×122, and text and stats are drawn only there. The 4.2″ (400×300) and 5.83″ (648×480)
buffers are shown as they are; give those tags `rot <tag> 1` for landscape (the web page does it).

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
  within a few metres; small boards have short-range antennas. On a XIAO ESP32-C3 check that its
  U.FL antenna is attached. A tag takes one connection at a time.
- **A tag stays invisible for minutes**: an upload that was cut off (board unplugged, out of range)
  can leave a tag silent for a few minutes. It comes back on its own; an NFC scan may wake it sooner.
- **"The tag didn't prepare in 4 attempts" (5.83″)**: the tag kept refusing to get ready for a
  big image for over a minute. Try again later; the next automatic refresh usually works.

## Files

- `ESP32_ATC_OEPL_Bridge.ino`: Bluetooth, upload protocol, serial commands, auto refresh, clock
- `render.h`: formatted text and stats-screen drawing. The web page has a line-by-line JavaScript
  port (`render.js`); `tools/render_test.cpp` and `tools/render_test.mjs` check they produce identical
  pixels. The JS test needs the page's `render.js` and `fonts.js`: `RENDER_JS=/path/to/render.js
  node tools/render_test.mjs stats 250 128`.
- `deflate.h`: the zlib encoder for compressed uploads (one dynamic-Huffman block, 4 KB window). The
  web page has the same encoder in JavaScript (`oepl.js`); `tools/deflate_test.cpp` and
  `tools/deflate_test.mjs` check they give identical bytes
  (`OEPL_JS=/path/to/oepl.js node tools/deflate_test.mjs < in > out`).
- `fonts.h`: bitmap fonts, generated by `tools/gen_fonts.py` from GNU FreeFont. Don't edit it by hand.
- `tools/fonts_to_js.py`: exports `fonts.h` for the web page.

## Credits and licence

- [atc1441 (Aaron Christophel)](https://github.com/atc1441) for the ATC_BLE_OEPL firmware and its
  upload protocol.
- Code: MIT licence, see `LICENSE`. Fonts in `fonts.h`: GNU FreeFont, GPLv3 with font exception.
