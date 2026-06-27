#ifndef ESSENTIALS_H
#define ESSENTIALS_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* 
    to try and do real time we need to do double buffering
    Replace fread with an interrupt-driven buffer
    for(;;) is a loop that is always true

*/
typedef struct data{
    uint16_t * registerblock;
    //10ms of audio
} data;

#pragma pack(push, 1)

typedef struct encoderpacket {
    int packetnum;
    data newdata;
}encoderpacket;

#pragma pack(pop)

#pragma pack(push, 1)

typedef struct WavHeader{
    // RIFF Chunk Descriptor
    char riff[4];
    char wave[4];       // "RIFF" "WAVE"
    uint32_t filesize;

    // "fmt " Sub-chunk
    char fmt[4];    // "fmt "
    uint32_t subchunk1Size;    // 16 for PCM
    uint16_t audioFormat;      // 1 for PCM (uncompressed)
    uint16_t numChannels;      // Mono = 1, Stereo = 2
    uint32_t sampleRate;       // e.g., 44100
    uint32_t byteRate;         // sampleRate * numChannels * bitsPerSample/8
    uint16_t blockAlign;       // numChannels * bitsPerSample/8
    uint16_t bitsPerSample;    // 8, 16, 24, or 32 bits

    // "data" Sub-chunk
    char data[4];    // "data"
    uint32_t subchunk2Size;    // Size of the raw audio data
} WavHeader;
#pragma pack(pop)

//how do we flash the data to each block

#endif
