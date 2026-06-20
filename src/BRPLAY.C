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

/* BRPLAY.C - WAV playback via BUSRADIO RF emission
 *
 * Target: IBM PC AT (80286, 6 or 8 MHz), MS-DOS 3.x+
 * Compiler: Borland Turbo C++ 3.0, small memory model
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>
#include <io.h>
#include <fcntl.h>
#include <alloc.h>

#define DEFAULT_BUS_DIVISOR       55000UL    /* matches BUSRADIO/BDTUNE */
#define WAV_HEADER_BYTES          44

extern void emit_pwm(unsigned sample, unsigned units);
extern void play_pcm_chunk(unsigned char far *buffer,
                           unsigned count, unsigned units);

static unsigned char far *audio_buffer = NULL;   /* raw 8-bit PCM samples */
static unsigned char far *burst_buffer = NULL;   /* DSM'd burst counts */
static unsigned long audio_length = 0;
static unsigned audio_rate = 0;

/* Read a WAV header and validate format.  Sets *sample_rate_out and
 * *sample_count_out.
 *
 * Returns:
 *   >0  byte offset of the PCM data (and *sample_rate_out,
 *       *sample_count_out set)
 *    0  not a supported mono 8-bit PCM WAV, or 'data' chunk not found
 */
static unsigned long parse_wav(int file_descriptor, unsigned *sample_rate_out,
                                unsigned long *sample_count_out)
{
    unsigned char header[44];
    unsigned long file_position;
    if (read(file_descriptor, header, 44) != 44) {
        printf("Short read on WAV header.\r\n");
        return 0UL;
    }
    if (memcmp(header,    "RIFF", 4) != 0 ||
        memcmp(header+8,  "WAVE", 4) != 0 ||
        memcmp(header+12, "fmt ", 4) != 0) {
        printf("Not a WAV file (bad magic).\r\n");
        return 0UL;
    }
    /* fmt chunk: bytes 20..21 = format (1=PCM), 22..23 = channels,
     * 24..27 = sample rate, 34..35 = bits/sample.
     */
    if (header[20] != 1 || header[21] != 0) {
        printf("Not PCM (format=%u).\r\n", header[20] | (header[21] << 8));
        return 0UL;
    }
    if (header[22] != 1) {
        printf("Not mono (channels=%u).\r\n", header[22]);
        return 0UL;
    }
    if (header[34] != 8) {
        printf("Not 8-bit (bits=%u).\r\n", header[34]);
        return 0UL;
    }
    *sample_rate_out = (unsigned)header[24] | ((unsigned)header[25] << 8);
    /* data chunk: search past fmt for "data" header. */
    /* Simple WAVs put it at offset 36; some have extra chunks. */
    if (memcmp(header+36, "data", 4) == 0) {
        *sample_count_out = (unsigned long)header[40]
               | ((unsigned long)header[41] << 8)
               | ((unsigned long)header[42] << 16)
               | ((unsigned long)header[43] << 24);
        return 44UL;
    }
    /* Otherwise scan forward looking for "data". */
    file_position = 36UL;
    lseek(file_descriptor, file_position, SEEK_SET);
    {
        unsigned char chunk_header[8];
        int scan_iteration;
        for (scan_iteration = 0; scan_iteration < 100; scan_iteration++) {
            if (read(file_descriptor, chunk_header, 8) != 8) break;
            if (memcmp(chunk_header, "data", 4) == 0) {
                *sample_count_out = (unsigned long)chunk_header[4]
                       | ((unsigned long)chunk_header[5] << 8)
                       | ((unsigned long)chunk_header[6] << 16)
                       | ((unsigned long)chunk_header[7] << 24);
                return file_position + 8UL;
            }
            /* Skip this chunk: bytes 4..7 = chunk size. */
            {
                unsigned long chunk_size = (unsigned long)chunk_header[4]
                                 | ((unsigned long)chunk_header[5] << 8)
                                 | ((unsigned long)chunk_header[6] << 16)
                                 | ((unsigned long)chunk_header[7] << 24);
                file_position += 8 + chunk_size;
                lseek(file_descriptor, file_position, SEEK_SET);
            }
        }
    }
    printf("Could not find 'data' chunk.\r\n");
    return 0UL;
}

