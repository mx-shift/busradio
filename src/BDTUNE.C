/*
 * Copyright 2026 Rick Altherr
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* BDTUNE.C - bus_divisor tuner for BUSRADIO
 *
 * Target: IBM PC AT (80286, 6 or 8 MHz), MS-DOS 3.x+
 * Compiler: Borland Turbo C++ 3.0, small memory model
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>
#include <alloc.h>

extern void emit_tone(unsigned half_period, unsigned cycle_count);

/*---- Configuration -------------------------------------------------------*/
#define DEFAULT_TARGET_HZ    400U     /* well inside AM IF passband (800 Hz */
                                      /* sat near the ~1 kHz roll-off knee)  */
#define DEFAULT_BUS_DIVISOR  55000UL
#define NOMINAL_RATE         8000U
#define DMA_BUFFER_SIZE      16384U

static unsigned target_hz   = DEFAULT_TARGET_HZ;
static unsigned long bus_divisor = DEFAULT_BUS_DIVISOR;
static unsigned SAMPLE_RATE = NOMINAL_RATE;

/*---- Sound Blaster I/O config -------------------------------------------*/
/* Defaults used when the BLASTER environment variable is absent. */
#define SB_DEFAULT_BASE     0x220
#define SB_DEFAULT_IRQ      5
#define SB_DEFAULT_DMA      1

/* DSP/mixer I/O port offsets from sb_base. */
#define SB_MIXER_ADDR       0x04   /* mixer register select */
#define SB_MIXER_DATA       0x05   /* mixer register data */
#define SB_DSP_RESET        0x06   /* DSP reset: pulse 1 then 0 */
#define SB_DSP_READ_DATA    0x0A   /* DSP read-data port */
#define SB_DSP_WRITE        0x0C   /* DSP command/data write; read = write status */
#define SB_DSP_READ_STATUS  0x0E   /* DSP read status; also 8-bit DMA IRQ ack */
#define SB_DSP_ACK_16BIT    0x0F   /* 16-bit DMA IRQ ack */

/* Bit 7 of the DSP status ports. */
#define SB_DSP_WRITE_BUSY   0x80   /* read at 0x0C: 1 = not ready for a byte */
#define SB_DSP_DATA_READY   0x80   /* read at 0x0E: 1 = a byte is waiting */

/* DSP commands and the reset handshake byte. */
#define SB_DSP_SET_RATE     0x40   /* set sample-rate time constant */
#define SB_DSP_ADC_8BIT     0x24   /* single-cycle 8-bit ADC */
#define SB_DSP_HALT_DMA     0xD0   /* halt an 8-bit DMA transfer */
#define SB_DSP_GET_VERSION  0xE1
#define SB_DSP_READY        0xAA   /* reset acknowledge byte */

/* Mixer registers (CT1335 SB Pro and CT1745 SB16). */
#define SB_MIX_RESET        0x00   /* SB Pro: reset the mixer */
#define SB_MIX_INPUT_CTL    0x0C   /* SB Pro: input select + filter */
#define SB_MIX_MIC_LVL      0x0A   /* SB Pro: mic input level */
#define SB_MIX_CD_LVL       0x28   /* SB Pro: CD input level */
#define SB_MIX_LINE_LVL     0x2E   /* SB Pro: line input level */
#define SB_MIX_RECSRC_L     0x3D   /* SB16: record source, left */
#define SB_MIX_RECSRC_R     0x3E   /* SB16: record source, right */

/* SB16 record-source bits (written to SB_MIX_RECSRC_L/R). */
#define SB16_REC_MIC        0x01
#define SB16_REC_CD         0x60   /* CD left + right */
#define SB16_REC_LINE       0x18   /* line left + right */

/* SB Pro input select (written to SB_MIX_INPUT_CTL) and channel levels. */
#define SBPRO_IN_MIC        0x20
#define SBPRO_IN_CD         0x22
#define SBPRO_IN_LINE       0x26
#define SBPRO_LVL_HIGH      0xEE   /* line/CD: near-max L+R */
#define SBPRO_LVL_MIC       0x06

/* 8-bit PCM silence (unsigned mid-scale). */
#define PCM_SILENCE         0x80

/* 8237 DMA controller (8-bit channels). */
#define DMA_MASK_REG        0x0A   /* single-channel mask */
#define DMA_MODE_REG        0x0B   /* mode */
#define DMA_CLEAR_FF        0x0C   /* clear byte-pointer flip-flop */
#define DMA_MASK_ON         0x04   /* OR with channel to set its mask bit */
#define DMA_MODE_ADC        0x54   /* single, auto-init, device->memory */
#define DMA1_ADDR           0x02   /* channel 1 base-address register */
#define DMA1_COUNT          0x03   /* channel 1 count register */
#define DMA1_PAGE           0x83   /* channel 1 page register */
#define DMA_64K             0x10000UL    /* a transfer may not cross this */
#define ISA_DMA_LIMIT       0x1000000UL  /* 16 MB ISA DMA ceiling */

/* 8259 programmable interrupt controller. */
#define PIC1_CMD            0x20   /* master: command/status */
#define PIC1_DATA           0x21   /* master: interrupt mask */
#define PIC2_CMD            0xA0   /* slave: command/status */
#define PIC2_DATA           0xA1   /* slave: interrupt mask */
#define PIC_EOI             0x20   /* non-specific end-of-interrupt */
#define PIC1_VECTOR_BASE    0x08   /* IRQ0..7  -> INT 08h.. */
#define PIC2_VECTOR_BASE    0x70   /* IRQ8..15 -> INT 70h.. */

