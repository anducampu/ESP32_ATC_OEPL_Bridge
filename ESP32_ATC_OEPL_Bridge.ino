/*
 * ESP32_ATC_OEPL_Bridge — ESP32 board → electronic shelf labels (ESL) running atc1441's ATC_BLE_OEPL firmware.
 * https://github.com/anducampu/ESP32_ATC_OEPL_Bridge — see README.md. MIT licence; fonts: GNU FreeFont (see LICENSE).
 *
 * Renders formatted text or a stats screen (render.h) and uploads it over BLE using the same protocol as
 * https://atc1441.github.io/ATC_BLE_OEPL_Image_Upload.html (service/characteristic 0x1337):
 *
 *   host → 0005                 display info  → tag notifies 0005 + payload (w/h/colors)
 *   host → 0064 + AvailDataInfo  "I have data" → tag notifies 00C6 block request
 *   host → 0002, then 0065 + part (233 B) per part; tag answers 00C5 (ok, next) / 00C4 (resend)
 *   ... more 00C6 per 4 KB block ...  → tag notifies 00C7 (done) or 00C8 (already shown)
 *   host → 0003                 draw
 *
 * Image format (data type 0x20 BW / 0x21 BWR): buffer of Wb×Hb pixels, row-major, MSB first,
 * bits continuous across rows. Plane 1: 1 = black. Plane 2 (colour tags): 1 = red.
 * The panel shows the buffer rotated 90° clockwise, so we draw on a landscape canvas
 * (Wv = Hb, Hv = Wb) and rotate it back while packing.
 *
 * Status: host → 0016 0000 → tag notifies 00D1 + battery mV (BE16) + raw ADC (BE16) +
 * chip temp (int8 °C) + panel temp (int8 °C).
 *
 * Libraries: NimBLE-Arduino 2.x, Adafruit GFX.
 * Boards: any ESP32 with Bluetooth LE — ESP32, ESP32-S3, ESP32-C3, ESP32-C6 (not the S2). Tested on a
 * Seeed XIAO ESP32-C3 and XIAO ESP32-S3; fits the default partition scheme (~850 KB). USB CDC On Boot = Enabled only
 * if the board's USB socket is wired straight to the chip (XIAO, SuperMini…); leave it Disabled if
 * the socket goes through a USB-serial chip (CP2102/CH340). Serial 115200.
 *
 * Serial commands (one per line):
 *   scan                       list ATC tags nearby
 *   target <name|mac>          set default tag (saved in flash), e.g. target ATC_583740
 *   info [name|mac]            connect and print display info
 *   img <name|mac|-> <len>     receive <len> raw buffer bytes (web page upload), then send them
 *   rot <name> <0-3>           per-tag extra rotation, clockwise quarter turns (saved), e.g.
 *                              rot ATC_448A42 1   — rot <name> alone shows the current value
 *   send <spec>                render and upload to the target
 *   to <name|mac> <spec>       same, for a specific tag
 *   stats <name|mac|->         read battery + temperatures, upload the stats screen
 *   auto                       show the auto-refresh list; auto off; auto <min> <tag,tag|all>;
 *                              auto add <tag> / auto del <tag>  (saved; refreshes the stats
 *                              screen of those tags every <min> minutes, default 10)
 *   time <unix-utc>            set the clock (the web page does this when it connects)
 *   tz <POSIX TZ>              time zone, default EET-2EEST,M3.5.0/3,M10.5.0/4 (Romania)
 *
 * <spec>: lines separated by '|'. Each line may start with {flags}:
 *   1..9  font (FreeSans, with Romanian ăâîșț ĂÂÎȘȚ, °, ℃ and €):
 *         1 = Sans 7pt, 2 = Sans 9pt, 3 = Bold 12pt, 4 = Bold 18pt, 5 = Bold 24pt,
 *         6 = Bold 32pt, 7 = Bold 40pt, 8 = Bold 52pt, 9 = Bold 64pt
 *         A line too wide for the screen automatically drops to the largest size that fits.
 *   r     red text        b  black text (default)
 *   i     inverted bar (white text on a full-width black — or red with r — band)
 *   <  =  >  align left / center (default) / right
 *   @n @-n  move this line n px down / up from its place (the web page sets it when you drag a line)
 *   Example:  send {3}Mere Golden|{5r}4,99 lei|{1}preț/kg - ofertă valabilă azi
 */

