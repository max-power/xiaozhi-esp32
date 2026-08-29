#include "sd_control.h"
#include "mcp_server.h"

#include <cJSON.h>
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

namespace {

std::string ToJsonString(cJSON* json) {
    char* str = cJSON_PrintUnformatted(json);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(json);
    return result;
}

std::string JsonError(const std::string& message) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", message.c_str());
    return ToJsonString(json);
}

std::string FormatTime(time_t t) {
    if (t == 0) return "unknown";
    char buf[24];
    struct tm tm_info;
    gmtime_r(&t, &tm_info);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    return buf;
}

std::string HumanSize(uint64_t b) {
    char buf[32];
    if      (b >= 1073741824ULL) snprintf(buf, sizeof(buf), "%.1f GB", b / 1073741824.0);
    else if (b >= 1048576ULL)    snprintf(buf, sizeof(buf), "%.1f MB", b / 1048576.0);
    else if (b >= 1024ULL)       snprintf(buf, sizeof(buf), "%.1f KB", b / 1024.0);
    else                         snprintf(buf, sizeof(buf), "%llu B",  (unsigned long long)b);
    return buf;
}

// Strip leading '/' from a path; the caller still needs to check Valid().
std::string CleanPath(std::string p) {
    while (!p.empty() && p[0] == '/') p.erase(p.begin());
    return p;
}

// Reject '..' (no parent-directory escapes) and, unless allowed, empty paths.
bool ValidPath(const std::string& p, bool allow_empty = false) {
    if (!allow_empty && p.empty()) return false;
    return p.find("..") == std::string::npos;
}

// ─── Actions ─────────────────────────────────────────────────────────────────

std::string SdInfo() {
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info("/sdcard", &total, &free_b) != ESP_OK) {
        return JsonError("esp_vfs_fat_info failed");
    }
    uint64_t used = total - free_b;
    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "total_bytes", (double)total);
    cJSON_AddNumberToObject(json, "used_bytes", (double)used);
    cJSON_AddNumberToObject(json, "free_bytes", (double)free_b);
    cJSON_AddStringToObject(json, "total", HumanSize(total).c_str());
    cJSON_AddStringToObject(json, "used", HumanSize(used).c_str());
    cJSON_AddStringToObject(json, "free", HumanSize(free_b).c_str());
    cJSON_AddNumberToObject(json, "used_pct", total ? (100.0 * used / total) : 0);
    return ToJsonString(json);
}

std::string SdList(const std::string& raw_path) {
    std::string rel = CleanPath(raw_path);
    if (!ValidPath(rel, /*allow_empty=*/true)) return JsonError("Invalid path");
    std::string dir_path = rel.empty() ? "/sdcard" : "/sdcard/" + rel;
    DIR* dir = opendir(dir_path.c_str());
    if (!dir) return JsonError("Directory not found");

    cJSON* json = cJSON_CreateArray();
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        char full[560];
        snprintf(full, sizeof(full), "%s/%s", dir_path.c_str(), entry->d_name);
        struct stat st = {};
        stat(full, &st);
        bool is_dir = S_ISDIR(st.st_mode);

        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", entry->d_name);
        cJSON_AddNumberToObject(item, "size", is_dir ? 0 : (double)st.st_size);
        cJSON_AddBoolToObject(item, "is_dir", is_dir);
        cJSON_AddStringToObject(item, "modified", FormatTime(st.st_mtime).c_str());
        cJSON_AddItemToArray(json, item);
    }
    closedir(dir);
    return ToJsonString(json);
}

std::string SdStat(const std::string& raw_path) {
    std::string rel = CleanPath(raw_path);
    if (!ValidPath(rel)) return JsonError("Invalid path");
    std::string full_path = "/sdcard/" + rel;
    struct stat st = {};
    if (stat(full_path.c_str(), &st) != 0) return JsonError("Path not found");
    bool is_dir = S_ISDIR(st.st_mode);
    uint64_t size = is_dir ? 0 : (uint64_t)st.st_size;
    auto slash = rel.rfind('/');
    std::string name = (slash == std::string::npos) ? rel : rel.substr(slash + 1);

    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "name", name.c_str());
    cJSON_AddStringToObject(json, "path", rel.c_str());
    cJSON_AddNumberToObject(json, "size", (double)size);
    cJSON_AddStringToObject(json, "size_human", HumanSize(size).c_str());
    cJSON_AddBoolToObject(json, "is_dir", is_dir);
    cJSON_AddStringToObject(json, "modified", FormatTime(st.st_mtime).c_str());
    return ToJsonString(json);
}