/* Port 0x80 (POST) read as a short I/O settling delay. */
#define IO_DELAY_PORT       0x80

static unsigned sb_base = SB_DEFAULT_BASE;
static int      sb_irq  = SB_DEFAULT_IRQ;
static int      sb_dma  = SB_DEFAULT_DMA;
static int      blaster_seen = 0;

static unsigned char dsp_ver_major = 0;
static unsigned char dsp_ver_minor = 0;

static void parse_blaster_env(void)
{
    char *blaster_env = getenv("BLASTER");
    char *cursor;
    if (!blaster_env) return;
    blaster_seen = 1;
    cursor = blaster_env;
    while (*cursor) {
        char field_tag = (char)((*cursor >= 'a' && *cursor <= 'z')
                                    ? *cursor - 'a' + 'A' : *cursor);
        cursor++;
        if (field_tag == 'A') sb_base = (unsigned)strtoul(cursor, &cursor, 16);
        else if (field_tag == 'I') sb_irq = (int)strtoul(cursor, &cursor, 10);
        else if (field_tag == 'D') sb_dma = (int)strtoul(cursor, &cursor, 10);
        else { while (*cursor && *cursor != ' ') cursor++; }
        while (*cursor == ' ') cursor++;
    }
}

/*---- DSP I/O -------------------------------------------------------------*/
/* Wait for the DSP write-ready bit, then write one command/data byte.
 *
 * Returns:
 *   1  the byte was sent
 *   0  the DSP never went ready (timed out); nothing was written
 */
static int dsp_write(unsigned char data_byte)
{
    unsigned retries_remaining = 30000;
    while (retries_remaining && (inp(sb_base + SB_DSP_WRITE) & SB_DSP_WRITE_BUSY))
        retries_remaining--;
    if (!retries_remaining)
        return 0;
    outp(sb_base + SB_DSP_WRITE, data_byte);
    return 1;
}

/* Wait for the DSP data-available bit, then read one byte from the
 * read-data port.
 *
 * Returns:
 *   0..255  the DSP data byte
 *   -1      the wait timed out; nothing was read
 */
static int dsp_read(void)
{
    unsigned retries_remaining = 30000;
    while (retries_remaining && !(inp(sb_base + SB_DSP_READ_STATUS) & SB_DSP_DATA_READY))
        retries_remaining--;
    if (!retries_remaining)
        return -1;
    return inp(sb_base + SB_DSP_READ_DATA) & 0xFF;
}

/*---- Mixer (CT1335 SB Pro / CT1745 SB16) --------------------------------*/
#define INPUT_LINE 0
#define INPUT_MIC  1
#define INPUT_CD   2
static int input_source = INPUT_LINE;

static void mixer_write(unsigned char mixer_register, unsigned char mixer_value)
{
    outp(sb_base + SB_MIXER_ADDR, mixer_register);
    outp(sb_base + SB_MIXER_DATA, mixer_value);
}

static void mixer_set_input(void)
{
    if (dsp_ver_major >= 4) {
        /* CT1745 record-source bits at 0x3D (left) / 0x3E (right):
         *   bit 7=mic, 6=CD-R, 5=CD-L, 4=Line-R, 3=Line-L.
         */
        unsigned char record_source;
        switch (input_source) {
        case INPUT_MIC: record_source = SB16_REC_MIC; break;
        case INPUT_CD:  record_source = SB16_REC_CD; break;
        default:        record_source = SB16_REC_LINE; break;
        }
        mixer_write(SB_MIX_RECSRC_L, record_source);
        mixer_write(SB_MIX_RECSRC_R, record_source);
    } else if (dsp_ver_major == 3) {
        /* CT1335 SB Pro: register 0x0C selects input + LPF bypass. */
        unsigned char input_select;
        switch (input_source) {
        case INPUT_MIC: input_select = SBPRO_IN_MIC; break;
        case INPUT_CD:  input_select = SBPRO_IN_CD; break;
        default:        input_select = SBPRO_IN_LINE; break;
        }
        mixer_write(SB_MIX_RESET, 0x00);
        mixer_write(SB_MIX_INPUT_CTL, input_select);
        if (input_source == INPUT_LINE) mixer_write(SB_MIX_LINE_LVL, SBPRO_LVL_HIGH);
        else if (input_source == INPUT_CD) mixer_write(SB_MIX_CD_LVL, SBPRO_LVL_HIGH);
        else mixer_write(SB_MIX_MIC_LVL, SBPRO_LVL_MIC);
    }
    /* SB 1.x / 2.0 has no mixer; nothing to configure. */
}

/*---- DSP reset -----------------------------------------------------------*/
/* Pulse the DSP reset line and wait for the 0xAA ready byte.
 *
 * Returns:
 *   1  reset acknowledged (read back 0xAA)
 *   0  no card or wrong ready byte
 */
static int sb_reset(void)
{
    int ready_byte;
    unsigned wait_index;
    outp(sb_base + SB_DSP_RESET, 1);
    for (wait_index = 0; wait_index < 4; wait_index++) inp(IO_DELAY_PORT);
    outp(sb_base + SB_DSP_RESET, 0);
    for (wait_index = 0; wait_index < 100; wait_index++) {
        if (inp(sb_base + SB_DSP_READ_STATUS) & SB_DSP_DATA_READY) break;
        delay(1);
    }
    ready_byte = inp(sb_base + SB_DSP_READ_DATA) & 0xFF;
    if (ready_byte != SB_DSP_READY)
        printf("  DSP reset read back 0x%02X (expected AA)\r\n", ready_byte);
    return ready_byte == SB_DSP_READY;
}