/* Open PATH, parse its WAV header, and load the PCM samples into the
 * global far audio_buffer (also sets audio_rate and audio_length).
 *
 * Returns:
 *   1  WAV loaded successfully
 *   0  open, parse, size-check, allocation, or read failed
 */
static int load_wav(const char *path)
{
    int file_descriptor;
    unsigned long data_offset;
    unsigned long total_bytes_read;
    static unsigned char scratch_buffer[1024];
    unsigned long write_position;

    file_descriptor = open(path, O_RDONLY | O_BINARY);
    if (file_descriptor < 0) { printf("Cannot open %s\r\n", path); return 0; }

    data_offset = parse_wav(file_descriptor, &audio_rate, &audio_length);
    if (data_offset == 0UL) { close(file_descriptor); return 0; }
    if (audio_length == 0UL || audio_length > 600000UL) {
        printf("Audio length %lu unreasonable (max 600 KB).\r\n",
               audio_length);
        close(file_descriptor);
        return 0;
    }
    printf("WAV: %u Hz mono 8-bit, %lu samples (%lu.%lus).\r\n",
           audio_rate, audio_length,
           audio_length / audio_rate,
           ((audio_length * 10UL / audio_rate) % 10UL));

    audio_buffer = (unsigned char far *)farmalloc(audio_length + 16UL);
    if (!audio_buffer) {
        printf("Cannot allocate %lu-byte audio buffer.\r\n", audio_length);
        close(file_descriptor);
        return 0;
    }

    /* Stream into the far buffer through a near scratch.  Borland's
     * read() takes a near buffer in small model.
     */
    lseek(file_descriptor, data_offset, SEEK_SET);
    total_bytes_read = 0;
    write_position = 0;
    while (total_bytes_read < audio_length) {
        unsigned bytes_to_read =
            (audio_length - total_bytes_read > sizeof scratch_buffer)
                ? (unsigned)sizeof scratch_buffer
                : (unsigned)(audio_length - total_bytes_read);
        int bytes_this_read =
            read(file_descriptor, scratch_buffer, bytes_to_read);
        unsigned scratch_index;
        if (bytes_this_read <= 0) break;
        for (scratch_index = 0; scratch_index < (unsigned)bytes_this_read;
             scratch_index++)
            audio_buffer[write_position + scratch_index] =
                scratch_buffer[scratch_index];
        write_position += bytes_this_read;
        total_bytes_read += bytes_this_read;
    }
    close(file_descriptor);
    if (total_bytes_read != audio_length) {
        printf("Short read: got %lu of %lu.\r\n", total_bytes_read,
               audio_length);
        return 0;
    }
    return 1;
}

/* Precompute (burst, quiet) word pairs per audio sample using
 * 1st-order DSM with Galois-LFSR dither.  Each sample becomes 2
 * bytes in burst_buffer: low = burst count, high = quiet count
 * (= units - burst).  Pre-packing the pair lets the realtime ASM
 * loop drop the per-sample subtract.  units must stay below 256 so
 * each count fits in one byte.  Running this once at load time
 * keeps the per-sample work in the realtime PWM loop minimal.
 *
 * Returns:
 *   1  burst-pair buffer allocated and filled
 *   0  could not allocate the burst-pair buffer
 */
static int precompute_bursts(unsigned units)
{
    unsigned long sample_index;
    unsigned accumulator = 0;   /* 1st-order DSM residual (low byte) */
    unsigned lfsr = 1;          /* Galois 16-bit LFSR, poly 0xB400 */
    unsigned long buffer_byte_count = audio_length * 2UL;

    burst_buffer = (unsigned char far *)farmalloc(buffer_byte_count + 16UL);
    if (!burst_buffer) {
        printf("Cannot allocate %lu-byte burst-pair buffer.\r\n",
               buffer_byte_count);
        return 0;
    }

    for (sample_index = 0; sample_index < audio_length; sample_index++) {
        unsigned scaled_sample = (unsigned)audio_buffer[sample_index] * units;
        unsigned dither;
        unsigned burst_count;

        if (lfsr & 1U) {
            lfsr = (lfsr >> 1) ^ 0xB400U;
        } else {
            lfsr = lfsr >> 1;
        }
        dither = lfsr & 0x000FU;

        scaled_sample = scaled_sample + dither + accumulator;
        burst_count = scaled_sample >> 8;
        if (burst_count > units) burst_count = units;   /* safety clamp */
        /* Normalized far pointer so the write doesn't wrap the 16-bit
         * offset for WAVs > 32767 samples (i*2 >= 65536) -- the same
         * fix as the read side in play_once.
         */
        {
            unsigned long byte_offset = sample_index * 2UL;
            unsigned char far *burst_pair_pointer = (unsigned char far *)MK_FP(
                FP_SEG(burst_buffer) + (unsigned)(byte_offset >> 4),
                FP_OFF(burst_buffer) + (unsigned)(byte_offset & 0x000FUL));
            burst_pair_pointer[0] = (unsigned char)burst_count;
            burst_pair_pointer[1] = (unsigned char)(units - burst_count);
        }
        accumulator = scaled_sample & 0xFFU;
    }
    return 1;
}

