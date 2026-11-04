#!/usr/bin/env bash
#
# One command, nothing installed on your machine but Docker.
#
#   ./build-docker.sh                    Linux binary  -> build/tinytoon
#   ./build-docker.sh windows            Windows .exe  -> build/tinytoon.exe
#   ./build-docker.sh windows --release  single file, no console, with icon
#   ./build-docker.sh linux  path/to/rom.bin
#
# The image is built the first time and reused afterwards. That first build
# compiles SDL3 from source for both targets and takes roughly ten minutes;
# every run after it takes seconds.
#
# Your ROM is mounted read-only and never enters the image.
#
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$RAIZ"

IMAGEM=tinytoon-build
ALVO=linux
RELEASE=0
ROM=""

for arg in "$@"; do
    case "$arg" in
        linux|windows) ALVO="$arg" ;;
        --release)     RELEASE=1 ;;
        --help|-h)     sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*)            echo "unknown option: $arg" >&2; exit 2 ;;
        *)             ROM="$arg" ;;
    esac
done
[ -n "$ROM" ] || ROM="rom.bin"

command -v docker >/dev/null 2>&1 || {
    echo "ERROR: docker not found. Install Docker, or use ./build.sh instead" >&2
    echo "       and install the toolchain yourself (see README.md)." >&2
    exit 1
}

[ -f "$ROM" ] || {
    echo "ERROR: ROM not found at '$ROM'." >&2
    echo "This repository does not ship the game; you supply your own dump." >&2
    echo "See LEGAL.md." >&2
    exit 1
}

if ! docker image inspect "$IMAGEM" >/dev/null 2>&1; then
    echo "== building the image (first time only; compiles SDL3, ~10 min)"
    docker build -t "$IMAGEM" .
fi

# The ROM goes in read-only and under a fixed name, so nothing in the build can
# modify it and the path inside the container never depends on where you keep it.
ROM_ABS="$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")"

# --user keeps the produced files owned by you. Without it every artifact comes
# out owned by root and the next build cannot overwrite them.
docker run --rm \
    -v "$RAIZ:/work" \
    -v "$ROM_ABS:/rom.bin:ro" \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    "$IMAGEM" \
    bash -c "set -e
        cd /work
        if [ '$ALVO' = windows ]; then
            export CC=x86_64-w64-mingw32-gcc
            export WINDRES=x86_64-w64-mingw32-windres
            export WINDOWS=1
            export SDL_CFLAGS='-I/opt/sdl3-win/include'
            export SDL_LIBS='-L/opt/sdl3-win/lib -lSDL3 -lkernel32 -luser32 \
-lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 \
-lsetupapi -lshell32 -ldinput8'
            export LDFLAGS='-static -static-libgcc'
            export BUILD=build/win
        else
            export BUILD=build
        fi
        ./build.sh /rom.bin $( [ $RELEASE = 1 ] && echo --release )"

echo
echo "== done"
ls -l build/tinytoon build/win/tinytoon.exe 2>/dev/null || true
