"""PGR4 setup helper. The installer runs it with the Python bundled in the installer.

  setup_game.py check-iso <disc.iso>        exit 0 when the disc matches this build
  setup_game.py check-tu <title update>     exit 0 when the title update matches this build
  setup_game.py install <disc.iso> <game dir> <title update> <dlc folder or -> <pgr4 documents dir>
                                            unpacks the title update, installs DLC, builds the game files
  setup_game.py dlc <folder or package> <pgr4 documents dir>
                                            installs the DLC packages found there (also inside .zip files)
  setup_game.py space <game dir>            exit 0 when its drive has room for the game files
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# The files this build was made from. A disc or title update must match these to be used.
REF_DEFAULT_XEX = 'f9a411d8f1a1a3c38bd4a5ab509b40a9bf9c07338187c66c073f0493b89f423e'
REF_TITLE_UPDATE = '9980cf82c3bbbb32b35c841532c832b5598e99ce50daba8c658645213ab3d9b8'
TITLE_ID = 0x4D5307F9
# The game files take 5.8 GB; with room to spare.
NEEDED_BYTES = 7 * 1024 ** 3


def free_bytes(path):
    """Free space on the drive a folder is (or will be) on."""
    path = os.path.abspath(path)
    while not os.path.exists(path):
        parent = os.path.dirname(path)
        if parent == path:
            break
        path = parent
    return shutil.disk_usage(path).free


def check_space(game_dir):
    free = free_bytes(game_dir)
    if free < NEEDED_BYTES:
        print('Not enough space for the game files in %s: they need %.1f GB, %.1f GB is free.' % (
            os.path.abspath(game_dir), NEEDED_BYTES / 1024 ** 3, free / 1024 ** 3))
        return False
    return True


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


class ToolFailed(Exception):
    pass


def run_tool(script, *args):
    """Runs a tool, its output passed on; a failure raises ToolFailed with its last error line."""
    p = subprocess.run([sys.executable, os.path.join(HERE, script)] + list(args), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, errors='replace')
    sys.stdout.write(p.stdout)
    if p.returncode != 0:
        last = next((line.strip() for line in reversed(p.stdout.splitlines()) if line.strip()), '')
        raise ToolFailed('%s failed (exit code %d)%s' % (script, p.returncode, ': ' + last if last else ''))


def check_iso(iso):
    tmp = tempfile.mkdtemp(prefix='pgr4-check-')
    try:
        run_tool('xiso.py', 'extract', iso, tmp, 'default.xex')
        found = os.path.join(tmp, 'default.xex')
        return os.path.exists(found) and sha256(found) == REF_DEFAULT_XEX
    except (ToolFailed, OSError):
        return False  # not a disc image the tool can read
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def check_tu(path):
    return os.path.isfile(path) and sha256(path) == REF_TITLE_UPDATE


def is_dlc_package(path):
    """A LIVE/PIRS package of PGR4 marketplace content (content type 2)."""
    try:
        with open(path, 'rb') as f:
            head = f.read(0x364)
    except OSError:
        return False
    return (len(head) == 0x364 and head[:4] in (b'LIVE', b'PIRS') and int.from_bytes(head[0x344:0x348], 'big') == 2
            and int.from_bytes(head[0x360:0x364], 'big') == TITLE_ID)


def install_dlc(source, docs):
    """Installs every DLC package under source (a folder, a package, or .zip files holding them) into
    <docs>\\dlc, unpacked the way the game reads them. Returns how many were installed."""
    dlc_dir = os.path.join(docs, 'dlc')
    os.makedirs(dlc_dir, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='pgr4-dlc-')
    try:
        candidates = []
        walk = [source] if os.path.isfile(source) else [os.path.join(r, n) for r, _, ns in os.walk(source) for n in ns]
        for path in walk:
            if path.lower().endswith('.zip'):
                try:
                    with zipfile.ZipFile(path) as z:
                        out = os.path.join(tmp, str(len(candidates)))
                        z.extractall(out)
                    candidates += [os.path.join(r, n) for r, _, ns in os.walk(out) for n in ns]
                except (zipfile.BadZipFile, OSError) as e:
                    print('Skipped %s: %s' % (path, e))
            else:
                candidates.append(path)
        installed = 0
        for path in candidates:
            if not is_dlc_package(path):
                continue
            name = os.path.basename(path)
            if os.path.isdir(os.path.join(dlc_dir, name)) or os.path.isdir(os.path.join(docs, 'dlc-disabled', name)):
                print('DLC %s is already installed' % name)
                continue
            run_tool('stfs.py', 'install', path, dlc_dir)
            installed += 1
        print('DLC: %d package(s) installed' % installed)
        return installed
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def install(iso, game_dir, tu, dlc, docs):
    # The title update and DLC first: they don't depend on the disc, and a disc build that failed
    # (no room for it) would otherwise take them with it.
    # 1. The title update: the port loads update\default.xexp from the documents folder.
    update_dir = os.path.join(docs, 'update')
    os.makedirs(update_dir, exist_ok=True)
    run_tool('stfs.py', 'update', tu, update_dir)
    # 2. Optional DLC packages.
    if dlc and dlc != '-' and os.path.exists(dlc):
        install_dlc(dlc, docs)
    # 3. The game files, from the disc.
    if not check_space(game_dir):
        return 3
    os.makedirs(game_dir, exist_ok=True)
    run_tool('xiso.py', 'extract', iso, game_dir)
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    try:
        if cmd == 'check-iso':
            return 0 if check_iso(argv[2]) else 1
        if cmd == 'check-tu':
            return 0 if check_tu(argv[2]) else 1
        if cmd == 'space':
            return 0 if check_space(argv[2]) else 1
        if cmd == 'dlc':
            install_dlc(argv[2], argv[3])
            return 0
        if cmd == 'install':
            return install(argv[2], argv[3], argv[4], argv[5], argv[6])
    except ToolFailed as e:
        print('Setup failed: %s' % e)
        return 4
    except OSError as e:
        print('Setup failed: %s' % e)
        return 5
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
