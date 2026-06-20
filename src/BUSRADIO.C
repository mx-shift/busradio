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

/* BUSRADIO.C - play a song over an AM radio via memory bus RF emission
 *
 * Target: IBM PC AT (80286, 6 or 8 MHz), MS-DOS 3.x+
 * Compiler: Borland Turbo C++ 3.0, small memory model
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>          /* isspace() for line validation */
#include <bios.h>
#include <dos.h>            /* disable()/enable() around the emission */

#define DEFAULT_BUS_DIVISOR     55000UL    /* tuned for 8 MHz 80286 */
#define MAX_NOTES               2048
#define LINE_LENGTH             256
#define FILENAME_LENGTH         80

extern void emit_tone(unsigned half_period, unsigned cycle_count);
extern void emit_silence(unsigned iterations);

static unsigned note_table[MAX_NOTES][2];   /* [0]=half_period (0=rest), [1]=cycles/ms */
static unsigned note_count = 0;

static unsigned long bus_divisor = DEFAULT_BUS_DIVISOR;
static int repeat_flag = 0;
static char song_filename[FILENAME_LENGTH];

static void usage(void)
{
    puts("BUSRADIO - play a song over an AM radio via memory bus RF emission\r");
    puts("\r");
    puts("  BUSRADIO [-r] [-d BUSDIV] <songfile>\r");
    puts("  BUSRADIO -h\r");
    puts("\r");
    puts("    -r          repeat the song until a key is pressed\r");
    puts("    -d BUSDIV   bus divisor for this machine (default 55000)\r");
    puts("    -h          show this help\r");
    puts("\r");
    puts("Song file: one `freq_hz duration_ms' per line. freq 0 is a rest.\r");
    puts("Text from ; to end of line is a comment.  The bus divisor sets\r");
    puts("note pitch and rest length.\r");
}

/* Read the song one line at a time and tokenize into note_table.  Strip any
 * trailing comment by NUL-ing at the ';' -- this keeps the note on lines like
 * `0 125 ; rest'.  Blank, whitespace-only, and comment-only lines are
 * skipped; any other line that isn't a `freq duration_ms' pair -- or whose
 * freq or duration exceeds 65535 -- is a hard error naming the line.
 *
 * Returns:
 *    0  success
 *   -1  file won't open
 *   -2  too many notes
 *   -3  malformed or out-of-range line (message already printed)
 */
static int load_song(const char *path)
{
    FILE *song_file;
    char line[LINE_LENGTH];
    char *comment_start, *cursor;
    char extra_field[2];
    unsigned line_number = 0;
    unsigned long frequency, duration, half_period, cycle_count;

    song_file = fopen(path, "r");   /* text mode: \r\n -> \n on DOS */
    if (song_file == NULL) return -1;

    note_count = 0;
    while (fgets(line, sizeof(line), song_file) != NULL) {
        line_number++;
        line[strcspn(line, "\r\n")] = '\0';     /* drop the line terminator */
        comment_start = strchr(line, ';');
        if (comment_start != NULL) *comment_start = '\0';   /* strip comment */

        /* Skip blank, whitespace-only, and comment-only lines. */
        for (cursor = line;
             *cursor != '\0' && isspace((unsigned char)*cursor);
             cursor++)
            ;
        if (*cursor == '\0')
            continue;

        /* Any remaining content must be exactly `freq duration'. */
        if (sscanf(line, "%lu %lu %1s",
                   &frequency, &duration, extra_field) != 2) {
            printf("Song file error on line %u: %s\r\n", line_number, cursor);
            fclose(song_file);
            return -3;
        }

        if (frequency > 0xFFFFUL || duration > 0xFFFFUL) {  /* 16-bit fields */
            printf("Song file error on line %u: value exceeds 65535: %s\r\n",
                   line_number, cursor);
            fclose(song_file);
            return -3;
        }

        if (note_count >= MAX_NOTES) { fclose(song_file); return -2; }

        if (frequency == 0) {
            note_table[note_count][0] = 0;
            note_table[note_count][1] = (unsigned)duration;
        } else {
            half_period = bus_divisor / frequency;
            if (half_period == 0) half_period = 1;               /* floor */
            if (half_period > 0xFFFFUL) half_period = 0xFFFFUL;  /* cap */
            cycle_count = frequency * duration / 1000UL;
            if (cycle_count == 0) cycle_count = 1;
            if (cycle_count > 0xFFFFUL) cycle_count = 0xFFFFUL;
            note_table[note_count][0] = (unsigned)half_period;
            note_table[note_count][1] = (unsigned)cycle_count;
        }
        note_count++;
    }
    fclose(song_file);
    return 0;
}

