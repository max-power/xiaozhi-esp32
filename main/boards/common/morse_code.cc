#include "morse_code.h"

#include <cctype>
#include <sstream>
#include <unordered_map>

#include <esp_log.h>

#define TAG "MorseCode"

namespace {

const std::unordered_map<char, std::string>& MorseTable() {
    static const std::unordered_map<char, std::string> table = {
        {'A', ".-"},    {'B', "-..."},  {'C', "-.-."}, {'D', "-.."},
        {'E', "."},     {'F', "..-."},  {'G', "--."},  {'H', "...."},
        {'I', ".."},    {'J', ".---"},  {'K', "-.-"},  {'L', ".-.."},
        {'M', "--"},    {'N', "-."},    {'O', "---"},  {'P', ".--."},
        {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},  {'T', "-"},
        {'U', "..-"},   {'V', "...-"},  {'W', ".--"},  {'X', "-..-"},
        {'Y', "-.--"},  {'Z', "--.."},
        {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"},
        {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
        {'8', "---.."}, {'9', "----."},
        {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'\'', ".----."},
        {'!', "-.-.--"}, {'/', "-..-."},  {'(', "-.--."},  {')', "-.--.-"},
        {'&', ".-..."},  {':', "---..."}, {';', "-.-.-."}, {'=', "-...-"},
        {'+', ".-.-."},  {'-', "-....-"}, {'_', "..--.-"}, {'"', ".-..-."},
        {'$', "...-..-"}, {'@', ".--.-."},
    };
    return table;
}

std::vector<std::string> SplitWords(const std::string& text) {
    std::vector<std::string> words;
    std::istringstream iss(text);
    std::string word;
    while (iss >> word) {
        words.push_back(word);
    }
    return words;
}

}  // namespace

std::vector<MorseSignal> EncodeMorse(const std::string& text, int wpm) {
    if (wpm <= 0) {
        wpm = 20;
    }
    const int unit_ms = 1200 / wpm;
    const auto& table = MorseTable();
    std::vector<MorseSignal> signals;

    auto words = SplitWords(text);
    for (size_t w = 0; w < words.size(); ++w) {
        const auto& word = words[w];
        for (size_t c = 0; c < word.size(); ++c) {
            char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(word[c])));
            auto it = table.find(ch);
            if (it == table.end()) {
                ESP_LOGW(TAG, "Skipping unsupported character: '%c'", ch);
                continue;
            }
            const auto& pattern = it->second;
            for (size_t i = 0; i < pattern.size(); ++i) {
                int duration = (pattern[i] == '.') ? unit_ms : unit_ms * 3;
                signals.push_back({MorseSignalType::kTone, duration});
                if (i + 1 < pattern.size()) {
                    signals.push_back({MorseSignalType::kSilence, unit_ms});  // intra-character gap
                }
            }
            if (c + 1 < word.size()) {
                signals.push_back({MorseSignalType::kSilence, unit_ms * 3});  // inter-character gap
            }
        }
        if (w + 1 < words.size()) {
            signals.push_back({MorseSignalType::kSilence, unit_ms * 7});  // inter-word gap
        }
    }
    return signals;
}
