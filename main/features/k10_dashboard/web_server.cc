#include "web_server.h"
#include "sensor_control.h"
#include "mcp_server.h"
#include "application.h"
#include "board.h"
#include "system_info.h"
#include "display.h"
#include "display/lvgl_display/lvgl_theme.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <mdns.h>
#include <lwip/apps/netbiosns.h>
#include <cJSON.h>
#include <ssid_manager.h>

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#define TAG "WebServer"
#define HOSTNAME CONFIG_XIAOZHI_MDNS_HOSTNAME

// HTML embedded at build time via CMake's EMBED_FILES (main/CMakeLists.txt).
extern const uint8_t web_dashboard_html_start[] asm("_binary_web_dashboard_html_start");
extern const uint8_t web_dashboard_html_end[] asm("_binary_web_dashboard_html_end");

WebServer::WebServer(EspVideo* camera, CircularStrip* led, AudioCodec* codec, bool sd_mounted)
    : camera_(camera), led_(led), codec_(codec), sd_mounted_(sd_mounted) {
    cam_mutex_ = xSemaphoreCreateMutex();
}

WebServer::~WebServer() {
    Stop();
    if (cam_mutex_) vSemaphoreDelete(cam_mutex_);
}

// ─── Start / Stop ──────────────────────────────────────────────────────────

void WebServer::Start() {
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set("DF-K10 Dashboard"));
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    netbiosns_init();
    netbiosns_set_name(HOSTNAME);
    ESP_LOGI(TAG, "mDNS: http://" HOSTNAME ".local");

    // ECONNRESET (recv err 104) is normal: browsers close keep-alive
    // connections between requests. Suppress so it doesn't flood the log.
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 24;

    if (httpd_start(&server_, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    auto reg = [&](const char* path, httpd_method_t meth, esp_err_t (*h)(httpd_req_t*)) {
        httpd_uri_t u;
        u.uri = path;
        u.method = meth;
        u.handler = h;
        u.user_ctx = this;
        httpd_register_uri_handler(server_, &u);
    };

    reg("/snapshot.jpg", HTTP_GET, HandleSnapshot);
    reg("/api/snapshot", HTTP_POST, HandleApiSnapshot);
    reg("/api/status", HTTP_GET, HandleApiStatus);
    reg("/api/led", HTTP_POST, HandleApiLed);
    reg("/api/volume", HTTP_POST, HandleApiVolume);
    reg("/api/orient", HTTP_GET, HandleApiOrient);
    reg("/api/wifi", HTTP_GET, HandleApiWifi);
    reg("/api/wifi", HTTP_POST, HandleApiWifi);
    reg("/api/wifi", HTTP_DELETE, HandleApiWifi);
    reg("/api/reboot", HTTP_POST, HandleApiReboot);
    reg("/api/theme", HTTP_GET, HandleApiTheme);
    reg("/api/theme", HTTP_POST, HandleApiTheme);
    reg("/api/fs", HTTP_GET, HandleApiFs);
    reg("/api/fs/rename", HTTP_POST, HandleApiFsRename);
    reg("/api/files/*", HTTP_GET, HandleApiFileServe);
    reg("/api/files/*", HTTP_PUT, HandleApiFileServe);
    reg("/api/files/*", HTTP_DELETE, HandleApiFileServe);
    reg("/api/tools", HTTP_GET, HandleApiTools);
    reg("/api/tool/*", HTTP_POST, HandleApiToolCall);

    // WebSocket — must be registered before the wildcard root handler.
    httpd_uri_t ws_uri = {};
    ws_uri.uri = "/ws";
    ws_uri.method = HTTP_GET;
    ws_uri.handler = HandleWs;
    ws_uri.user_ctx = this;
    ws_uri.is_websocket = true;
    httpd_register_uri_handler(server_, &ws_uri);

    reg("/*", HTTP_GET, HandleRoot);

    ws_running_ = true;
    xTaskCreate(WsPushTask, "ws_push", 5120, this, 5, &ws_task_);

    ESP_LOGI(TAG, "HTTP server started");
}

void WebServer::Stop() {
    ws_running_ = false;
    if (ws_task_) {
        vTaskDelay(pdMS_TO_TICKS(400));  // give the task one cycle to notice
        ws_task_ = nullptr;
    }
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}

// ─── Helpers ────────────────────────────────────────────────────────────────

namespace {

std::string ReadBody(httpd_req_t* req) {
    std::string body(req->content_len, '\0');
    int received = 0;
    while (received < (int)req->content_len) {
        int n = httpd_req_recv(req, &body[received], req->content_len - received);
        if (n <= 0) break;
        received += n;
    }
    body.resize(received);
    return body;
}

void SendJson(httpd_req_t* req, const std::string& json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json.c_str(), (ssize_t)json.size());
}

void JsonOk(httpd_req_t* req) { SendJson(req, "{\"ok\":true}"); }

void JsonErr(httpd_req_t* req, const std::string& msg) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", msg.c_str());
    char* s = cJSON_PrintUnformatted(root);
    SendJson(req, s);
    cJSON_free(s);
    cJSON_Delete(root);
}