/*---- DMA buffer + ISR ----------------------------------------------------*/
static unsigned char far *dma_buffer;
static unsigned long dma_phys_addr;
static volatile unsigned dma_irq_count = 0;
static void (interrupt far *old_irq_vec)(void);
static int irq_installed = 0;

/* Issue the single-cycle 8-bit ADC command (0x24 + 16-bit count) to
 * (re)arm a capture.
 *
 * Returns:
 *   1  all three command bytes were sent
 *   0  a write timed out; the ADC was not armed
 */
static int start_adc(void)
{
    unsigned transfer_count = DMA_BUFFER_SIZE - 1;
    int ok = dsp_write(SB_DSP_ADC_8BIT);
    ok = ok && dsp_write((unsigned char)(transfer_count & 0xFF));
    ok = ok && dsp_write((unsigned char)((transfer_count >> 8) & 0xFF));
    return ok;
}

static void interrupt far sb_isr(void)
{
    volatile int discarded_status;
    dma_irq_count++;
    discarded_status = inp(sb_base + SB_DSP_READ_STATUS);
    (void)discarded_status;
    if (sb_irq >= 8) outp(PIC2_CMD, PIC_EOI);
    outp(PIC1_CMD, PIC_EOI);
    (void)start_adc();      /* re-arm; an ISR can't report a failure */
}

/* Allocate a DMA-safe capture buffer: farmalloc enough slack to bump a
 * paragraph-aligned region that does not straddle a 64 KB boundary and
 * stays below the 16 MB ISA DMA limit, then prefill it with silence.
 *
 * Returns:
 *   1  buffer allocated; dma_buffer / dma_phys_addr set
 *   0  out of memory, bump overflowed the alloc, or region above 16 MB
 */
static int alloc_dma_buffer(void)
{
    unsigned char far *raw_buffer;
    unsigned long raw_phys_addr, aligned_phys_addr;
    raw_buffer = (unsigned char far *)farmalloc(DMA_BUFFER_SIZE * 2UL + 256UL);
    if (!raw_buffer) return 0;
    raw_phys_addr = ((unsigned long)FP_SEG(raw_buffer) << 4)
                    + FP_OFF(raw_buffer);
    aligned_phys_addr = (raw_phys_addr + 15UL) & ~0xFUL;
    if ((aligned_phys_addr & 0xFFFFUL) + DMA_BUFFER_SIZE > DMA_64K)
        aligned_phys_addr = (aligned_phys_addr | 0xFFFFUL) + 1UL;
    if (aligned_phys_addr + DMA_BUFFER_SIZE
            > raw_phys_addr + DMA_BUFFER_SIZE * 2UL + 256UL) {
        printf("alloc_dma_buffer: bump overflowed alloc\r\n");
        farfree(raw_buffer);
        return 0;
    }
    if (aligned_phys_addr + DMA_BUFFER_SIZE > ISA_DMA_LIMIT) {
        printf("alloc_dma_buffer: phys 0x%lX above 16 MB ISA DMA limit\r\n",
               aligned_phys_addr);
        farfree(raw_buffer);
        return 0;
    }
    dma_buffer = (unsigned char far *)MK_FP((unsigned)(aligned_phys_addr >> 4),
                                            0);
    dma_phys_addr = aligned_phys_addr;
    _fmemset(dma_buffer, PCM_SILENCE, DMA_BUFFER_SIZE);
    return 1;
}

static void dma_start(void)
{
    unsigned offset = (unsigned)(dma_phys_addr & 0xFFFF);
    unsigned char page = (unsigned char)((dma_phys_addr >> 16) & 0xFF);
    disable();
    outp(DMA_MASK_REG, DMA_MASK_ON | sb_dma);
    outp(DMA_CLEAR_FF, 0);
    outp(DMA_MODE_REG, DMA_MODE_ADC | sb_dma);
    outp(DMA1_ADDR, offset & 0xFF);
    outp(DMA1_ADDR, (offset >> 8) & 0xFF);
    outp(DMA1_PAGE, page);
    outp(DMA1_COUNT, (DMA_BUFFER_SIZE - 1) & 0xFF);
    outp(DMA1_COUNT, ((DMA_BUFFER_SIZE - 1) >> 8) & 0xFF);
    outp(DMA_MASK_REG, sb_dma);   /* unmask */
    enable();
}

/* Read the 8237 current-count register for the SB channel and convert
 * it to a forward ring offset into dma_buffer.
 *
 * Returns:
 *   current DMA write position, 0..(DMA_BUFFER_SIZE - 1)
 */
static unsigned dma_position(void)
{
    unsigned current_count;
    disable();
    outp(DMA_CLEAR_FF, 0);
    current_count = inp(DMA1_COUNT);
    current_count |= (unsigned)inp(DMA1_COUNT) << 8;
    enable();
    return (DMA_BUFFER_SIZE - 1 - current_count) & (DMA_BUFFER_SIZE - 1);
}

