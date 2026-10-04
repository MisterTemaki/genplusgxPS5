#!/usr/bin/env python3
"""forge_crc.py file CRC32HEX -- appends 4 bytes so the file's CRC32 becomes CRC32HEX (for the name-by-CRC test).
CRC32 is affine in the appended bits, so 32 single-bit probes and Gaussian elimination over GF(2) solve it."""
import sys, zlib
path, target = sys.argv[1], int(sys.argv[2], 16)
data = open(path, 'rb').read()
base = zlib.crc32(data + b'\0\0\0\0')
cols = []
for bit in range(32):
    tail = (1 << bit).to_bytes(4, 'little')
    cols.append(zlib.crc32(data + tail) ^ base)
want = base ^ target
# solve sum(x_i * cols[i]) = want
rows = [(cols[i], 1 << i) for i in range(32)]
basis = {}
for v, mask in rows:
    for b in range(31, -1, -1):
        if not (v >> b) & 1:
            continue
        if b in basis:
            v ^= basis[b][0]; mask ^= basis[b][1]
        else:
            basis[b] = (v, mask); break
x, v = 0, want
for b in range(31, -1, -1):
    if (v >> b) & 1:
        v ^= basis[b][0]; x ^= basis[b][1]
assert v == 0
out = data + x.to_bytes(4, 'little')
assert zlib.crc32(out) == target
open(path, 'wb').write(out)
print(f"{path}: crc32 {target:08X}")
