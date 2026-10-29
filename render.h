// render.h — text and stats-screen drawing shared by the sketch and the host test
// (tools/render_test.cpp). The companion web page has a line-by-line JS port (render.js):
// keep both in sync (same integer maths, same order) so the preview matches the tag pixel for pixel.
//
// Canvas type C needs width(), height(), drawPixel(x, y, c), fillRect(x, y, w, h, c) — Adafruit's
// GFXcanvas1 on the ESP32. Drawing uses two planes: black and red (1 = ink).
#pragma once
#include <stdint.h>
#include <string>
#include "fonts.h"

// ---------------------------------------------------------------- text

// UTF-8 → font codes: ASCII stays, extra glyphs follow 0x7F in the order of EXTRA in
// tools/gen_fonts.py. Cedilla ş/ţ (common on old keyboards) map to the correct comma-below forms.
static const uint16_t kExtraChars[] = {0x0102, 0x0103, 0x00C2, 0x00E2, 0x00CE, 0x00EE,
                                       0x0218, 0x0219, 0x021A, 0x021B, 0x00B0, 0x20AC, 0x2103};

static uint8_t codeFor(uint32_t cp) {
  if (cp >= 0x20 && cp < 0x7F) return cp;
  if (cp == 0x015E) cp = 0x0218;  // Ş
  if (cp == 0x015F) cp = 0x0219;  // ş
  if (cp == 0x0162) cp = 0x021A;  // Ţ
  if (cp == 0x0163) cp = 0x021B;  // ţ
  if (cp == 0x2013 || cp == 0x2014) return '-';  // en/em dash
  if (cp == 0x201E || cp == 0x201C || cp == 0x201D) return '"';
  for (size_t i = 0; i < sizeof(kExtraChars) / sizeof(kExtraChars[0]); i++)
    if (kExtraChars[i] == cp) return 0x7F + i;
  return '?';
}

static std::string decodeUtf8(const std::string &in) {
  std::string out;
  for (size_t i = 0; i < in.size();) {
    uint8_t c = in[i];
    uint32_t cp;
    int n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
    else { i++; continue; }
    for (int k = 1; k < n && i + k < in.size(); k++) cp = (cp << 6) | (in[i + k] & 0x3F);
    i += n;
    out += (char)codeFor(cp);
  }
  return out;
}

static const TagGlyph &glyphFor(const TagFont &f, uint8_t code) {
  int idx = code - 0x20;
  if (idx < 0 || idx >= f.count) idx = '?' - 0x20;
  return f.glyphs[idx];
}

struct Ink { int x1, y1, w, h; };  // tight ink box relative to the pen at (0, baseline)

// t is already decoded (font codes).
static Ink textBounds(const TagFont &f, const std::string &t) {
  int pen = 0, minX = 32767, minY = 32767, maxX = -32768, maxY = -32768;
  for (size_t i = 0; i < t.size(); i++) {
    const TagGlyph &g = glyphFor(f, t[i]);
    if (g.width && g.height) {
      if (pen + g.xOffset < minX) minX = pen + g.xOffset;
      if (pen + g.xOffset + g.width > maxX) maxX = pen + g.xOffset + g.width;
      if (g.yOffset < minY) minY = g.yOffset;
      if (g.yOffset + g.height > maxY) maxY = g.yOffset + g.height;
    }
    pen += g.xAdvance;
  }
  if (minX > maxX) return {0, 0, 0, 0};
  return {minX, minY, maxX - minX, maxY - minY};
}

template <class C>
static void drawText(C &c, const TagFont &f, const std::string &t, int x, int baseline, uint16_t color) {
  for (size_t i = 0; i < t.size(); i++) {
    const TagGlyph &g = glyphFor(f, t[i]);
    const uint8_t *bm = f.bitmap + g.offset;
    uint32_t bit = 0;
    for (int yy = 0; yy < g.height; yy++)
      for (int xx = 0; xx < g.width; xx++, bit++)
        if (bm[bit >> 3] & (0x80 >> (bit & 7))) c.drawPixel(x + g.xOffset + xx, baseline + g.yOffset + yy, color);
    x += g.xAdvance;
  }
}

// ---------------------------------------------------------------- visible area

