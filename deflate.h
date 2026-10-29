// deflate.h — zlib compression for compressed image uploads (data type 0x30), like atc1441's uploader:
// payload = raw size (u32 LE) + zlib(06, Wb u16 LE, Hb u16 LE, planes, raw buffer) with a 4 KB window.
// The tags' decoder only takes DYNAMIC Huffman blocks (fixed-Huffman streams hang it mid-upload), so:
// greedy LZ77 with one hash candidate, blocks of up to 2048 tokens, each with its own length-limited
// Huffman codes. Same bytes as zlibCompress() in the web page's oepl.js (check with
// tools/deflate_test.cpp + tools/deflate_test.mjs). The input is read once, in order, through `at(i)`
// into an 8 KB ring, so the raw image never has to be in RAM: text and stats are packed from the
// drawing canvases on the fly.
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
static const int kWindow = 4096, kMaxMatch = 258, kRing = 8192, kHash = 2048;

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

static const uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
static const int kBlockTokens = 2048;

static int lenCode(int len) { int c = 0; while (c < 28 && kLenBase[c + 1] <= len) c++; return c; }
static int distCode(int dist) { int d = 0; while (d < 23 && kDistBase[d + 1] <= dist) d++; return d; }

// Work space for codeLengths(), ~5 KB: lives in the Compressor (heap), the Arduino loop task's
// 8 KB stack is too small for it.
struct HuffScratch {
  int32_t f[286], w[572];
  int16_t leaves[286], parent[572];
};

// Huffman code lengths for freq[] (n ≤ 286 symbols), at most `limit` bits. Two-queue construction with
// fixed tie-breaks (lighter first, leaf before internal node, lower symbol first); too deep → halve
// the counts and build again. At least two symbols get a code, so every tree is complete.
static void codeLengths(const int32_t *freq, int n, int limit, uint8_t *len, HuffScratch &s) {
  int32_t *f = s.f, *w = s.w;
  int16_t *leaves = s.leaves, *parent = s.parent;
  int used = 0;
  for (int i = 0; i < n; i++) f[i] = freq[i], used += freq[i] != 0;
  for (int i = 0; used < 2; i++) if (!f[i]) f[i] = 1, used++;
  for (;;) {
    int m = 0;
    for (int i = 0; i < n; i++) if (f[i]) leaves[m++] = i;
    for (int k = 1; k < m; k++) {  // insertion sort by (count, symbol)
      int16_t v = leaves[k];
      int j = k - 1;
      while (j >= 0 && (f[leaves[j]] > f[v] || (f[leaves[j]] == f[v] && leaves[j] > v))) leaves[j + 1] = leaves[j], j--;
      leaves[j + 1] = v;
    }
    for (int k = 0; k < m; k++) w[k] = f[leaves[k]];
    int li = 0, ni = m, next = m;  // next leaf, next internal node to take, next free node
    auto take = [&]() { return (li < m && (ni >= next || w[li] <= w[ni])) ? li++ : ni++; };
    while (next < 2 * m - 1) {
      int x = take(), y = take();
      w[next] = w[x] + w[y];
      parent[x] = parent[y] = next;
      next++;
    }
    memset(len, 0, n);
    int max = 0;
    for (int k = 0; k < m; k++) {
      int d = 0;
      for (int v = k; v != 2 * m - 2; v = parent[v]) d++;
      len[leaves[k]] = d;
      if (d > max) max = d;
    }
    if (max <= limit) return;
    for (int i = 0; i < n; i++) if (f[i]) f[i] = (f[i] + 1) >> 1;
  }
}

// Canonical codes (RFC 1951 3.2.2) for the given lengths.
static void canonical(const uint8_t *len, int n, uint16_t *code) {
  uint16_t count[16] = {0}, next[16] = {0};
  for (int i = 0; i < n; i++) count[len[i]]++;
  count[0] = 0;
  for (int b = 1, c = 0; b < 16; b++) c = (c + count[b - 1]) << 1, next[b] = c;
  for (int i = 0; i < n; i++) code[i] = len[i] ? next[len[i]]++ : 0;
}