// Sanitise a relative path: strip leading '/', reject '..'.
bool CleanRelPath(std::string& path) {
    while (!path.empty() && path[0] == '/') path.erase(path.begin());
    return path.find("..") == std::string::npos;
}

// Fields shared by GET /api/status and the WebSocket "status" push.
void AppendStatusFields(cJSON* root, AudioCodec* codec) {
    wifi_ap_record_t ap = {};
    esp_wifi_sta_get_ap_info(&ap);
    esp_netif_ip_info_t ip_info = {};
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) esp_netif_get_ip_info(netif, &ip_info);
    char ip_str[16] = {};
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));

    cJSON_AddStringToObject(root, "ssid", (const char*)ap.ssid);
    cJSON_AddNumberToObject(root, "rssi", ap.rssi);
    cJSON_AddStringToObject(root, "ip", ip_str);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000ULL));
    cJSON_AddNumberToObject(root, "free_heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "total_heap", heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "total_psram", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "volume", codec ? codec->output_volume() : -1);

    auto& board = Board::GetInstance();
    int bat_level = 0;
    bool bat_charging = false, bat_discharging = false;
    if (board.GetBatteryLevel(bat_level, bat_charging, bat_discharging)) {
        cJSON* bat = cJSON_AddObjectToObject(root, "battery");
        cJSON_AddNumberToObject(bat, "level", bat_level);
        cJSON_AddBoolToObject(bat, "charging", bat_charging);
    }
    if (auto backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(root, "brightness", backlight->brightness());
    }
    if (auto display = board.GetDisplay()) {
        if (auto theme = display->GetTheme()) {
            cJSON_AddStringToObject(root, "theme", theme->name().c_str());
        }
    }
    float chip_temp = 0.0f;
    if (board.GetTemperature(chip_temp)) {
        cJSON_AddNumberToObject(root, "chip_temp", chip_temp);
    }

    float t = 0, h = 0, lux = 0, x = 0, y = 0, z = 0;
    bool temp_ok = ReadSensorTemperature(t, h);
    bool light_ok = ReadSensorLight(lux);
    bool accel_ok = ReadSensorAccel(x, y, z);
    cJSON* s = cJSON_AddObjectToObject(root, "sensors");
    cJSON_AddBoolToObject(s, "valid", temp_ok || light_ok || accel_ok);
    if (temp_ok) {
        cJSON_AddNumberToObject(s, "temperature", t);
        cJSON_AddNumberToObject(s, "humidity", h);
    }
    if (light_ok) cJSON_AddNumberToObject(s, "lux", lux);
    if (accel_ok) {
        cJSON_AddNumberToObject(s, "accel_x", x);
        cJSON_AddNumberToObject(s, "accel_y", y);
        cJSON_AddNumberToObject(s, "accel_z", z);
    }

    auto app_desc = esp_app_get_description();
    esp_chip_info_t chip_info = {};
    esp_chip_info(&chip_info);
    cJSON* brd = cJSON_AddObjectToObject(root, "board");
    cJSON_AddStringToObject(brd, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(brd, "chip", SystemInfo::GetChipModelName().c_str());
    cJSON_AddNumberToObject(brd, "cores", chip_info.cores);
    cJSON_AddNumberToObject(brd, "revision", chip_info.revision);
    cJSON_AddNumberToObject(brd, "flash_size", (double)SystemInfo::GetFlashSize());
    cJSON_AddStringToObject(brd, "idf_ver", app_desc->idf_ver);
    cJSON_AddStringToObject(brd, "app_ver", app_desc->version);
    cJSON_AddStringToObject(brd, "compiled", (std::string(app_desc->date) + " " + app_desc->time).c_str());
}

}  // namespace

