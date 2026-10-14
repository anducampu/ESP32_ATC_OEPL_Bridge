// Host test for render.h: draws the stats screen / a text spec and prints the planes as text
// ("." white, "#" black, "r" red), one row per line. Compare with tools/render_test.mjs (web port).
//   g++ -std=c++17 -I.. tools/render_test.cpp -o /tmp/render_test && /tmp/render_test stats 250 128
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../render.h"

struct Canvas {
  int w, h;
  std::vector<uint8_t> d;
  Canvas(int w_, int h_) : w(w_), h(h_), d(w_ * h_) {}
  int width() const { return w; }
  int height() const { return h; }
  void drawPixel(int x, int y, uint16_t c) { if (x >= 0 && y >= 0 && x < w && y < h) d[y * w + x] = c ? 1 : 0; }
  void fillRect(int x, int y, int fw, int fh, uint16_t c) {
    for (int yy = y; yy < y + fh; yy++) for (int xx = x; xx < x + fw; xx++) drawPixel(xx, yy, c);
  }
};

int main(int argc, char **argv) {
  if (argc < 4) return 1;
  int W = atoi(argv[2]), H = atoi(argv[3]);
  Canvas blk(W, H), red(W, H);
  if (!strcmp(argv[1], "stats")) {
    Stats s;
    s.name = argc > 4 ? argv[4] : "ATC_9B0442";
    s.mv = argc > 5 ? atoi(argv[5]) : 3030;
    s.boardC = argc > 6 ? atoi(argv[6]) : 14;
    s.screenC = argc > 7 ? atoi(argv[7]) : 18;
    s.updated = argc > 8 ? argv[8] : "14:37";
    renderStats(s, blk, red, true);
  } else {
    renderSpec(argv[4], blk, red, true);
  }
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) putchar(red.d[y * W + x] ? 'r' : blk.d[y * W + x] ? '#' : '.');
    putchar('\n');
  }
}
