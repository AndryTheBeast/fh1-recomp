// Tries raw LZX (libmspack lzxd) on one compressed zip entry with each window size, to find how
// FH1's zip method 21 is stored. fh1_lzx_probe IN EXPECTED_SIZE
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
extern "C" {
#include <mspack.h>
#include <lzx.h>
}

struct Mem {
  struct mspack_file base;
  std::vector<unsigned char>* data;
  size_t pos;
};

static struct mspack_file* m_open(struct mspack_system*, const char*, int) { return nullptr; }
static void m_close(struct mspack_file*) {}
static int m_read(struct mspack_file* f, void* buf, int bytes) {
  Mem* m = reinterpret_cast<Mem*>(f);
  size_t n = std::min<size_t>(bytes, m->data->size() - m->pos);
  std::memcpy(buf, m->data->data() + m->pos, n);
  m->pos += n;
  return int(n);
}
static int m_write(struct mspack_file* f, void* buf, int bytes) {
  Mem* m = reinterpret_cast<Mem*>(f);
  m->data->insert(m->data->end(), (unsigned char*)buf, (unsigned char*)buf + bytes);
  return bytes;
}
static int m_seek(struct mspack_file*, off_t, int) { return -1; }
static off_t m_tell(struct mspack_file*) { return 0; }
static void m_msg(struct mspack_file*, const char*, ...) {}
static void* m_alloc(struct mspack_system*, size_t n) { return std::malloc(n); }
static void m_free(void* p) { std::free(p); }
static void m_copy(void* s, void* d, size_t n) { std::memmove(d, s, n); }

int main(int argc, char** argv) {
  FILE* f = std::fopen(argv[1], "rb");
  std::vector<unsigned char> in;
  int c;
  while ((c = std::fgetc(f)) != EOF) in.push_back((unsigned char)c);
  std::fclose(f);
  long expected = std::atol(argv[2]);
  struct mspack_system sys = {m_open, m_close, m_read, m_write, m_seek, m_tell, m_msg, m_alloc, m_free, m_copy, nullptr};
  for (int skip = 0; skip <= 4; skip += 2) {
    for (int bits = 15; bits <= 21; ++bits) {
      std::vector<unsigned char> src(in.begin() + skip, in.end()), out;
      Mem mi{{}, &src, 0}, mo{{}, &out, 0};
      auto* s = lzxd_init(&sys, &mi.base, &mo.base, bits, 0, 4096, expected, 0);
      int r = s ? lzxd_decompress(s, expected) : -99;
      if (s) lzxd_free(s);
      std::printf("skip %d window %d: result %d, %zu bytes", skip, bits, r, out.size());
      if (!out.empty()) {
        std::printf(" first:");
        for (size_t i = 0; i < 12 && i < out.size(); ++i) std::printf(" %02x", out[i]);
      }
      std::printf("\n");
    }
  }
}
