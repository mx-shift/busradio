# Copyright 2026 Rick Altherr
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Convert an audio file to a BRPLAY-compatible WAV.

BRPLAY needs mono, 8-bit unsigned PCM at a sample rate well below the
host's CPU clock (1-6 kHz typical).  This script does the full
preparation chain:
    1. Bandpass to keep bass fundamentals + harmonics, drop content
       beyond Nyquist.
    2. Resample to the target rate (default 6 kHz).
    3. AGC to even out the level across the track.
    4. Drive into a look-ahead peak limiter for loudness without
       clipping.  By default --target-crest auto-fits the drive so any
       input lands at the same output density; --target-crest 0 falls
       back to a fixed --gain.
    5. Quantize to 8-bit unsigned PCM.

Usage:
    python wav_extract.py INPUT OUTPUT [--target-crest DB] [--gain G]
        [--start S] [--duration S] [--rate HZ] [--hp HZ] [--lp HZ]
        [--target-rms R]

Input can be any format librosa/soundfile understands (FLAC, MP3, WAV,
OGG, ...).  Output is always a BRPLAY-format .WAV.
"""
import argparse
import math
import numpy as np
import librosa
import scipy.signal as ss
import soundfile as sf

CEILING = 0.95


def high_shelf(x, sr, f0, gain_db):
    """RBJ high-shelf biquad: lift everything above f0 by gain_db.  Used as
    pre-emphasis -- it cannot beat the receiver's bandwidth, but it brightens
    the top of the band the radio still passes so the result is less muffled."""
    A = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * math.pi * f0 / sr
    cw, sw = math.cos(w0), math.sin(w0)
    tsa = 2.0 * math.sqrt(A) * (sw / 2.0 * math.sqrt(2.0))   # shelf slope S = 1
    b = np.array([     A * ((A + 1) + (A - 1) * cw + tsa),
                  -2 * A * ((A - 1) + (A + 1) * cw),
                       A * ((A + 1) + (A - 1) * cw - tsa)])
    a = np.array([         (A + 1) - (A - 1) * cw + tsa,
                   2 *    ((A - 1) - (A + 1) * cw),
                          (A + 1) - (A - 1) * cw - tsa])
    return ss.lfilter(b / a[0], a / a[0], x)


def agc(x, sr, target_rms=0.5, attack_ms=2.0, release_ms=200.0,
        max_gain_db=30.0):
    """Sliding-window AGC: track short-term RMS and divide it out so
    every region of the signal lands at target_rms.  Fast attack
    catches transients; slower release prevents pumping."""
    att = 1.0 - np.exp(-1.0 / (attack_ms  * 0.001 * sr))
    rel = 1.0 - np.exp(-1.0 / (release_ms * 0.001 * sr))
    max_gain = 10.0 ** (max_gain_db / 20.0)
    sq_env = 0.0
    out = np.empty_like(x)
    for i, v in enumerate(x):
        sq = v * v
        coeff = att if sq > sq_env else rel
        sq_env = sq_env + coeff * (sq - sq_env)
        rms = np.sqrt(sq_env) + 1e-9
        gain = min(target_rms / rms, max_gain)
        out[i] = v * gain
    return out


def limiter(x, sr, ceiling=CEILING, attack_ms=1.0, release_ms=80.0):
    """Look-ahead peak limiter.

    Builds a gain-reduction envelope in two rate-limited passes: a
    forward pass drops the gain instantly at each peak and lets it
    recover at the release rate, and a backward pass ramps the gain down
    *ahead* of each peak at the attack rate (the look-ahead).  Because
    neither pass ever lifts the gain above ceiling/|x|, the result
    satisfies |output| <= ceiling with no hard clipping."""
    n = len(x)
    if n == 0:
        return x
    need = np.minimum(1.0, ceiling / (np.abs(x) + 1e-12)).tolist()
    rise = math.exp(1.0 / (release_ms * 1e-3 * sr))   # release: max gain rise / sample
    fall = math.exp(1.0 / (attack_ms  * 1e-3 * sr))   # attack: look-ahead ramp / sample
    gain = [0.0] * n

    prev = need[0]
    for i in range(n):                      # forward: instant attack, slow release
        cap = prev * rise
        g = need[i] if need[i] < cap else cap
        gain[i] = g
        prev = g

    nxt = gain[-1]
    for i in range(n - 1, -1, -1):          # backward: ramp down before peaks
        cap = nxt * fall
        if gain[i] > cap:
            gain[i] = cap
        nxt = gain[i]

    return x * np.asarray(gain, dtype=x.dtype)


def crest_db(y):
    pk = float(np.max(np.abs(y)))
    rms = float(np.sqrt(np.mean(y ** 2)))
    return 20.0 * math.log10((pk + 1e-12) / (rms + 1e-12))


def normalize(y, ceiling=CEILING):
    pk = float(np.max(np.abs(y)))
    return y * (ceiling / pk) if pk > 0 else y


def fit_drive(x, sr, target_crest_db, ceiling=CEILING, release_ms=40.0,
              iters=16, tol=0.1):
    """Binary-search the pre-limiter drive (dB) so the limited and
    peak-normalized output lands at target_crest_db.  Output crest falls
    monotonically as drive rises (more peaks pushed into the limiter), so
    the search is well-behaved.  A target below the limiter's crest floor
    (set by release_ms) saturates the drive; a target above the source's
    own crest backs the drive off -- either way the caller compares the
    returned crest with its target to tell hit from clamp.

    Returns (output, drive_db, achieved_crest_db)."""
    lo, hi = -6.0, 54.0
    out = None
    for _ in range(iters):
        drive = 0.5 * (lo + hi)
        y = normalize(
            limiter(x * (10.0 ** (drive / 20.0)), sr, release_ms=release_ms),
            ceiling)
        c = crest_db(y)
        out = (y, drive, c)
        if abs(c - target_crest_db) <= tol:
            break
        if c > target_crest_db:     # too dynamic -> drive harder
            lo = drive
        else:                       # too squashed -> back off
            hi = drive
    return out


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input',  help='source audio file')
    ap.add_argument('output', help='destination .WAV (BRPLAY format)')
    ap.add_argument('--start',    type=float, default=0.0,
                    help='offset in seconds (default 0)')
    ap.add_argument('--duration', type=float, default=None,
                    help='clip length in seconds (default: to end)')
    ap.add_argument('--rate', type=int, default=6000,
                    help='output sample rate Hz (default 6000)')
    ap.add_argument('--hp',   type=float, default=80.0,
                    help='high-pass corner Hz (default 80)')
    ap.add_argument('--lp',   type=float, default=None,
                    help='low-pass corner Hz (default rate/2 - 100)')
    ap.add_argument('--target-rms', type=float, default=0.5,
                    help='AGC target RMS, 0..1 (default 0.5)')
    ap.add_argument('--target-crest', type=float, default=6.0,
                    help='auto-fit limiter drive to this output crest '
                         'factor in dB; set 0 to use fixed --gain '
                         '(default 6)')
    ap.add_argument('--gain', type=float, default=8.0,
                    help='fixed pre-limiter drive (linear) used when '
                         '--target-crest is 0 (default 8)')
    ap.add_argument('--release', type=float, default=40.0,
                    help='limiter release ms; shorter = denser but more '
                         'pumping, and lowers the reachable crest floor '
                         '(default 40)')
    ap.add_argument('--pre-emphasis', type=float, default=6.0,
                    help='high-shelf boost in dB above 600 Hz to brighten the '
                         'highs the AM receiver still passes (default 6; 0 off)')
    args = ap.parse_args()

    if args.lp is None:
        args.lp = args.rate / 2.0 - 100.0

    y, sr = librosa.load(args.input, sr=44100, mono=True,
                         offset=args.start, duration=args.duration)

    sos_hp = ss.butter(4, args.hp, btype='high', fs=sr, output='sos')
    sos_lp = ss.butter(8, args.lp, btype='low',  fs=sr, output='sos')
    y = ss.sosfilt(sos_hp, y)
    y = ss.sosfilt(sos_lp, y)

    y_out = librosa.resample(y, orig_sr=sr, target_sr=args.rate)
    if args.pre_emphasis != 0.0:
        y_out = high_shelf(y_out, args.rate, 600.0, args.pre_emphasis)
    y_out = agc(y_out, args.rate, target_rms=args.target_rms)

    if args.target_crest > 0:
        y_out, drive_db, achieved = fit_drive(y_out, args.rate,
                                              args.target_crest,
                                              release_ms=args.release)
        gap = achieved - args.target_crest
        note = ("" if abs(gap) <= 0.15 else
                " (limiter floor; lower --release to go denser)" if gap > 0
                else " (source already denser than target)")
        mode = (f"crest {achieved:.1f} dB [target {args.target_crest:.1f}"
                f"{note}] @ drive {drive_db:+.1f} dB")
    else:
        y_out = normalize(limiter(y_out * args.gain, args.rate,
                                  release_ms=args.release))
        mode = f"fixed drive x{args.gain:g}"

    sf.write(args.output, y_out.astype(np.float32), args.rate,
             subtype='PCM_U8')

    rms_db = 20 * np.log10(float(np.sqrt(np.mean(y_out ** 2))) + 1e-12)
    print(f"Wrote {args.output}: {len(y_out)} samples, "
          f"{len(y_out)/args.rate:.2f} s @ {args.rate} Hz, 8-bit unsigned "
          f"mono | {mode} | RMS {rms_db:.1f} dBFS, crest "
          f"{crest_db(y_out):.1f} dB")


if __name__ == '__main__':
    main()