// Pixels of the canvas the panel doesn't show, per edge.
struct Insets { int t = 0, r = 0, b = 0, l = 0; };

// Turn a panel's hidden strip, given on the base landscape canvas, to the canvas the drawing is
// made on. That canvas is turned `rot` quarter turns clockwise *onto* the base canvas (the sketch's
// canvasPixel() and the page's viewToPicture() use the same convention): with rot = 1 the base's
// bottom strip ends up on the drawn canvas's right edge. Which strip a panel hides is panel data
// (the sketch's panelBaseHidden(), the page's MODELS[…].hidden), not the renderer's business.
static inline Insets rotateInsets(Insets b, int rot) {
  Insets p;
  switch (rot & 3) {
    case 1: p.t = b.r; p.r = b.b; p.b = b.l; p.l = b.t; break;
    case 2: p.t = b.b; p.r = b.l; p.b = b.t; p.l = b.r; break;
    case 3: p.t = b.l; p.r = b.t; p.b = b.r; p.l = b.b; break;
    default: p = b; break;
  }
  return p;
}

// A rectangle of a canvas that behaves like a canvas of its own (clipped, offset).
template <class C>
struct Area {
  C &c;
  int ox, oy, w, h;
  int width() const { return w; }
  int height() const { return h; }
  void drawPixel(int x, int y, uint16_t col) {
    if (x >= 0 && y >= 0 && x < w && y < h) c.drawPixel(x + ox, y + oy, col);
  }
  void fillRect(int x, int y, int fw, int fh, uint16_t col) {
    int xa = x < 0 ? 0 : x, ya = y < 0 ? 0 : y, xb = x + fw > w ? w : x + fw, yb = y + fh > h ? h : y + fh;
    if (xb > xa && yb > ya) c.fillRect(xa + ox, ya + oy, xb - xa, yb - ya, col);
  }
};

// Clear both whole planes, then hand out the visible rectangle of each as a canvas of its own.
template <class C>
struct Planes { Area<C> b, r; };
template <class C>
static Planes<C> areas(C &blk, C &red, Insets hid) {
  blk.fillRect(0, 0, blk.width(), blk.height(), 0);
  red.fillRect(0, 0, red.width(), red.height(), 0);
  const int w = blk.width() - hid.l - hid.r, h = blk.height() - hid.t - hid.b;
  return Planes<C>{Area<C>{blk, hid.l, hid.t, w, h}, Area<C>{red, hid.l, hid.t, w, h}};
}

// ---------------------------------------------------------------- formatted text (spec)

struct LineStyle {
  uint8_t font = 2;
  bool red = false;
  bool invert = false;
  char align = '=';
  int dy = 0;  // extra vertical offset in pixels (@n / @-n), set by dragging the line on the web page
};

static std::string parseLine(std::string s, LineStyle &st) {
  if (!s.empty() && s[0] == '{') {
    size_t end = s.find('}');
    if (end != std::string::npos && end > 0) {
      for (size_t i = 1; i < end; i++) {
        char c = s[i];
        if (c >= '1' && c <= '0' + kFontCount) st.font = c - '0';
        else if (c == 'r') st.red = true;
        else if (c == 'b') st.red = false;
        else if (c == 'i') st.invert = true;
        else if (c == '<' || c == '=' || c == '>') st.align = c;
        else if (c == '@') {
          int sign = 1, v = 0;
          size_t j = i + 1;
          if (j < end && s[j] == '-') { sign = -1; j++; }
          while (j < end && s[j] >= '0' && s[j] <= '9') v = v * 10 + (s[j++] - '0');
          st.dy = sign * v;
          i = j - 1;
        }
      }
      s = s.substr(end + 1);
    }
  }
  return decodeUtf8(s);
}

