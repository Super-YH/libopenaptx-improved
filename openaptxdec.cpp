/*
 * aptX decoder utility (Modified for WAV output)
 * Copyright (C) 2018-2021  Pali Rohár <pali.rohar@gmail.com>
 * WAV output modifications by AI based on user request.
 *
 * Read README file for license details. Due to license abuse
 * this program must not be used in any Freedesktop project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h> // For standard integer types
#include <stdlib.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "openaptx.h"

// ===================== WAV Header Structure =====================
#pragma pack(push, 1) // Ensure no padding in the struct
struct wav_header {
    // RIFF Chunk
    char     riff_id[4];      // "RIFF"
    uint32_t chunk_size;      // 36 + subchunk2_size
    char     wave_id[4];      // "WAVE"
    // Fmt sub-chunk
    char     fmt_id[4];       // "fmt "
    uint32_t fmt_chunk_size;  // 16 for PCM
    uint16_t audio_format;    // 1 for PCM
    uint16_t num_channels;    // 2 for stereo
    uint32_t sample_rate;     // e.g., 44100
    uint32_t byte_rate;       // sample_rate * num_channels * bits_per_sample / 8
    uint16_t block_align;     // num_channels * bits_per_sample / 8
    uint16_t bits_per_sample; // 24
    // Data sub-chunk
    char     data_id[4];      // "data"
    uint32_t data_chunk_size; // num_samples * num_channels * bits_per_sample / 8
};
#pragma pack(pop)
// =================================================================

static unsigned char input_buffer[512*6];
static unsigned char output_buffer[512*3*2*6+3*2*4];

extern const int aptx_major;
extern const int aptx_minor;
extern const int aptx_patch;

void write_wav_header(FILE *f, uint32_t sample_rate, uint32_t data_size) {
    struct wav_header header;

    // Fill the header
    memcpy(header.riff_id, "RIFF", 4);
    header.chunk_size = 36 + data_size;
    memcpy(header.wave_id, "WAVE", 4);
    memcpy(header.fmt_id, "fmt ", 4);
    header.fmt_chunk_size = 16;
    header.audio_format = 1; // PCM
    header.num_channels = 2; // Stereo
    header.sample_rate = sample_rate;
    header.bits_per_sample = 24;
    header.byte_rate = sample_rate * header.num_channels * (header.bits_per_sample / 8);
    header.block_align = header.num_channels * (header.bits_per_sample / 8);
    memcpy(header.data_id, "data", 4);
    header.data_chunk_size = data_size;

    // Write the header to file
    fseek(f, 0, SEEK_SET);
    fwrite(&header, sizeof(struct wav_header), 1, f);
}

int main(int argc, char *argv[])
{
    int i;
    int hd;
    int ret;
    uint32_t sample_rate = 44100; // Default sample rate
    size_t total_written = 0;
    size_t length;
    size_t processed;
    size_t written;
    size_t dropped;
    int synced;
    int syncing;
    struct aptx_context *ctx;
    FILE *output_file = stdout;

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(output_file), _O_BINARY);
#endif

    hd = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "aptX to WAV decoder utility %d.%d.%d (using libopenaptx %d.%d.%d)\n", OPENAPTX_MAJOR, OPENAPTX_MINOR, OPENAPTX_PATCH, aptx_major, aptx_minor, aptx_patch);
            fprintf(stderr, "\n");
            fprintf(stderr, "This utility decodes aptX or aptX HD audio stream\n");
            fprintf(stderr, "from stdin to a WAV file on stdout\n");
            fprintf(stderr, "\nUsage:\n");
            fprintf(stderr, "        %s [options]\n", argv[0]);
            fprintf(stderr, "\nOptions:\n");
            fprintf(stderr, "        -h, --help       Display this help\n");
            fprintf(stderr, "        --hd             Decode from aptX HD\n");
            fprintf(stderr, "        -r, --rate RATE  Set sample rate for WAV (default 44100)\n");
            return 1;
        } else if (strcmp(argv[i], "--hd") == 0) {
            hd = 1;
        } else if ((strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--rate") == 0) && i + 1 < argc) {
            sample_rate = atoi(argv[++i]);
            if (sample_rate == 0) {
                fprintf(stderr, "%s: Invalid sample rate\n", argv[0]);
                return 1;
            }
        }
        else {
            fprintf(stderr, "%s: Invalid option %s\n", argv[0], argv[i]);
            return 1;
        }
    }

    ctx = aptx_init(hd);
    if (!ctx) {
        fprintf(stderr, "%s: Cannot initialize aptX decoder\n", argv[0]);
        return 1;
    }

    // Write initial dummy WAV header
    write_wav_header(output_file, sample_rate, 0);

    /* Try to guess type of input stream based on the first six bytes */
    length = fread(input_buffer, 1, 6, stdin);
    if (length >= 4 && memcmp(input_buffer, "\x4b\xbf\x4b\xbf", 4) == 0) {
        if (hd)
            fprintf(stderr, "%s: Input looks like start of aptX audio stream (not aptX HD), try without --hd\n", argv[0]);
    } else if (length >= 6 && memcmp(input_buffer, "\x73\xbe\xff\x73\xbe\xff", 6) == 0) {
        if (!hd)
            fprintf(stderr, "%s: Input looks like start of aptX HD audio stream, try with --hd\n", argv[0]);
    }

    ret = 0;
    syncing = 0;

    while (length > 0) {
        processed = aptx_decode_sync(ctx, input_buffer, length, output_buffer, sizeof(output_buffer), &written, &synced, &dropped);

        if (!synced) {
            if (!syncing) {
                fprintf(stderr, "%s: Stream damaged, synchronizing...\n", argv[0]);
                syncing = 1;
                ret = 1;
            }
            if (dropped > 0) {
                fprintf(stderr, "%s: Synchronization successful, dropped %lu byte%s\n", argv[0], (unsigned long)dropped, (dropped != 1) ? "s" : "");
                syncing = 0; // Sync seems successful for now
            }
        } else {
             if (dropped > 0) { // Should not happen with synced=1, but for safety
                if (!syncing) fprintf(stderr, "%s: Stream damaged, synchronizing...\n", argv[0]);
                fprintf(stderr, "%s: Synchronization successful, dropped %lu byte%s\n", argv[0], (unsigned long)dropped, (dropped != 1) ? "s" : "");
                syncing = 0;
                ret = 1;
            } else if (syncing) {
                fprintf(stderr, "%s: Synchronization successful\n", argv[0]);
                syncing = 0;
            }
        }

        if (processed != length) {
            fprintf(stderr, "%s: Unrecoverable aptX decoding failure\n", argv[0]);
            ret = 1;
            break;
        }

        if (written > 0) {
            if (fwrite(output_buffer, 1, written, output_file) != written) {
                fprintf(stderr, "%s: Failed to write WAV data\n", argv[0]);
                ret = 1;
                break;
            }
            total_written += written;
        }

        if (!feof(stdin)) {
            length = fread(input_buffer, 1, sizeof(input_buffer), stdin);
            if (ferror(stdin)) {
                fprintf(stderr, "%s: Failed to read input data\n", argv[0]);
                ret = 1;
                break;
            }
        } else {
            length = 0;
        }
    }
    
    dropped = aptx_decode_sync_finish(ctx);
    if (dropped) {
        fprintf(stderr, "%s: Incomplete data at end of stream, dropped %lu byte%s\n", argv[0], (unsigned long)dropped, (dropped != 1) ? "s" : "");
        ret = 1;
    }

    aptx_finish(ctx);
    
    // Final step: update the WAV header with the correct size
    write_wav_header(output_file, sample_rate, total_written);
    
    fflush(output_file);
    
    if (ret != 0) {
        fprintf(stderr, "\nWarning: The stream was damaged and data was lost. The resulting WAV file may have glitches.\n");
    }

    return ret;
}