// ─── Root / Camera ──────────────────────────────────────────────────────────

esp_err_t WebServer::HandleRoot(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char*)web_dashboard_html_start,
                     web_dashboard_html_end - web_dashboard_html_start);
    return ESP_OK;
}

esp_err_t WebServer::HandleSnapshot(httpd_req_t* req) {
    auto* self = static_cast<WebServer*>(req->user_ctx);
    xSemaphoreTake(self->cam_mutex_, portMAX_DELAY);
    if (self->cached_jpeg_.empty()) {
        xSemaphoreGive(self->cam_mutex_);
        JsonErr(req, "No snapshot yet");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t ret = httpd_resp_send(req, self->cached_jpeg_.data(), self->cached_jpeg_.size());
    xSemaphoreGive(self->cam_mutex_);
    return ret;
}

esp_err_t WebServer::HandleApiSnapshot(httpd_req_t* req) {
    auto* self = static_cast<WebServer*>(req->user_ctx);
    if (!self->camera_) {
        JsonErr(req, "No camera");
        return ESP_OK;
    }
    xSemaphoreTake(self->cam_mutex_, portMAX_DELAY);
    std::string jpeg;
    bool ok = self->camera_->CaptureJpeg(jpeg);
    if (ok) self->cached_jpeg_ = std::move(jpeg);
    xSemaphoreGive(self->cam_mutex_);
    if (ok || !self->cached_jpeg_.empty()) JsonOk(req);
    else JsonErr(req, "Capture failed");
    return ESP_OK;
}

// ─── Status ─────────────────────────────────────────────────────────────────

esp_err_t WebServer::HandleApiStatus(httpd_req_t* req) {
    auto* self = static_cast<WebServer*>(req->user_ctx);
    cJSON* root = cJSON_CreateObject();
    AppendStatusFields(root, self->codec_);
    char* js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    SendJson(req, js);
    cJSON_free(js);
    return ESP_OK;
}

// ─── LED / Volume ───────────────────────────────────────────────────────────

esp_err_t WebServer::HandleApiLed(httpd_req_t* req) {
    auto* self = static_cast<WebServer*>(req->user_ctx);
    std::string body = ReadBody(req);
    cJSON* j = cJSON_Parse(body.c_str());
    if (!j) {
        JsonErr(req, "Invalid JSON");
        return ESP_OK;
    }

    cJSON* action_j = cJSON_GetObjectItem(j, "action");
    std::string action = (action_j && action_j->valuestring) ? action_j->valuestring : "set";

    if (action == "off") {
        cJSON_Delete(j);
        if (self->led_) self->led_->SetAllColor({0, 0, 0});
        JsonOk(req);
        return ESP_OK;
    }

    cJSON* r_j = cJSON_GetObjectItem(j, "r");
    cJSON* g_j = cJSON_GetObjectItem(j, "g");
    cJSON* b_j = cJSON_GetObjectItem(j, "b");
    cJSON* br_j = cJSON_GetObjectItem(j, "brightness");
    int r = r_j ? (int)r_j->valuedouble : 255;
    int g = g_j ? (int)g_j->valuedouble : 255;
    int b_val = b_j ? (int)b_j->valuedouble : 255;
    int bright = br_j ? (int)br_j->valuedouble : 128;
    cJSON_Delete(j);

    if (!self->led_) {
        JsonOk(req);
        return ESP_OK;
    }

    float sc = bright / 255.0f;
    StripColor color = {(uint8_t)(r * sc), (uint8_t)(g * sc), (uint8_t)(b_val * sc)};

    if (action == "set") self->led_->SetAllColor(color);
    else if (action == "blink") self->led_->Blink(color, 500);
    else if (action == "pulse") self->led_->Breathe({0, 0, 0}, color, 10);
    else if (action == "scroll") self->led_->Scroll({0, 0, 0}, color, 1, 100);

    JsonOk(req);
    return ESP_OK;
}

esp_err_t WebServer::HandleApiVolume(httpd_req_t* req) {
    auto* self = static_cast<WebServer*>(req->user_ctx);
    std::string body = ReadBody(req);
    cJSON* j = cJSON_Parse(body.c_str());
    if (!j) {
        JsonErr(req, "Invalid JSON");
        return ESP_OK;
    }
    cJSON* vol_j = cJSON_GetObjectItem(j, "volume");
    if (!vol_j) {
        cJSON_Delete(j);
        JsonErr(req, "Missing volume");
        return ESP_OK;
    }
    int vol = std::max(0, std::min(100, (int)vol_j->valuedouble));
    cJSON_Delete(j);
    if (self->codec_) self->codec_->SetOutputVolume(vol);
    JsonOk(req);
    return ESP_OK;
}

esp_err_t WebServer::HandleApiOrient(httpd_req_t* req) {
    float x = 0, y = 0, z = 0;
    ReadSensorAccel(x, y, z);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "x", x);
    cJSON_AddNumberToObject(root, "y", y);
    cJSON_AddNumberToObject(root, "z", z);
    char* js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, js, strlen(js));
    cJSON_free(js);
    return ESP_OK;
}

