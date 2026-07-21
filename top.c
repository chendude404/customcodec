#include <string.h>
#include "declarations.h"

int main(int argc, char ** argv)
{
    //define two modes: input from .wav ->output .glx
    //real time recording
    //step 1 input from .wav

    //call of format ./our.exe our.wav alpha seed int (1 for mu law, 0 for not) out.glx
    if(argc != 5)
    {
        printf("call of format ./our.exe our.wav alpha, seed, mulaw, out.glx");
        return -1;
    }

    //parse inputs into ints
    int16_t alphaval = (int16_t)atoi(argv[2]); //ALPHA IS SECOND INPUT VALUE SO WE CAN WRITE A PYTHONN HARNESS

    uint32_t * seedpointer = malloc(sizeof(uint32_t));

    *seedpointer = (uint32_t)strtoul(argv[3], NULL, 10);
    //smaples
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
      //10ms oer packet
    
    printf("sample bit depth = %d", header->bitsPerSample);
    printf("We have %d frames in this data", numframes);
    printf("For this code, we get %d frames per packet", framesperpacket);
    if(header->bitsPerSample != 16)
    {
        printf("Can only process 16 bit audio atm");
        return -1;
    }
    int16_t * stereo = malloc(sizeof(int16_t) * 2 * framesperpacket);
    int16_t * databus = malloc(framesperpacket * sizeof(int16_t));
    int validframesread = framesperpacket;
    //10ms -> 480 or 441 frames per packet
    //only take 480 (hardcode)
    int mulaw = argv[4];
    if(mulaw != 1 && mulaw != 0)
    {
        printf("Argv[4] must be Mu Law: 1 for Yes, 0 for No");
    }
    
    FILE * outfp = fopen(argv[5], "wb");
    if(outfp == NULL)
    {
        printf("Could not write to output file, Error");
        return -1;
    }
    printf("We are writing to output file %s", argv[4]);


    writeglx(header, outfp, alphaval, numframes, seedpointer, mulaw);
    

    if(header->numChannels == 1) //mono audio
    {
        while(validframesread == framesperpacket)
        {
            validframesread = fread(databus, sizeof(int16_t), framesperpacket, wavfp);
            //PROCESS PACKET HERE
            if(validframesread == framesperpacket)
                resamplepackets(databus, outfp, framesperpacket, alphaval, seedpointer, mulaw);
        }
    }
    else if(header->numChannels == 2) //stereo
    {
        validframesread = 2 * framesperpacket;
        while(validframesread == 2*framesperpacket)
        {
            validframesread = fread(stereo, sizeof(int16_t), 2 * framesperpacket, wavfp);
            int counter = 0;
            for(int i = 0; i + 1 < validframesread; i += 2)
            {
                databus[counter] = stereo[i]/2 + stereo[i + 1]/2;
                counter++;
            }
            //PROCESS PACKET HERE
            if(validframesread == 2*framesperpacket)
                resamplepackets(databus, outfp, framesperpacket, alphaval, seedpointer, mulaw);
            else
                validframesread = counter; // leftover frames, in mono-frame units, for the guard
        }
    }

    // process the final partial packet left in databus
    if(validframesread > 0 && validframesread < framesperpacket)
        resamplepackets(databus, outfp, validframesread, alphaval, seedpointer);

    free(header);
    free(databus);
    free(stereo);
    free(seedpointer);
    return EXIT_SUCCESS;
    
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
            break; // fp is now at the start of the audio samples; stop scanning
        }
        else
        {
            fseek(fp, sz, SEEK_CUR);
        }

        if(sz & 1) fseek(fp, 1, SEEK_CUR);
    }

    if (!have_fmt || !have_data)
    {
        printf("Missing fmt or data chunk\n");
        free(header);
        return NULL;
    }

    return header;
}

void writeglxheader(WavHeader * header, FILE * fp, int alpha, int numframes, uint32_t * seedpointer, int mulaw)
{
    //FORMAT OF .GLX HEADER 
        /*
        typedef struct GlxHeader {
        char magic[4];       // e.g. "GLX1"
        uint32_t sampleRate; // output rate (16000)
        uint8_t  bitsPerSym; // 3
        uint8_t  alphaIdx;   // the alpha index used
        uint8_t mulaw        // 1 or 0
        uint32_t seedpointer //our seedvalue
        uint32_t numPackets; // total number of packets
        } GlxHeader;
        */
    
    char magic[] = "GLX1"; //includes \0
    fwrite(&magic, sizeof(char), 4, fp); //only 4 bytes so we don't  include null terminator
    uint32_t sampleRate = 16000;
    uint8_t scaledalpha = alpha * 255;
    uint8_t bitdepthandalpha[] = {3, scaledalpha};
    fwrite(&sampleRate, sizeof(uint32_t), 1, fp);
    fwrite(&bitdepthandalpha, sizeof(uint8_t), 2, fp);
    fwrite(&mulaw, sizeof(uint8_t), 1, fp);
    fwrite(seedpointer, sizeof(uint32_t), 1, fp);
    uint32_t numpackets = numframes / (header->sampleRate / 100);
    fwrite(&numpackets, sizeof(uint32_t), 1, fp);

}