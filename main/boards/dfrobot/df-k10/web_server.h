#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <string>

#include "esp_video.h"
#include "led/circular_strip.h"
#include "audio_codec.h"

// Local-network dashboard: a small HTTP+WebSocket server on the device
// itself, serving a browser UI that controls the LED/volume/camera/wifi
// directly and can invoke any registered MCP tool (via McpServer::CallTool).
class WebServer {
public:
    WebServer(EspVideo* camera, CircularStrip* led, AudioCodec* codec, bool sd_mounted);
    ~WebServer();

    void Start();
    void Stop();

private:
    EspVideo* camera_;
    CircularStrip* led_;
    AudioCodec* codec_;
    bool sd_mounted_;

    httpd_handle_t server_ = nullptr;

    // WebSocket push task — single client, pushes orientation + status.
    TaskHandle_t ws_task_ = nullptr;
    volatile int ws_client_fd_ = -1;
    volatile bool ws_running_ = false;

    // Cached JPEG snapshot, served by GET /snapshot.jpg. Only ever refreshed
    // by an explicit POST /api/snapshot.
    std::string cached_jpeg_;
    SemaphoreHandle_t cam_mutex_ = nullptr;

    static esp_err_t HandleRoot(httpd_req_t* req);
    static esp_err_t HandleSnapshot(httpd_req_t* req);
    static esp_err_t HandleApiSnapshot(httpd_req_t* req);
    static esp_err_t HandleApiStatus(httpd_req_t* req);
    static esp_err_t HandleApiLed(httpd_req_t* req);
    static esp_err_t HandleApiVolume(httpd_req_t* req);
    static esp_err_t HandleApiOrient(httpd_req_t* req);
    static esp_err_t HandleApiWifi(httpd_req_t* req);
    static esp_err_t HandleApiReboot(httpd_req_t* req);
    static esp_err_t HandleApiTheme(httpd_req_t* req);
    static esp_err_t HandleApiFs(httpd_req_t* req);
    static esp_err_t HandleApiFileServe(httpd_req_t* req);
    static esp_err_t HandleApiTools(httpd_req_t* req);
    static esp_err_t HandleApiToolCall(httpd_req_t* req);
    static esp_err_t HandleWs(httpd_req_t* req);
    static void WsPushTask(void* arg);
};

#endif  // WEB_SERVER_H
