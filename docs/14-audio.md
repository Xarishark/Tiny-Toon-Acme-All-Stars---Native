# 14 — Audio

> State: sound generated **alongside the clock**, stereo, with LFO, verified
> against Nuked-OPN2. Measured as unused by this game: SSG-EG.

## Why the Z80 is the gate

The 68000 does not touch the YM2612 in this game. At boot it writes ~8 KB of
driver into Z80 RAM, byte by byte, and after that only deposits commands into a
queue and requests the bus at `$A11100` — 52,930 times every 3,000 frames. What
generates music and effects is the Z80 running that driver.

Without a Z80 executing, the game is silent no matter how complete the YM2612 is.
So it came first.

## An interpreter, not a recompiler

The 68000 code is statically recompiled because it lives in ROM and can be
analysed before running. The Z80 driver **lives in RAM and is written at run
time** — there is nothing to analyse statically. That is a difference in nature
between the two processors of this port, not a convenience choice.

## Field-based decoding

The Z80 opcode map is regular: `$40-$7F` is entirely `LD r,r'`, `$80-$BF` is
entirely `ALU A,r`. Instead of 256 hand-written cases, the decoder uses the
opcode's fields:

```
x = op >> 6      y = (op >> 3) & 7     z = op & 7
p = y >> 1       q = y & 1
```

Half the table becomes two loops. Less code, and above all fewer places to get a
letter wrong.

The `DD`/`FD` prefixes swap HL for IX/IY and `(HL)` for `(IX+d)`. Instead of
duplicating the tables, the prefix is a parameter that changes register access.

## Validation: zexdoc and zexall

These are Z80 programs that exercise every instruction with millions of operand
and flag combinations, comparing the result's CRC against that of a real Z80.
Same spirit as the 68000's differential test — a reference written by somebody
else — only packaged as a CRC instead of an emulator.

```
zexdoc  (documented flags)      67 of 67   OK
zexall  (includes X and Y)      66 of 67
```

5.7 billion instructions per pass.

**The single failure, with exact scope:** `bit n,<b,c,d,e,h,l,(hl),a>` in zexall.
The undocumented X and Y flags of `BIT n,(HL)` do not come from the operand —
they come from the high byte of **MEMPTR**, an internal Z80 register no manual
describes and this core does not model. For `BIT n,r` and `BIT n,(IX+d)` the
flags are correct; only the `(HL)` form depends on MEMPTR, and the test groups
all three into one CRC.

Modelling MEMPTR properly means touching nearly every instruction (it is updated
by `LD A,(nn)`, `JP`, `CALL`, `EX (SP),HL`, `ADD HL,rp`, the block instructions,
interrupts) in exchange for two flag bits no software reads. It is recorded as a
known gap, with the scope measured: **if a sound bug ever points at flags, this is
the first place to look.**

Implementing it halfway would be worse than not implementing it — it would be
wrong in a different way, without the test noticing.

## The Z80 bus

```
$0000-$1FFF  own RAM, 8 KB          $6000-$60FF  bank register
$2000-$3FFF  mirror                 $7F11        PSG
$4000-$5FFF  YM2612                 $8000-$FFFF  32 KB window into the 68000 bus
```

The bank register is a **shifter**: each write pushes one bit into the top of a
9-bit register, so changing bank costs nine writes. Not a quirk — it is what fit
on the pins.

Z80 RAM left `memory.c` when the Z80 started executing. Two copies of the same
state would mean two owners: the 68000 writing to one and the Z80 reading from
the other.

## Two halves of the YM2612

They live in separate files on purpose, because they fail in different ways:

- **`ym2612.c` — the system.** Registers, two timers, status, DAC. This is what
  the driver depends on to **advance**: almost every Mega Drive driver keeps time
  by reading the timer overflow bit, not by counting frames. An error here
  produces **total silence**, because the driver waits forever on a bit.
- **`ym2612_fm.c` — the synthesis.** Phases, envelopes, eight algorithms,
  feedback. This is what the sound **coming out** depends on. An error here
  produces **wrong sound**.

## The logarithmic domain

