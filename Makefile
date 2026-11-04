# Build of the recompiled game.
#
# Two source trees, and the distinction is the whole legal design of this
# project:
#
#   runtime/       hand-written Mega Drive runtime. Our work. Ships with the
#                  repository.
#   build/recomp/  ~1,950 C files, one per routine of the game, GENERATED from
#                  your ROM by tools/recompile.py. Derived from copyrighted
#                  data, so it never ships and never gets committed.
#
# You are not expected to run this by hand. Use ./build.sh, which checks your
# toolchain, verifies the ROM and generates the C before getting here.
#
# Parallel and incremental on purpose: a full build is ~1,950 translation units,
# and touching runtime/vdp.c must not cost recompiling the game's 1,946
# routines. Use -j.

CC      ?= cc
WINDRES ?= windres
OS      ?= $(shell uname -s 2>/dev/null || echo Windows)
BUILD   ?= build
OBJDIR  ?= $(BUILD)/obj
RECOMP  ?= $(BUILD)/recomp
OUT     ?= $(BUILD)/tinytoon

# -O2 is not optional in practice. The generated code is a huge number of small
# functions; at -O0 the game does not reach full speed on modest hardware.
#
# -MMD -MP writes the header dependencies next to each object. Without it,
# editing m68k.h would rebuild nothing and produce a silently inconsistent
# binary — the worst kind of failure, because the compiler never complains.
CFLAGS  ?= -std=c99 -O2 -MMD -MP
CFLAGS  += -DTINYTOON_SDL -Iruntime

LDFLAGS ?=
LIBS    ?= -lm

# --- SDL3 -------------------------------------------------------------------
#
# pkg-config is the portable way to ask where SDL3 is. If your SDL3 is not
# registered with pkg-config, pass the flags yourself:
#
#   make SDL_CFLAGS=-I/path/include SDL_LIBS="-L/path/lib -lSDL3"
SDL_CFLAGS ?= $(shell pkg-config --cflags sdl3 2>/dev/null)
SDL_LIBS   ?= $(shell pkg-config --libs sdl3 2>/dev/null || echo -lSDL3)

CFLAGS += $(SDL_CFLAGS)
LIBS   += $(SDL_LIBS)

# --- platform ---------------------------------------------------------------
ifeq ($(RELEASE),1)
    # A release build is a program someone uses, not a diagnostic binary:
    # log to a file, errors in a message box, a ROM file picker, no console.
    CFLAGS  += -DTINYTOON_RELEASE
endif

ifneq (,$(findstring MINGW,$(OS))$(findstring Windows,$(OS))$(WINDOWS))
    OUT := $(OUT).exe
    ifeq ($(RELEASE),1)
        LDFLAGS += -mwindows -s
        LIBS    += -lcomdlg32
    endif
    # Icone e version info existem so' no release: um binario de diagnostico
    # nao precisa deles, e exigir o .rc quebraria o build de desenvolvimento.
    ifeq ($(RELEASE),1)
        RES ?= $(BUILD)/tinytoon.res
    else
        RES :=
    endif
else ifeq ($(OS),Linux)
    # -rdynamic keeps the symbol names visible to backtrace(), which is what
    # turns a crash into a stack trace in the game's own function names.
    LDFLAGS += -rdynamic
    RES :=
else
    RES :=
endif

# --- sources ----------------------------------------------------------------
#
# The exclusion list is explicit on purpose. A wildcard once dragged in test
# programs with their own main() and broke the link with "multiple definition
# of main", reported at the linker, far from the cause. The reference
# implementations (Nuked-OPN2) deliberately define the SAME symbols as our
# YM2612, so they must stay out too; they live in runtime/testes/.
RUNTIME_SRCS := $(filter-out runtime/ym2612_nuked.c runtime/ym3438.c runtime/ponte_jogo.c, \
                             $(wildcard runtime/*.c))
RECOMP_SRCS  := $(wildcard $(RECOMP)/*.c)
SRCS         := $(RUNTIME_SRCS) $(RECOMP_SRCS)
OBJS         := $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS))

.PHONY: all clean
all: $(OUT)

$(OUT): $(OBJS) $(RES)
	@echo "== linking $(words $(OBJS)) objects"
	@$(CC) $(LDFLAGS) -o $@ $(OBJS) $(RES) $(LIBS)
	@echo "== done: $@"

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tinytoon.res: $(BUILD)/tinytoon.rc
	@$(WINDRES) -O coff -I$(BUILD) $< $@

-include $(OBJS:.o=.d)

clean:
	rm -rf $(OBJDIR) $(OUT)
