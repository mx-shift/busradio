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
#include <string.h>
#include <ctype.h>          /* isspace() for line validation */
#include <bios.h>

#define MAX_NOTES               2048
#define LINE_LENGTH             256
#define FILENAME_LENGTH         80

static unsigned note_table[MAX_NOTES][2];   /* [0]=freq_hz (0=rest), [1]=duration_ms */
static unsigned note_count = 0;

static int repeat_flag = 0;
static char song_filename[FILENAME_LENGTH];

static void usage(void)
{
    puts("BUSRADIO - play a song over an AM radio via memory bus RF emission\r");
    puts("\r");
    puts("  BUSRADIO [-r] <songfile>\r");
    puts("  BUSRADIO -h\r");
    puts("\r");
    puts("    -r          repeat the song until a key is pressed\r");
    puts("    -h          show this help\r");
    puts("\r");
    puts("Song file: one `freq_hz duration_ms' per line. freq 0 is a rest.\r");
    puts("Text from ; to end of line is a comment.\r");
}

/* Read the song one line at a time and tokenize into note_table.  Strip any
 * trailing comment by NUL-ing at the ';' -- this keeps the note on lines like
 * `0 125 ; rest'.  Blank, whitespace-only, and comment-only lines are
 * skipped; any other line that isn't a `freq duration' pair -- or whose freq
 * or duration exceeds 65535 -- is a hard error naming the line.
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
    unsigned long frequency, duration;

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
        note_table[note_count][0] = (unsigned)frequency;
        note_table[note_count][1] = (unsigned)duration;
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
            (void)bioskey(0);
            return 1;
        }
        /* playback goes here */
    }
    return 0;
}

/* Parse arguments, load the song file, and play it (optionally repeating).
 *
 * Returns:
 *   0  done, or help shown (-h)
 *   1  usage error (bad/missing argument or filename)
 *   2  song file won't open
 *   4  malformed song line, or too many notes
 */
int main(int argc, char **argv)
{
    int argument_index;
    int have_filename = 0;
    int result;

    for (argument_index = 1; argument_index < argc; argument_index++) {
        char *argument = argv[argument_index];
        if (strcmp(argument, "-r") == 0) {
            repeat_flag = 1;
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

    printf("Loaded %u notes.\r\n", note_count);

    do {
        if (play_once() != 0) break;
    } while (repeat_flag);

    return 0;
}