// ─── WiFi ───────────────────────────────────────────────────────────────────

esp_err_t WebServer::HandleApiWifi(httpd_req_t* req) {
    auto& mgr = SsidManager::GetInstance();

    if (req->method == HTTP_GET) {
        wifi_ap_record_t ap = {};
        esp_wifi_sta_get_ap_info(&ap);
        std::string cur = (const char*)ap.ssid;

        cJSON* root = cJSON_CreateObject();
        cJSON* arr = cJSON_AddArrayToObject(root, "networks");
        for (const auto& item : mgr.GetSsidList()) {
            cJSON* n = cJSON_CreateObject();
            cJSON_AddStringToObject(n, "ssid", item.ssid.c_str());
            cJSON_AddBoolToObject(n, "current", item.ssid == cur);
            cJSON_AddItemToArray(arr, n);
        }
        char* js = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        SendJson(req, js);
        cJSON_free(js);
        return ESP_OK;
    }

    std::string body = ReadBody(req);
    cJSON* j = cJSON_Parse(body.c_str());
    if (!j) {
        JsonErr(req, "Invalid JSON");
        return ESP_OK;
    }

    if (req->method == HTTP_POST) {
        cJSON* s = cJSON_GetObjectItem(j, "ssid");
        cJSON* p = cJSON_GetObjectItem(j, "password");
        if (s && cJSON_IsString(s)) {
            mgr.AddSsid(s->valuestring, p && cJSON_IsString(p) ? p->valuestring : "");
            cJSON_Delete(j);
            JsonOk(req);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        } else {
            cJSON_Delete(j);
            JsonErr(req, "Missing ssid");
        }
    } else if (req->method == HTTP_DELETE) {
        cJSON* idx = cJSON_GetObjectItem(j, "index");
        if (idx) {
            mgr.RemoveSsid((int)idx->valuedouble);
            JsonOk(req);
        } else {
            JsonErr(req, "Missing index");
        }
        cJSON_Delete(j);
    }
    return ESP_OK;
}

// ─── Reboot ─────────────────────────────────────────────────────────────────

esp_err_t WebServer::HandleApiReboot(httpd_req_t* req) {
    JsonOk(req);
    vTaskDelay(pdMS_TO_TICKS(300));  // let the HTTP response flush
    Application::GetInstance().Reboot();
    return ESP_OK;  // never reached
}

// ─── Theme (light/dark only — see camera.h-adjacent design notes) ──────────