/*---- SB lifecycle --------------------------------------------------------*/
/* Bring up the Sound Blaster: reset the DSP, read its version, set the
 * record input, allocate the DMA buffer, hook and unmask the IRQ, start
 * auto-init ADC capture, and measure the actual sample rate.
 *
 * Returns:
 *   1  card initialized; capture running
 *   0  card not found, or DMA buffer allocation failed
 */
static int sb_init(void)
{
    unsigned time_constant;
    int interrupt_vector;
    int discarded_status;

    /* Drain any pending DSP IRQ left over from a previous invocation,
     * then EOI the PIC so a stale interrupt can't fire into the
     * brand-new ISR.
     */
    discarded_status = inp(sb_base + SB_DSP_READ_STATUS);
    discarded_status = inp(sb_base + SB_DSP_ACK_16BIT);
    (void)discarded_status;
    if (sb_irq >= 8) outp(PIC2_CMD, PIC_EOI);
    outp(PIC1_CMD, PIC_EOI);

    if (!sb_reset()) {
        printf("Sound Blaster not found at I/O 0x%X.\r\n", sb_base);
        return 0;
    }

    {
        int v_major = 0, v_minor = 0;
        if (!dsp_write(SB_DSP_GET_VERSION)
                || (v_major = dsp_read()) < 0
                || (v_minor = dsp_read()) < 0) {
            printf("Sound Blaster DSP stopped responding.\r\n");
            return 0;
        }
        dsp_ver_major = (unsigned char)v_major;
        dsp_ver_minor = (unsigned char)v_minor;
    }
    printf("  DSP version %u.%u (%s mixer)\r\n",
           dsp_ver_major, dsp_ver_minor,
           dsp_ver_major >= 4 ? "CT1745" :
           dsp_ver_major == 3 ? "CT1335" : "none");

    mixer_set_input();

    if (!alloc_dma_buffer()) {
        printf("Cannot allocate DMA buffer.\r\n");
        return 0;
    }

    interrupt_vector = (sb_irq < 8) ? sb_irq + PIC1_VECTOR_BASE : sb_irq + PIC2_VECTOR_BASE - 8;
    old_irq_vec = getvect(interrupt_vector);
    setvect(interrupt_vector, sb_isr);
    irq_installed = 1;

    if (sb_irq < 8)
        outp(PIC1_DATA, inp(PIC1_DATA) & ~(1 << sb_irq));
    else {
        outp(PIC2_DATA, inp(PIC2_DATA) & ~(1 << (sb_irq - 8)));
        outp(PIC1_DATA, inp(PIC1_DATA) & ~(1 << 2));
    }

    dma_start();
    time_constant = (unsigned)(256UL - (1000000UL / NOMINAL_RATE));
    {
        int dsp_ok = dsp_write(SB_DSP_SET_RATE);
        dsp_ok = dsp_ok && dsp_write((unsigned char)time_constant);
        dsp_ok = dsp_ok && start_adc();
        if (!dsp_ok) {
            printf("Sound Blaster DSP stopped responding.\r\n");
            return 0;
        }
    }

    /* Measure actual ADC rate. */
    {
        unsigned position_start, position_end, samples_captured;
        delay(50);
        position_start = dma_position();
        delay(100);
        position_end = dma_position();
        samples_captured = (unsigned)((position_end - position_start)
                                          & (DMA_BUFFER_SIZE - 1));
        if (samples_captured >= 100U
                && samples_captured < (DMA_BUFFER_SIZE - 1U))
            SAMPLE_RATE = samples_captured * 10U;
    }
    return 1;
}

static void sb_shutdown(void)
{
    int interrupt_vector;
    if (sb_irq < 8)
        outp(PIC1_DATA, inp(PIC1_DATA) | (1 << sb_irq));
    else
        outp(PIC2_DATA, inp(PIC2_DATA) | (1 << (sb_irq - 8)));
    (void)dsp_write(SB_DSP_HALT_DMA);   /* polite halt; reset + mask below force it */
    outp(sb_base + SB_DSP_RESET, 1);
    for (interrupt_vector = 0; interrupt_vector < 4; interrupt_vector++)
        inp(IO_DELAY_PORT);
    outp(sb_base + SB_DSP_RESET, 0);
    outp(DMA_MASK_REG, DMA_MASK_ON | sb_dma);
    if (irq_installed) {
        interrupt_vector = (sb_irq < 8) ? sb_irq + PIC1_VECTOR_BASE : sb_irq + PIC2_VECTOR_BASE - 8;
        setvect(interrupt_vector, old_irq_vec);
        irq_installed = 0;
    }
}

/*---- TX helper -----------------------------------------------------------*/
/* Convert a target frequency to the emit_tone half-period count by
 * dividing bus_divisor by frequency_hz.
 *
 * Returns:
 *   the half-period count, clamped to the range 1..0xFFFF
 */
static unsigned hz_to_half_period(unsigned frequency_hz)
{
    unsigned long half_period = bus_divisor / (unsigned long)frequency_hz;
    if (half_period == 0)     half_period = 1;
    if (half_period > 0xFFFF) half_period = 0xFFFF;
    return (unsigned)half_period;
}

/*---- Goertzel filter bank ------------------------------------------------*/
#define N_BINS     31
#define CENTER_BIN 15

static long  bin_coeff[N_BINS];      /* Q10: 2*cos(2pi*f/SR), [-2..2] */
static unsigned bin_freq[N_BINS];

/* 4-term Taylor series cos(x) ~ 1 - x^2/2 + x^4/24 - x^6/720, all in Q14.
 *
 * Returns:
 *   cos(x) in Q14, range -16384..16384 (i.e. -1.0..1.0)
 */
