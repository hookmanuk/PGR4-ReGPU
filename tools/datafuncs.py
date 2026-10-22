"""datafuncs.py <image.bin> <out/ppc> <switch_tables.toml> <config.toml> [--write]: functions only data points to.

Functions reached only through pointers (virtual methods, this-adjusting thunks, callbacks) have no call
XenonAnalyse can see, and small ones often have no .pdata entry either: XenonRecomp then makes no function
there, and a call through the pointer finds nothing (crash.cpp PPCIndirectMiss). Worse, a configured
`functions` entry sized "to the next .pdata function" can swallow them.

Candidates: words in .rdata/.data pointing into .text, 4-aligned, not already a recompiled function nor a
jump-table target, and placed like a function start (right after a blr, padding, or an unconditional b).
Most other code pointers in data point into the middle of functions (C++ exception tables) and are not
functions. Import thunks are excluded too: data tables point at them (an I/O dispatch table of kernel
calls), and a recompiled copy of a thunk would replace the import in the function table with its
placeholder (nop, nop, nop, blr), so the call would do nothing. Each candidate becomes a `functions` entry running to the next known function start, and
configured entries that contain one are cut there. Without --write, the changes are only printed.
"""
import bisect
import glob
import re
import struct
import sys

BASE = 0x82000000
img_path, ppc_dir, switches_path, config_path = sys.argv[1:5]
write = '--write' in sys.argv
img = open(img_path, 'rb').read()


def word(a):
    return struct.unpack_from('>I', img, a - BASE)[0]


pe = struct.unpack_from('<I', img, 0x3C)[0]
nsec = struct.unpack_from('<H', img, pe + 6)[0]
opt = struct.unpack_from('<H', img, pe + 20)[0]
sec = pe + 24 + opt
text, data, pdata = None, [], None
for i in range(nsec):
    name = img[sec + i * 40:sec + i * 40 + 8].rstrip(b'\0').decode(errors='replace')
    vsize, vaddr = struct.unpack_from('<II', img, sec + i * 40 + 8)
    if name == '.text':
        text = (BASE + vaddr, BASE + vaddr + vsize)
    elif name in ('.rdata', '.data'):
        data.append((vaddr, vsize))
    elif name == '.pdata':
        pdata = (vaddr, vsize)
pdata_starts = set()
for off in range(pdata[0], pdata[0] + pdata[1], 8):
    b = struct.unpack_from('>I', img, off)[0]
    if b:
        pdata_starts.add(b)
made = set()
for path in glob.glob(ppc_dir + '/ppc_recomp.*.cpp'):
    made.update(int(m, 16) for m in re.findall(r'PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)', open(path, errors='replace').read()))
labels = set(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]+)', open(switches_path).read()))
thunks = set(int(m, 16) for m in re.findall(r'\{ 0x([0-9A-F]+), __imp__', open(ppc_dir + '/ppc_func_mapping.cpp').read()))

refs = set()
for vaddr, vsize in data:
    for off in range(vaddr, vaddr + vsize - 3, 4):
        v = struct.unpack_from('>I', img, off)[0]
        if not (text[0] <= v < text[1]) or v % 4 or v in labels or v in thunks or not word(v):
            continue
        prev = word(v - 4)
        if prev == 0x4E800020 or prev == 0 or (prev >> 26 == 18 and not prev & 1):  # after blr, padding, or b
            refs.add(v)

config = open(config_path, encoding='utf-8').read()
for t in sorted(thunks):
    config = re.sub(r'    \{ address = 0x%08X, size = 0x[0-9A-Fa-f]+ \},\n' % t, '', config)
entries = {int(m.group(1), 16): int(m.group(2), 16) for m in re.finditer(r'\{ address = 0x([0-9A-Fa-f]+), size = 0x([0-9A-Fa-f]+) \}', config)}
new = sorted(r for r in refs if r not in made or any(s < r < s + z for s, z in entries.items()))
boundaries = sorted(made | pdata_starts | set(entries) | refs | {text[1]})


def next_after(a):
    return boundaries[bisect.bisect_right(boundaries, a)]


cut = {}
for s, z in entries.items():
    inner = [r for r in refs if s < r < s + z]
    if inner:
        cut[s] = min(inner) - s
added = {r: next_after(r) - r for r in new if r not in entries}
print(f'# {len(refs)} data-referenced function starts; {len(added)} new functions; {len(cut)} configured entries cut')
for s in sorted(cut):
    print(f'cut   0x{s:08X}: 0x{entries[s]:X} -> 0x{cut[s]:X}')
for r in sorted(added):
    print(f'add   0x{r:08X} size 0x{added[r]:X}')
if write:
    for s, z in cut.items():
        config = config.replace(f'{{ address = 0x{s:08X}, size = 0x{entries[s]:X} }}', f'{{ address = 0x{s:08X}, size = 0x{z:X} }}')
    i = config.rindex(']')
    block = '    # Functions only data points to (virtual methods, thunks, callbacks): tools/datafuncs.py.\n'
    block += ''.join(f'    {{ address = 0x{r:08X}, size = 0x{z:X} }},\n' for r, z in sorted(added.items()))
    config = config[:i] + block + config[i:]
    open(config_path, 'w', encoding='utf-8').write(config)
    print('written', config_path)
