#include "morse_signaler.h"

#include <stdexcept>

#include "mcp_server.h"

MorseSignaler::MorseSignaler(CircularStrip* led) {
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool("self.morse.play",
        "Convert text to Morse code and blink it on the LED strip.\n"
        "Supports A-Z, 0-9, spaces, and common punctuation.",
        PropertyList({
            Property("text", kPropertyTypeString),
            Property("wpm", kPropertyTypeInteger, 8, 3, 40),
        }),
        [led](const PropertyList& properties) -> ReturnValue {
            auto text = properties["text"].value<std::string>();
            auto wpm = properties["wpm"].value<int>();

            if (text.empty()) {
                throw std::runtime_error("text must not be empty");
            }

            led->Morse(StripColor{255, 255, 255}, text, wpm);
            return true;
        });
}