static long cos_q14(long x_q14)
{
    long sign = 1;
    long x_squared, x_to_4th, x_to_6th;
    long TWO_PI_Q14 = 102944L;
    long PI_Q14     = TWO_PI_Q14 / 2;
    while (x_q14 < 0) x_q14 += TWO_PI_Q14;
    while (x_q14 >= TWO_PI_Q14) x_q14 -= TWO_PI_Q14;
    if (x_q14 > PI_Q14) { x_q14 = TWO_PI_Q14 - x_q14; }
    if (x_q14 > PI_Q14 / 2) { x_q14 = PI_Q14 - x_q14; sign = -1; }
    x_squared = (x_q14 * x_q14) >> 14;
    x_to_4th  = (x_squared * x_squared) >> 14;
    x_to_6th  = (x_to_4th * x_squared) >> 14;
    return sign * ((1L << 14) - (x_squared / 2) + (x_to_4th / 24)
                   - (x_to_6th / 720));
}

static void goertzel_setup_bins(unsigned target_frequency)
{
    int bin_index;
    for (bin_index = 0; bin_index < N_BINS; bin_index++) {
        long bin_frequency = (long)target_frequency / 2L +
                 ((long)target_frequency * 3L * (long)bin_index)
                     / (2L * (long)(N_BINS - 1));
        long angle_q14;
        long cosine_q14;
        if (bin_frequency < 50L) bin_frequency = 50L;
        if (bin_frequency > (long)(SAMPLE_RATE / 2 - 100))
            bin_frequency = (long)(SAMPLE_RATE / 2 - 100);
        bin_freq[bin_index] = (unsigned)bin_frequency;
        angle_q14 = 102944L * bin_frequency / (long)SAMPLE_RATE;
        cosine_q14 = cos_q14(angle_q14);
        bin_coeff[bin_index] = (2L * cosine_q14) >> 4;     /* Q10 */
    }
}

/* Goertzel magnitude over sample_count samples starting at ring offset
 * start_offset.
 * Uses the alpha-max-plus-beta-min approximation (max + 0.5*min) for
 * the final magnitude.
 *
 * Returns:
 *   the approximate bin magnitude (larger == more energy at this bin)
 */
static unsigned long goertzel_mag(unsigned start_offset, unsigned sample_count,
                                  long coeff_q10)
{
    long state_current = 0, state_prev = 0, state_prev2 = 0;
    unsigned sample_index;
    long real_part, imag_part, abs_real, abs_imag;
    for (sample_index = 0; sample_index < sample_count; sample_index++) {
        long sample = (long)dma_buffer[(start_offset + sample_index)
                                           & (DMA_BUFFER_SIZE - 1)] - 128L;
        state_current = sample + ((coeff_q10 * state_prev) >> 10) - state_prev2;
        state_prev2 = state_prev;
        state_prev = state_current;
    }
    real_part = state_prev - ((coeff_q10 * state_prev2) >> 11);
    imag_part = (state_prev2 * (coeff_q10 / 2)) >> 9;          /* approx */
    abs_real = real_part < 0 ? -real_part : real_part;
    abs_imag = imag_part < 0 ? -imag_part : imag_part;
    /* alpha-max-plus-beta-min: max + 0.5*min */
    return (unsigned long)((abs_real > abs_imag)
        ? abs_real + (abs_imag >> 1)
        : abs_imag + (abs_real >> 1));
}

/* Linear ratio (×10) -> dB.  6 dB / octave + linear interp.  ±1 dB.
 *
 * Returns:
 *   the SNR in dB (>= 0; 0 when the ratio is <= 1.0)
 */
static int snr_x10_to_db(long ratio_x10)
{
    int octave_count;
    if (ratio_x10 <= 10) return 0;
    octave_count = 0;
    while (ratio_x10 >= 20) { ratio_x10 >>= 1; octave_count++; }
    return 6 * octave_count + (int)((ratio_x10 - 10) * 6 / 10);
}

/*---- Two-phase tuner -----------------------------------------------------*/
#define PHASE_TUNE 0
#define PHASE_CAL  1

