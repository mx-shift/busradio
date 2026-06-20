# busradio: AM-radio audio over an IBM PC AT's bus emissions

Turn an unmodified IBM PC AT (80286) into a crude AM transmitter by
deliberately leaking RF off the system bus — no DAC, no Sound Blaster
for output, no PWM speaker tricks, just software driving the bus hard
enough to be heard on a nearby AM radio.

Tested on an 8 MHz 80286. No special hardware: an AT and a cheap AM
radio placed within ~30 cm of the case. The carrier lands around
1500 kHz; sweep the dial for it.

Released under the Apache License 2.0 — see `LICENSE`.

---

## Programs

| Program    | What it does |
|------------|--------------|
| `BUSRADIO` | Plays song files (`freq_Hz duration_ms` per line) as tones on a nearby AM radio. |
| `BDTUNE`   | Calibrates the per-machine `bus_divisor` so notes are in tune (needs a Sound Blaster to listen back). |

---

## How it works

### The carrier: flipping address bits A1..A10 together

Every memory access drives the CPU's address and data lines, and those
switching edges radiate RF; the premise is to drive that on purpose and
shape it into an AM signal a radio can pick up. What I *learned* on real
hardware: the radiating is essentially all from the **address** bus —
holding the data fixed, flipping it 0000/FFFF, or writing a counter all
sounded the same on the radio. So the carrier comes from toggling the
address lines (data is held constant — it does nothing), and flipping
*bits A1..A10 together* makes it strong.

The emission loop alternates two phases:

- **Burst** (carrier on): word stores alternate between two scratch
  addresses 0x7FE apart — `[di+0]` and `[di+07FEh]` — so address bits
  A1..A10 all flip *together* on every store, at half the bus-cycle
  rate. Ten wires switching at once sum into one carrier roughly ten
  times stronger than toggling a single wire. The carrier lands around
  1500 kHz, inside the AM band.
- **Quiet** (carrier off): stores all go to one fixed address, so no
  address wire toggles and the carrier collapses. These are still
  memory *stores*, not register-only work — keeping the bus busy stops
  the CPU's prefetch queue from refilling and spraying broadband hash.

Both phases run the same per-iteration instruction sequence, so they
take identical time — the carrier frequency stays put and only its
amplitude is modulated (AM, not FM). Interrupts are disabled during
emission so the timer and keyboard handlers can't chop the carrier.

Audio rides on the carrier by *varying the burst pattern over time*:
that variation multiplies — mixes — with the carrier, and the mixing
products are sidebands at carrier ± the audio frequencies. Those
sidebands are the signal an AM radio tuned to the carrier recovers, no
differently from a broadcast station.

### Tones, and one number for everything

A tone at frequency `f` is burst and quiet alternating at `f` — a burst
block then a quiet block, each half a period long:

    half_period = bus_divisor / f      (inner-loop iterations per half cycle)

That's on-off keying: the carrier times a 50% square wave at `f`, with
sidebands at carrier ± `f` — the *rate* of alternation is the pitch.

`bus_divisor` is a per-CPU calibration constant (default 55000 for an
8 MHz 286; faster CPUs need more). `BDTUNE` finds yours.

Rests reuse it: a `D` ms rest is `2·bus_divisor·D/1000` carrier-off
iterations (frequency cancels out of a note's count), so one number sets
both pitch and rest length.

---

## Building

Borland Turbo C++ 3.0 + Microsoft MASM 6.x, under DOS or DOSBox-X. From
a prompt with `TCC`, `ML`, `TLINK` on `PATH` and `INCLUDE`/`LIB` set:

    cd src
    BUILD

Output lands in `src\BUILD\`.

---

## BUSRADIO

    BUSRADIO [-r] [-d BUSDIV] <songfile>
    BUSRADIO -h

    -r          repeat the song until a key is pressed
    -d BUSDIV   bus divisor for this machine (default 55000)
    -h          show this help

Song files are one note per line, `<freq_Hz> <duration_ms>`. `freq 0`
is a rest. `;` starts a comment:

    330 250
    330 250
    349 500
    0   125     ; rest
    330 250

Up to 2048 notes per file.
Examples:

    BUSRADIO SCALE.TXT             -- C-major scale at the default divisor
    BUSRADIO -d 55000 SCALE.TXT   -- with an explicit bus divisor
    BUSRADIO -r SCALE.TXT         -- loop until keypress

`songs/SCALE_REF.WAV` is a reference recording of what the scale should
sound like, handy while calibrating.

---

## Calibrating

Almost every pitch or timing problem is a wrong `bus_divisor`. `BDTUNE`
finds yours in two passes — TUNE to lock the dial, then CAL to trim the
divisor:

1. Run `BDTUNE`: it emits a reference tone and records the radio back
   through a Sound Blaster ADC. Put an AM radio within ~30 cm of the case
   and patch its headphone/line output into the SB input you pick with
   `-i`.
2. **TUNE** — sweep the dial near 1500 kHz while BDTUNE reports the tone's
   SNR; stop where it peaks (higher is better, ~12 dB+ usable), then press
   SPACE to switch to CAL.
3. **CAL** — adjust `bus_divisor` until the recovered tone matches the
   target: `BDTUNE`'s keys do progressively finer steps and `a` auto-snaps
   via its Goertzel filter. The value it lands on is what you pass to every
   program here.

Rough starting points (BDTUNE finds the exact value):

| CPU            | `bus_divisor` |
|----------------|---------------|
| 6 MHz 80286    | ~41000        |
| 8 MHz 80286    | 55000         |
| 12 MHz 80286   | ~82000        |

### BDTUNE — calibration

    BDTUNE [-t HZ] [-d BUSDIV] [-i SRC]
    BDTUNE -h

    -t HZ      target tone frequency (default 400)
    -d BUSDIV  starting bus divisor (default 55000)
    -i SRC     SB recording input: mic, line, or cd (default line)
    -h         show this help

`BDTUNE` reads the Sound Blaster's I/O from the `BLASTER` environment
variable (A/I/D fields; defaults to A220 I5 D1 if unset). The default
target is 400 Hz — well inside an AM receiver's audio passband, where
the tone is loud and the match is unambiguous.
