#!/bin/sh
# Rebuild XenonRecomp and regenerate out/ppc from game/default.xex + Title Update #2 (out/tu2/default.xexp).
# First time: py -3 tools/xiso.py extract "<PGR4 disc>.iso" game
#             py -3 tools/stfs.py update "<TU_16L61VP_...>" out/tu2
set -e
cd "$(dirname "$0")/.."
# XenonRecomp's fixes for this game (patches/xenonrecomp.patch), unless they're already applied.
if ! git -C thirdparty/XenonRecomp apply --reverse --check ../../patches/xenonrecomp.patch 2>/dev/null; then
    git -C thirdparty/XenonRecomp apply ../../patches/xenonrecomp.patch
fi
cmd //c "tools\vcenv.cmd ninja -C build/tools xenonrecomp/XenonRecomp/XenonRecomp.exe" | tail -1
rm -rf out/ppc && mkdir -p out/ppc
build/tools/xenonrecomp/XenonRecomp/XenonRecomp.exe config/pgr4.toml thirdparty/XenonRecomp/XenonUtils/ppc_context.h > out/recomp.log 2>&1
# Six "Switch case ... outside function" lines are expected: data words before functions decoded as code
# (bogus functions nothing calls; the real functions translate their switches correctly).
grep -v "^Recompiling" out/recomp.log || true
