#!/usr/bin/env bash
#
# One command. Checks your toolchain, verifies your ROM, generates the C, and
# builds a native executable for the machine you are on.
#
#   ./build.sh                 use ./rom.bin
#   ./build.sh path/to/rom.bin use a ROM somewhere else
#   ./build.sh --release       windowed build: no console, log to file, icon
#   ./build.sh --clean         throw away objects and start over
#
# If anything below is missing, this script tells you exactly what and stops.
# It will not produce a half-built binary and let you discover it later.
#
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$RAIZ"

ROM=""
RELEASE=0
CLEAN=0
JOBS="$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )"

for arg in "$@"; do
    case "$arg" in
        --release) RELEASE=1 ;;
        --clean)   CLEAN=1 ;;
        -j*)       JOBS="${arg#-j}" ;;
        --help|-h) sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*)        echo "unknown option: $arg" >&2; exit 2 ;;
        *)         ROM="$arg" ;;
    esac
done
[ -n "$ROM" ] || ROM="rom.bin"

falhar() { echo; echo "ERROR: $1" >&2; echo; [ $# -gt 1 ] && echo "$2" >&2; exit 1; }

# --- 1. toolchain -----------------------------------------------------------
echo "== checking the toolchain"

PY=""
for c in python3 python; do
    if command -v "$c" >/dev/null 2>&1 && "$c" -c 'import sys; sys.exit(0 if sys.version_info >= (3,9) else 1)' 2>/dev/null; then
        PY="$c"; break
    fi
done
[ -n "$PY" ] || falhar "Python 3.9 or newer not found." \
    "Install Python 3, then re-run. On Debian/Ubuntu: sudo apt install python3"

"$PY" -c 'import capstone' 2>/dev/null || falhar \
    "the Python module 'capstone' is missing." \
    "It is the disassembler the recompiler is built on.  pip install capstone"

command -v "${CC:-cc}" >/dev/null 2>&1 || falhar \
    "no C compiler found (tried '${CC:-cc}')." \
    "Install gcc or clang, or set CC.  On Debian/Ubuntu: sudo apt install build-essential"

command -v make >/dev/null 2>&1 || falhar "make not found." \
    "On Debian/Ubuntu: sudo apt install make"

if ! pkg-config --exists sdl3 2>/dev/null; then
    echo "   note: pkg-config does not know about SDL3; assuming -lSDL3 works."
    echo "         If linking fails, install SDL3 3.2 or newer, or pass"
    echo "         SDL_CFLAGS/SDL_LIBS by hand.  See README.md."
fi
echo "   python: $($PY --version 2>&1) | compiler: ${CC:-cc} | jobs: $JOBS"

# --- 2. the ROM -------------------------------------------------------------
#
# The ROM is not in this repository and never will be: it is copyrighted
# commercial data. You supply your own dump. See LEGAL.md.
echo "== checking the ROM"
[ -f "$ROM" ] || falhar "ROM not found at '$ROM'." \
    "This repository does not ship the game. You need your own dump of
Tiny Toon Adventures: ACME All-Stars (Mega Drive/Genesis, 1 MB raw .bin).
Put it next to this script as rom.bin, or pass its path:

    ./build.sh /path/to/your/rom.bin

See LEGAL.md for what 'your own dump' means here."

"$PY" tools/rom_info.py --check --rom "$ROM" || falhar \
    "that file is not the ROM this port was built against." \
    "tools/rom_info.py above says which field disagrees. Common causes:
an .smd (interleaved) dump instead of raw .bin, a truncated download,
or a different regional release."

# --- 3. generate the C ------------------------------------------------------
#
# This is the step that cannot ship pre-built: the output is derived from the
# ROM. It takes a couple of minutes the first time and is skipped afterwards
# unless the recompiler itself changed.
# The generated C is shared between targets: only objects and the executable
# live under BUILD. Regenerating ~1,950 files per target would be waste.
BUILD="${BUILD:-build}"
RECOMP="build/recomp"
if [ "$CLEAN" = 1 ]; then rm -rf "$BUILD/obj" "$RECOMP"; fi
if [ ! -d "$RECOMP" ] || [ tools/recompile.py -nt "$RECOMP" ]; then
    echo "== translating 68000 code to C (this takes a minute or two)"
    "$PY" tools/recompile.py --all "$RECOMP" --rom "$ROM" >/dev/null
fi
echo "   $(ls "$RECOMP"/*.c 2>/dev/null | wc -l) generated C files"

# --- 4. generated headers ---------------------------------------------------
"$PY" tools/gerar_baseline.py >/dev/null
"$PY" tools/gerar_versao.py   >/dev/null

# Icone e informacao de versao: so' fazem sentido num .exe de release, e o
# windres so' existe no caminho Windows.
if [ -n "${WINDOWS:-}" ] && [ "$RELEASE" = 1 ]; then
    "$PY" tools/gerar_rc.py --saida "$BUILD" >/dev/null
fi

# --- 5. compile -------------------------------------------------------------
echo "== compiling ($JOBS parallel jobs)"
make -j"$JOBS" RELEASE="$RELEASE" RECOMP="$RECOMP" BUILD="$BUILD"

echo
echo "Put your ROM next to the executable as rom.bin, or pass it as the first"
echo "argument. Controls and options are in README.md."