Understanding this is understanding the chip: the sine is tabulated as
**attenuation in dB**, not as amplitude. Adding attenuations multiplies
amplitudes, so applying envelope and volume becomes integer addition, and only at
the end does an exponential table return the amplitude. There is not a single
multiplier in the signal path.

```
attenuation = log_sine[phase] + envelope + volume
sample      = 2^(-attenuation)
```

## Three scaling errors, and what each produced

None showed up at compile time. All were found by **measuring** — recording WAV
and looking at energy per second, not "it seems like sound came out".

**The YM2612 clock wrong by 7×.** I had `7670454/7`; the chip receives the same
clock as the 68000, undivided. That does not produce wrong sound: it produces
**music at the wrong tempo**, because both timers count on that base. The symptom
was 239 notes in 3,000 frames where there should have been 920.

**The operator output in 10 bits.** The exponential was scaled to 1024 instead of
8192. Two consequences at once: sound eight times too quiet **and the wrong
timbre**, because in FM the modulator's amplitude *is* the modulation index.

**Z80 cycle debt discarded.** `z80_bus_executar` asked for 1 cycle, executed a
4-cycle instruction and forgot the 3 left over. Since the call comes on every
68000 access, that gave one instruction per access — 145 million per 3,000
frames, three times the chip's real rate.

## Checking without ears

`TINYTOON_WAV=sound.wav` in the headless build records the output. You can then
measure energy per second, count clipped samples and compare against a reference
recording:

```
   18s  rms    192  peak   1437
   24s  rms   1220  peak   6346  ######
   30s  rms      0  peak      0            <- screen transition
   36s  rms    907  peak   4411  ####
```

Zero clipped samples in 60 s.

## The error that turned PCM into noise

Reported as "some sound effects play noise instead of the sound", comparing
against Kega Fusion. The cause was architectural, and it is worth more than the
fix.

Audio was generated **in a batch of 882 samples at the end of the frame**. At
that instant register `$2A` — the DAC sample — has **one** value: the last one
the Z80 wrote. All 882 samples received that same value, when they should have
been ~880 distinct ones. All PCM became noise, and for the same reason FM only
saw register state at the frame boundary: **every note change and every envelope
step happened with 50 Hz granularity**.

`audio_gerar()` is now called from the same point that advances the Z80, sample
by sample, and the result goes into a ring the host drains. Sound and emulation
see the same time.

### And the half I got wrong on the first attempt

Generating alongside the clock, on its own, made the sound **worse**: the noise
spread from the effects into the music too.

The cause was tying production to the **memory access count**, which is an
estimate and varies frame to frame. Measured: 63.7 s of audio where there should
have been 60.0 — 6% too many samples. The device queue fills, the next frame is
discarded whole, and **discarding samples in the middle of a waveform is
literally a click generator**.

The fix has two halves, and both matter:

- **Generating alongside the clock** is what makes the DAC be sampled at the
  right instant. Without it, PCM becomes noise.
- **Locking the total per frame** is what stops the rate from drifting. The
  amount generated per access is proportional to how much of the frame has
  elapsed, and V-blank completes whatever is missing. Measured result: 1065.0
  pairs per frame, exactly.

Verification: maximum jump between consecutive samples of 6,500 (on a ±32,768
scale), with 99.99% of jumps below 1,000 — a normal waveform distribution,
without the giant step a discarded chunk produces.

## Stereo, and the native rate

Each channel has two bits in `$B4` deciding whether it goes left, right or both.
Summing everything to mono discarded information the game was deliberately
providing — 49% of samples have L ≠ R.

Generation moved to the **chip's native rate** (53,267 Hz), letting SDL convert
for the device. That removed a resampling stage of my own, which was
sample-and-hold — the crudest method there is.

## The output rate, and the rare hiss

Two defects, and the second was the structural one.

**Integer division.** 53,267 / 50 is not an integer: it is 1065.34. Producing
1065 per frame means a real output of 53,250 samples per second, not 53,267.
While the device was opened at 53,267, that 17-sample difference drained the
queue slowly until it **ran dry** — a dropout roughly every 63 seconds, which is
a rare hiss with no apparent pattern. It cost time precisely because it was rare:
it looked like a synthesis defect, and it was integer division.