esp_err_t WebServer::HandleApiTheme(httpd_req_t* req) {
    auto& mgr = LvglThemeManager::GetInstance();
    auto* display = Board::GetInstance().GetDisplay();

    if (req->method == HTTP_GET) {
        cJSON* root = cJSON_CreateObject();
        auto* cur = display ? display->GetTheme() : nullptr;
        cJSON_AddStringToObject(root, "current", cur ? cur->name().c_str() : "light");
        cJSON* names = cJSON_AddArrayToObject(root, "themes");
        cJSON_AddItemToArray(names, cJSON_CreateString("light"));
        cJSON_AddItemToArray(names, cJSON_CreateString("dark"));
        char* js = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        SendJson(req, js);
        cJSON_free(js);
        return ESP_OK;
    }

    // POST {theme:"light"|"dark"} — same mechanism as self.screen.set_theme.
    std::string body = ReadBody(req);
    cJSON* j = cJSON_Parse(body.c_str());
    if (!j) {
        JsonErr(req, "Invalid JSON");
        return ESP_OK;
    }
    cJSON* tn = cJSON_GetObjectItem(j, "theme");
    if (!tn || !cJSON_IsString(tn)) {
        cJSON_Delete(j);
        JsonErr(req, "Need {theme}");
        return ESP_OK;
    }
    auto* theme = mgr.GetTheme(tn->valuestring);
    cJSON_Delete(j);
    if (!theme || !display) {
        JsonErr(req, "Unknown theme");
        return ESP_OK;
    }
    display->SetTheme(theme);
    JsonOk(req);
    return ESP_OK;
}

// ─── SD file browser ────────────────────────────────────────────────────────

// GET /api/fs?path=<rel>  →  {mounted, path, entries:[{name,size,dir,mtime}]}
esp_err_t WebServer::HandleApiFs(httpd_req_t* req) {
    auto* self = static_cast<WebServer*>(req->user_ctx);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "mounted", self->sd_mounted_);

    char rel[256] = {};
    char query[300] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "path", rel, sizeof(rel));
    }
    std::string rel_str(rel);
    if (!CleanRelPath(rel_str)) {
        cJSON_Delete(root);
        JsonErr(req, "Bad path");
        return ESP_OK;
    }
    cJSON_AddStringToObject(root, "path", rel_str.c_str());
    cJSON* arr = cJSON_AddArrayToObject(root, "entries");

    if (self->sd_mounted_) {
        std::string dir_path = rel_str.empty() ? "/sdcard" : "/sdcard/" + rel_str;
        DIR* dir = opendir(dir_path.c_str());
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (entry->d_name[0] == '.') continue;
                std::string full = dir_path + "/" + entry->d_name;
                struct stat st = {};
                stat(full.c_str(), &st);
                bool is_dir = S_ISDIR(st.st_mode);
                cJSON* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "name", entry->d_name);
                cJSON_AddNumberToObject(item, "size", is_dir ? 0 : (double)st.st_size);
                cJSON_AddBoolToObject(item, "dir", is_dir);
                cJSON_AddNumberToObject(item, "mtime", (double)st.st_mtime);
                cJSON_AddItemToArray(arr, item);
            }
            closedir(dir);
        }
    }

    char* js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    SendJson(req, js);
    cJSON_free(js);
    return ESP_OK;
}