// Lines separated by '|', each with optional {flags} (see the sketch header). Lines stack top to
// bottom and the block is centred vertically. Every line always stays on the screen: a line too wide
// drops to a smaller font, a stack too tall shrinks its biggest lines, and a moved line (@n) is kept
// inside the top/bottom edges. Draws on an already cleared visible area (see renderSpec at the end).
template <class C>
static void renderSpecArea(const std::string &spec, C &blk, C &red, bool hasRed) {
  const int W = blk.width(), H = blk.height(), gap = 3;
  const int margin = 4, vm = 2;

  struct L { std::string text; LineStyle st; Ink ink; };
  L lines[12];
  int count = 0, total = 0;
  size_t pos = 0;
  while (pos <= spec.size() && count < 12) {
    size_t bar = spec.find('|', pos);
    if (bar == std::string::npos) bar = spec.size();
    L &l = lines[count];
    l.text = parseLine(spec.substr(pos, bar - pos), l.st);
    if (!hasRed) l.st.red = false;
    for (;;) {
      l.ink = textBounds(kFonts[l.st.font - 1], l.text);
      if (l.ink.w <= W - 2 * margin || l.st.font == 1) break;
      l.st.font--;
    }
    if (l.text.empty()) { l.ink = textBounds(kFonts[l.st.font - 1], "A"); l.ink.w = 0; }  // blank line keeps its height
    count++;
    pos = bar + 1;
  }
  // Too tall for the screen: shrink the biggest line one size at a time until the stack fits.
  for (;;) {
    total = 0;
    int big = -1;
    for (int i = 0; i < count; i++) {
      total += lines[i].ink.h + (i ? gap : 0) + (lines[i].st.invert ? 4 : 0);
      if (lines[i].st.font > 1 && (big < 0 || lines[i].st.font > lines[big].st.font)) big = i;
    }
    if (total <= H - 2 * vm || big < 0) break;
    L &l = lines[big];
    l.st.font--;
    int w0 = l.ink.w;
    l.ink = textBounds(kFonts[l.st.font - 1], l.text.empty() ? std::string("A") : l.text);
    if (l.text.empty() || w0 == 0) l.ink.w = 0;
  }

  int y = (H - total) / 2;
  if (y < 0) y = 0;
  for (int i = 0; i < count; i++) {
    L &l = lines[i];
    if (i) y += gap;
    int pad = l.st.invert ? 2 : 0;
    int x;
    if (l.st.align == '<') x = margin;
    else if (l.st.align == '>') x = W - margin - l.ink.w;
    else x = (W - l.ink.w) / 2;
    C &plane = l.st.red ? red : blk;
    uint16_t color = 1;
    int ly = y + l.st.dy;  // the offset moves only this line, the others keep their places
    int lineH = l.ink.h + 2 * pad;
    if (ly > H - vm - lineH) ly = H - vm - lineH;  // never past the bottom edge…
    if (ly < vm) ly = vm;                          // …or the top one
    if (l.st.invert) {
      plane.fillRect(0, ly, W, l.ink.h + 2 * pad, 1);
      color = 0;
    }
    drawText(plane, kFonts[l.st.font - 1], l.text, x - l.ink.x1, ly + pad - l.ink.y1, color);
    y += l.ink.h + 2 * pad;
  }
}

// ---------------------------------------------------------------- stats screen

struct Stats {
  std::string name;      // e.g. ATC_123456 (ASCII)
  int mv = -1;           // battery millivolts, -1 unknown
  int boardC = -999;     // chip temperature °C, -999 unknown
  int screenC = -999;    // panel temperature °C, -999 unknown
  std::string updated;   // e.g. "14:37" or "14.10 14:37" — empty = unknown
};

// 2×CR2450 (3 V lithium): 2.2 V empty, 3.1 V full.
static int batteryPercent(int mv) {
  if (mv < 0) return -1;
  int p = (mv - 2200) * 100 / 900;
  return p < 0 ? 0 : p > 100 ? 100 : p;
}

// Largest font (1..maxFont) whose ink fits maxW × maxH; font 1 if none does.
static int fitFont(const std::string &t, int maxW, int maxH, int maxFont) {
  for (int f = maxFont; f > 1; f--) {
    Ink k = textBounds(kFonts[f - 1], t);
    if (k.w <= maxW && k.h <= maxH) return f;
  }
  return 1;
}

// Sizes come from a sample text ("88°C", "100%", …) so equal fields get equal sizes and an
// unknown value ("--") isn't blown up to fill the box.
static int fitSample(const char *sampleUtf8, int maxW, int maxH, int maxFont) {
  return fitFont(decodeUtf8(sampleUtf8), maxW, maxH, maxFont);
}

static std::string tempText(int c) {
  if (c == -999) return "--";
  return std::to_string(c) + "\xE2\x84\x83";  // ℃ (one glyph, tighter than "°C")
}