/* Play all notes once.
 *
 * Returns:
 *   0  played to the end
 *   1  a key was pressed between notes
 */
static int play_once(void)
{
    unsigned note_index;
    for (note_index = 0; note_index < note_count; note_index++) {
        if (bioskey(1)) {
            (void)bioskey(0);   /* drain one keystroke */
            return 1;
        }
        if (note_table[note_index][0] == 0) {
            /* Rest: timed by the SAME bus_divisor-calibrated loop as notes,
             * carrier off (silence).  iters = 2*bd*ms/1000 -- frequency
             * cancels out of a note's per-ms iteration count, so one
             * calibration times notes and rests alike.  Chunk past 65535.
             */
            unsigned long rest_iterations =
                2UL * bus_divisor *
                (unsigned long)note_table[note_index][1] / 1000UL;
            disable();
            while (rest_iterations > 0UL) {
                unsigned chunk_iterations = (rest_iterations > 0xFFFFUL)
                    ? 0xFFFFU : (unsigned)rest_iterations;
                emit_silence(chunk_iterations);
                rest_iterations -= chunk_iterations;
            }
            enable();
        } else {
            disable();      /* interrupts off for the note -> clean carrier */
            emit_tone(note_table[note_index][0], note_table[note_index][1]);
            enable();       /* back on between notes (keyboard poll, timer) */
        }
    }
    return 0;
}

/* Parse arguments, load the song file, and play it (optionally repeating).
 *
 * Returns:
 *   0  done, or help shown (-h)
 *   1  usage error (bad/missing argument, -d value, or filename)
 *   2  song file won't open
 *   4  malformed song line, or too many notes
 */
int main(int argc, char **argv)
{
    int argument_index;
    int have_filename = 0;
    int result;
    unsigned long parsed_divisor;
    char *parse_end;

    for (argument_index = 1; argument_index < argc; argument_index++) {
        char *argument = argv[argument_index];
        if (strcmp(argument, "-r") == 0) {
            repeat_flag = 1;
        } else if (strcmp(argument, "-d") == 0 && argument_index + 1 < argc) {
            parsed_divisor = strtoul(argv[++argument_index], &parse_end, 10);
            if (*parse_end != '\0' || parsed_divisor == 0) {
                printf("Invalid -d value.\r\n");
                return 1;
            }
            bus_divisor = parsed_divisor;
        } else if (strcmp(argument, "-h") == 0) {
            usage();
            return 0;
        } else if (argument[0] != '-' && !have_filename) {
            if (strlen(argument) >= FILENAME_LENGTH) {
                printf("Song file name too long.\r\n");
                return 1;
            }
            strcpy(song_filename, argument);
            have_filename = 1;
        } else {
            usage();
            return 1;
        }
    }
    if (!have_filename) {
        usage();
        return 1;
    }

    result = load_song(song_filename);
    switch (result) {
    case -1:
        printf("Cannot open song file.\r\n");
        return 2;
    case -2:
        printf("Too many notes (>%d).\r\n", MAX_NOTES);
        return 4;
    case -3:
        return 4;   /* load_song printed the offending line */
    }
    /* result 0: loaded OK, fall through to play */

    printf("Bus divisor: %lu\r\n", bus_divisor);
    printf("Playing. Tune AM radio 540-1600 kHz. Any key to stop.\r\n");

    do {
        if (play_once() != 0) break;
    } while (repeat_flag);

    return 0;
}
