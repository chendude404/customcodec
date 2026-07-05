#ifndef DECLARATIONS_H
#define DECLARATIONS_H

#include "essentials.h"
#include <stdbool.h>

WavHeader * readfile(FILE * fp);

void resamplepackets(int16_t * databus, FILE * fp, int16_t numsamples, int alphaval, uint32_t * seed);

void writeglxheader(WavHeader * header, FILE * fp, int alpha, int numframes, uint32_t * seedpointer, int mulaw);

#endif
