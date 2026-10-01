#!/usr/bin/env python3
"""View an FH1 car photo from the save (ForzaProfile/Thumbnails/Thumbnail_N.xdc) as a PNG.

    python tools/thumb_view.py <copy of Thumbnail_N.xdc> ...   -> <file>.png next to it

Format: "cxds" header, raw deflate from offset 0x14, then a 52-byte header and a 768x288
tiled (Xbox 360 2D tiling) big-endian ARGB8888 image. Work on a COPY, never on the save itself.
"""
import zlib, struct, sys
W,H=768,288
def toff(x,y,w,l2):
    aw=(w+31)&~31
    macro=((x>>5)+(y>>5)*(aw>>5))<<(l2+7)
    micro=((x&7)+((y&6)<<2))<<l2
    off=macro+((micro&~15)<<1)+(micro&15)+((y&8)<<(3+l2))+((y&1)<<4)
    return ((off&~511)<<3)+((off&448)<<2)+(off&63)+((y&16)<<7)+(((((y&8)>>2)+(x>>3))&3)<<6)
def png(path,w,h,rows):
    raw=b''.join(b'\0'+r for r in rows)
    def ch(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
    open(path,'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(raw))+ch(b'IEND',b''))
for f in sys.argv[1:]:
    raw = open(f, 'rb').read()
    d = zlib.decompressobj(-15).decompress(raw[0x14:])[52:]
    rows = []
    for y in range(H):
        r = bytearray()
        for x in range(W):
            o = toff(x, y, W, 2)
            r += d[o + 1:o + 4]  # A R G B big-endian -> RGB
        rows.append(bytes(r))
    png(f + '.png', W, H, rows)
    print(f + '.png')
