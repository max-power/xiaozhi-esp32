#ifndef MORSE_SIGNALER_H
#define MORSE_SIGNALER_H

#include "led/circular_strip.h"

// Registers an MCP tool that converts text to Morse code and blinks it on an
// LED strip.
class MorseSignaler {
public:
    explicit MorseSignaler(CircularStrip* led);
};

#endif  // MORSE_SIGNALER_H