static void tune_loop(void)
{
    int running = 1;
    int phase = PHASE_TUNE;
    unsigned long measured_x10 = 0;
    long peak_diff = 0;
    int peak_bin = CENTER_BIN;
    long bin_diffs[N_BINS];
    unsigned long magnitude_emit[N_BINS];
    unsigned long magnitude_silent[N_BINS];
    long bin_diffs_ema[N_BINS];
    long tune_peak_raw = 0;
    long peak_db_hi = 0;
    int  peak_age = 0;
    long last_noise = 0;
    int  settling = 0;
    int  tune_intro = 0;
    int  cal_intro = 0;
    long snr_db_ema_x16 = 0;          /* TUNE: smoothed SNR for display */
    const unsigned BURST_MS = 400;
    const unsigned SILENT_MS = 400;
    const unsigned N_SAMPLES = 600;

    goertzel_setup_bins(target_hz);
    {
        int bin_index;
        for (bin_index = 0; bin_index < N_BINS; bin_index++)
            bin_diffs_ema[bin_index] = 0;
    }

    while (running) {
        unsigned half_period = hz_to_half_period(target_hz);
        unsigned long cycle_count;
        unsigned capture_offset;
        long frequency_error;
        int bin_index;

        /*---- Emit ----*/
        cycle_count = (unsigned long)target_hz * BURST_MS / 1000UL;
        if (cycle_count == 0) cycle_count = 1;
        {
            /* During the tone, leave ONLY the SB DMA-restart IRQ unmasked.  That
             * keeps the single-cycle capture alive (constant bus contention ->
             * steady pitch, no end-of-tone drift) while the timer (IRQ0) and
             * keyboard handlers stay masked so they can't splatter the bus.
             * emit_tone no longer touches interrupts itself.
             */
            unsigned char saved_master_mask = (unsigned char)inp(PIC1_DATA);
            unsigned char saved_slave_mask  = (unsigned char)inp(PIC2_DATA);
            if (sb_irq < 8) {
                outp(PIC1_DATA, 0xFF & ~(1 << sb_irq));   /* only SB on master */
                outp(PIC2_DATA, 0xFF);                    /* all slave off */
            } else {
                outp(PIC1_DATA, 0xFF & ~(1 << 2));            /* only cascade on master */
                outp(PIC2_DATA, 0xFF & ~(1 << (sb_irq - 8))); /* only SB on slave */
            }
            enable();
            while (cycle_count > 0) {
                unsigned chunk_cycles = (cycle_count > 0xFFFFUL)
                                            ? 0xFFFFU : (unsigned)cycle_count;
                emit_tone(half_period, chunk_cycles);
                cycle_count -= chunk_cycles;
            }
            disable();
            outp(PIC1_DATA, saved_master_mask);       /* restore the normal masks */
            outp(PIC2_DATA, saved_slave_mask);
            enable();
        }
        capture_offset = (dma_position() - N_SAMPLES) & (DMA_BUFFER_SIZE - 1);
        for (bin_index = 0; bin_index < N_BINS; bin_index++)
            magnitude_emit[bin_index] =
                goertzel_mag(capture_offset, N_SAMPLES, bin_coeff[bin_index]);

        /*---- Silent ----*/
        delay(SILENT_MS);
        capture_offset = (dma_position() - N_SAMPLES) & (DMA_BUFFER_SIZE - 1);
        for (bin_index = 0; bin_index < N_BINS; bin_index++)
            magnitude_silent[bin_index] =
                goertzel_mag(capture_offset, N_SAMPLES, bin_coeff[bin_index]);

        /*---- Diff: raw + EMA-smoothed ----*/
        {
            long peak_raw_diff = 0;
            peak_diff = 0;
            peak_bin = 0;
            for (bin_index = 0; bin_index < N_BINS; bin_index++) {
                long diff = (long)magnitude_emit[bin_index]
                            - (long)magnitude_silent[bin_index];
                bin_diffs_ema[bin_index] =
                    (bin_diffs_ema[bin_index] * 7L + diff) / 8L;
                bin_diffs[bin_index] = bin_diffs_ema[bin_index];
                if (bin_diffs_ema[bin_index] > peak_diff) {
                    peak_diff = bin_diffs_ema[bin_index];
                    peak_bin = bin_index;
                }
                if (diff > peak_raw_diff) peak_raw_diff = diff;
            }
            tune_peak_raw = peak_raw_diff;
        }

        /*---- Parabolic interpolation on the EMA peak (CAL only). ----*/
        {
            unsigned long interpolated_x10;
            if (peak_bin > 0 && peak_bin < N_BINS - 1 &&
                bin_diffs[peak_bin - 1] > 0 && bin_diffs[peak_bin + 1] > 0) {
                long lower_diff = bin_diffs[peak_bin - 1];
                long middle_diff = bin_diffs[peak_bin];
                long higher_diff = bin_diffs[peak_bin + 1];
                long denominator =
                    2L * (lower_diff - 2L * middle_diff + higher_diff);
                long bin_step  = (long)bin_freq[peak_bin + 1] -
                             (long)bin_freq[peak_bin];
                if (denominator != 0) {
                    long delta_q10 =
                        1024L * (lower_diff - higher_diff) / denominator;
                    if (delta_q10 < -512) delta_q10 = -512;
                    if (delta_q10 >  512) delta_q10 =  512;
                    interpolated_x10 = (unsigned long)
                        ((long)bin_freq[peak_bin] * 10L +
                         (delta_q10 * bin_step * 10L) / 1024L);
                } else {
                    interpolated_x10 = (unsigned long)bin_freq[peak_bin] * 10UL;
                }
            } else {
                interpolated_x10 = (unsigned long)bin_freq[peak_bin] * 10UL;
            }
            measured_x10 = interpolated_x10;
        }
        frequency_error = (long)(measured_x10 / 10) - (long)target_hz;

        if (phase == PHASE_CAL) {
            const char *verdict;
            long abs_frequency_error;

            if (!cal_intro) {
                cprintf("\r\nCAL: adjust 'bd' until the emitted tone "
                        "matches %u Hz.\r\n", target_hz);
                cprintf("Direction: BIGGER bd = LOWER tone, "
                        "SMALLER bd = HIGHER tone.\r\n");
                cprintf("Steps:     +/- = +-1000   ]/[ = +-100   "
                        "./, = +-10   a = auto-snap\r\n");
                cprintf("Other:     SPACE = back to TUNE   q = quit "
                        "(prints final bd)\r\n\r\n");
                cal_intro = 1;
            }

            abs_frequency_error =
                frequency_error < 0 ? -frequency_error : frequency_error;
            if      (abs_frequency_error == 0)  verdict = "ON TARGET";
            else if (frequency_error < 0) {
                if      (abs_frequency_error > 100) verdict = "raise: - ";
                else if (abs_frequency_error > 10)  verdict = "raise: [ ";
                else                                verdict = "raise: , ";
            } else {
                if      (abs_frequency_error > 100) verdict = "lower: + ";
                else if (abs_frequency_error > 10)  verdict = "lower: ] ";
                else                                verdict = "lower: . ";
            }

            cprintf("\rtone %3lu.%lu Hz  tgt %u Hz  off %+4ld Hz  "
                    "bd %6lu  %s",
                    measured_x10 / 10, measured_x10 % 10,
                    target_hz, frequency_error, bus_divisor, verdict);
        } else {
            const int BAR = 30;
            int current_position, peak_position, column_index;
            long noise_avg;
            long snr_x10;
            int snr_db;
            const char *status;
            int dial_changed;

            if (!tune_intro) {
                cprintf("\r\nTUNE: step the AM dial across 10 kHz steps.\r\n");
                cprintf("SNR (dB) is how much the tone stands out above\r\n");
                cprintf("the radio's background noise.\r\n");
                cprintf("Rough guide:  <6 dB no tone   6-12 weak   12-20 ok\r\n");
                cprintf("              20-30 strong   30+ EXCELLENT\r\n");
                cprintf("Wait 2-3 seconds per step for it to settle.\r\n");
                cprintf("Press SPACE when you find the strongest spot to\r\n");
                cprintf("switch to bd calibration.\r\n\r\n");
                tune_intro = 1;
            }

            {
                unsigned long magnitude_sum = 0;
                int bin_index;
                for (bin_index = 0; bin_index < N_BINS; bin_index++)
                    magnitude_sum += magnitude_silent[bin_index];
                noise_avg = (long)(magnitude_sum / N_BINS);
            }
            if (noise_avg < 100) noise_avg = 100;

            dial_changed = 0;
            if (last_noise > 0) {
                long noise_delta = noise_avg - last_noise;
                if (noise_delta < 0) noise_delta = -noise_delta;
                if (noise_delta * 2 > last_noise) dial_changed = 1;
            }
            last_noise = noise_avg;
            if (dial_changed) settling = 2;
            else if (settling > 0) settling--;

            snr_x10 = (tune_peak_raw * 10L) / noise_avg;
            if (snr_x10 < 0) snr_x10 = 0;
            snr_db = snr_x10_to_db(snr_x10);
            if (snr_db > 60) snr_db = 60;

            /* Light EMA on the dB value to stop cycle-to-cycle ADC
             * noise from shaking the display.  alpha = 1/4: a step
             * change in real SNR settles to within 1 dB inside ~4
             * cycles (~3 s with 800 ms cycles).  Reset on dial
             * change so a new station snaps quickly.
             */
            if (settling > 0 || snr_db_ema_x16 == 0) {
                snr_db_ema_x16 = (long)snr_db * 16L;
            } else {
                snr_db_ema_x16 = (snr_db_ema_x16 * 3L + (long)snr_db * 16L) / 4L;
            }
            snr_db = (int)((snr_db_ema_x16 + 8L) / 16L);

            if (settling == 0) {
                if (snr_db > peak_db_hi) {
                    peak_db_hi = snr_db;
                    peak_age = 0;
                } else if (++peak_age > 10) {
                    if (peak_db_hi > 0) peak_db_hi--;
                    peak_age = 0;
                }
            }

            if      (snr_db <  6) status = "no tone  ";
            else if (snr_db < 12) status = "weak     ";
            else if (snr_db < 20) status = "ok       ";
            else if (snr_db < 30) status = "strong   ";
            else                  status = "EXCELLENT";

            current_position = snr_db;
            peak_position = (int)peak_db_hi;
            if (current_position > BAR - 1) current_position = BAR - 1;
            if (peak_position    > BAR - 1) peak_position    = BAR - 1;
            if (current_position < 0) current_position = 0;
            if (peak_position    < 0) peak_position    = 0;

            if (settling > 0) {
                cprintf("\rmeasuring...   best=%2ld dB  [", peak_db_hi);
                for (column_index = 0; column_index < BAR; column_index++)
                    putch(column_index == peak_position ? '|' : '.');
                cprintf("]              ");
            } else {
                cprintf("\rSNR now=%2d dB  best=%2ld dB  [",
                        snr_db, peak_db_hi);
                for (column_index = 0; column_index < BAR; column_index++) {
                    char bar_char;
                    if (column_index == peak_position)      bar_char = '|';
                    else if (column_index <= current_position) bar_char = '#';
                    else                                    bar_char = '.';
                    putch(bar_char);
                }
                cprintf("] %s ", status);
            }
        }

        while (kbhit()) {
            int key = getch();
            int reset_ema = 0;
            switch (key) {
            case ' ':
                phase = (phase == PHASE_TUNE) ? PHASE_CAL : PHASE_TUNE;
                peak_db_hi = 0; peak_age = 0;
                last_noise = 0; settling = 0;
                tune_intro = 0; cal_intro = 0;
                reset_ema = 1;
                cprintf("\r\n");
                break;
            case '+': case '=':
                if (phase == PHASE_CAL) { bus_divisor += 1000UL; reset_ema = 1; }
                break;
            case '-': case '_':
                if (phase == PHASE_CAL) {
                    bus_divisor = (bus_divisor > 1000UL)
                                      ? bus_divisor - 1000UL : 1UL;
                    reset_ema = 1;
                }
                break;
            case ']':
                if (phase == PHASE_CAL) { bus_divisor += 100UL; reset_ema = 1; }
                break;
            case '[':
                if (phase == PHASE_CAL) {
                    bus_divisor = (bus_divisor > 100UL)
                                      ? bus_divisor - 100UL : 1UL;
                    reset_ema = 1;
                }
                break;
            case '.': case '>':
                if (phase == PHASE_CAL) { bus_divisor += 10UL; reset_ema = 1; }
                break;
            case ',': case '<':
                if (phase == PHASE_CAL) {
                    bus_divisor = (bus_divisor > 10UL)
                                      ? bus_divisor - 10UL : 1UL;
                    reset_ema = 1;
                }
                break;
            case 'a': case 'A':
                if (phase == PHASE_CAL && measured_x10 > 0) {
                    unsigned long clamped_divisor = bus_divisor;
                    unsigned long measured_freq_x10 = measured_x10;
                    if (clamped_divisor > 1000000UL)
                        clamped_divisor = 1000000UL;
                    bus_divisor = clamped_divisor * measured_freq_x10
                                  / (10UL * (unsigned long)target_hz);
                    if (bus_divisor == 0) bus_divisor = 1;
                    reset_ema = 1;
                }
                break;
            case 'q': case 'Q': case 27: running = 0; break;
            default: break;
            }
            if (reset_ema) {
                int bin_index;
                for (bin_index = 0; bin_index < N_BINS; bin_index++)
                    bin_diffs_ema[bin_index] = 0;
            }
        }
    }
    putch('\r'); putch('\n');
}