// GET    /api/files/<rel-path>  — serve file inline
// DELETE /api/files/<rel-path>  — delete file
esp_err_t WebServer::HandleApiFileServe(httpd_req_t* req) {
    auto* self = static_cast<WebServer*>(req->user_ctx);
    if (!self->sd_mounted_) {
        if (req->method == HTTP_DELETE) JsonErr(req, "SD not mounted");
        else httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD not mounted");
        return ESP_OK;
    }

    static const char kPrefix[] = "/api/files/";
    static const size_t kPrefixLen = sizeof(kPrefix) - 1;
    const char* src = req->uri + kPrefixLen;
    const char* qs = strchr(src, '?');
    std::string rel(src, qs ? (size_t)(qs - src) : strlen(src));

    if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos) {
        if (req->method == HTTP_DELETE) JsonErr(req, "Bad path");
        else httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_OK;
    }

    std::string path = "/sdcard/" + rel;

    if (req->method == HTTP_DELETE) {
        if (remove(path.c_str()) == 0) {
            ESP_LOGI(TAG, "Deleted %s", path.c_str());
            JsonOk(req);
        } else {
            JsonErr(req, "Delete failed");
        }
        return ESP_OK;
    }

    if (req->method == HTTP_PUT) {
        std::string body = ReadBody(req);
        FILE* wf = fopen(path.c_str(), "wb");
        if (!wf) {
            JsonErr(req, "Write failed");
            return ESP_OK;
        }
        size_t written = fwrite(body.data(), 1, body.size(), wf);
        fclose(wf);
        if (written != body.size()) {
            JsonErr(req, "Write incomplete");
            return ESP_OK;
        }
        ESP_LOGI(TAG, "Wrote %s (%u bytes)", path.c_str(), (unsigned)written);
        JsonOk(req);
        return ESP_OK;
    }

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }

    auto dot = rel.rfind('.');
    std::string ext = dot != std::string::npos ? rel.substr(dot) : "";
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    if (ext == ".jpg" || ext == ".jpeg") httpd_resp_set_type(req, "image/jpeg");
    else if (ext == ".bmp") httpd_resp_set_type(req, "image/bmp");
    else if (ext == ".png") httpd_resp_set_type(req, "image/png");
    else if (ext == ".txt" || ext == ".log" || ext == ".csv" || ext == ".md" || ext == ".yaml" || ext == ".yml")
        httpd_resp_set_type(req, "text/plain");
    else if (ext == ".json") httpd_resp_set_type(req, "application/json");
    else if (ext == ".html" || ext == ".htm") httpd_resp_set_type(req, "text/html");
    else httpd_resp_set_type(req, "application/octet-stream");

    auto slash = rel.rfind('/');
    std::string basename = slash != std::string::npos ? rel.substr(slash + 1) : rel;
    httpd_resp_set_hdr(req, "Content-Disposition", ("inline; filename=\"" + basename + "\"").c_str());
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

// POST /api/fs/rename  {"path": "<rel to item>", "name": "<new base name>"}
// Renames in place — the new name may not contain '/', so this can't be used
// to move a file into a different directory.
esp_err_t WebServer::HandleApiFsRename(httpd_req_t* req) {
    auto* self = static_cast<WebServer*>(req->user_ctx);
    if (!self->sd_mounted_) {
        JsonErr(req, "SD not mounted");
        return ESP_OK;
    }

    std::string body = ReadBody(req);
    cJSON* j = cJSON_Parse(body.c_str());
    if (!j) {
        JsonErr(req, "Invalid JSON");
        return ESP_OK;
    }
    cJSON* path_j = cJSON_GetObjectItem(j, "path");
    cJSON* name_j = cJSON_GetObjectItem(j, "name");
    if (!cJSON_IsString(path_j) || !cJSON_IsString(name_j)) {
        cJSON_Delete(j);
        JsonErr(req, "Missing path or name");
        return ESP_OK;
    }
    std::string rel(path_j->valuestring);
    std::string new_name(name_j->valuestring);
    cJSON_Delete(j);

    if (!CleanRelPath(rel) || rel.empty()) {
        JsonErr(req, "Bad path");
        return ESP_OK;
    }
    if (new_name.empty() || new_name.find('/') != std::string::npos || new_name.find("..") != std::string::npos) {
        JsonErr(req, "Bad name");
        return ESP_OK;
    }

    auto slash = rel.rfind('/');
    std::string dir = slash != std::string::npos ? rel.substr(0, slash + 1) : "";
    std::string old_path = "/sdcard/" + rel;
    std::string new_path = "/sdcard/" + dir + new_name;

    if (rename(old_path.c_str(), new_path.c_str()) == 0) {
        ESP_LOGI(TAG, "Renamed %s -> %s", old_path.c_str(), new_path.c_str());
        JsonOk(req);
    } else {
        JsonErr(req, "Rename failed");
    }
    return ESP_OK;
}

