#include <string.h>
#include "declarations.h"

int main(int argc, char ** argv)
{
    //define two modes: input from .wav ->output .glx
    //real time recording
    //step 1 input from .wav

    //call of format ./our.exe our.wav alpha
    if(argc != 4)
    {
        printf("call of format ./our.exe our.wav alpha, out.glx");
        return -1;
    }

    FILE * wavfp = fopen(argv[1], "rb"); //have the wav file be argv[2]
    //determine rate, size so we can determine number of packets we need
    if(!wavfp)
    {
        printf("Can't open file, exiting");
        return -1;
    }

    WavHeader * header = readfile(wavfp);
    if(!header)
    {
        printf("Error Parsing Header");
        return -1;
    }

    //now FP should be at data 
    //if numchannels is 2 we need to downmix all frames
    if(header->audioFormat != 1)
    {
        printf("Cannot yet decode non-PCM files");
        return -1;
    }

    //can we assume 16 bit input 
    int numframes = header->subchunk2Size / header->blockAlign;
    printf("Audio Channels = %d\n", header->numChannels);
    int framesperpacket = header->sampleRate / 100; //10ms oer packet
    
    printf("sample bit depth = %d", header->bitsPerSample);
    if(header->bitsPerSample != 16)
    {
        printf("Can only process 16 bit audio atm");
        return -1;
    }
    int16_t * stereo = malloc(sizeof(int16_t) * framesperpacket);
    int16_t * databus = malloc(framesperpacket * sizeof(int16_t));
    int validframesread = framesperpacket;
    if(header->numChannels == 1) //mono audio
    {
        while(validframesread == framesperpacket)
        {
            validframesread = fread(databus, sizeof(int16_t), framesperpacket, wavfp);
            //PROCESS PACKET HERE
        }
    }
    else if(header->numChannels == 2) //stereo
    {
        while(validframesread == 2*framesperpacket)
        {
            validframesread = fread(stereo, sizeof(int16_t), 2 * framesperpacket, wavfp);
            int counter = 0;
            for(int i = 0; i < 2 * framesperpacket; i += 2)
            {
                databus[counter] = stereo[i]/2 + stereo[i + 1]/2;
                counter++;
            }

            //PROCESS PACKET HERE
        }
    }
    
}

WavHeader * readfile(FILE * fp)
{
    WavHeader * header = malloc(sizeof(WavHeader));
    fread(header->riff, sizeof(char), 4, fp);
    fread(&header->filesize, sizeof(uint32_t), 1, fp);
    fread(header->wave, sizeof(char), 4, fp);
    if(memcmp(header->riff, "RIFF", 4) != 0 || memcmp(header->wave, "WAVE", 4) != 0)
    {
        printf("Incorrect File Format, saved as %.4s, %.4s", header->riff, header->wave);
        free(header);
        return NULL;
    }

    char id[4];
    uint32_t sz;
    int have_fmt = 0, have_data = 0;

    while (fread(id, 1, 4, fp) == 4)
    {
        fread(&sz, sizeof(uint32_t), 1, fp);

        if (memcmp(id, "fmt ", 4) == 0)
        {
            memcpy(header->fmt, id, 4);
            fread(&header->audioFormat,  sizeof(uint16_t), 1, fp);
            fread(&header->numChannels,  sizeof(uint16_t), 1, fp);
            fread(&header->sampleRate,   sizeof(uint32_t), 1, fp);
            fread(&header->byteRate,     sizeof(uint32_t), 1, fp);
            fread(&header->blockAlign,   sizeof(uint16_t), 1, fp);
            fread(&header->bitsPerSample,sizeof(uint16_t), 1, fp);
            have_fmt = 1;
            if (sz > 16) fseek(fp, sz - 16, SEEK_CUR);
        }
        else if (memcmp(id, "data", 4) == 0)
        {
            memcpy(header->data, id, 4);
            header->subchunk2Size = sz;
            have_data = 1;
        }
        else
        {
            fseek(fp, sz, SEEK_CUR);
        }

        if (sz & 1) fseek(fp, 1, SEEK_CUR);
    }

    if (!have_fmt || !have_data)
    {
        printf("Missing fmt or data chunk\n");
        free(header);
        return NULL;
    }

    return header;
}