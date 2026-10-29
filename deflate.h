// deflate.h — zlib compression for compressed image uploads (data type 0x30), like atc1441's uploader:
// payload = raw size (u32 LE) + zlib(06, Wb u16 LE, Hb u16 LE, planes, raw buffer) with a 4 KB window.
// Greedy LZ77 with one hash candidate and fixed Huffman codes: simple, small, and the same bytes as
// zlibCompress() in the web page's oepl.js (check with tools/deflate_test.cpp + tools/deflate_test.mjs).
// The input is read once, in order, through `at(i)` into an 8 KB ring, so the raw image never has to
// be in RAM: text and stats are packed from the drawing canvases on the fly.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <functional>
#include <new>

namespace zdeflate {

static const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t kDistBase[24] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073};
static const uint8_t kDistExtra[24] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10};
static const int kWindow = 4096, kMaxMatch = 258, kRing = 8192;

// Growing output buffer that gives up (fail = true) past `max` bytes or when memory runs out.
struct Out {
  uint8_t *buf = nullptr;
  size_t len = 0, cap = 0, max = 0;
  bool fail = false;
  void put(uint8_t b) {
    if (fail) return;
    if (len == cap) {
      size_t n = cap ? cap * 2 : 1024;
      if (n > max) n = max;
      uint8_t *p = n > cap ? (uint8_t *)realloc(buf, n) : nullptr;
      if (!p) { fail = true; return; }
      buf = p;
      cap = n;
    }
    buf[len++] = b;
  }
};

struct Compressor {
  const std::function<uint8_t(size_t)> &at;
  size_t n, filled = 0;
  uint8_t ring[kRing];
  int32_t head[kWindow];  // hash → last position + 1
  Out &out;
  uint32_t acc = 0;
  int nacc = 0;

  Compressor(const std::function<uint8_t(size_t)> &a, size_t len, Out &o) : at(a), n(len), out(o) {
    memset(head, 0, sizeof head);
  }
  uint8_t src(size_t p) {
    while (filled <= p) ring[filled & (kRing - 1)] = at(filled), filled++;
    return ring[p & (kRing - 1)];
  }
  void bits(uint32_t v, int k) {  // LSB first
    acc |= v << nacc;
    nacc += k;
    while (nacc >= 8) out.put(acc & 0xff), acc >>= 8, nacc -= 8;
  }
  void huff(uint32_t code, int k) {  // Huffman codes go MSB first
    uint32_t r = 0;
    for (int i = 0; i < k; i++) r |= ((code >> i) & 1) << (k - 1 - i);
    bits(r, k);
  }
  void sym(int s) {
    if (s < 144) huff(0x30 + s, 8);
    else if (s < 256) huff(0x190 + s - 144, 9);
    else if (s < 280) huff(s - 256, 7);
    else huff(0xc0 + s - 280, 8);
  }
  uint32_t hash(size_t p) {
    uint32_t v = ((uint32_t)src(p) << 16) | ((uint32_t)src(p + 1) << 8) | src(p + 2);
    return ((v * 2654435761u) >> 20) & (kWindow - 1);
  }
  void run() {
    out.put(0x48);
    out.put(0xc7);
    bits(1, 1);
    bits(1, 2);  // BFINAL, BTYPE = fixed Huffman
    uint32_t a = 1, b = 0;
    size_t adlerAt = 0;
    for (size_t i = 0; i < n && !out.fail;) {
      int len = 0;
      size_t dist = 0;
      if (i + 3 <= n) {
        uint32_t h = hash(i);
        long cand = (long)head[h] - 1;
        head[h] = i + 1;
        if (cand >= 0 && i - cand <= (size_t)kWindow) {
          while (len < kMaxMatch && i + len < n && src(cand + len) == src(i + len)) len++;
          dist = i - cand;
        }
      }
      if (len >= 3) {
        int c = 0;
        while (c < 28 && kLenBase[c + 1] <= len) c++;
        sym(257 + c);
        bits(len - kLenBase[c], kLenExtra[c]);
        int d = 0;
        while (d < 23 && kDistBase[d + 1] <= dist) d++;
        huff(d, 5);
        bits(dist - kDistBase[d], kDistExtra[d]);
        for (int k = 1; k < len; k++)
          if (i + k + 3 <= n) head[hash(i + k)] = i + k + 1;
        i += len;
      } else {
        sym(src(i));
        i++;
      }
      for (; adlerAt < i; adlerAt++) a = (a + src(adlerAt)) % 65521, b = (b + a) % 65521;  // still in the ring
    }
    sym(256);
    if (nacc) out.put(acc & 0xff);
    uint32_t ad = (b << 16) | a;
    out.put(ad >> 24);
    out.put(ad >> 16);
    out.put(ad >> 8);
    out.put(ad);
  }
};

// Compressed upload payload for a raw buffer of `rawLen` bytes read through rawAt(i). Returns a
// malloc'd buffer (free() it) and its length, or nullptr when it would be bigger than `maxLen` or
// memory ran out — then send the raw buffer instead.
inline uint8_t *compressImage(const std::function<uint8_t(size_t)> &rawAt, size_t rawLen, uint16_t wb, uint16_t hb,
                              uint8_t planes, size_t maxLen, size_t &outLen) {
  const uint8_t hdr[6] = {6, (uint8_t)(wb & 0xff), (uint8_t)(wb >> 8), (uint8_t)(hb & 0xff), (uint8_t)(hb >> 8), planes};
  std::function<uint8_t(size_t)> at = [&](size_t i) -> uint8_t { return i < 6 ? hdr[i] : rawAt(i - 6); };
  Out out;
  out.max = maxLen;
  for (int k = 0; k < 4; k++) out.put((rawLen >> (8 * k)) & 0xff);
  Compressor *c = new (std::nothrow) Compressor(at, rawLen + 6, out);  // ~25 KB: keep it off the stack
  if (!c) { free(out.buf); return nullptr; }
  c->run();
  delete c;
  if (out.fail) { free(out.buf); return nullptr; }
  outLen = out.len;
  return out.buf;
}

}  // namespace zdeflate
