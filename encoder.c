
//REAL TIME
#define FRAMES 216
int16_t buffer[FRAMES];

while (recording) {
    // blocks until 512 new samples are ready
    read_from_audio_device(buffer, FRAMES);
    
    // now you have 216 contiguous int16_t samples
    // write them to your WAV file, ring buffer, etc.
    dither first
    quantize and resample
    fwrite(buffer, sizeof(int16_t), FRAMES, wav_file);
}

int * dither()