; Copyright 2026 Rick Altherr
;
; Licensed under the Apache License, Version 2.0 (the "License");
; you may not use this file except in compliance with the License.
; You may obtain a copy of the License at
;
;     http://www.apache.org/licenses/LICENSE-2.0
;
; Unless required by applicable law or agreed to in writing, software
; distributed under the License is distributed on an "AS IS" BASIS,
; WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
; See the License for the specific language governing permissions and
; limitations under the License.

; BRPLOOPS.ASM - PWM emission loops for BRPLAY.
;
; Exports (C calling convention, small model):
;   void emit_pwm(unsigned sample, unsigned units);
;   void play_pcm_chunk(unsigned char far *buf,
;                       unsigned count, unsigned units);
;
; play_pcm_chunk plays a chunk of PRECOMPUTED (burst, quiet) word
; pairs: the C side runs DSM + LFSR dither once at load time and
; stores 2 bytes per sample (low = burst count, high = quiet count,
; both 0..units).  The realtime path here just reads that word,
; splits it, and dispatches the burst+quiet PWM.  Holding per-sample
; work to that one word read keeps the sample-rate noise tone the
; memory traffic adds to the AM output small.
;
; Carrier = the address bus, not the data bus.  Each burst store alternates
; di+0 / di+07FEh, flipping address bits A1..A10 together every store; ten
; wires switching at once sum into a carrier ~10x stronger than toggling
; one.  The quiet phase writes a single fixed address (no toggle -> carrier
; off), store-for-store cycle-matched to the burst.  Data is held at 0
; throughout (it does not radiate).
;
; The audio rides on that carrier as AM: each sample bursts for b of its
; units iterations, so the carrier is present for a fraction b/units of the
; slot -- that ratio is the sample's amplitude.  Varying the ratio across
; samples mixes the audio onto the carrier as sidebands; the fast in-slot
; on/off is a subcarrier up at the sample rate that the radio's IF averages
; off.
;
; Caveat: burst count is one byte, so units must fit in 256.

        .MODEL  SMALL, C
        .CODE

;--- emit_pwm: single-sample PWM, C-callable ---------------------------

emit_pwm PROC C USES bx cx di, sample:WORD, units:WORD

        mov     bx, sample
        mov     cx, units
        cmp     bx, cx
        jbe     ep_ok
        mov     bx, cx
ep_ok:
        sub     cx, bx
        cli
        mov     di, OFFSET pwm_scratch

ep_burst:
        or      bx, bx
        jz      ep_quiet_start
        mov     word ptr [di+0000h], bx
        mov     word ptr [di+0100h], bx
        mov     word ptr [di+0200h], bx
        mov     word ptr [di+0300h], bx
        mov     word ptr [di+0400h], bx
        mov     word ptr [di+0500h], bx
        mov     word ptr [di+0600h], bx
        mov     word ptr [di+0700h], bx
        dec     bx
        jmp     short ep_burst

ep_quiet_start:
        xor     ax, ax
ep_quiet:
        or      cx, cx
        jz      ep_done
        add     ax, 1
        add     ax, 1
        add     ax, 1
        add     ax, 1
        add     ax, 1
        add     ax, 1
        add     ax, 1
        add     ax, 1
        add     ax, 1
        add     ax, 1
        add     ax, 1
        add     ax, 1
        dec     cx
        jmp     short ep_quiet

ep_done:
        sti
        ret
emit_pwm ENDP

;--- play_pcm_chunk: fetch precomputed burst count + dispatch PWM ------

play_pcm_chunk PROC C USES bx cx dx si di es bp,
                       buf_ofs:WORD, buf_seg:WORD,
                       count:WORD, units:WORD

        mov     es, buf_seg               ; read all [bp]-relative params while
        mov     si, buf_ofs               ; BP is still the stack frame ptr
        mov     dx, count
        mov     di, OFFSET pwm_scratch
                                          ; units is unused (quiet count is
                                          ; precomputed); BP stays the frame ptr
        cli

ppc_sample:
        or      dx, dx
        jz      ppc_chunk_done
        dec     dx

        ; --- Fetch precomputed (burst, quiet) word pair ---
        mov     ax, es:[si]               ; AL = burst, AH = quiet
        add     si, 2

        ; --- Split: BX = burst (zero-extended), CX = quiet (zero-ext) ---
        mov     bl, al
        xor     bh, bh
        mov     cl, ah
        xor     ch, ch
        xor     ax, ax                    ; data = 0 (data bus does not radiate)

        ; --- Burst: alternate di+0 / di+07FEh -> toggle A1..A10 together,
        ;     carrier on ---
        or      bx, bx
        jz      ppc_quiet_start
ppc_burst:
        mov     word ptr [di+0], ax
        mov     word ptr [di+07FEh], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+07FEh], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+07FEh], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+07FEh], ax
        dec     bx
        jnz     ppc_burst

        ; --- Quiet: fixed address -> carrier off; 8 stores, cycle-matched
        ;     to the burst ---
ppc_quiet_start:
        or      cx, cx
        jz      ppc_sample_done
ppc_quiet:
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        dec     cx
        jnz     ppc_quiet

ppc_sample_done:
        jmp     ppc_sample

ppc_chunk_done:
        sti
        ret
play_pcm_chunk ENDP

        .DATA
        even
pwm_scratch     dw      1024 dup(0)

        END