#include <NimBLEDevice.h>
#include <Adafruit_GFX.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <functional>
#include "render.h"  // fonts, formatted text, stats screen (shared with the web page's render.js)

static const NimBLEUUID kUuid((uint16_t)0x1337);
static const uint16_t kManufacturerId = 0x1337;
static const size_t kBlockSize = 4096;
static const size_t kPartSize = 230;

struct Notif {
  uint8_t len;
  uint8_t data[64];
};
static QueueHandle_t notifQueue;
static NimBLEClient *client = nullptr;
static NimBLERemoteCharacteristic *chr = nullptr;
static Preferences prefs;
static String targetTag;
static String connectedName;  // name of the tag found by the last findTag()

struct DisplayInfo {
  uint16_t wb, hb;  // buffer geometry as the tag expects it
  uint8_t colors;
};

// ---------------------------------------------------------------- BLE plumbing

static void onNotify(NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
  Notif n;
  n.len = len > sizeof(n.data) ? sizeof(n.data) : len;
  memcpy(n.data, data, n.len);
  xQueueSend(notifQueue, &n, 0);
}

static bool waitNotif(Notif &n, uint32_t timeoutMs) {
  return xQueueReceive(notifQueue, &n, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

static bool sendRaw(const uint8_t *data, size_t len) {
  if (!chr) return false;
  return chr->writeValue(data, len, false);
}

static bool sendCmd(uint16_t cmd, const uint8_t *payload = nullptr, size_t len = 0) {
  uint8_t buf[260];
  buf[0] = cmd >> 8;
  buf[1] = cmd & 0xFF;
  if (len) memcpy(buf + 2, payload, len);
  return sendRaw(buf, 2 + len);
}

static bool isAtcTag(const NimBLEAdvertisedDevice *d) {
  if (d->getName().rfind("ATC_", 0) == 0) return true;
  if (d->haveManufacturerData()) {
    std::string m = d->getManufacturerData();
    if (m.size() >= 2 && (uint8_t)m[0] == (kManufacturerId & 0xFF) && (uint8_t)m[1] == (kManufacturerId >> 8))
      return true;
  }
  return false;
}

static void cmdScan() {
  Serial.println("Scanning 6 s...");
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults res = scan->getResults(6000, false);
  int found = 0;
  for (int i = 0; i < res.getCount(); i++) {
    const NimBLEAdvertisedDevice *d = res.getDevice(i);
    if (!isAtcTag(d)) continue;
    Serial.printf("  %-14s  %s  RSSI %d\n", d->getName().c_str(), d->getAddress().toString().c_str(), d->getRSSI());
    found++;
  }
  Serial.printf("%d tag(s) found.\n", found);
  scan->clearResults();
}

// Find the tag by name or MAC in a fresh scan (we need the address type, so a scan is simplest).
static bool findTag(const String &want, NimBLEAddress &addr) {
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults res = scan->getResults(6000, false);
  bool ok = false;
  for (int i = 0; i < res.getCount() && !ok; i++) {
    const NimBLEAdvertisedDevice *d = res.getDevice(i);
    if (!isAtcTag(d)) continue;
    String name = d->getName().c_str();
    String mac = d->getAddress().toString().c_str();
    if (want.length() == 0 || name.equalsIgnoreCase(want) || mac.equalsIgnoreCase(want)) {
      addr = d->getAddress();
      connectedName = name;
      Serial.printf("Using %s (%s), RSSI %d\n", name.c_str(), mac.c_str(), d->getRSSI());
      ok = true;
    }
  }
  scan->clearResults();
  return ok;
}

static void disconnectTag() {
  if (client && client->isConnected()) client->disconnect();
  chr = nullptr;
}

static bool connectTag(const String &want) {
  NimBLEAddress addr;
  if (!findTag(want, addr)) {
    Serial.printf("Tag '%s' not found.\n", want.c_str());
    return false;
  }
  if (!client) {
    client = NimBLEDevice::createClient();
    client->setConnectTimeout(10 * 1000);
  }
  for (int attempt = 1; attempt <= 3; attempt++) {
    if (client->connect(addr)) break;
    Serial.printf("Connect attempt %d failed\n", attempt);
    if (attempt == 3) return false;
    delay(300);
  }
  Serial.printf("Connected, MTU %u\n", client->getMTU());
  if (client->getMTU() < 2 + 3 + kPartSize + 3) {
    Serial.println("MTU too small for 235-byte block parts.");
    disconnectTag();
    return false;
  }
  NimBLERemoteService *svc = client->getService(kUuid);
  chr = svc ? svc->getCharacteristic(kUuid) : nullptr;
  if (!chr || !chr->subscribe(true, onNotify)) {
    Serial.println("Service/characteristic 0x1337 not found or subscribe failed.");
    disconnectTag();
    return false;
  }
  delay(500);  // the tag ignores commands sent right after subscribing
  xQueueReset(notifQueue);
  return true;
}

static bool readDisplayInfo(DisplayInfo &di) {
  xQueueReset(notifQueue);
  sendCmd(0x0005);
  Notif n;
  uint32_t until = millis() + 3000;
  while ((int32_t)(until - millis()) > 0) {
    if (!waitNotif(n, 200)) continue;
    if (n.len < 33 || n.data[0] != 0x00 || n.data[1] != 0x05) continue;
    const uint8_t *p = n.data + 2;
    bool whInverted = p[19] == 1;
    uint16_t a = p[22] | (p[23] << 8);
    uint16_t b = p[24] | (p[25] << 8);
    di.wb = whInverted ? a : b;
    di.hb = whInverted ? b : a;
    di.colors = p[30];
    return di.wb && di.hb && di.wb < 1000 && di.hb < 1000;
  }
  return false;
}

// ---------------------------------------------------------------- upload

static uint32_t crc32(const uint8_t *d, size_t n) {
  uint32_t c = 0xFFFFFFFF;
  while (n--) {
    c ^= *d++;
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
  }
  return ~c;
}

static bool sendBlock(const uint8_t *img, size_t imgLen, uint8_t blockId) {
  size_t start = (size_t)blockId * kBlockSize;
  if (start >= imgLen) return false;
  size_t len = imgLen - start < kBlockSize ? imgLen - start : kBlockSize;

  // block = [len16 LE][sum16 LE][data]
  static uint8_t block[kBlockSize + 4];
  uint16_t sum = 0;
  for (size_t i = 0; i < len; i++) sum += img[start + i];
  block[0] = len & 0xFF;
  block[1] = len >> 8;
  block[2] = sum & 0xFF;
  block[3] = sum >> 8;
  memcpy(block + 4, img + start, len);
  size_t blockLen = len + 4;
  int parts = (blockLen + kPartSize - 1) / kPartSize;

  sendCmd(0x0002);
  delay(50);
  for (int part = 0; part < parts;) {
    uint8_t pkt[3 + kPartSize] = {0};
    pkt[1] = blockId;
    pkt[2] = part;
    size_t off = part * kPartSize;
    size_t n = blockLen - off < kPartSize ? blockLen - off : kPartSize;
    memcpy(pkt + 3, block + off, n);
    uint8_t c = 0;
    for (size_t i = 1; i < sizeof(pkt); i++) c += pkt[i];
    pkt[0] = c;
    sendCmd(0x0065, pkt, sizeof(pkt));

    Notif r;
    if (!waitNotif(r, 5000)) {
      Serial.printf("Timeout on block %u part %d\n", blockId, part);
      return false;
    }
    if (r.len >= 2 && r.data[0] == 0x00 && r.data[1] == 0x63) {
      continue;  // plain command ack (e.g. for 0002), keep waiting for the part ack
    } else if (r.len >= 2 && r.data[0] == 0x00 && r.data[1] == 0xC5) {
      part++;
    } else if (r.len >= 2 && r.data[0] == 0x00 && r.data[1] == 0xC4) {
      Serial.printf("Part %d rejected, resending\n", part);
      delay(100);
    } else {
      // Anything else (e.g. a fresh 00C6) — hand it back to the main loop.
      xQueueSendToFront(notifQueue, &r, 0);
      return true;
    }
  }
  Serial.printf("Block %u sent (%d parts)\n", blockId, parts);
  return true;
}

static bool uploadImage(const uint8_t *img, size_t imgLen, bool color) {
  uint32_t ver = crc32(img, imgLen);
  // AvailDataInfo: checksum u8, dataVer u64, dataSize u32, dataType u8, dataTypeArg u8, nextCheckIn u16
  uint8_t info[17] = {0};
  info[0] = 0xFF;
  memcpy(info + 1, &ver, 4);  // ESP32 is little-endian; upper 4 bytes of dataVer stay 0
  uint32_t sz = imgLen;
  memcpy(info + 9, &sz, 4);
  info[13] = color ? 0x21 : 0x20;
  xQueueReset(notifQueue);
  sendCmd(0x0064, info, sizeof(info));

  uint32_t t0 = millis();
  while (true) {
    Notif n;
    if (!waitNotif(n, 10000)) {
      Serial.println("Timeout waiting for tag.");
      return false;
    }
    if (n.len < 2 || n.data[0] != 0x00) continue;
    switch (n.data[1]) {
      case 0xC6:  // block request: 00C6 checksum ver[8] blockId ...
        if (n.len >= 12 && !sendBlock(img, imgLen, n.data[11])) return false;
        break;
      case 0xC7:
      case 0xC8:
        Serial.printf(n.data[1] == 0xC7 ? "Upload OK (%lu ms), drawing...\n" : "Tag already has this image, drawing...\n",
                      millis() - t0);
        sendCmd(0x0003);
        return true;
      default:
        break;  // 0063 command acks etc.
    }
  }
}

// ---------------------------------------------------------------- drawing → tag

// Per-tag rotation, stored in NVS as "r_<name>" (clockwise quarter turns of the drawn text).
static uint8_t tagRotation(const String &name) {
  return prefs.getUChar(("r_" + name).c_str(), 0) & 3;
}

// Map a buffer pixel (xb, yb) back to the drawing canvas, honouring the panel's quarter turn
// plus the tag's extra rotation `rot`.
static bool canvasPixel(GFXcanvas1 &c, int xb, int yb, int Wb, int Hb, uint8_t rot) {
  // Panel turn: base canvas (xc, yc), Wc = Hb, lives at buffer (yc, Wc-1-xc).
  int xc = Hb - 1 - yb, yc = xb;
  // The canvas we drew on is turned `rot` x 90 deg clockwise onto the base canvas.
  int x, y;
  switch (rot) {
    case 1: x = yc; y = c.height() - 1 - xc; break;
    case 2: x = c.width() - 1 - xc; y = c.height() - 1 - yc; break;
    case 3: x = c.width() - 1 - yc; y = xc; break;
    default: x = xc; y = yc; break;
  }
  return c.getPixel(x, y);
}

// Rows/columns of the base landscape canvas a panel doesn't show. 2.13" (buffer 128×250): rows
// 122–127, measured with a test pattern on a Hanshow 2.13" BWR (UC, type 19) — the top edge is
// visible. Other panels: add them here (and to MODELS in the web page's oepl.js).
static Insets panelBaseHidden(const DisplayInfo &di) {
  Insets i;
  if (di.wb == 128 && di.hb == 250) i.b = 6;
  return i;
}

// Connect, read display info, draw with `draw(blk, red, color)` on a canvas sized for the tag and
// its rotation, pack and upload. `before` runs after connecting (e.g. to read the status).
static bool drawAndUpload(const String &tag, const std::function<void()> &before,
                          const std::function<void(GFXcanvas1 &, GFXcanvas1 &, bool, Insets)> &draw) {
  if (!connectTag(tag)) return false;
  DisplayInfo di;
  if (!readDisplayInfo(di)) {
    Serial.println("No display info from tag.");
    disconnectTag();
    return false;
  }
  bool color = di.colors >= 2;
  Serial.printf("Display buffer %ux%u, colors %u\n", di.wb, di.hb, di.colors);
  before();

  uint8_t rot = tagRotation(connectedName);
  int cw = rot & 1 ? di.wb : di.hb, ch = rot & 1 ? di.hb : di.wb;
  GFXcanvas1 blk(cw, ch), red(cw, ch);
  Serial.printf("Canvas %dx%d, rotation %u x 90 deg CW\n", cw, ch, rot);
  if (!blk.getBuffer() || !red.getBuffer()) {
    Serial.println("Out of memory for canvas.");
    disconnectTag();
    return false;
  }
  draw(blk, red, color, rotateInsets(panelBaseHidden(di), rot));  // draw only where the panel shows

  size_t plane = ((size_t)di.wb * di.hb + 7) / 8;
  size_t len = plane * (color ? 2 : 1);
  uint8_t *img = (uint8_t *)calloc(len, 1);
  if (!img) {
    Serial.println("Out of memory for image.");
    disconnectTag();
    return false;
  }
  size_t bit = 0;
  for (int yb = 0; yb < di.hb; yb++) {
    for (int xb = 0; xb < di.wb; xb++, bit++) {
      bool r = color && canvasPixel(red, xb, yb, di.wb, di.hb, rot);
      bool k = !r && canvasPixel(blk, xb, yb, di.wb, di.hb, rot);
      if (k) img[bit >> 3] |= 0x80 >> (bit & 7);
      if (r) img[plane + (bit >> 3)] |= 0x80 >> (bit & 7);
    }
  }
  Serial.printf("Uploading %u bytes (%s)...\n", (unsigned)len, color ? "BWR" : "BW");
  bool ok = uploadImage(img, len, color);
  free(img);
  delay(500);  // let the 0003 go out before dropping the link
  disconnectTag();
  Serial.println(ok ? "Done. (BWR refresh takes ~15 s)" : "Upload failed.");
  return ok;
}

static void cmdSend(const String &tag, const String &spec) {
  std::string s(spec.c_str());
  drawAndUpload(tag, [] {}, [&](GFXcanvas1 &blk, GFXcanvas1 &red, bool color, Insets hid) {
    renderSpec(s, blk, red, color, hid);
  });
}

// ---------------------------------------------------------------- stats screen

// 0016 → 00D1: battery mV (BE16), raw ADC (BE16), chip temp (int8), panel temp (int8).
static bool readStatus(Stats &st) {
  xQueueReset(notifQueue);
  const uint8_t arg[2] = {0, 0};
  sendCmd(0x0016, arg, 2);
  Notif n;
  uint32_t until = millis() + 3000;
  while ((int32_t)(until - millis()) > 0) {
    if (!waitNotif(n, 200)) continue;
    if (n.len < 8 || n.data[0] != 0x00 || n.data[1] != 0xD1) continue;
    st.mv = (n.data[2] << 8) | n.data[3];
    st.boardC = (int8_t)n.data[6];
    st.screenC = (int8_t)n.data[7];
    return true;
  }
  return false;
}

static bool timeKnown() { return time(nullptr) > 1700000000; }

static std::string nowText() {
  if (!timeKnown()) return "";
  time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  char b[16];
  strftime(b, sizeof b, "%H:%M", &lt);
  return b;
}

// stats <tag>: read battery + temperatures, draw the stats screen, upload. Prints one
// "STATS <name> <mV> <board°C> <screen°C> <HH:MM|-->" line for the web page.
static bool cmdStats(const String &tag) {
  Stats st;
  bool ok = drawAndUpload(
      tag,
      [&] {
        st.name = connectedName.c_str();
        if (!readStatus(st)) Serial.println("No status from tag (battery/temperature unknown).");
        st.updated = nowText();
        Serial.printf("STATS %s %d %d %d %s\n", st.name.c_str(), st.mv, st.boardC, st.screenC,
                      st.updated.empty() ? "--" : st.updated.c_str());
      },
      [&](GFXcanvas1 &blk, GFXcanvas1 &red, bool color, Insets hid) { renderStats(st, blk, red, color, hid); });
  return ok;
}

// ---------------------------------------------------------------- clock (set by the computer)

static String tzString;

static void applyTz() {
  setenv("TZ", tzString.c_str(), 1);
  tzset();
}

// ---------------------------------------------------------------- auto refresh

static uint16_t autoMinutes = 0;  // 0 = off
static String autoTags;          // comma separated names, or "all"
static uint32_t nextAuto = 0;
static String lastAuto;  // e.g. "19:02 ok 1/1" — when the last automatic refresh ran, how it went

static void printAuto() {
  if (!autoMinutes || !autoTags.length()) Serial.println("AUTO off");
  else Serial.printf("AUTO %u %s last %s\n", autoMinutes, autoTags.c_str(), lastAuto.length() ? lastAuto.c_str() : "never");
}

static void saveAuto() {
  prefs.putUShort("auto_min", autoMinutes);
  prefs.putString("auto_tags", autoTags);
  nextAuto = millis();  // first refresh right away
  printAuto();
}

static bool listHas(const String &list, const String &name) {
  return ("," + list + ",").indexOf("," + name + ",") >= 0;
}

// A tag that is still busy redrawing (or briefly out of range) isn't found: try once more.
static bool statsWithRetry(const String &tag) {
  if (cmdStats(tag)) return true;
  Serial.printf("Retrying %s in 20 s...\n", tag.c_str());
  delay(20000);
  return cmdStats(tag);
}

static void runAuto() {
  Serial.printf("AUTO run: %s\n", autoTags.c_str());
  int ok = 0, total = 0;
  if (autoTags == "all") {
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    NimBLEScanResults res = scan->getResults(6000, false);
    String names;
    for (int i = 0; i < res.getCount(); i++)
      if (isAtcTag(res.getDevice(i))) names += String(names.length() ? "," : "") + res.getDevice(i)->getName().c_str();
    scan->clearResults();
    for (int a = 0, b; a < (int)names.length(); a = b + 1) {
      b = names.indexOf(',', a);
      if (b < 0) b = names.length();
      total++;
      ok += statsWithRetry(names.substring(a, b));
    }
  } else {
    for (int a = 0, b; a < (int)autoTags.length(); a = b + 1) {
      b = autoTags.indexOf(',', a);
      if (b < 0) b = autoTags.length();
      total++;
      ok += statsWithRetry(autoTags.substring(a, b));
    }
  }
  char when[24];
  if (timeKnown()) snprintf(when, sizeof when, "%s", nowText().c_str());
  else snprintf(when, sizeof when, "uptime-%lum", (unsigned long)(millis() / 60000));
  lastAuto = String(when) + " ok " + ok + "/" + total;
  Serial.printf("AUTO run done: %s\n", lastAuto.c_str());
}

// auto                      show            auto off           stop
// auto <min> <tags|all>     set list        auto add|del <tag> edit the list (add starts at 10 min)
static void cmdAuto(const String &arg) {
  int sp = arg.indexOf(' ');
  String a = sp < 0 ? arg : arg.substring(0, sp), rest = sp < 0 ? "" : arg.substring(sp + 1);
  rest.trim();
  if (!arg.length()) { printAuto(); return; }
  if (a == "off") { autoMinutes = 0; autoTags = ""; saveAuto(); return; }
  if (a == "add" && rest.length()) {
    if (autoTags == "all") { printAuto(); return; }
    if (!listHas(autoTags, rest)) autoTags += String(autoTags.length() ? "," : "") + rest;
    if (!autoMinutes) autoMinutes = 10;
    saveAuto();
    return;
  }
  if (a == "del" && rest.length()) {
    String out;
    for (int i = 0, j; i < (int)autoTags.length(); i = j + 1) {
      j = autoTags.indexOf(',', i);
      if (j < 0) j = autoTags.length();
      String t = autoTags.substring(i, j);
      if (t != rest && t.length()) out += String(out.length() ? "," : "") + t;
    }
    autoTags = out;
    saveAuto();
    return;
  }
  int m = a.toInt();
  if (m >= 1 && m <= 1440 && rest.length()) {
    autoMinutes = m;
    autoTags = rest;
    autoTags.replace(" ", "");
    saveAuto();
    return;
  }
  Serial.println("usage: auto | auto off | auto <minutes> <tag,tag|all> | auto add <tag> | auto del <tag>");
}

// img <tag> <len>: receive a ready-made buffer (as packed by the web page: plane 1 = black,
// optional plane 2 = red, in the tag's buffer orientation) and upload it unchanged.
// Handshake: we print "READY <len>", the host sends the bytes in chunks of up to 1024 and waits
// for "ACK <total>" after each one. Tag "-" means the saved target.
static void cmdImg(const String &arg) {
  int sp = arg.lastIndexOf(' ');
  String tag = sp < 0 ? "" : arg.substring(0, sp);
  long len = sp < 0 ? arg.toInt() : arg.substring(sp + 1).toInt();
  tag.trim();
  if (tag == "-") tag = targetTag;
  if (len <= 0 || len > 128 * 1024) {
    Serial.println("ERR bad length");
    return;
  }
  uint8_t *img = (uint8_t *)malloc(len);
  if (!img) {
    Serial.println("ERR out of memory");
    return;
  }
  Serial.printf("READY %ld\n", len);
  long got = 0;
  uint32_t last = millis();
  while (got < len) {
    int n = Serial.read(img + got, min((long)Serial.available(), len - got));
    if (n > 0) {
      long before = got;
      got += n;
      last = millis();
      if (got / 1024 != before / 1024 || got == len) Serial.printf("ACK %ld\n", got);
    } else if (millis() - last > 5000) {
      Serial.printf("ERR receive timeout at %ld\n", got);
      free(img);
      return;
    } else {
      delay(1);
    }
  }
  if (!connectTag(tag)) {
    Serial.println("ERR tag not found or connect failed");
    free(img);
    return;
  }
  DisplayInfo di;
  if (!readDisplayInfo(di)) {
    Serial.println("ERR no display info from tag");
    disconnectTag();
    free(img);
    return;
  }
  size_t plane = ((size_t)di.wb * di.hb + 7) / 8;
  Serial.printf("Display buffer %ux%u, colors %u\n", di.wb, di.hb, di.colors);
  bool color = (size_t)len == 2 * plane;
  if (!color && (size_t)len != plane) {
    Serial.printf("ERR image is %ld bytes, this tag needs %u (BW) or %u (BWR)\n", len, (unsigned)plane, (unsigned)(2 * plane));
    disconnectTag();
    free(img);
    return;
  }
  Serial.printf("Uploading %ld bytes (%s)...\n", len, color ? "BWR" : "BW");
  bool ok = uploadImage(img, len, color);
  free(img);
  delay(500);
  disconnectTag();
  Serial.println(ok ? "Done. (BWR refresh takes ~15 s)" : "Upload failed.");
}

static void cmdInfo(const String &arg) {
  if (!connectTag(arg.length() ? arg : targetTag)) return;
  DisplayInfo di;
  if (readDisplayInfo(di))
    Serial.printf("Buffer %ux%u (canvas %ux%u), colors %u\n", di.wb, di.hb, di.hb, di.wb, di.colors);
  else
    Serial.println("No display info.");
  disconnectTag();
}

// ---------------------------------------------------------------- main

void setup() {
  Serial.setRxBufferSize(16384);
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Native USB (S3/C3/C6…): never stall when nobody reads the port (board on a charger or page
  // closed): give up on a full output buffer after 20 ms. A reading host drains it much faster.
  // Boards with a USB-UART chip (CP2102, CH340) don't need this: a UART never blocks.
  Serial.setTxTimeoutMs(20);
#endif
  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) delay(10);
  notifQueue = xQueueCreate(16, sizeof(Notif));
  NimBLEDevice::init("");
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(9);
  prefs.begin("oepl", false);
  targetTag = prefs.getString("target", "");
  tzString = prefs.getString("tz", "EET-2EEST,M3.5.0/3,M10.5.0/4");
  autoMinutes = prefs.getUShort("auto_min", 0);
  autoTags = prefs.getString("auto_tags", "");
  applyTz();
  Serial.println("\nESP32_ATC_OEPL_Bridge ready. Commands: scan | target | info | img | rot | send | to | stats | auto | time | tz");
  Serial.printf("Target: %s\n", targetTag.length() ? targetTag.c_str() : "(first ATC tag found)");
  printAuto();
  // Give a connected page a few seconds to set the clock before the first auto refresh.
  nextAuto = millis() + 20000;
}

void loop() {
  if (autoMinutes && autoTags.length() && (int32_t)(millis() - nextAuto) >= 0) {
    nextAuto = millis() + autoMinutes * 60000UL;
    runAuto();
  }
  if (!Serial.available()) {
    delay(10);
    return;
  }
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (!line.length()) return;
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  arg.trim();

  if (cmd == "scan") cmdScan();
  else if (cmd == "target") {
    targetTag = arg;
    prefs.putString("target", targetTag);
    Serial.printf("Target = %s\n", targetTag.c_str());
  } else if (cmd == "rot") {
    int s2 = arg.indexOf(' ');
    String name = s2 < 0 ? arg : arg.substring(0, s2);
    if (!name.length()) Serial.println("usage: rot <name> [0-3]");
    else {
      if (s2 >= 0) prefs.putUChar(("r_" + name).c_str(), arg.substring(s2 + 1).toInt() & 3);
      Serial.printf("%s rotation = %u x 90 deg CW\n", name.c_str(), tagRotation(name));
    }
  } else if (cmd == "info") cmdInfo(arg);
  else if (cmd == "img") cmdImg(arg);
  else if (cmd == "send") cmdSend(targetTag, arg);
  else if (cmd == "to") {
    int s2 = arg.indexOf(' ');
    if (s2 < 0) Serial.println("usage: to <name|mac> <spec>");
    else cmdSend(arg.substring(0, s2), arg.substring(s2 + 1));
  } else if (cmd == "stats") cmdStats(arg == "-" || !arg.length() ? targetTag : arg);
  else if (cmd == "auto") cmdAuto(arg);
  else if (cmd == "time") {  // time <unix UTC seconds>
    struct timeval tv = {(time_t)strtoll(arg.c_str(), nullptr, 10), 0};
    if (tv.tv_sec > 1700000000) settimeofday(&tv, nullptr);
    Serial.printf("TIME %s\n", timeKnown() ? nowText().c_str() : "unknown");
  } else if (cmd == "tz") {  // tz <POSIX TZ>, e.g. EET-2EEST,M3.5.0/3,M10.5.0/4
    if (arg.length()) { tzString = arg; prefs.putString("tz", tzString); applyTz(); }
    Serial.printf("TZ %s\n", tzString.c_str());
  } else Serial.println("Unknown command.");
}