**No feedback.** Production and consumption were perfectly matched at 1065 per
frame, which meant the queue level was merely "whatever was left over": every
scheduling hiccup lowered the water line permanently, and nothing raised it back.
Measured, the queue repeatedly scraped zero — and a queue at zero is the device
playing silence.

The frame now waits until the audio device has consumed down to a target,
instead of waiting on a wall clock. The audio device's clock becomes the master,
which is the right master here — 40 ms of missing audio is heard immediately, 1%
of frame variation is not seen. It self-regulates in both directions.

```
                     before              after
queue            0 – 2,828, hitting 0    1,990 – 3,194 (40–60 ms)
frames with an empty ring    several     0
steady-state speed              —        50.03 frames/s (+0.06%)
```

## LFO: exact tremolo, approximate vibrato

Without LFO the sound is "flat" — right note, right timbre, and still audibly
different from the console, because almost every Mega Drive tune uses vibrato on
sustained notes.

- **Tremolo (AM)** is exact: the depth in dB is in the datasheet (0 / 1.4 / 5.9 /
  11.8) and enters by adding attenuation, which is this chip's currency.
- **Vibrato (PM)** is computed from the **documented depths in cents** (3.4 / 6.7
  / 10 / 14 / 20 / 40 / 80), not from the chip's internal table, which I do not
  have. It gives vibrato at the right rate and depth, but not bit-exact. **It is
  marked in the code as the remaining difference.**

Implementing PM from the wrong table would be worse than approximating: it would
produce out-of-tune notes instead of slightly different vibrato.

## Differential verification against Nuked-OPN2

After two rounds of trying to guess by ear what was wrong — and guessing wrong —
the audio got the same treatment the 68000 got with Musashi: **an independent
reference**.

[Nuked-OPN2](https://github.com/nukeykt/Nuked-OPN2) was written from analysis of
the chip's **silicon**, not from the datasheet. It is independent of my reading,
which had already got the operator scale and the chip clock wrong.

### Two tools, for two questions

**`tools/ymdiff.sh` — the whole game.** Builds two binaries differing in one
thing: whose sample comes out. It answers "does it differ?".

**`runtime/testes/fmdiff.c` — patch by patch.** It answers "**where** does it
differ?", which is the useful question. Running the game mixes six channels,
envelopes at different phases and the DAC; one patch at a time isolates a
variable.

### Result

```
patch                        energy mine/ref   spectral centroid mine/ref
alg7 4 carriers TL=0              10.50               0.94
alg0 chain TL=0                   10.30               0.86
alg4 two chains                   10.50               1.11
alg4 feedback 7                   10.41               1.34
alg2 / alg5 TL=40                 10.3                0.97 / 0.95
DAC square wave 170 Hz            10.42                 —

envelope (AR31 DR16 SL8 SR8), per 50 ms:
  mine  714  555  250  136  128  121
  ref   710  551  261  151  146  138
```

The energy is 10.4× across **everything** — a uniform scale factor of mine, not a
fidelity error. What matters is the shape, and it matches: spectral centroid
within ±15% on seven of the eight patches, and the envelope within 5-10% point by
point.

### The bug this found

After key-off, mine did not go quiet: it sat at a constant floor of **591**,
identical across all patches. An identical value across different tests is not a
patch coincidence — and 591 × 10.4 = 6,146 = exactly `dac_value × 64` from the
previous test.

**`fm_reset` was not clearing the DAC state.** Channel 6 kept emitting the last
sample forever, a constant level added to everything. That was why the first
envelope measurement reported "decay 4× too slow" — it was not the envelope, it
was a floor that never came down.

The lesson from measurement: when a number appears **identical** across
experiments that should diverge, it does not come from the experiment. That is
what separated "my envelope is wrong" from "my envelope is right and there is
garbage added on top".

## What is missing

| | |
|---|---|
| SSG-EG | sawtooth envelope mode (register `$90`). Measured: this game never enables it |
| Bit-exact PM | today computed from documented cents, not from the chip's table |
| MEMPTR | the X and Y flags of `BIT n,(HL)` — see above |
