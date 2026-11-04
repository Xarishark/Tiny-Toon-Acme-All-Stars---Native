# Builds every mod in this directory, one shared library each.
#
# A mod is a directory with .c files in it. Add one and it gets built; there is
# no list to keep in sync.
#
#   make                     for the host
#   make WINDOWS=1 CC=x86_64-w64-mingw32-gcc     cross-compile to .dll
#
# The output lands in this directory, which is where the game looks for it when
# run from a development build. For a release, copy the libraries into `mods/`
# beside the executable.
#
# A mod links against NOTHING from the game. Every service arrives through the
# MdModHost struct handed to md_mod_iniciar. That is what lets a mod built today
# work with a statically linked, stripped release executable, which exports no
# symbols at all. See runtime/mod.h.

CC     ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -fPIC -I..

ifneq (,$(WINDOWS)$(findstring mingw,$(CC)))
    EXT := .dll
    LDFLAGS += -shared
else ifeq ($(shell uname -s 2>/dev/null),Darwin)
    EXT := .dylib
    LDFLAGS += -dynamiclib
else
    EXT := .so
    LDFLAGS += -shared
endif

# Every subdirectory that contains at least one .c file is a mod.
MODS := $(patsubst %/,%,$(dir $(wildcard */*.c)))
ALVOS := $(foreach m,$(MODS),$(m)$(EXT))

.PHONY: all clean
all: $(ALVOS)
	@echo "== mods: $(ALVOS)"

# -Wl,--no-undefined (and the mach-o equivalent) makes an unresolved symbol a
# link error instead of a load-time failure. Without it, a mod that accidentally
# calls into the game builds fine and then fails to open, with a message that
# names a symbol rather than the mistake.
%$(EXT): $(wildcard %/*.c)
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $*/*.c

clean:
	rm -f *$(EXT)
