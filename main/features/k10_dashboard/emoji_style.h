#ifndef EMOJI_STYLE_H
#define EMOJI_STYLE_H

// Registers the self.set_emoji_style MCP tool, letting the assistant switch
// between the Fluent Emoji (animated, the default) and Noto (static) emoji
// sets at runtime, with no reboot. Both are looked up on demand rather than
// held in memory — the images stay in flash either way.
void InitializeEmojiStyleTool();

#endif  // EMOJI_STYLE_H
