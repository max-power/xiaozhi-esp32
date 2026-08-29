#include "morse_signaler.h"

#include <memory>
#include <stdexcept>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "mcp_server.h"
#include "morse_code.h"

#define TAG "MorseSignaler"

namespace {

struct PlaybackArgs {
    MorseSignaler* self;
    std::string text;
    int wpm;
};

}  // namespace

MorseSignaler::MorseSignaler(CircularStrip* led) : led_(led) {
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool("self.morse.play",
        "Convert text to Morse code and blink it on the LED strip.\n"
        "Supports A-Z, 0-9, spaces, and common punctuation. The default speed (8 wpm) is "
        "tuned to be readable by eye; raise wpm for a faster, less readable blink.",
        PropertyList({
            Property("text", kPropertyTypeString),
            Property("wpm", kPropertyTypeInteger, 8, 3, 40),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto text = properties["text"].value<std::string>();
            auto wpm = properties["wpm"].value<int>();

            if (text.empty()) {
                throw std::runtime_error("text must not be empty");
            }
            bool expected = false;
            if (!playing_.compare_exchange_strong(expected, true)) {
                throw std::runtime_error("Morse playback is already in progress");
            }

            auto args = new PlaybackArgs{this, text, wpm};
            if (xTaskCreate(&MorseSignaler::TaskEntry, "morse", 4096, args, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
                delete args;
                playing_ = false;
                throw std::runtime_error("Failed to start Morse playback task");
            }
            return true;
        });
}

void MorseSignaler::TaskEntry(void* arg) {
    std::unique_ptr<PlaybackArgs> args(static_cast<PlaybackArgs*>(arg));
    args->self->RunPlayback(args->text, args->wpm);
    vTaskDelete(nullptr);
}

void MorseSignaler::RunPlayback(const std::string& text, int wpm) {
    auto signals = EncodeMorse(text, wpm);

    for (const auto& signal : signals) {
        led_->SetAllColor(signal.type == MorseSignalType::kTone ? StripColor{255, 255, 255} : StripColor{0, 0, 0});
        vTaskDelay(pdMS_TO_TICKS(signal.duration_ms));
    }

    led_->OnStateChanged();

    ESP_LOGI(TAG, "Morse playback finished (%d symbols)", static_cast<int>(signals.size()));
    playing_ = false;
}
