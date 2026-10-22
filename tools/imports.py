"""imports.py <xex> <image.bin> <xam_table.inc> <xboxkrnl_table.inc>: the XEX's imported function names.

Each import record address holds a word: type (byte 3: 0 variable, 1 function), library index (byte 2),
ordinal (low 16 bits). Only function records are printed, as lib:name.
"""
import re
import struct
import sys

xex = open(sys.argv[1], 'rb').read()
img = open(sys.argv[2], 'rb').read()
base = 0x82000000


def table(path):
    t = {}
    for m in re.finditer(r'\{\s*"(\w+)"\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\}|XE_EXPORT\(\s*\w+\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(\w+)', open(path).read()):
        if m.group(1):
            t[int(m.group(2), 0)] = m.group(1)
        else:
            t[int(m.group(3), 16)] = m.group(4)
    return t


tables = {'xam.xex': table(sys.argv[3]), 'xboxkrnl.exe': table(sys.argv[4])}
_, _, _, _, _, count = struct.unpack_from('>4sIIIII', xex, 0)
hdrs = {}
for i in range(count):
    k, v = struct.unpack_from('>II', xex, 24 + i * 8)
    hdrs[k] = v
off = hdrs[0x000103FF]
size, strsize, nlibs = struct.unpack_from('>III', xex, off)
p = off + 12
names = []
end = p + strsize
while p < end and len(names) < nlibs:
    e = xex.index(b'\0', p)
    names.append(xex[p:e].decode())
    p = e + 1
    while p < end and xex[p] == 0:
        p += 1
p = off + 12 + strsize
out = []
for _ in range(nlibs):
    lsize = struct.unpack_from('>I', xex, p)[0]
    name_index, nrec = struct.unpack_from('>HH', xex, p + 0x24)
    lib = names[name_index]
    for r in range(nrec):
        addr = struct.unpack_from('>I', xex, p + 0x28 + r * 4)[0]
        word = struct.unpack_from('<I', img, addr - base)[0]  # xexdump byte-swapped the data words in place
        if word == 0x00000060:
            continue  # a call thunk xexdump already replaced (nop)
        ordinal = word & 0xFFFF
        out.append(f'{lib}:{tables.get(lib, {}).get(ordinal, "ordinal_%d" % ordinal)}')
    p += 0x28 + nrec * 4
for n in sorted(set(out)):
    print(n)
