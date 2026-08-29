#ifndef SD_CONTROL_H
#define SD_CONTROL_H

// Registers the self.sd MCP tool for SD card file operations.
// sd_mounted: whether the card mounted successfully, so the tool can report
// a clean error instead of failing every filesystem call.
void InitializeSdTool(bool sd_mounted);

#endif  // SD_CONTROL_H
