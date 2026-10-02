// Decodes one entry of FH1's zip archives stored with method 21: Xbox XMemCompress LZX, 128 KB
// window. The caller (tools/fh1_unpack_archives.py) strips the frame headers (0xFF + 16-bit
// uncompressed size + 16-bit block size, or just a 16-bit block size for full 32 KB frames, all
// big-endian) and passes the concatenated LZX blocks. Built by tools/build_shader_tools.ps1 with
// libmspack's lzxd.c from sdk/thirdparty.
//     fh1_lzx_decode IN OUT UNCOMPRESSED_SIZE
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
extern "C" {
#include <mspack.h>
#include <lzx.h>
}

namespace {
struct Mem {
  struct mspack_file base;
  std::vector<unsigned char>* data;
  size_t pos;
};
struct mspack_file* Open(struct mspack_system*, const char*, int) { return nullptr; }
void Close(struct mspack_file*) {}
int Read(struct mspack_file* f, void* buf, int bytes) {
  Mem* m = reinterpret_cast<Mem*>(f);
  size_t n = std::min<size_t>(size_t(bytes), m->data->size() - m->pos);
  std::memcpy(buf, m->data->data() + m->pos, n);
  m->pos += n;
  return int(n);
}
int Write(struct mspack_file* f, void* buf, int bytes) {
  Mem* m = reinterpret_cast<Mem*>(f);
  m->data->insert(m->data->end(), static_cast<unsigned char*>(buf), static_cast<unsigned char*>(buf) + bytes);
  return bytes;
}
int Seek(struct mspack_file*, off_t, int) { return -1; }
off_t Tell(struct mspack_file*) { return 0; }
void Message(struct mspack_file*, const char*, ...) {}
void* Alloc(struct mspack_system*, size_t n) { return std::malloc(n); }
void Free(void* p) { std::free(p); }
void Copy(void* s, void* d, size_t n) { std::memmove(d, s, n); }
}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: fh1_lzx_decode IN OUT UNCOMPRESSED_SIZE\n");
    return 2;
  }
  std::vector<unsigned char> in, out;
  if (FILE* f = std::fopen(argv[1], "rb")) {
    unsigned char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) in.insert(in.end(), buf, buf + n);
    std::fclose(f);
  } else {
    return 3;
  }
  long size = std::atol(argv[3]);
  struct mspack_system sys = {Open, Close, Read, Write, Seek, Tell, Message, Alloc, Free, Copy, nullptr};
  Mem mi{{}, &in, 0}, mo{{}, &out, 0};
  struct lzxd_stream* s = lzxd_init(&sys, &mi.base, &mo.base, 17, 0, 4096, size, 0);
  int result = s ? lzxd_decompress(s, size) : -1;
  if (s) lzxd_free(s);
  if (result != 0 || long(out.size()) != size) {
    std::fprintf(stderr, "decode failed (result %d, %zu of %ld bytes)\n", result, out.size(), size);
    return 1;
  }
  FILE* f = std::fopen(argv[2], "wb");
  if (!f) return 3;
  std::fwrite(out.data(), 1, out.size(), f);
  std::fclose(f);
  return 0;
}
