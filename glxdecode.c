// Decodes a .glx file back to a 16 kHz mono 16-bit PCM .wav so it can be played.
// Usage: ./glxdecode out.glx out.wav
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static void write_le32(FILE *f, uint32_t v){ fputc(v&0xff,f); fputc((v>>8)&0xff,f); fputc((v>>16)&0xff,f); fputc((v>>24)&0xff,f); }
static void write_le16(FILE *f, uint16_t v){ fputc(v&0xff,f); fputc((v>>8)&0xff,f); }

int main(int argc, char **argv)
{
    if(argc != 3){ printf("usage: %s in.glx out.wav\n", argv[0]); return -1; }

    FILE *in = fopen(argv[1], "rb");
    if(!in){ printf("cannot open %s\n", argv[1]); return -1; }

    // --- GLX header (14 bytes): magic[4], sampleRate u32, bitsPerSym u8, alphaIdx u8, numPackets u32 ---
    char magic[4];
    uint32_t sampleRate, numPackets;
    uint8_t bitsPerSym, alphaIdx;
    fread(magic, 1, 4, in);
    fread(&sampleRate, 4, 1, in);
    fread(&bitsPerSym, 1, 1, in);
    fread(&alphaIdx, 1, 1, in);
    fread(&numPackets, 4, 1, in);
    if(memcmp(magic, "GLX1", 4) != 0){ printf("not a GLX1 file\n"); return -1; }
    printf("rate=%u bits=%u alpha=%u packets=%u\n", sampleRate, bitsPerSym, alphaIdx, numPackets);

    // --- decode 3-bit symbols (8 symbols per 3 bytes, MSB-first) into 16-bit PCM ---
    FILE *tmp = tmpfile();
    uint32_t nsamples = 0;
    unsigned char b[3];
    while(fread(b, 1, 3, in) == 3){
        uint32_t v = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
        for(int k = 0; k < 8; k++){
            int s = (v >> (21 - 3*k)) & 0x7;          // 0..7
            int16_t sample = (int16_t)(s * 8192 - 28672); // inverse of (x+32768)>>13 bin centre
            write_le16(tmp, (uint16_t)sample);
            nsamples++;
        }
    }

    // --- write a standard 16 kHz mono 16-bit WAV ---
    FILE *out = fopen(argv[2], "wb");
    if(!out){ printf("cannot write %s\n", argv[2]); return -1; }
    uint32_t dataBytes = nsamples * 2;
    fwrite("RIFF", 1, 4, out); write_le32(out, 36 + dataBytes); fwrite("WAVE", 1, 4, out);
    fwrite("fmt ", 1, 4, out); write_le32(out, 16);
    write_le16(out, 1);            // PCM
    write_le16(out, 1);            // mono
    write_le32(out, sampleRate);
    write_le32(out, sampleRate*2); // byte rate
    write_le16(out, 2);            // block align
    write_le16(out, 16);          // bits per sample
    fwrite("data", 1, 4, out); write_le32(out, dataBytes);

    rewind(tmp);
    unsigned char buf[4096]; size_t n;
    while((n = fread(buf, 1, sizeof buf, tmp)) > 0) fwrite(buf, 1, n, out);

    fclose(tmp); fclose(in); fclose(out);
    printf("wrote %u samples (%.2f s) to %s\n", nsamples, (double)nsamples/sampleRate, argv[2]);
    return 0;
}