/*---- Usage and main ------------------------------------------------------*/
static void usage(void)
{
    puts("BDTUNE - bus divisor tuner for BUSRADIO\r");
    puts("\r");
    puts("  BDTUNE [-t HZ] [-d BUSDIV] [-i SRC]\r");
    puts("  BDTUNE -h\r");
    puts("\r");
    puts("    -t HZ      target tone frequency, default 400\r");
    puts("    -d BUSDIV  starting bus divisor, default 55000 (8 MHz 286)\r");
    puts("    -i SRC     SB recording input: mic, line (default), or cd\r");
    puts("    -h         show this help\r");
    puts("\r");
    puts("Reads SB I/O from the BLASTER environment variable (A/I/D fields).\r");
    puts("If unset, defaults to A220 I5 D1.\r");
    puts("\r");
    puts("Hook an AM radio's audio output to the SB input you specify with\r");
    puts("-i (default Line In).  BDTUNE emits the target tone via BUSRADIO\r");
    puts("RF and walks you through finding the strongest AM dial step (TUNE\r");
    puts("phase) and then nudging bus divisor until the recovered audio\r");
    puts("matches the target (CAL phase).  The final bus divisor is what\r");
    puts("you pass to BUSRADIO and BRPLAY.\r");
}

/* Parse arguments, bring up the Sound Blaster, run the tune/cal loop,
 * shut down, and print the final bus_divisor.
 *
 * Returns:
 *   0  success (normal exit, or -h help requested)
 *   1  bad command-line argument (bad -t, bad -i, or unknown option)
 *   2  Sound Blaster initialization failed
 */
