#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Read a 32-bit little-endian unsigned int, byte-by-byte for portability. */
static int read_u32le(FILE *f, uint32_t *out) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return -1;
    *out = (uint32_t)b[0] | (uint32_t)b[1] << 8 |
           (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 0;
}

static int read_u16le(FILE *f, uint16_t *out) {
    uint8_t b[2];
    if (fread(b, 1, 2, f) != 2) return -1;
    *out = (uint16_t)(b[0] | b[1] << 8);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s file.wav\n", argv[0]); return 1; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }

    char id[4];
    uint32_t riff_size;
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4) != 0) goto bad;
    read_u32le(f, &riff_size);                 /* file size - 8 */
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "WAVE", 4) != 0) goto bad;

    uint16_t channels = 0, bits = 0;
    uint32_t rate = 0, data_bytes = 0;
    int have_fmt = 0, have_data = 0;

    /* Walk chunks: 4-byte id, 4-byte size, payload (padded to even length). */
    while (fread(id, 1, 4, f) == 4) {
        uint32_t sz;
        if (read_u32le(f, &sz)) break;

        if (memcmp(id, "fmt ", 4) == 0) {
            uint16_t fmt_tag, block_align;
            uint32_t byte_rate;
            read_u16le(f, &fmt_tag);
            read_u16le(f, &channels);
            read_u32le(f, &rate);
            read_u32le(f, &byte_rate);
            read_u16le(f, &block_align);
            read_u16le(f, &bits);
            have_fmt = 1;
            /* skip any fmt-extension bytes beyond the 16 we read */
            if (sz > 16) fseek(f, sz - 16, SEEK_CUR);
        } else if (memcmp(id, "data", 4) == 0) {
            data_bytes = sz;                   /* <-- the PCM payload size */
            have_data = 1;
            fseek(f, sz, SEEK_CUR);            /* skip the samples */
        } else {
            fseek(f, sz, SEEK_CUR);            /* unknown chunk: skip */
        }
        if (sz & 1) fseek(f, 1, SEEK_CUR);     /* chunks are word-aligned */
    }
    fclose(f);

    if (!have_fmt || !have_data) goto bad_nofile;

    uint32_t samp_width = bits / 8;
    uint32_t n_frames   = data_bytes / (channels * samp_width);
    double   duration   = (double)n_frames / rate;

    printf("channels:   %u\n", channels);
    printf("rate:       %u Hz\n", rate);
    printf("bits:       %u\n", bits);
    printf("PCM data:   %u bytes\n", data_bytes);
    printf("frames:     %u (samples per channel)\n", n_frames);
    printf("duration:   %.3f s\n", duration);
    return 0;

bad:
    fclose(f);
bad_nofile:
    fprintf(stderr, "not a valid WAV\n");
    return 1;
}