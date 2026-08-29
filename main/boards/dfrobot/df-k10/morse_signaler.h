#ifndef MORSE_SIGNALER_H
#define MORSE_SIGNALER_H

#include <atomic>
#include <string>

#include "led/circular_strip.h"

// Registers an MCP tool that converts text to Morse code and blinks it on an
// LED strip.
class MorseSignaler {
public:
    explicit MorseSignaler(CircularStrip* led);

private:
    CircularStrip* led_;
    std::atomic<bool> playing_{false};

    static void TaskEntry(void* arg);
    void RunPlayback(const std::string& text, int wpm);
};

#endif  // MORSE_SIGNALER_H
