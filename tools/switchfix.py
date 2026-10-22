"""switchfix.py <image.bin> <recomp.log> [out/ppc]: sizes for functions whose jump tables XenonRecomp cut off.

XenonRecomp logs "Switch case at A is trying to jump outside function: T" when the function it
recompiles around a jump table ends before the table's targets. Leaf functions without a .pdata entry
get a guessed size, often too small. For each such switch, the function is taken to start where the
previous .pdata function ends (skipping padding) and to run to the next .pdata function; the output
is TOML `functions` entries for config/pgr4.toml. Switches inside a .pdata function are listed
separately (their entry's length disagrees with the table and needs a look).

With the recompiled output folder, a switch's function starts at the nearest function XenonRecomp
made before it instead: a call into a gap between .pdata functions starts a function of its own there
(a gap can hold several leaf functions), and that is the one whose guessed size was short.
"""
import glob
import bisect
import re
import struct
import sys

BASE = 0x82000000
img = open(sys.argv[1], 'rb').read()
log = open(sys.argv[2], errors='replace').read()
made = []
if len(sys.argv) > 3:
    for path in glob.glob(sys.argv[3] + '/ppc_recomp.*.cpp'):
        made += [int(m, 16) for m in re.findall(r'PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)', open(path, errors='replace').read())]
made.sort()

# .pdata: 8-byte entries {begin, packed}, length in words in bits 8..29 of packed.
pe = struct.unpack_from('<I', img, 0x3C)[0]
nsec = struct.unpack_from('<H', img, pe + 6)[0]
opt = struct.unpack_from('<H', img, pe + 20)[0]
sec = pe + 24 + opt
pdata = None
for i in range(nsec):
    name = img[sec + i * 40:sec + i * 40 + 8].rstrip(b'\0')
    vsize, vaddr = struct.unpack_from('<II', img, sec + i * 40 + 8)
    if name == b'.pdata':
        pdata = (vaddr, vsize)
assert pdata, 'no .pdata section'
funcs = []
for off in range(pdata[0], pdata[0] + pdata[1], 8):
    begin, packed = struct.unpack_from('>II', img, off)
    if not begin:
        continue
    funcs.append((begin, begin + ((packed >> 8) & 0x3FFFFF) * 4))
funcs.sort()
starts = [f[0] for f in funcs]

fixes, inside = {}, set()
for a, t in set(re.findall(r'Switch case at ([0-9A-F]+) is trying to jump outside function: ([0-9A-F]+)', log)):
    a, t = int(a, 16), int(t, 16)
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0 and funcs[i][0] <= a < funcs[i][1]:
        # Inside a .pdata function: XenonRecomp split it at a call into its middle. The piece holding
        # the switch runs to the .pdata function's end.
        j = bisect.bisect_right(made, a) - 1
        if j >= 0 and funcs[i][0] < made[j] <= a:
            fixes[made[j]] = max(fixes.get(made[j], 0), funcs[i][1] - made[j])
        else:
            inside.add((funcs[i][0], funcs[i][1], a))
        continue
    start = funcs[i][1] if i >= 0 else a
    while struct.unpack_from('>I', img, start - BASE)[0] == 0:  # padding between functions
        start += 4
    j = bisect.bisect_right(made, a) - 1
    if j >= 0 and made[j] > start:
        start = made[j]
    end = starts[i + 1] if i + 1 < len(starts) else t + 4
    fixes[start] = max(fixes.get(start, 0), end - start)

print(f'# {len(fixes)} functions without .pdata (from {sys.argv[2]})')
for start in sorted(fixes):
    print(f'    {{ address = 0x{start:08X}, size = 0x{fixes[start]:X} }},')
if inside:
    print(f'# {len(inside)} switches inside .pdata functions (not fixed here):')
    for b, e, a in sorted(inside):
        print(f'#   switch {a:08X} in {b:08X}..{e:08X}')
