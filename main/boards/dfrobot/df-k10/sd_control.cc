#include "sd_control.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <cctype>
#include <string>
#include <ctime>

#define TAG "SdControl"

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::string sd_fmt_time(time_t t) {
    if (t == 0) return "unknown";
    char buf[24];
    struct tm tm_info;
    gmtime_r(&t, &tm_info);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    return buf;
}

static std::string sd_human_size(uint64_t b) {
    char buf[32];
    if      (b >= 1073741824ULL) snprintf(buf, sizeof(buf), "%.1f GB", b / 1073741824.0);
    else if (b >= 1048576ULL)    snprintf(buf, sizeof(buf), "%.1f MB", b / 1048576.0);
    else if (b >= 1024ULL)       snprintf(buf, sizeof(buf), "%.1f KB", b / 1024.0);
    else                         snprintf(buf, sizeof(buf), "%llu B",  (unsigned long long)b);
    return buf;
}

// ─── Tool registration ───────────────────────────────────────────────────────

void InitializeSdTool(bool sd_mounted) {
    auto& mcp = McpServer::GetInstance();

    // ── self.sd ───────────────────────────────────────────────────────────────
    // action=info   — capacity / free space (no path needed)
    // action=list   — list directory ('path' optional, empty = root)
    // action=stat   — file/dir info ('path')
    // action=mkdir  — create directory, parents created as needed ('path')
    // action=delete — delete file or empty directory ('path')
    // action=rename — rename/move ('path' = source, 'to' = destination)
    // action=read   — read text file ('path'), max 4096 bytes
    // action=write  — write/append text file ('path', 'content', 'append')
    mcp.AddTool("self.sd",
        "SD card operations. 'action': info, list ('path' optional), stat, mkdir, delete, "
        "rename ('path'=src 'to'=dst), read, write ('content', 'append'). 'path' relative to root.",
        PropertyList({
            Property("action",  kPropertyTypeString,  ""),
            Property("path",    kPropertyTypeString,  ""),
            Property("to",      kPropertyTypeString,  ""),
            Property("content", kPropertyTypeString,  ""),
            Property("append",  kPropertyTypeBoolean, false)
        }),
        [sd_mounted](const PropertyList& props) -> ReturnValue {
            std::string action = props["action"].value<std::string>();

            if (!sd_mounted && action != "info")
                return "{\"error\":\"SD card not mounted\"}";

            // Sanitise a relative path: strip leading '/', reject '..'
            auto clean = [](std::string p) -> std::string {
                while (!p.empty() && p[0] == '/') p.erase(p.begin());
                return p;
            };
            auto valid = [](const std::string& p, bool allow_empty = false) -> bool {
                if (!allow_empty && p.empty()) return false;
                return p.find("..") == std::string::npos;
            };

            // ── info ─────────────────────────────────────────────────────────
            if (action == "info") {
                if (!sd_mounted) return "{\"error\":\"SD card not mounted\"}";
                uint64_t total = 0, free_b = 0;
                if (esp_vfs_fat_info("/sdcard", &total, &free_b) != ESP_OK)
                    return "{\"error\":\"esp_vfs_fat_info failed\"}";
                uint64_t used = total - free_b;
                return "{\"total_bytes\":" + std::to_string(total) +
                       ",\"used_bytes\":"  + std::to_string(used)  +
                       ",\"free_bytes\":"  + std::to_string(free_b) +
                       ",\"total\":\""     + sd_human_size(total)  + "\""  +
                       ",\"used\":\""      + sd_human_size(used)   + "\""  +
                       ",\"free\":\""      + sd_human_size(free_b) + "\""  +
                       ",\"used_pct\":"    + std::to_string((int)(100.0 * used / total)) + "}";
            }

            // ── list ─────────────────────────────────────────────────────────
            if (action == "list") {
                std::string rel = clean(props["path"].value<std::string>());
                if (!valid(rel, /*allow_empty=*/true)) return "{\"error\":\"Invalid path\"}";
                std::string dir_path = rel.empty() ? "/sdcard" : "/sdcard/" + rel;
                DIR* dir = opendir(dir_path.c_str());
                if (!dir) return "{\"error\":\"Directory not found\"}";
                std::string json = "[";
                bool first = true;
                struct dirent* entry;
                while ((entry = readdir(dir)) != nullptr) {
                    if (entry->d_name[0] == '.') continue;
                    char full[560];
                    snprintf(full, sizeof(full), "%s/%s", dir_path.c_str(), entry->d_name);
                    struct stat st = {};
                    stat(full, &st);
                    bool is_dir = S_ISDIR(st.st_mode);
                    if (!first) json += ",";
                    json += "{\"name\":\"" + std::string(entry->d_name) + "\","
                            "\"size\":" + std::to_string(is_dir ? 0 : (long)st.st_size) + ","
                            "\"is_dir\":" + (is_dir ? "true" : "false") + ","
                            "\"modified\":\"" + sd_fmt_time(st.st_mtime) + "\"}";
                    first = false;
                }
                closedir(dir);
                json += "]";
                return json;
            }

            // ── stat ─────────────────────────────────────────────────────────
            if (action == "stat") {
                std::string rel = clean(props["path"].value<std::string>());
                if (!valid(rel)) return "{\"error\":\"Invalid path\"}";
                std::string full_path = "/sdcard/" + rel;
                struct stat st = {};
                if (stat(full_path.c_str(), &st) != 0) return "{\"error\":\"Path not found\"}";
                bool is_dir = S_ISDIR(st.st_mode);
                long size = is_dir ? 0 : (long)st.st_size;
                auto slash = rel.rfind('/');
                std::string name = (slash == std::string::npos) ? rel : rel.substr(slash + 1);
                return "{\"name\":\"" + name + "\",\"path\":\"" + rel + "\","
                       "\"size\":" + std::to_string(size) + ",\"size_human\":\"" + sd_human_size((uint64_t)size) + "\","
                       "\"is_dir\":" + (is_dir ? "true" : "false") + ","
                       "\"modified\":\"" + sd_fmt_time(st.st_mtime) + "\"}";
            }

            // ── mkdir ────────────────────────────────────────────────────────
            if (action == "mkdir") {
                std::string rel = clean(props["path"].value<std::string>());
                if (!valid(rel)) return "{\"error\":\"Invalid path\"}";
                std::string current = "/sdcard";
                std::string remaining = rel;
                while (!remaining.empty()) {
                    auto slash = remaining.find('/');
                    std::string seg = (slash == std::string::npos) ? remaining : remaining.substr(0, slash);
                    remaining = (slash == std::string::npos) ? "" : remaining.substr(slash + 1);
                    if (seg.empty()) continue;
                    current += "/" + seg;
                    struct stat st = {};
                    if (stat(current.c_str(), &st) == 0) {
                        if (!S_ISDIR(st.st_mode))
                            return "{\"error\":\"Path component is a file: " + current + "\"}";
                    } else {
                        if (mkdir(current.c_str(), 0775) != 0)
                            return "{\"error\":\"mkdir failed for: " + current + "\"}";
                    }
                }
                return "{\"success\":true,\"path\":\"" + rel + "\"}";
            }

            // ── delete ───────────────────────────────────────────────────────
            if (action == "delete") {
                std::string rel = clean(props["path"].value<std::string>());
                if (!valid(rel)) return "{\"error\":\"Invalid path\"}";
                std::string full_path = "/sdcard/" + rel;
                struct stat st = {};
                if (stat(full_path.c_str(), &st) != 0) return "{\"error\":\"Path not found\"}";
                if (S_ISDIR(st.st_mode)) {
                    DIR* d = opendir(full_path.c_str());
                    bool empty = true;
                    if (d) {
                        struct dirent* e;
                        while ((e = readdir(d)) != nullptr)
                            if (e->d_name[0] != '.') { empty = false; break; }
                        closedir(d);
                    }
                    if (!empty) return "{\"error\":\"Directory is not empty\"}";
                    if (rmdir(full_path.c_str()) != 0) return "{\"error\":\"rmdir failed\"}";
                } else {
                    if (remove(full_path.c_str()) != 0) return "{\"error\":\"Delete failed\"}";
                }
                return "{\"success\":true,\"path\":\"" + rel + "\"}";
            }

            // ── rename ───────────────────────────────────────────────────────
            if (action == "rename") {
                std::string from = clean(props["path"].value<std::string>());
                std::string to   = clean(props["to"].value<std::string>());
                if (!valid(from) || !valid(to)) return "{\"error\":\"Invalid path\"}";
                if (rename(("/sdcard/" + from).c_str(), ("/sdcard/" + to).c_str()) != 0)
                    return "{\"error\":\"Rename failed — check paths and that destination parent exists\"}";
                return "{\"success\":true,\"from\":\"" + from + "\",\"to\":\"" + to + "\"}";
            }

            // ── read ─────────────────────────────────────────────────────────
            if (action == "read") {
                std::string rel = clean(props["path"].value<std::string>());
                if (!valid(rel)) return "{\"error\":\"Invalid path\"}";
                FILE* f = fopen(("/sdcard/" + rel).c_str(), "r");
                if (!f) return "{\"error\":\"File not found\"}";
                constexpr size_t kMaxBytes = 4096;
                std::string content(kMaxBytes, '\0');
                size_t n = fread(&content[0], 1, kMaxBytes, f);
                fclose(f);
                content.resize(n);
                std::string escaped;
                escaped.reserve(n + 32);
                for (char c : content) {
                    if      (c == '"')  escaped += "\\\"";
                    else if (c == '\\') escaped += "\\\\";
                    else if (c == '\n') escaped += "\\n";
                    else if (c == '\r') escaped += "\\r";
                    else if (c == '\t') escaped += "\\t";
                    else                escaped += c;
                }
                return "{\"content\":\"" + escaped + "\","
                       "\"truncated\":" + (n == kMaxBytes ? "true" : "false") + ","
                       "\"bytes\":" + std::to_string(n) + "}";
            }

            // ── write ────────────────────────────────────────────────────────
            if (action == "write") {
                std::string rel = clean(props["path"].value<std::string>());
                if (!valid(rel)) return "{\"error\":\"Invalid path\"}";
                auto dot = rel.rfind('.');
                if (dot != std::string::npos) {
                    std::string ext = rel.substr(dot);
                    for (char& c : ext) c = (char)tolower((unsigned char)c);
                    const char* allowed[] = {
                        ".txt", ".text", ".md", ".markdown", ".log",
                        ".html", ".htm", ".css",
                        ".js", ".mjs", ".ts", ".jsx", ".tsx",
                        ".json", ".xml", ".svg", ".csv", ".yaml", ".yml",
                        ".ini", ".cfg", ".conf", ".toml", ".env",
                        ".sh", ".bash", ".py", ".rb", ".php",
                        ".c", ".h", ".cpp", ".cc", ".hh",
                        ".sql", ".graphql",
                        nullptr
                    };
                    bool ok = false;
                    for (int i = 0; allowed[i]; ++i) if (ext == allowed[i]) { ok = true; break; }
                    if (!ok) return "{\"error\":\"Extension not allowed — only text/* types permitted\"}";
                }
                bool append = props["append"].value<bool>();
                FILE* f = fopen(("/sdcard/" + rel).c_str(), append ? "a" : "w");
                if (!f) return "{\"error\":\"Cannot open file for writing\"}";
                std::string content = props["content"].value<std::string>();
                size_t written = fwrite(content.c_str(), 1, content.size(), f);
                fclose(f);
                return "{\"success\":true,\"bytes\":" + std::to_string(written) + ","
                       "\"append\":" + (append ? "true" : "false") + "}";
            }

            return "{\"error\":\"Unknown action — use info, list, stat, mkdir, delete, rename, read, write\"}";
        });

    ESP_LOGI(TAG, "SD tool registered (mounted=%d)", sd_mounted);
}
