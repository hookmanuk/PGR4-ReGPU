"""missedswitch.py <image.bin> <switch_tables.toml> [--write]: absolute jump tables XenonAnalyse missed.

Pattern: cmplwi crN,rX,max / ... / lis r12,hi / addi r12,r12,lo / rlwinm r0,rX,2,0,29 / lwzx r0,r12,r0 /
mtctr r0 / bctr, table of max+1 absolute words (or, with no bound check of its own, the words pointing near it). A missed one becomes an indirect call to a label
(crash.cpp PPCIndirectMiss). With --write, the missing tables are appended.
"""
import re
import struct
import sys

BASE = 0x82000000
img_path, toml_path = sys.argv[1:3]
img = open(img_path, 'rb').read()
toml = open(toml_path).read()
known = set(int(m, 16) for m in re.findall(r'base = 0x([0-9A-Fa-f]+)', toml))
w = lambda a: struct.unpack_from('>I', img, a - BASE)[0]
text = (0x82290000, 0x82290000 + 0x76520C)
out = []
for a in range(text[0] + 24, text[1], 4):
    if w(a) != 0x4E800420 or w(a - 4) != 0x7C0903A6:  # bctr, mtctr r0
        continue
    lwzx, rl, addi, lis = w(a - 8), w(a - 12), w(a - 16), w(a - 20)
    if lwzx != 0x7C0C002E or lis >> 16 != 0x3D80 or addi >> 16 != 0x398C or rl >> 26 != 21:
        continue
    r = (rl >> 21) & 31
    table = ((lis & 0xFFFF) << 16) + (addi & 0xFFFF if addi & 0x8000 == 0 else (addi & 0xFFFF) - 0x10000)
    table &= 0xFFFFFFFF
    # Bound from the nearest preceding cmplwi crN,rX,imm (and its bgt default).
    count = default = None
    for b in range(a - 24, a - 64, -4):
        i = w(b)
        if i >> 26 == 10 and (i >> 16) & 31 == r:
            count = (i & 0xFFFF) + 1
            j = w(b + 4)
            if j >> 26 == 16:
                d = j & 0xFFFC
                default = b + 4 + (d - 0x10000 if d & 0x8000 else d)
            break
    if count is None:  # bound checked before an earlier dispatch: the words that point near this one
        count = 0
        while abs(w(table + 4 * count) - a) < 0x2000:
            count += 1
    if not count or a - 20 in known:
        continue
    labels = [w(table + 4 * k) for k in range(count)]
    if not all(text[0] <= l < text[1] for l in labels):
        continue
    out.append((a - 20, r, default or labels[0], labels))
print(f'# {len(out)} missed tables')
block = ''
for base, r, d, labels in out:
    print(f'0x{base:08X} r{r} {len(labels)} labels')
    block += f'\n[[switch]]\nbase = 0x{base:08X}\nr = {r}\ndefault = 0x{d:08X}\nlabels = [\n' + ''.join(f'    0x{l:08X},\n' for l in labels) + ']\n'
if '--write' in sys.argv and out:
    open(toml_path, 'a').write(block)
    print('written', toml_path)
