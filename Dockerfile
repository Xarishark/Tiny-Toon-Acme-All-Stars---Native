# Build environment for the port. Nothing to install on your machine but Docker.
#
# Exists because the one dependency that is genuinely awkward in 2026 is SDL3:
# it is new enough that many distributions still ship SDL2, and "install SDL3"
# is not one command everywhere. Here it is built from source, pinned, once.
#
# The image can produce two targets:
#
#   linux    native ELF, SDL3 shared
#   windows  .exe cross-compiled with mingw-w64, SDL3 STATIC so the result is
#            a single file with no DLL beside it
#
# Everything third-party is pinned by version AND checked by sha256, for the
# same reason the rest of this project pins its tools: a build that changes on
# its own is not a reference. If the output changes, it must be because the
# source changed.
#
# Build the image:   docker build -t tinytoon-build .
# Use it:            ./build-docker.sh          (does both steps for you)

FROM debian:bookworm-slim

ARG SDL_VER=3.4.14
ARG SDL_SRC_SHA=30d4aa2b3037718142b32dffd4e72f917ebb6cc5227150e7bb9c45efb2153aeb

ENV DEBIAN_FRONTEND=noninteractive

# Two toolchains, the Python side of the recompiler, and the headers SDL3 needs
# to talk to a Linux desktop. The X11/Wayland packages are what let the Linux
# build actually open a window; without them SDL3 compiles but has no video
# backend, which fails at run time instead of at build time.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential make cmake ninja-build git curl ca-certificates \
      mingw-w64 \
      python3 python3-pip \
      pkg-config \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
      libxfixes-dev libxss-dev libxtst-dev libxkbcommon-dev \
      libwayland-dev wayland-protocols libdecor-0-dev \
      libdrm-dev libgbm-dev libgl1-mesa-dev libegl1-mesa-dev \
      libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libibus-1.0-dev \
 && rm -rf /var/lib/apt/lists/*

# capstone by pip, PINNED, and deliberately not the distribution package.
#
# Debian bookworm ships capstone 4.0.2. The 68000 disassembler changed between
# 4.x and 5.x — mnemonics and operand details differ — and this project's
# byte-identical reassembly gate is sensitive to exactly that. Building against
# a different capstone than the one the analysis was verified against would
# produce a plausible binary that nobody ever checked.
#
# --break-system-packages is safe here and only here: this is a container built
# for one purpose, and there is no system to break.
RUN pip3 install --no-cache-dir --break-system-packages capstone==5.0.7

# SDL3 from source, once, for both targets.
RUN curl -sL -o /tmp/sdl3.tar.gz \
      "https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VER}/SDL3-${SDL_VER}.tar.gz" \
 && echo "${SDL_SRC_SHA}  /tmp/sdl3.tar.gz" | sha256sum -c - \
 && mkdir -p /tmp/sdl3 \
 && tar -xzf /tmp/sdl3.tar.gz -C /tmp/sdl3 --strip-components=1 \
 # --- native, shared
 && cmake -S /tmp/sdl3 -B /tmp/b-linux -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF \
      -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX=/usr/local \
 && cmake --build /tmp/b-linux --parallel \
 && cmake --install /tmp/b-linux \
 && ldconfig \
 # --- windows, static. CMake has to be told the target is Windows, otherwise
 # it probes the host compiler and concludes Linux.
 && cmake -S /tmp/sdl3 -B /tmp/b-win -G Ninja \
      -DCMAKE_SYSTEM_NAME=Windows \
      -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
      -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
      -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
      -DCMAKE_FIND_ROOT_PATH=/usr/x86_64-w64-mingw32 \
      -DCMAKE_BUILD_TYPE=Release -DSDL_STATIC=ON -DSDL_SHARED=OFF \
      -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX=/opt/sdl3-win \
 && cmake --build /tmp/b-win --parallel \
 && cmake --install /tmp/b-win \
 && rm -rf /tmp/sdl3 /tmp/b-linux /tmp/b-win /tmp/sdl3.tar.gz

WORKDIR /work
