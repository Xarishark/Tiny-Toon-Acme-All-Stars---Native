# 26 — Packaging: from a diagnostic binary to a program

> `./build.sh --release` (or `./build-docker.sh windows --release`) produces a
> **4.84 MB single file, no DLL, no console**, with an icon and version
> information. It depends only on Windows system DLLs.

## What existed, and why it was not enough

The development build already produced a running `.exe`. Measured, it had seven
problems — all invisible to whoever built it, all immediate for whoever receives
it:

| | |
|---|---|
| CONSOLE subsystem | a black console window behind the game |
| `SDL3.dll` alongside | 5.4 MB you can lose or copy halfway |
| **any file accepted as a ROM** | a wrong dump booted into garbage and showed a black screen |
| errors via `printf` | in a windowed app, `printf` is the same as silence |
| the ROM had to be `rom.bin` in the **current directory** | worked from a terminal and failed for shortcuts, "open with" and drag-and-drop |
| no icon, no version | a bug report that cannot say which build it was |
| dumped three PNGs on every exit | litter in the directory of someone who only wants to play |

## The ROM does not ship, and that shapes the rest

The ROM is copyrighted commercial data and the dump belongs to the user. It does
not accompany the executable and never will. Two practical consequences:

**The icon is original.** Cropping a sprite from the game would embed
ROM-derived data in the distributable. `tools/gerar_icone.py` draws a stylised
cartridge by arithmetic, in six sizes, without touching the ROM.

**The program will receive the wrong file.** That is not a hypothesis; it is the
normal first-run path. So it diagnoses.

## ROM diagnosis

`runtime/rom_check.c`, with the identity coming from
`analysis/rom-baseline.json` through a **generated** header
(`tools/gerar_baseline.py`) — copying the values by hand would create a second
source of truth, and two sources diverge.

Seven cases, seven messages, each with the fix:

| verdict | how it is detected |
|---|---|
| `ROM_OK` | SHA-1 matches the baseline |
| `ROM_OUTRA_REVISAO` | header and checksum match, SHA-1 does not — **it still runs**, with both hashes in the log |
| `ROM_SMD` | `size % 16384 == 512` — tested first, because in an interleaved dump no other test means anything |
| `ROM_NAO_E_MEGADRIVE` | no `SEGA MEGA DRIVE` marker at `$100` |
| `ROM_OUTRO_JOGO` | the overseas title at `$150` does not match |
| `ROM_TAMANHO_ERRADO` | ≠ 1,048,576 bytes |
| `ROM_CORROMPIDA` | the checksum at `$18E` does not match the sum over `$200`..end |

All verified with files constructed for each case. And `ROM_OK` on the real ROM
validates the SHA-1 written in C for free: it only passes if it produces exactly
`2672018d9e005a9a3b5006fa8f61e08f2d1909aa`.

A different revision does **not** block running. The game probably works, and
refusing would be stricter than useful — but it is stated, so the difference is
not later blamed on a bug in the port.

## What the release build changes

`TINYTOON_RELEASE` enables `runtime/release.c`:

- **log to a file**, `tinytoon.log` beside the executable, line buffered —
  without that, a hang takes with it exactly the lines that would explain it;
- **errors in a message box**, because in a windowed app `printf` does not exist;
- **the executable's directory**, not the current one — the two differ for
  shortcuts, "open with" and drag-and-drop, and the difference failed silently;
- **a file picker** when the ROM is not found, preceded by an explanation of why
  it did not come with the program.

Outside a release build all of this becomes a no-op and development behaviour
does not change at all.

## Static SDL3

The official SDL3 package for mingw ships **only the import library** —
`libSDL3.dll.a` and nothing else. Without `libSDL3.a` there is no single-file
executable, so the build container compiles SDL3 from source with
`SDL_STATIC=ON`, alongside the shared build used for Linux.

Both are pinned by sha256, for the same reason the vasm container is: a build
that changes on its own is not a reference.

## A quoting trap

Passing the version through `-DTINYTOON_VERSAO=\"$VERSION\"` **does not
survive**. The value crosses ssh, then docker, then make, then the shell of the
compile rule, and each layer eats one level of quoting. The symptom was

```
error: invalid suffix "b98d" on integer constant
```

— the compiler receiving the git hash with no quotes at all. The fix is to have
no layers: `tools/gerar_versao.py` writes `runtime/versao.h`, and a file arrives
intact on the other side.

The `-sujo` ("dirty") marker is not cosmetic. A build made with uncommitted
changes exists nowhere else, and nothing reported about it is reproducible.

## Usage

```bash
./build-docker.sh windows --release
```

```
build/win/tinytoon.exe    4,841,984 bytes, GUI subsystem, icon, no DLL
```

Keep the ROM out of whatever you distribute. It is easy to leave one beside the
executable for testing and then zip the whole folder to send to somebody, which
would be redistributing copyrighted data without noticing.

## What is still missing

- **Code signing.** Without a certificate, SmartScreen will warn on first run. A
  certificate costs money and is the publisher's decision.
- **An installer.** Today it is a loose file, which is the right format for a
  port that needs the ROM beside it.
- **Linux and Steam Deck packaging.** The runtime already compiles on Linux;
  what is missing is the equivalent packaging work — an AppImage would be the
  single-file equivalent.
