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

; BRLOOPS.ASM - RF-emission timing loops for BUSRADIO.
;
; Exports (C calling convention, small model):
;   void emit_tone(unsigned half_period, unsigned cycle_count);
;   void emit_silence(unsigned iters);
;
; Assembler: MASM 6.x  (ml /c /nologo BRLOOPS.ASM)
; These routines are kept in asm so the exact inner-loop instruction mix
; is fixed.  Do not let a compiler reorder the burst/quiet pair -- that
; pattern IS the RF signature.

        .MODEL  SMALL, C
        .CODE

;-----------------------------------------------------------------
; emit_tone - play one note: cycle_count full cycles, each a BURST half
; (8 unrolled di+0/di+07FEh stores, half_period iterations) and a QUIET
; half (8 unrolled fixed-address stores, half_period iterations).
; Alternating the two keys the carrier on and off at the note's rate --
; on-off keying, whose sidebands at carrier +/- f are the tone.  Interrupts
; disabled during each burst/quiet pair to keep timing stable.
;-----------------------------------------------------------------
emit_tone PROC C USES bx di, half_period:WORD, cycle_count:WORD

        mov     ax, half_period
        mov     [et_halfper], ax
        mov     bx, cycle_count
        mov     di, OFFSET rf_scratch     ; set once -- di is never modified
        xor     ax, ax                    ; data = 0 (data bus does not radiate)
                                          ; emit_tone does NOT touch interrupts -- the
                                          ; CALLER owns the policy.  BUSRADIO wraps this
                                          ; in cli/sti (no DMA -> total silence); BDTUNE
                                          ; masks every IRQ except the SB DMA-restart so
                                          ; the single-cycle capture stays alive (steady
                                          ; pitch) without timer/keyboard bus hash.
et_cycle:
        or      bx, bx
        jz      et_done
        dec     bx

        ; --- BURST: di+0 / di+07FEh flips
        ;     address bits A1..A10 together every store -> one strong carrier.
        ;     This inner loop is byte-for-byte identical to BRPLAY's
        ;     play_pcm_chunk burst, so a single bd calibrates BUSRADIO,
        ;     BDTUNE and BRPLAY, and all three put the carrier at the same
        ;     dial frequency (~1500 kHz). ---
        mov     cx, [et_halfper]
et_burst:
        mov     word ptr [di+0], ax
        mov     word ptr [di+07FEh], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+07FEh], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+07FEh], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+07FEh], ax
        dec     cx
        jnz     et_burst

        ; --- QUIET: fixed-address stores -> no toggle -> carrier off.
        ;     Memory stores (not register adds) keep the bus busy so the BIU
        ;     can't prefetch -> no between-station hash. Matches BRPLAY. ---
        mov     cx, [et_halfper]
et_quiet:
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        dec     cx
        jnz     et_quiet

        jmp     et_cycle
et_done:
        ret
emit_tone ENDP

;-----------------------------------------------------------------
; emit_silence - run `iters` QUIET iterations (8 fixed-address stores,
; carrier off = silence) with the SAME per-iteration timing as emit_tone's
; quiet half.  Rests are therefore timed by the same bus_divisor as notes:
; a rest of D ms = 2*bus_divisor*D/1000 iters (frequency cancels out of a
; note's iteration count), so no separate rest calibration is needed.  The
; C caller chunks counts above 65535.
;-----------------------------------------------------------------
emit_silence PROC C USES bx di, iters:WORD

        mov     bx, iters
        or      bx, bx
        jz      es_done
        mov     di, OFFSET rf_scratch
        xor     ax, ax
es_loop:
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        mov     word ptr [di+0], ax
        dec     bx
        jnz     es_loop
es_done:
        ret
emit_silence ENDP

        .DATA
et_halfper      dw      0
        even
rf_scratch      dw      1024 dup(0)       ; 2 KB: burst toggles up to di+07FEh

        END