static void play_once(unsigned units, int *abort_flag)
{
    /* Process the audio in fixed chunks so kbhit can be polled
     * between chunks without touching CPU activity inside the
     * tight ASM PWM loop.  256 samples is ~85 ms at 3 kHz —
     * imperceptibly fast keyboard response.  Each sample is 2
     * bytes in burst_buffer, so the offset advances by 2 per sample.
     */
    const unsigned CHUNK = 256;
    unsigned long sample_position = 0;
    while (sample_position < audio_length) {
        unsigned long remaining_samples = audio_length - sample_position;
        unsigned chunk_samples = (remaining_samples > (unsigned long)CHUNK)
                                     ? CHUNK
                                     : (unsigned)remaining_samples;
        /* Normalize the far pointer: put the high bits of the byte offset
         * into the segment so the 16-bit offset never overflows for WAVs
         * > 32767 samples (where pos*2 >= 65536).  The in-chunk offset then
         * stays tiny, so play_pcm_chunk's SI can't wrap mid-chunk either.
         */
        {
            unsigned long byte_offset = sample_position * 2UL;
            unsigned char far *chunk_pointer = (unsigned char far *)MK_FP(
                FP_SEG(burst_buffer) + (unsigned)(byte_offset >> 4),
                FP_OFF(burst_buffer) + (unsigned)(byte_offset & 0x000FUL));
            play_pcm_chunk(chunk_pointer, chunk_samples, units);
        }
        sample_position += chunk_samples;
        if (kbhit()) {
            (void)getch();
            *abort_flag = 1;
            return;
        }
    }
}

static void usage(void)
{
    puts("BRPLAY - WAV playback via BUSRADIO RF emission\r");
    puts("\r");
    puts("  BRPLAY [-r] [-b] [-d BUSDIV] [-u UNITS] <wavfile>\r");
    puts("  BRPLAY -h\r");
    puts("\r");
    puts("    -r          repeat until a key is pressed\r");
    puts("    -b          blank CGA display while playing (its video\r");
    puts("                fetch otherwise chops the RF carrier)\r");
    puts("    -d BUSDIV   bus divisor from BDTUNE (default 55000)\r");
    puts("    -u UNITS    explicit units_per_sample (overrides -d)\r");
    puts("    -h          show this help\r");
    puts("\r");
    puts("WAV must be mono, 8-bit unsigned PCM.  The WAV's own sample\r");
    puts("rate is read from its header; -d BUSDIV calibrates the\r");
    puts("playback timing to match.  Use the bus divisor BDTUNE finds\r");
    puts("for your machine -- the same value drives BUSRADIO -- to\r");
    puts("reproduce a WAV at its true sample rate.\r");
}

/* CGA display blanking (-b): the 6845 steals bus cycles to fetch video
 * RAM at the 15.7 kHz horizontal rate, chopping the emission burst and
 * smearing the RF carrier across a ~70 kHz cluster.  The output-enable
 * bit (3D8h.3) only blanks the signal -- the CRTC keeps fetching.  To
 * actually halt the RAM fetch, set the 6845's "horizontal displayed"
 * register (R1) to 0: zero characters per line -> no video RAM cycles ->
 * the bus is the CPU's alone.  Restored from the BIOS column count at
 * 0040:004A.  CGA-specific, so it is opt-in.
 */