std::string SdMkdir(const std::string& raw_path) {
    std::string rel = CleanPath(raw_path);
    if (!ValidPath(rel)) return JsonError("Invalid path");
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
            if (!S_ISDIR(st.st_mode)) return JsonError("Path component is a file: " + current);
        } else {
            if (mkdir(current.c_str(), 0775) != 0) return JsonError("mkdir failed for: " + current);
        }
    }
    cJSON* json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    cJSON_AddStringToObject(json, "path", rel.c_str());
    return ToJsonString(json);
}

std::string SdDelete(const std::string& raw_path) {
    std::string rel = CleanPath(raw_path);
    if (!ValidPath(rel)) return JsonError("Invalid path");
    std::string full_path = "/sdcard/" + rel;
    struct stat st = {};
    if (stat(full_path.c_str(), &st) != 0) return JsonError("Path not found");
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(full_path.c_str());
        bool empty = true;
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr)
                if (e->d_name[0] != '.') { empty = false; break; }
            closedir(d);
        }
        if (!empty) return JsonError("Directory is not empty");
        if (rmdir(full_path.c_str()) != 0) return JsonError("rmdir failed");
    } else {
        if (remove(full_path.c_str()) != 0) return JsonError("Delete failed");
    }
    cJSON* json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    cJSON_AddStringToObject(json, "path", rel.c_str());
    return ToJsonString(json);
}

std::string SdRename(const std::string& raw_from, const std::string& raw_to) {
    std::string from = CleanPath(raw_from);
    std::string to   = CleanPath(raw_to);
    if (!ValidPath(from) || !ValidPath(to)) return JsonError("Invalid path");
    if (rename(("/sdcard/" + from).c_str(), ("/sdcard/" + to).c_str()) != 0) {
        return JsonError("Rename failed — check paths and that destination parent exists");
    }
    cJSON* json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    cJSON_AddStringToObject(json, "from", from.c_str());
    cJSON_AddStringToObject(json, "to", to.c_str());
    return ToJsonString(json);
}

std::string SdRead(const std::string& raw_path) {
    std::string rel = CleanPath(raw_path);
    if (!ValidPath(rel)) return JsonError("Invalid path");
    FILE* f = fopen(("/sdcard/" + rel).c_str(), "r");
    if (!f) return JsonError("File not found");
    constexpr size_t kMaxBytes = 4096;
    std::string content(kMaxBytes, '\0');
    size_t n = fread(&content[0], 1, kMaxBytes, f);
    fclose(f);
    content.resize(n);

    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "content", content.c_str());
    cJSON_AddBoolToObject(json, "truncated", n == kMaxBytes);
    cJSON_AddNumberToObject(json, "bytes", (double)n);
    return ToJsonString(json);
}

bool IsAllowedTextExtension(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return true;  // no extension: allow
    std::string ext = path.substr(dot);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    static const char* kAllowed[] = {
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
    for (int i = 0; kAllowed[i]; ++i) {
        if (ext == kAllowed[i]) return true;
    }
    return false;
}

std::string SdWrite(const std::string& raw_path, const std::string& content, bool append) {
    std::string rel = CleanPath(raw_path);
    if (!ValidPath(rel)) return JsonError("Invalid path");
    if (!IsAllowedTextExtension(rel)) return JsonError("Extension not allowed — only text/* types permitted");
    FILE* f = fopen(("/sdcard/" + rel).c_str(), append ? "a" : "w");
    if (!f) return JsonError("Cannot open file for writing");
    size_t written = fwrite(content.c_str(), 1, content.size(), f);
    fclose(f);

    cJSON* json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    cJSON_AddNumberToObject(json, "bytes", (double)written);
    cJSON_AddBoolToObject(json, "append", append);
    return ToJsonString(json);
}

}  // namespace

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

            if (!sd_mounted && action != "info") {
                return JsonError("SD card not mounted");
            }

            if (action == "info") {
                if (!sd_mounted) return JsonError("SD card not mounted");
                return SdInfo();
            }
            if (action == "list") {
                return SdList(props["path"].value<std::string>());
            }
            if (action == "stat") {
                return SdStat(props["path"].value<std::string>());
            }
            if (action == "mkdir") {
                return SdMkdir(props["path"].value<std::string>());
            }
            if (action == "delete") {
                return SdDelete(props["path"].value<std::string>());
            }
            if (action == "rename") {
                return SdRename(props["path"].value<std::string>(), props["to"].value<std::string>());
            }
            if (action == "read") {
                return SdRead(props["path"].value<std::string>());
            }
            if (action == "write") {
                return SdWrite(props["path"].value<std::string>(),
                                props["content"].value<std::string>(),
                                props["append"].value<bool>());
            }
            return JsonError("Unknown action — use info, list, stat, mkdir, delete, rename, read, write");
        });

    ESP_LOGI(TAG, "SD tool registered (mounted=%d)", sd_mounted);
}