struct Compressor {
  const std::function<uint8_t(size_t)> &at;
  size_t n, filled = 0;
  uint8_t ring[kRing];
  int32_t head[kHash];  // hash → last position + 1
  uint16_t tokLit[kBlockTokens], tokDist[kBlockTokens];
  int ntok = 0;
  HuffScratch hs;
  int32_t fl[286], fd[30], fc[19];
  uint8_t ll[286], dl[30], cl[19];
  uint16_t cc[19], lc[286], dc[30];
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
  void flush(bool final) {
    memset(fl, 0, sizeof fl), memset(fd, 0, sizeof fd), memset(fc, 0, sizeof fc);
    for (int t = 0; t < ntok; t++) {
      if (tokDist[t]) fl[257 + lenCode(tokLit[t])]++, fd[distCode(tokDist[t])]++;
      else fl[tokLit[t]]++;
    }
    fl[256]++;
    codeLengths(fl, 286, 15, ll, hs);
    codeLengths(fd, 30, 15, dl, hs);
    int hlit = 286, hdist = 30;
    while (hlit > 257 && !ll[hlit - 1]) hlit--;
    while (hdist > 1 && !dl[hdist - 1]) hdist--;
    for (int i = 0; i < hlit; i++) fc[ll[i]]++;  // no run-length codes: plain 0–15
    for (int i = 0; i < hdist; i++) fc[dl[i]]++;
    codeLengths(fc, 19, 7, cl, hs);
    canonical(cl, 19, cc);
    int hclen = 19;
    while (hclen > 4 && !cl[kClOrder[hclen - 1]]) hclen--;
    bits(final ? 1 : 0, 1);
    bits(2, 2);
    bits(hlit - 257, 5);
    bits(hdist - 1, 5);
    bits(hclen - 4, 4);
    for (int i = 0; i < hclen; i++) bits(cl[kClOrder[i]], 3);
    for (int i = 0; i < hlit; i++) huff(cc[ll[i]], cl[ll[i]]);
    for (int i = 0; i < hdist; i++) huff(cc[dl[i]], cl[dl[i]]);
    canonical(ll, 286, lc);
    canonical(dl, 30, dc);
    for (int t = 0; t < ntok; t++) {
      if (tokDist[t]) {
        int len = tokLit[t], dist = tokDist[t], c = lenCode(len), d = distCode(dist);
        huff(lc[257 + c], ll[257 + c]);
        bits(len - kLenBase[c], kLenExtra[c]);
        huff(dc[d], dl[d]);
        bits(dist - kDistBase[d], kDistExtra[d]);
      } else {
        huff(lc[tokLit[t]], ll[tokLit[t]]);
      }
    }
    huff(lc[256], ll[256]);
    ntok = 0;
  }
  uint32_t hash(size_t p) {
    uint32_t v = ((uint32_t)src(p) << 16) | ((uint32_t)src(p + 1) << 8) | src(p + 2);
    return ((v * 2654435761u) >> 21) & (kHash - 1);
  }
  void run() {
    out.put(0x48);
    out.put(0xc7);
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
        tokLit[ntok] = len, tokDist[ntok] = dist, ntok++;
        for (int k = 1; k < len; k++)
          if (i + k + 3 <= n) head[hash(i + k)] = i + k + 1;
        i += len;
      } else {
        tokLit[ntok] = src(i), tokDist[ntok] = 0, ntok++;
        i++;
      }
      for (; adlerAt < i; adlerAt++) a = (a + src(adlerAt)) % 65521, b = (b + a) % 65521;  // still in the ring
      if (ntok == kBlockTokens && i < n) flush(false);
    }
    flush(true);
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
  Compressor *c = new (std::nothrow) Compressor(at, rawLen + 6, out);  // ~38 KB: keep it off the stack
  if (!c) { free(out.buf); return nullptr; }
  c->run();
  delete c;
  if (out.fail) { free(out.buf); return nullptr; }
  outLen = out.len;
  return out.buf;
}

}  // namespace zdeflate