static unsigned char cga_mode = 3;
static int cga_blanked = 0;
static void blank_cga(void)
{
    cga_mode = *(unsigned char far *)MK_FP(0x40, 0x49);  /* current video mode */
    outportb(0x3D4, 1);          /* CRTC index = R1 (horizontal displayed) */
    outportb(0x3D5, 0);          /* 0 displayed chars -> no video RAM fetch */
    cga_blanked = 1;
}
static void unblank_cga(void)
{
    union REGS registers;
    if (!cga_blanked) return;
    /* Re-set the saved mode via BIOS: reprograms all CRTC registers
     * cleanly (just restoring R1 leaves some 6845s wedged).
     */
    registers.h.ah = 0x00;
    registers.h.al = cga_mode;
    int86(0x10, &registers, &registers);
}

/* Parse the command line, load the WAV, precompute bursts, and play.
 *
 * Returns:
 *   0  success (also the -h path)
 *   1  bad arguments (invalid -u, missing wavfile, or usage error)
 *   2  load_wav failed
 *   3  precompute_bursts failed (out of memory)
 */
int main(int argc, char **argv)
{
    int arg_index;
    int repeat = 0;
    char *wav_path = NULL;
    unsigned long bus_divisor = DEFAULT_BUS_DIVISOR;
    unsigned units = 0;            /* 0 = derive from bus_divisor */
    int abort_flag = 0;
    int blank = 0;                 /* -b: blank CGA display while playing */

    setvbuf(stdout, NULL, _IONBF, 0);

    for (arg_index = 1; arg_index < argc; arg_index++) {
        char *arg = argv[arg_index];
        if (strcmp(arg, "-r") == 0) {
            repeat = 1;
        } else if (strcmp(arg, "-b") == 0) {
            blank = 1;
        } else if (strcmp(arg, "-d") == 0 && arg_index + 1 < argc) {
            bus_divisor = strtoul(argv[++arg_index], NULL, 10);
            if (bus_divisor < 1000UL) bus_divisor = 1000UL;
        } else if (strcmp(arg, "-u") == 0 && arg_index + 1 < argc) {
            units = (unsigned)atoi(argv[++arg_index]);
            if (units < 30U || units > 60000U) {
                printf("Invalid -u: %u (use 30..60000)\r\n", units);
                return 1;
            }
        } else if (strcmp(arg, "-h") == 0) {
            usage(); return 0;
        } else if (wav_path == NULL) {
            wav_path = arg;
        } else {
            usage(); return 1;
        }
    }
    if (!wav_path) { usage(); return 1; }

    if (!load_wav(wav_path)) return 2;

    /* Derive units_per_sample from bus_divisor + WAV sample rate.  BDTUNE
     * defines bus_divisor as half the inner-loop iteration rate
     * (half_period = bd/freq), and BRPLAY now runs emit_tone's EXACT
     * iteration, so one sample = units iterations plays at
     * sample_rate = iter_rate/units = 2*bd/units.  For sample_rate = sr:
     * units = 2*bd/sr.  The single bus_divisor that BDTUNE finds therefore
     * tunes BUSRADIO notes, BDTUNE tones, and a BRPLAY WAV at its true rate
     * — all at the same carrier dial frequency.
     * (Was an empirical bd*23/(15*sr); the fudge factor existed only because
     * the two loops used to differ in timing.  They no longer do.)
     */
    if (units == 0) {
        units = (unsigned)(2UL * bus_divisor / (unsigned long)audio_rate);
        if (units < 10U)  units = 10U;
        if (units > 255U) units = 255U;     /* burst count is one byte */
        printf("Bus divisor %lu, WAV %u Hz -> %u units/sample "
               "(%u amplitude levels)\r\n",
               bus_divisor, audio_rate, units, units);
    } else {
        if (units > 255U) {
            printf("-u %u exceeds 255 (burst count is one byte); "
                   "clamping.\r\n", units);
            units = 255U;
        }
        printf("%u units/sample (bus divisor ignored)\r\n", units);
    }

    printf("Precomputing burst counts...");
    if (!precompute_bursts(units)) {
        farfree(audio_buffer);
        return 3;
    }
    printf(" done.\r\n");

    printf("Playing.  Press any key to %s.\r\n",
           repeat ? "stop" : "abort");

    if (blank) blank_cga();
    do {
        play_once(units, &abort_flag);
    } while (repeat && !abort_flag);
    if (blank) unblank_cga();

    farfree(burst_buffer);
    farfree(audio_buffer);
    return 0;
}
