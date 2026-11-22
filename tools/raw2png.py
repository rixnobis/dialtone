# raw2png.py in.raw w h out.png : pcsx-redux takeScreenShot 16bpp (BGR555) dump to png
import sys, struct, zlib
src, w, h, dst = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
data = open(src, 'rb').read()
rows = []
for y in range(h):
    row = bytearray([0])
    for x in range(w):
        v = data[(y * w + x) * 2] | data[(y * w + x) * 2 + 1] << 8
        r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
        row += bytes([r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2])
    rows.append(bytes(row))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b'')
open(dst, 'wb').write(png)
