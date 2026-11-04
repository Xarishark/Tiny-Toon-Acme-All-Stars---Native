# Building

Two paths. Docker needs nothing installed but Docker and works the same
everywhere. Native is faster to iterate on but you install the toolchain.

Both end at the same place: a native executable, built from your ROM, on your
machine.

---

## The one thing that is not optional

Your own 1 MB raw `.bin` dump of the game. See [LEGAL.md](LEGAL.md).

Put it next to the build script as `rom.bin`, or pass its path as the first
argument. The build verifies it before doing anything else and tells you exactly
what is wrong if something is:

| what it says | what happened |
|---|---|
| size is wrong | truncated download, or a `.zip`/`.7z` not yet extracted |
| SMD (interleaved) dump | you have a `.smd`; convert it to raw `.bin` |
| not a Mega Drive ROM | no `SEGA MEGA DRIVE` marker at `$100` |
| a different game | valid Mega Drive ROM, wrong title |
| checksum does not match | the file is damaged or was modified |
| different revision | right game, different build — **it will still run**, with both SHA-1s in the log |

---

## Docker

```bash
./build-docker.sh                    # Linux ELF        -> build/tinytoon
./build-docker.sh windows            # Windows .exe     -> build/win/tinytoon.exe
./build-docker.sh windows --release  # single file, no console, with icon
./build-docker.sh linux /path/to/rom.bin
```

The image is built on first use. That build compiles SDL3 from source for both
targets and takes roughly ten minutes; every run afterwards takes seconds.

Your ROM is mounted **read-only** at a fixed path and never enters the image.
The container runs as your user, so the artifacts come out owned by you rather
than by root.

To rebuild the image from scratch: `docker rmi tinytoon-build`.

---

## Native

```bash
./build.sh                  # ./rom.bin, development build
./build.sh --release        # windowed: no console, log to file, icon
./build.sh --clean          # discard objects and generated C, start over
./build.sh -j8              # limit parallelism
```

### Prerequisites

| | |
|---|---|
| Python | 3.9 or newer |
| `capstone` | `pip install capstone` |
| C compiler | gcc or clang |
| make | any POSIX make |
| SDL3 | **3.2 or newer** — not SDL2 |

`build.sh` checks all of these before doing anything and stops with a specific
message naming what is missing.

### Debian / Ubuntu

```bash
sudo apt install build-essential make python3 python3-pip pkg-config
pip install capstone
```

SDL3 is new enough that many distributions still ship SDL2 only. If
`pkg-config --exists sdl3` fails, either use the Docker path or build SDL3:

```bash
git clone --depth 1 --branch release-3.4.14 https://github.com/libsdl-org/SDL
cmake -S SDL -B SDL/build -DCMAKE_BUILD_TYPE=Release
cmake --build SDL/build --parallel
sudo cmake --install SDL/build
sudo ldconfig
```

### Fedora / Arch

```bash
# Fedora
sudo dnf install gcc make python3 python3-pip pkgconf-pkg-config SDL3-devel
# Arch
sudo pacman -S base-devel python python-capstone sdl3
```

### macOS

**Untested.** The runtime is portable C and SDL3 supports macOS, so it should
work, but nobody has run it. If you try it, the result is worth reporting either
way.

```bash
brew install sdl3 python make
pip3 install capstone
./build.sh
```

### Windows

The straightforward path is `./build-docker.sh windows` from anywhere, which
cross-compiles and produces a single-file `.exe`.

Natively, use [MSYS2](https://www.msys2.org/) with the UCRT64 environment:

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL3 \
          mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-pip make
pip install capstone
./build.sh
```

---

## What the build actually does

```
1. check the toolchain          stop early with a specific message
2. verify the ROM               tools/rom_info.py --check
3. translate 68000 to C         tools/recompile.py --all build/recomp
4. generate headers             ROM identity, build version
5. compile in parallel          make -j
```

Step 3 is the one that cannot ship pre-built, and the one that takes a minute or
two the first time. It is skipped on later builds unless the recompiler itself
changed — the check is a timestamp comparison against `tools/recompile.py`, so
editing the recompiler correctly forces a regeneration. That check exists
because without it a change to the recompiler would silently compile the old C
and the build would pass without containing the change.

The generated C is shared between targets; only objects and the executable live
under the per-target build directory.

---

## Build options

Everything is a normal make variable, so you can override any of it:

```bash
make CC=clang
make SDL_CFLAGS=-I/opt/sdl3/include SDL_LIBS="-L/opt/sdl3/lib -lSDL3"
make CFLAGS="-std=c99 -O0 -g"        # debuggable, but slow at run time
```

`-O2` is not decorative. The generated code is an enormous number of small
functions; at `-O0` the game does not reach full speed on modest hardware.

---

## If it goes wrong

**`multiple definition of main`** — something under `runtime/testes/` reached
the link. Those are standalone verification programs with their own `main()`,
and the Makefile's source list excludes them explicitly. If you added sources,
check that list.

**Link fails on SDL symbols** — you have SDL2, or SDL3 headers without the
library. `pkg-config --modversion sdl3` should print 3.2 or higher.

**The game runs but there is no window** — SDL3 built without a video backend.
On Linux that means the X11/Wayland development headers were missing when SDL3
was compiled. The Docker image installs them for this reason.

**It builds and immediately exits** — check `tinytoon.log` next to the
executable in a `--release` build, or the console output otherwise. The ROM
check reports there.

---

## Mods

```bash
cd mods
make                                          # for the host
make WINDOWS=1 CC=x86_64-w64-mingw32-gcc      # cross-compile to .dll
```

Any subdirectory with `.c` files in it is a mod and gets built; there is no list
to keep in sync. Copy the resulting library into `mods/` beside the executable.

A mod links against **nothing** from the game — every service arrives through a
struct of function pointers. That is what makes a mod work against the release
build, which is statically linked and stripped and exports no symbols at all.
See [docs/27-mods.md](docs/27-mods.md).

A headless run writes `tela-mod.png` alongside the clean `tela.png`, so a filter
can be verified without a screen: the difference between the two files is the
mod's effect.

## Verification

The gates that keep the project honest are described in
[docs/22-differential-testing.md](docs/22-differential-testing.md) and
[docs/25-codecs.md](docs/25-codecs.md). Most of them need reference
implementations that are not vendored here — Musashi, Nuked-OPN2, vasm — each
fetched and pinned by its own container in the original repository.

The one you can run immediately, with nothing extra:

```bash
python3 tools/rom_info.py --check    # your ROM against the recorded baseline
python3 tools/trace_code.py          # re-run the tracer; unresolved flow must be 0
python3 tools/rom_coverage.py        # how much of the ROM is classified
```
