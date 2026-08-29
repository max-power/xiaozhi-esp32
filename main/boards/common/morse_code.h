#ifndef MORSE_CODE_H
#define MORSE_CODE_H

#include <string>
#include <vector>

enum class MorseSignalType {
    kTone,
    kSilence,
};

struct MorseSignal {
    MorseSignalType type;
    int duration_ms;
};

// Encodes text into a sequence of tone/silence signals using standard ITU
// Morse timing (one unit = 1200 / wpm milliseconds). Letters are matched
// case-insensitively; characters with no Morse representation are skipped.
std::vector<MorseSignal> EncodeMorse(const std::string& text, int wpm = 20);

#endif  // MORSE_CODE_H