int main(int argc, char **argv)
{
    int argument_index;

    setvbuf(stdout, NULL, _IONBF, 0);
    parse_blaster_env();

    for (argument_index = 1; argument_index < argc; argument_index++) {
        char *argument = argv[argument_index];
        if (strcmp(argument, "-t") == 0 && argument_index + 1 < argc) {
            target_hz = (unsigned)atoi(argv[++argument_index]);
            if (target_hz < 50 || target_hz > 4000) {
                printf("Bad -t value: %u (use 50..4000)\r\n", target_hz);
                return 1;
            }
        } else if (strcmp(argument, "-d") == 0 && argument_index + 1 < argc) {
            bus_divisor = strtoul(argv[++argument_index], NULL, 10);
            if (bus_divisor == 0) bus_divisor = 1;
        } else if (strcmp(argument, "-i") == 0 && argument_index + 1 < argc) {
            char *source_name = argv[++argument_index];
            if      (strcmp(source_name, "mic")  == 0)
                input_source = INPUT_MIC;
            else if (strcmp(source_name, "line") == 0)
                input_source = INPUT_LINE;
            else if (strcmp(source_name, "cd")   == 0)
                input_source = INPUT_CD;
            else {
                printf("Bad -i: %s (use mic, line, or cd)\r\n", source_name);
                return 1;
            }
        } else if (strcmp(argument, "-h") == 0) {
            usage(); return 0;
        } else {
            usage(); return 1;
        }
    }

    printf("BDTUNE: target %u Hz, starting bus divisor %lu\r\n", target_hz, bus_divisor);
    printf("Sound Blaster at I/O 0x%X IRQ %d DMA %d (from %s).\r\n",
           sb_base, sb_irq, sb_dma,
           blaster_seen ? "BLASTER env" : "defaults");
    if (!sb_init()) return 2;
    printf("ADC sampling at %u Hz.\r\n\r\n", SAMPLE_RATE);

    tune_loop();
    sb_shutdown();

    printf("\r\nFinal bus divisor: %lu\r\n", bus_divisor);
    printf("Pass this to BUSRADIO with:  BUSRADIO -d %lu songfile.txt\r\n",
           bus_divisor);
    return 0;
}
