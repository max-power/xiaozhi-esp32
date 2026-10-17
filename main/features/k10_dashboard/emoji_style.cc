#include "emoji_style.h"
#include "mcp_server.h"
#include "assets.h"
#include "board.h"
#include "display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "display/lvgl_display/emoji_collection.h"
#include "display/lvgl_display/lvgl_image.h"

#include <esp_log.h>
#include <cJSON.h>
#include <memory>
#include <string>

#define TAG "EmojiStyle"

namespace {

// Every emoji name all three sets provide — checked against all three
// directories when this was written; keep in sync if any set's coverage
// changes.
constexpr const char* kEmojiNames[] = {
    "angry",     "confident", "confused", "cool",    "crying",  "delicious", "embarrassed",
    "funny",     "happy",     "kissy",    "laughing", "loving", "neutral",   "relaxed",
    "sad",       "shocked",   "silly",    "sleepy",  "surprised", "thinking", "winking",
};
constexpr int kNumEmojis = sizeof(kEmojiNames) / sizeof(kEmojiNames[0]);

struct EmbeddedEmoji {
    const char* name;
    const uint8_t* start;
    const uint8_t* end;
};

// Noto static (PNG) — embedded directly from the `78/xiaozhi-fonts`
// dependency's own current assets (main/CMakeLists.txt EMBED_FILES). Not
// vendored: this is that package's normal, currently-shipped content,
// unlike the animated GIFs below, which had to be vendored separately
// after a newer package version dropped its gif/ directory.
#define EMOJI_SYMS(var, name, ext)                                                  \
    extern const uint8_t var##_##name##_##ext##_start[] asm("_binary_" #name "_" #ext "_start"); \
    extern const uint8_t var##_##name##_##ext##_end[] asm("_binary_" #name "_" #ext "_end");
#define NOTO_STATIC_SYMS(name) EMOJI_SYMS(noto_static, name, png)
NOTO_STATIC_SYMS(angry) NOTO_STATIC_SYMS(confident) NOTO_STATIC_SYMS(confused) NOTO_STATIC_SYMS(cool)
NOTO_STATIC_SYMS(crying) NOTO_STATIC_SYMS(delicious) NOTO_STATIC_SYMS(embarrassed) NOTO_STATIC_SYMS(funny)
NOTO_STATIC_SYMS(happy) NOTO_STATIC_SYMS(kissy) NOTO_STATIC_SYMS(laughing) NOTO_STATIC_SYMS(loving)
NOTO_STATIC_SYMS(neutral) NOTO_STATIC_SYMS(relaxed) NOTO_STATIC_SYMS(sad) NOTO_STATIC_SYMS(shocked)
NOTO_STATIC_SYMS(silly) NOTO_STATIC_SYMS(sleepy) NOTO_STATIC_SYMS(surprised) NOTO_STATIC_SYMS(thinking)
NOTO_STATIC_SYMS(winking)
#undef NOTO_STATIC_SYMS

#define NOTO_STATIC_ENTRY(name) {#name, noto_static_##name##_png_start, noto_static_##name##_png_end}
constexpr EmbeddedEmoji kNotoStaticEmojis[] = {
    NOTO_STATIC_ENTRY(angry),     NOTO_STATIC_ENTRY(confident), NOTO_STATIC_ENTRY(confused),
    NOTO_STATIC_ENTRY(cool),      NOTO_STATIC_ENTRY(crying),    NOTO_STATIC_ENTRY(delicious),
    NOTO_STATIC_ENTRY(embarrassed), NOTO_STATIC_ENTRY(funny),   NOTO_STATIC_ENTRY(happy),
    NOTO_STATIC_ENTRY(kissy),     NOTO_STATIC_ENTRY(laughing),  NOTO_STATIC_ENTRY(loving),
    NOTO_STATIC_ENTRY(neutral),   NOTO_STATIC_ENTRY(relaxed),   NOTO_STATIC_ENTRY(sad),
    NOTO_STATIC_ENTRY(shocked),   NOTO_STATIC_ENTRY(silly),     NOTO_STATIC_ENTRY(sleepy),
    NOTO_STATIC_ENTRY(surprised), NOTO_STATIC_ENTRY(thinking),  NOTO_STATIC_ENTRY(winking),
};
#undef NOTO_STATIC_ENTRY
#undef EMOJI_SYMS

// TODO(noto-animated): the vendored animated Noto GIFs (assets/emoji-gif/
// noto-emoji_128/, ~4.5MB) don't fit via EMBED_FILES — that links into the
// ~4MB app partition, not the separate 8MB assets partition Fluent's set
// actually lives in. Adding it properly means extending
// scripts/build_default_assets.py to bundle multiple named emoji
// collections into that assets partition instead.

std::shared_ptr<EmojiCollection> BuildEmbeddedCollection(const EmbeddedEmoji* entries, int count) {
    auto collection = std::make_shared<EmojiCollection>();
    for (int i = 0; i < count; i++) {
        size_t size = entries[i].end - entries[i].start;
        collection->AddEmoji(entries[i].name, new LvglRawImage((void*)entries[i].start, size));
    }
    return collection;
}

// Fluent (animated GIF) — the build-time default (DEFAULT_EMOJI_COLLECTION),
// already sitting in the default assets partition. Reconstructed on demand
// rather than also embedded separately, to avoid storing it twice.
std::shared_ptr<EmojiCollection> BuildFluentCollection() {
    auto collection = std::make_shared<EmojiCollection>();
    auto& assets = Assets::GetInstance();
    for (int i = 0; i < kNumEmojis; i++) {
        void* ptr = nullptr;
        size_t size = 0;
        std::string file = std::string(kEmojiNames[i]) + ".gif";
        if (!assets.GetAssetData(file, ptr, size)) {
            ESP_LOGW(TAG, "Fluent emoji %s not found in assets partition", file.c_str());
            continue;
        }
        collection->AddEmoji(kEmojiNames[i], new LvglRawImage(ptr, size));
    }
    return collection;
}

std::string JsonError(const std::string& message) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", message.c_str());
    char* s = cJSON_PrintUnformatted(json);
    std::string result(s);
    cJSON_free(s);
    cJSON_Delete(json);
    return result;
}

}  // namespace

void InitializeEmojiStyleTool() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.set_emoji_style",
        "Switch the on-screen expression style. 'style': \"fluent\" (animated, the default) or "
        "\"noto\" (static). Takes effect immediately, no reboot.",
        PropertyList({Property("style", kPropertyTypeString)}),
        [](const PropertyList& props) -> ReturnValue {
            std::string style = props["style"].value<std::string>();
            std::shared_ptr<EmojiCollection> collection;
            if (style == "fluent") {
                collection = BuildFluentCollection();
            } else if (style == "noto") {
                collection = BuildEmbeddedCollection(kNotoStaticEmojis, kNumEmojis);
            } else {
                return JsonError("Unknown style: " + style + " — use fluent or noto");
            }

            auto& theme_manager = LvglThemeManager::GetInstance();
            auto light_theme = theme_manager.GetTheme("light");
            auto dark_theme = theme_manager.GetTheme("dark");
            if (light_theme) light_theme->set_emoji_collection(collection);
            if (dark_theme) dark_theme->set_emoji_collection(collection);

            auto display = Board::GetInstance().GetDisplay();
            if (display) display->SetEmotion("happy");

            return true;
        });

    ESP_LOGI(TAG, "self.set_emoji_style registered");
}
