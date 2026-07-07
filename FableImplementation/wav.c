#include "glx.h"
#include <stdlib.h>
#include <string.h>

/*
WAV reader refactored from the chunk-walker in ../top.c (readfile) and
../claudius.c; writer from ../glxdecode.c. Only 16-bit PCM is supported,
stereo is downmixed to mono the same way ../top.c did (s[2i]/2 + s[2i+1]/2).
*/
//Code Reviewed 7/6/2026
static int read_u32le(FILE *f, uint32_t *out) 
{
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return -1;
    *out = (uint32_t)b[0] | (uint32_t)b[1] << 8 |
           (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 0;
}

static int read_u16le(FILE *f, uint16_t *out) 
{
    uint8_t b[2];
    if (fread(b, 1, 2, f) != 2) return -1;
    *out = (uint16_t)(b[0] | b[1] << 8);
    return 0;
}

static void write_le16(FILE *f, uint16_t v)
{
    fputc(v & 0xff, f);
    fputc((v >> 8) & 0xff, f);
}

static void write_le32(FILE *f, uint32_t v)
{
    fputc(v & 0xff, f);
    fputc((v >> 8) & 0xff, f);
    fputc((v >> 16) & 0xff, f);
    fputc((v >> 24) & 0xff, f);
}
//correct above ^^

int16_t *wav_read_mono16(const char *path, uint32_t *rate_out, size_t *n_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("Can't open file %s, exiting\n", path);
        return NULL;
    }
    //reads wav header 
    char id[4];
    uint32_t sz;
    if (fread(id, 1, 4, fp) != 4 || memcmp(id, "RIFF", 4) != 0 ||
        read_u32le(fp, &sz) ||
        fread(id, 1, 4, fp) != 4 || memcmp(id, "WAVE", 4) != 0) {
        printf("Incorrect File Format: not a RIFF/WAVE file\n");
        fclose(fp);
        return NULL;
    }
    //kk

    uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0, blockAlign = 0;
    uint32_t sampleRate = 0, byteRate = 0, dataBytes = 0;
    int have_fmt = 0, have_data = 0;

    /* Walk chunks: 4-byte id, 4-byte size, payload (padded to even length). */
    while (fread(id, 1, 4, fp) == 4) {
        if (read_u32le(fp, &sz)) break;

        if (memcmp(id, "fmt ", 4) == 0) {
            read_u16le(fp, &audioFormat);
            read_u16le(fp, &numChannels);
            read_u32le(fp, &sampleRate);
            read_u32le(fp, &byteRate);
            read_u16le(fp, &blockAlign);
            read_u16le(fp, &bitsPerSample);
            have_fmt = 1;
            if (sz > 16) fseek(fp, sz - 16, SEEK_CUR);
        } else if (memcmp(id, "data", 4) == 0) {
            dataBytes = sz;
            have_data = 1;
            break;  /* fp is now at the start of the audio samples */
        } else {
            fseek(fp, sz, SEEK_CUR);
        }
        if (sz & 1) fseek(fp, 1, SEEK_CUR);
    }

    if (!have_fmt || !have_data) 
    {
        printf("Missing fmt or data chunk\n");
        fclose(fp);
        return NULL;
    }
    if (audioFormat != 1) 
    {
        printf("Cannot yet decode non-PCM files\n");
        fclose(fp);
        return NULL;
    }
    if (bitsPerSample != 16) 
    {
        printf("Can only process 16 bit audio atm\n");
        fclose(fp);
        return NULL;
    }
    if (numChannels != 1 && numChannels != 2) {
        printf("Only mono or stereo input is supported\n");
        fclose(fp);
        return NULL;
    }
    //numframes is total length of audio * 48khz should be
    size_t numframes = dataBytes / blockAlign;
    printf("Audio Channels = %u\n", numChannels);
    printf("sample bit depth = %u\n", bitsPerSample);
    printf("We have %zu frames in this data\n", numframes);
    //nah need to fix. shouldn't do the whole thing at once....
    //no real time compatability then
    int16_t *mono = malloc(numframes * sizeof(int16_t));
    if(!mono) 
    {
        fclose(fp);
        return NULL;
    }

    if(numChannels == 1) 
    {
        numframes = fread(mono, sizeof(int16_t), numframes, fp);
    } 
    else 
    {
        int16_t frame[2];
        size_t got = 0;
        while (got < numframes && fread(frame, sizeof(int16_t), 2, fp) == 2) {
            mono[got++] = frame[0] / 2 + frame[1] / 2;  /* downmix */
        }
        numframes = got;
    }

    fclose(fp);
    *rate_out = sampleRate;
    *n_out = numframes;
    return mono;
}
//check with wav header format TODO
int wav_write_mono16(const char *path, const int16_t *pcm, size_t n, uint32_t rate)
{
    FILE *out = fopen(path, "wb");
    if(!out) 
    {
        printf("cannot write %s\n", path);
        return -1;
    }

    uint32_t dataBytes = (uint32_t)(n * 2);
    fwrite("RIFF", 1, 4, out);
    write_le32(out, 36 + dataBytes);
    fwrite("WAVE", 1, 4, out);
    fwrite("fmt ", 1, 4, out);
    write_le32(out, 16);
    write_le16(out, 1);            /* PCM */
    write_le16(out, 1);            /* mono */
    write_le32(out, rate);
    write_le32(out, rate * 2);     /* byte rate */
    write_le16(out, 2);            /* block align */
    write_le16(out, 16);           /* bits per sample */
    fwrite("data", 1, 4, out);
    write_le32(out, dataBytes);

    for (size_t i = 0; i < n; i++)
        write_le16(out, (uint16_t)pcm[i]);

    fclose(out);
    return 0;
}
