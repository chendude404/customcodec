

//we take a 16 bit WAV -> then quantize and dither -> output is 3 bit
//
Wav header


psuedocode for encoder
open wav file 
File * wavfp = fopen("audio.wav", "rb");
    embedd into packets



break apart .wav into 0.25 second intervals: if 48khz 
    packet size of 16 * 12,000 -> 24kb per sample: 
    wrap each packet as a struct 

    #pragma pack(push, 1)
    typedef struct encoderpacket 
    {
        int packetnum
        data * newdata;

    }
    #pragma pack(pop)

    //how do we flash the data to each block
    typedef struct data
    {
        uint16_t * registerblock -> malloc(sizeof(int16_t) * 12000)
    }

    top file()
