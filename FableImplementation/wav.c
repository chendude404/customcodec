#include "glx.h"
#include <stdlib.h>
#include <string.h>

/*
Streaming WAV I/O. The chunk-walking header parse is the same as before
(from ../top.c readfile / ../claudius.c); the whole-file read/write pair
was replaced by open/read/close + open/write/close so the codec can run
one 10 ms packet at a time — the target device may not have enough RAM
to hold a whole file. Only 16-bit PCM is supported; stereo is downmixed
to mono the same way ../top.c did (s[2i]/2 + s[2i+1]/2).
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

/* ── Reader ─────────────────────────────────────────────────────────── */

int wav_reader_open(WavReader *r, const char *path)
{
    memset(r, 0, sizeof *r);

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("Can't open file %s, exiting\n", path);
        return -1;
    }
    //reads wav header
    char id[4];
    uint32_t sz;
    if (fread(id, 1, 4, fp) != 4 || memcmp(id, "RIFF", 4) != 0 ||
        read_u32le(fp, &sz) ||
        fread(id, 1, 4, fp) != 4 || memcmp(id, "WAVE", 4) != 0) {
        printf("Incorrect File Format: not a RIFF/WAVE file\n");
        fclose(fp);
        return -1;
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
        return -1;
    }
    if (audioFormat != 1)
    {
        printf("Cannot yet decode non-PCM files\n");
        fclose(fp);
        return -1;
    }
    if (bitsPerSample != 16)
    {
        printf("Can only process 16 bit audio atm\n");
        fclose(fp);
        return -1;
    }
    if (numChannels != 1 && numChannels != 2) {
        printf("Only mono or stereo input is supported\n");
        fclose(fp);
        return -1;
    }
    //numframes is total length of audio * 48khz should be
    size_t numframes = dataBytes / blockAlign;
    printf("Audio Channels = %u\n", numChannels);
    printf("sample bit depth = %u\n", bitsPerSample);
    printf("We have %zu frames in this data\n", numframes);

    r->fp = fp;
    r->channels = numChannels;
    r->rate = sampleRate;
    r->framesTotal = numframes;
    r->framesRead = 0;
    return 0;
}

size_t wav_reader_read(WavReader *r, int16_t *mono, size_t nframes)
{
    if (!r->fp) return 0;

    size_t left = r->framesTotal - r->framesRead;
    if (nframes > left) nframes = left;   /* never read past the data chunk */

    size_t got = 0;
    if (r->channels == 1)
    {
        got = fread(mono, sizeof(int16_t), nframes, r->fp);
    }
    else
    {
        /* downmix on the fly through a small fixed buffer */
        int16_t frames[2 * 64];
        while (got < nframes) {
            size_t want = nframes - got;
            if (want > 64) want = 64;
            size_t rd = fread(frames, 2 * sizeof(int16_t), want, r->fp);
            for (size_t i = 0; i < rd; i++)
                mono[got + i] = frames[2*i] / 2 + frames[2*i + 1] / 2;
            got += rd;
            if (rd < want) break;   /* short file */
        }
    }
    r->framesRead += got;
    return got;
}

void wav_reader_close(WavReader *r)
{
    if (r->fp) fclose(r->fp);
    r->fp = NULL;
}

/* ── Writer ─────────────────────────────────────────────────────────── */

int wav_writer_open(WavWriter *w, const char *path, uint32_t rate)
{
    memset(w, 0, sizeof *w);

    FILE *out = fopen(path, "wb");
    if(!out)
    {
        printf("cannot write %s\n", path);
        return -1;
    }

    /* sizes are placeholders (0 samples); wav_writer_close patches them */
    fwrite("RIFF", 1, 4, out);
    write_le32(out, 36);
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
    write_le32(out, 0);

    w->fp = out;
    w->framesWritten = 0;
    return 0;
}

int wav_writer_write(WavWriter *w, const int16_t *pcm, size_t n)
{
    if (!w->fp) return -1;
    for (size_t i = 0; i < n; i++)
        write_le16(w->fp, (uint16_t)pcm[i]);
    w->framesWritten += n;
    return ferror(w->fp) ? -1 : 0;
}

int wav_writer_close(WavWriter *w)
{
    if (!w->fp) return -1;

    /* patch the RIFF size (offset 4) and data chunk size (offset 40) */
    uint32_t dataBytes = (uint32_t)(w->framesWritten * 2);
    int rc = 0;
    if (fseek(w->fp, 4, SEEK_SET) != 0) rc = -1;
    write_le32(w->fp, 36 + dataBytes);
    if (fseek(w->fp, 40, SEEK_SET) != 0) rc = -1;
    write_le32(w->fp, dataBytes);
    if (ferror(w->fp)) rc = -1;

    fclose(w->fp);
    w->fp = NULL;
    return rc;
}