// Height of capital letters / digits for font f (top of 'H' to the baseline).
static int capHeight(int f) { return -glyphFor(kFonts[f - 1], 'H').yOffset; }

// Draw t (decoded) with font f so its cap band (not descenders) is centred on cy.
template <class C>
static void drawCapCentered(C &c, int f, const std::string &t, int bx, int bw, int cy, char align, uint16_t color) {
  Ink k = textBounds(kFonts[f - 1], t);
  int x = align == '<' ? bx : align == '>' ? bx + bw - k.w : bx + (bw - k.w) / 2;
  drawText(c, kFonts[f - 1], t, x - k.x1, cy + capHeight(f) / 2, color);
}

// Layout: black name bar from the top edge, the two temperatures (label + red value) centred on the
// whole screen — side by side on a landscape canvas, stacked on a portrait one — and battery + last
// update along the bottom, all on one centre line.
template <class C>
static void renderStatsArea(const Stats &s, C &blk, C &red, bool hasRed) {
  const int W = blk.width(), H = blk.height();
  C &accent = hasRed ? red : blk;

  const int mx = 4, my = 4;
  const int x0 = mx, w = W - 2 * mx, h = H - 2 * my;
  const int gap = h >= 200 ? 6 : 3;
  const int barH = my + (h * 26 + 50) / 100, fh = (h * 18 + 50) / 100;
  const int fy = H - my - fh;

  // name bar
  blk.fillRect(0, 0, W, barH, 1);
  std::string name = decodeUtf8(s.name);
  int nf = fitFont(name, w - 8, barH - 4, 9);
  drawCapCentered(blk, nf, name, x0 + 4, w - 8, barH / 2, '=', 0);

  // footer: battery icon + percentage and last update — side by side on a landscape canvas, two
  // rows on a portrait one; everything centred on the row's middle line, nudged 2 px down
  const bool landscape = W >= H;
  const int rowH = landscape ? fh : (fh - gap) / 2;
  const int bw = landscape ? (w - gap) / 2 : w;
  const int cy1 = fy + rowH / 2 + 2, cy2 = landscape ? cy1 : cy1 + rowH + gap;
  int pct = batteryPercent(s.mv);
  int ih = rowH - 6;
  if (ih > 30) ih = 30;
  int iw = ih * 2, nub = ih / 5 < 2 ? 2 : ih / 5, bt = ih / 10 < 1 ? 1 : ih / 10;
  int iy = cy1 - ih / 2;
  blk.fillRect(x0, iy, iw, bt, 1);
  blk.fillRect(x0, iy + ih - bt, iw, bt, 1);
  blk.fillRect(x0, iy, bt, ih, 1);
  blk.fillRect(x0 + iw - bt, iy, bt, ih, 1);
  blk.fillRect(x0 + iw, iy + ih / 4, nub, ih - 2 * (ih / 4), 1);
  if (pct > 0) {
    int inner = iw - 2 * bt - 2;
    C &fill = pct < 20 ? accent : blk;
    fill.fillRect(x0 + bt + 1, iy + bt + 1, inner * pct / 100 < 1 ? 1 : inner * pct / 100, ih - 2 * bt - 2, 1);
  }
  std::string batt = decodeUtf8(pct < 0 ? std::string("--") : std::to_string(pct) + "%");
  std::string upd = decodeUtf8("Updated " + (s.updated.empty() ? std::string("--:--") : s.updated));
  int tx = x0 + iw + nub + 4;
  int ff = fitSample("100%", x0 + bw - tx, rowH, 9), uf = fitFont(upd, bw, rowH, 9);
  if (uf < ff && landscape) ff = uf;  // same size for both texts on one row
  drawCapCentered(blk, ff, batt, tx, x0 + bw - tx, cy1, '<', 1);
  drawCapCentered(blk, landscape ? ff : uf, upd, landscape ? x0 + w - bw : x0, bw, cy2, landscape ? '>' : '=', 1);

  // temperatures: the label + value block sits midway between the name bar and the footer, measured
  // on the real ink (letters, digits, ° — not just cap height), so both gaps are equal
  int footTop = iy;
  {
    Ink kb = textBounds(kFonts[ff - 1], batt);
    if (cy1 + capHeight(ff) / 2 + kb.y1 < footTop) footTop = cy1 + capHeight(ff) / 2 + kb.y1;
    if (landscape) {
      Ink ku = textBounds(kFonts[ff - 1], upd);
      if (cy1 + capHeight(ff) / 2 + ku.y1 < footTop) footTop = cy1 + capHeight(ff) / 2 + ku.y1;
    }
  }
  const int avail = footTop - barH - 2 * gap;
  const int cellW = landscape ? (w - gap) / 2 : w, cellH = landscape ? avail : (avail - gap) / 2;
  const int lh = (cellH * 24 + 50) / 100;
  const int lf = fitSample("SCREEN", cellW, lh, 4);
  const int tf = fitSample("88\xE2\x84\x83", cellW - 4, cellH - lh - 2, 9);  // shrinks for e.g. -12°C
  const int lc = capHeight(lf), vc = capHeight(tf), lv = vc / 5 < 3 ? 3 : vc / 5;
  const int cellInk = lc + lv + vc;
  const char *labels[2] = {"SCREEN", "BOARD"};
  const int temps[2] = {s.screenC, s.boardC};
  // Pass 1 with the block at y = 0: where does the ink start and end?
  int inkTop = 32767, inkBot = -32768, lbase[2], vbase[2], vfont[2];
  std::string ltext[2], vtext[2];
  for (int i = 0; i < 2; i++) {
    int cy = landscape ? 0 : i * (cellInk + 2 * gap);
    ltext[i] = decodeUtf8(labels[i]);
    vtext[i] = decodeUtf8(tempText(temps[i]));
    vfont[i] = fitFont(vtext[i], cellW - 4, cellH - lh - 2, tf);
    lbase[i] = cy + lc / 2 + capHeight(lf) / 2;
    vbase[i] = cy + lc + lv + vc / 2 + capHeight(vfont[i]) / 2;
    Ink kl = textBounds(kFonts[lf - 1], ltext[i]), kv = textBounds(kFonts[vfont[i] - 1], vtext[i]);
    if (lbase[i] + kl.y1 < inkTop) inkTop = lbase[i] + kl.y1;
    if (vbase[i] + kv.y1 < inkTop) inkTop = vbase[i] + kv.y1;
    if (lbase[i] + kl.y1 + kl.h > inkBot) inkBot = lbase[i] + kl.y1 + kl.h;
    if (vbase[i] + kv.y1 + kv.h > inkBot) inkBot = vbase[i] + kv.y1 + kv.h;
  }
  // Pass 2: shift so that (ink top - bar bottom) == (footer top - ink bottom).
  const int top = (barH + footTop - inkTop - inkBot) / 2;
  for (int i = 0; i < 2; i++) {
    int cx = landscape ? x0 + i * (cellW + gap) : x0;
    Ink kl = textBounds(kFonts[lf - 1], ltext[i]), kv = textBounds(kFonts[vfont[i] - 1], vtext[i]);
    drawText(blk, kFonts[lf - 1], ltext[i], cx + (cellW - kl.w) / 2 - kl.x1, top + lbase[i], 1);
    drawText(accent, kFonts[vfont[i] - 1], vtext[i], cx + 2 + (cellW - 4 - kv.w) / 2 - kv.x1, top + vbase[i], 1);
  }
  int t = h >= 200 ? 3 : 2;  // divider between the two cells, exactly as tall as the text
  if (landscape) blk.fillRect(x0 + (w - t) / 2, top + inkTop, t, inkBot - inkTop, 1);
  else blk.fillRect(x0 + 4, top + cellInk + gap - t / 2, w - 8, t, 1);
}

// ---------------------------------------------------------------- entry points

// Clear the whole canvas, then draw only on the part the panel shows (`hid`: see rotateInsets()).
template <class C>
static void renderSpec(const std::string &spec, C &blk, C &red, bool hasRed, Insets hid = Insets()) {
  Planes<C> a = areas(blk, red, hid);
  renderSpecArea(spec, a.b, a.r, hasRed);
}

template <class C>
static void renderStats(const Stats &s, C &blk, C &red, bool hasRed, Insets hid = Insets()) {
  Planes<C> a = areas(blk, red, hid);
  renderStatsArea(s, a.b, a.r, hasRed);
}
