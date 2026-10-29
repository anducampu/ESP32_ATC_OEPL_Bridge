// Host test for deflate.h: compresses stdin and writes the zlib stream to stdout. Must give the same
// bytes as tools/deflate_test.mjs (the web page's zlibCompress()); python3 -c "import zlib,sys;
// sys.stdout.buffer.write(zlib.decompress(sys.stdin.buffer.read(), 12))" undoes it.
//   g++ -std=c++17 -I.. tools/deflate_test.cpp -o /tmp/deflate_test && /tmp/deflate_test < in > out
#include <cstdio>
#include <vector>
#include "deflate.h"

int main() {
  std::vector<uint8_t> in;
  for (int c; (c = getchar()) != EOF;) in.push_back(c);
  std::function<uint8_t(size_t)> at = [&](size_t i) -> uint8_t { return in[i]; };
  zdeflate::Out out;
  out.max = in.size() * 2 + 1024;
  zdeflate::Compressor *c = new zdeflate::Compressor(at, in.size(), out);
  c->run();
  delete c;
  fwrite(out.buf, 1, out.len, stdout);
  return out.fail;
}