// ─── MCP tool passthrough ───────────────────────────────────────────────────

esp_err_t WebServer::HandleApiTools(httpd_req_t* req) {
    std::string json = McpServer::GetInstance().GetToolsJson(/*include_user_only=*/true);
    SendJson(req, json);
    return ESP_OK;
}

esp_err_t WebServer::HandleApiToolCall(httpd_req_t* req) {
    static const char kPrefix[] = "/api/tool/";
    static const size_t kPrefixLen = sizeof(kPrefix) - 1;
    const char* name_start = req->uri + kPrefixLen;
    const char* qs = strchr(name_start, '?');
    std::string tool_name(name_start, qs ? (size_t)(qs - name_start) : strlen(name_start));

    if (tool_name.empty()) {
        JsonErr(req, "Missing tool name in path");
        return ESP_OK;
    }

    std::string body = ReadBody(req);
    cJSON* args = body.empty() ? cJSON_CreateObject() : cJSON_Parse(body.c_str());
    if (!args) {
        JsonErr(req, "Invalid JSON body");
        return ESP_OK;
    }

    std::string result;
    try {
        result = McpServer::GetInstance().CallTool(tool_name, args);
    } catch (const std::exception& e) {
        cJSON_Delete(args);
        JsonErr(req, e.what());
        return ESP_OK;
    }
    cJSON_Delete(args);
    SendJson(req, result);
    return ESP_OK;
}

// ─── WebSocket ──────────────────────────────────────────────────────────────

esp_err_t WebServer::HandleWs(httpd_req_t* req) {
    auto* self = static_cast<WebServer*>(req->user_ctx);
    int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
        // Initial upgrade handshake — httpd handles the protocol switch, we
        // just record which socket fd this client is on.
        self->ws_client_fd_ = fd;
        ESP_LOGI(TAG, "WS client connected fd=%d", fd);
        return ESP_OK;
    }

    // If WsPushTask already cleared the fd (send failure), the connection is
    // already dead — skip recv_frame to avoid a spurious warning on a torn
    // down socket.
    if (fd != self->ws_client_fd_) {
        return ESP_OK;
    }

    httpd_ws_frame_t pkt = {};
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);  // header-only probe
    if (ret != ESP_OK || pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "WS client disconnected fd=%d", fd);
        self->ws_client_fd_ = -1;
    }
    return ESP_OK;
}

namespace {
bool WsSend(httpd_handle_t server, int fd, const char* text, size_t len) {
    httpd_ws_frame_t pkt = {};
    pkt.type = HTTPD_WS_TYPE_TEXT;
    pkt.payload = (uint8_t*)text;
    pkt.len = len;
    pkt.final = true;
    return httpd_ws_send_frame_async(server, fd, &pkt) == ESP_OK;
}
}  // namespace

void WebServer::WsPushTask(void* arg) {
    auto* self = static_cast<WebServer*>(arg);
    int64_t last_status_us = 0;
    char orient_buf[96];

    while (self->ws_running_) {
        vTaskDelay(pdMS_TO_TICKS(300));

        int fd = self->ws_client_fd_;
        if (fd < 0 || !self->server_) continue;

        // Orientation, every 300ms — cheap, no blocking I2C.
        float x = 0, y = 0, z = 0;
        ReadSensorAccel(x, y, z);
        int olen = snprintf(orient_buf, sizeof(orient_buf), "{\"type\":\"orient\",\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}", x,
                             y, z);
        if (!WsSend(self->server_, fd, orient_buf, olen)) {
            self->ws_client_fd_ = -1;
            continue;
        }

        // Status, every 8s — includes a blocking AHT20 read.
        int64_t now = esp_timer_get_time();
        if (now - last_status_us < 8000000LL) continue;
        last_status_us = now;

        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "status");
        AppendStatusFields(root, self->codec_);
        char* js = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!WsSend(self->server_, fd, js, strlen(js))) self->ws_client_fd_ = -1;
        cJSON_free(js);
    }
    vTaskDelete(nullptr);
}
