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

"""Generate a pure-sine WAV in BRPLAY's format (mono 8-bit unsigned
PCM at the configured sample rate) for confirming the playback chain
works and for calibrating bd against a known frequency.

Usage:
    python tone_wav.py [--hz 800] [--rate 3000] [--secs 8]
                       [--out TONE.WAV]
"""
import argparse
import numpy as np
import soundfile as sf


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--hz',   type=int,   default=800,
                    help='tone frequency in Hz (default 800)')
    ap.add_argument('--rate', type=int,   default=3000,
                    help='sample rate in Hz (default 3000)')
    ap.add_argument('--secs', type=float, default=8.0,
                    help='clip duration in seconds (default 8)')
    ap.add_argument('--out',  type=str,   default=None,
                    help='output filename (default TONE<HZ>.WAV)')
    args = ap.parse_args()

    out = args.out or f"TONE{args.hz}.WAV"

    t = np.arange(int(args.rate * args.secs)) / args.rate
    y = 0.7 * np.sin(2 * np.pi * args.hz * t)

    # 50 ms attack/release ramps to avoid pop on start/end.
    ramp = int(0.050 * args.rate)
    if ramp > 0 and ramp * 2 < len(y):
        y[:ramp]  *= np.linspace(0, 1, ramp)
        y[-ramp:] *= np.linspace(1, 0, ramp)

    sf.write(out, y.astype(np.float32), args.rate, subtype='PCM_U8')
    print(f"Wrote {out}: {args.hz} Hz sine, {args.secs} s, "
          f"{len(y)} samples @ {args.rate} Hz, 8-bit unsigned mono")


if __name__ == '__main__':
    main()
