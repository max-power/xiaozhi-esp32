#include "mcp_remote_client.h"
#include "board.h"

#include <esp_log.h>
#include <cctype>
#include <set>

#define TAG "McpRemoteClient"

namespace {

constexpr const char* kRegistryUrl = "https://registry.modelcontextprotocol.io/v0/servers";

cJSON* MakeError(const std::string& message) {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "error", message.c_str());
    return err;
}

cJSON* WrapError(cJSON* error_detail) {
    cJSON* out = cJSON_CreateObject();
    cJSON_AddItemToObject(out, "error", error_detail);
    return out;
}

// Distills whatever error info a response carries. Follows the standard
// JSON-RPC {"error": {code, message}} shape when present, but falls back to
// wrapping the whole response when a server puts its error in some other
// shape — e.g. a flat {"error":"missing_api_key","message":"...",
// "register":{...}} body some servers attach to a 401 — so useful sibling
// fields (like self-registration instructions) don't get silently dropped.
// Returns nullptr if the response has no "error" field at all.
cJSON* ExtractResponseError(cJSON* response) {
    cJSON* error_field = cJSON_GetObjectItem(response, "error");
    if (!error_field) return nullptr;
    if (cJSON_IsObject(error_field)) {
        return WrapError(cJSON_Duplicate(error_field, true));
    }
    return WrapError(cJSON_Duplicate(response, true));
}

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// A Streamable-HTTP MCP response body is either a bare JSON object
// (Content-Type: application/json) or a single SSE event
// (Content-Type: text/event-stream, body shaped like
// "event: message\ndata: {...}\n\n"). Discovery only ever needs one
// request/response, never a long-lived stream, so just pull the JSON out of
// whichever shape came back.
cJSON* ParseJsonRpcBody(const std::string& body, const std::string& content_type) {
    if (content_type.find("event-stream") != std::string::npos) {
        size_t pos = body.find("data:");
        if (pos == std::string::npos) return nullptr;
        pos += 5;
        size_t end = body.find('\n', pos);
        std::string data = body.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        while (!data.empty() && data.front() == ' ') data.erase(data.begin());
        return cJSON_Parse(data.c_str());
    }
    return cJSON_Parse(body.c_str());
}

// One JSON-RPC request/response round trip. `session_id` is read from (and
// then fed back into) the Mcp-Session-Id header, per the MCP Streamable HTTP
// spec: the server issues one on the `initialize` response and expects it
// echoed on every request after that. Pass expect_response=false for
// notifications, which get no JSON-RPC result to parse.
cJSON* SendJsonRpc(const std::string& url, const std::string& auth_token, std::string& session_id,
                    cJSON* request, bool expect_response, int* out_status = nullptr) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Accept", "application/json, text/event-stream");
    if (!auth_token.empty()) {
        http->SetHeader("Authorization", "Bearer " + auth_token);
    }
    if (!session_id.empty()) {
        http->SetHeader("Mcp-Session-Id", session_id);
    }

    char* body_str = cJSON_PrintUnformatted(request);
    std::string body(body_str);
    cJSON_free(body_str);
    http->SetContent(std::move(body));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open connection to %s (error 0x%x)", url.c_str(), http->GetLastError());
        return nullptr;
    }

    int status = http->GetStatusCode();
    if (out_status) *out_status = status;
    std::string sid = http->GetResponseHeader("Mcp-Session-Id");
    if (!sid.empty()) session_id = sid;
    std::string content_type = http->GetResponseHeader("Content-Type");
    std::string resp_body = http->ReadAll();
    http->Close();

    if (status < 200 || status >= 300) {
        // Don't bail out here: plenty of servers (this one included) attach a
        // real JSON-RPC error object to the body even on a non-2xx status,
        // and that message ("...pass your API token as a Bearer token") is
        // far more useful to the caller — than a bare "request
        // failed". Keep going and let the body be parsed below; callers
        // already know to look for an "error" field in the result.
        ESP_LOGW(TAG, "MCP server returned HTTP %d: %s", status, resp_body.c_str());
    }
    if (resp_body.empty()) {
        // Many servers reply to a notification with an empty 202 Accepted
        // body — that's success, not a parse failure. For a request that
        // does expect a real result, an empty body is a genuine failure.
        return expect_response ? nullptr : cJSON_CreateObject();
    }
    cJSON* parsed = ParseJsonRpcBody(resp_body, content_type);
    if (!parsed) {
        ESP_LOGE(TAG, "Failed to parse MCP response (HTTP %d, content-type: %s): %s",
                 status, content_type.c_str(), resp_body.c_str());
    }
    return parsed;
}

// Does the `initialize` + `notifications/initialized` handshake every MCP
// session opens with. On success, `session_id` holds whatever the server
// wants echoed back on later requests (may be empty — not all servers issue
// one). On failure, returns an owned error object via `out_error` and false.
bool InitializeMcpSession(const std::string& url, const std::string& auth_token,
                           std::string& session_id, cJSON** out_error) {
    cJSON* init_req = cJSON_CreateObject();
    cJSON_AddStringToObject(init_req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(init_req, "id", 1);
    cJSON_AddStringToObject(init_req, "method", "initialize");
    cJSON* init_params = cJSON_AddObjectToObject(init_req, "params");
    cJSON_AddStringToObject(init_params, "protocolVersion", "2024-11-05");
    cJSON_AddObjectToObject(init_params, "capabilities");
    cJSON* client_info = cJSON_AddObjectToObject(init_params, "clientInfo");
    cJSON_AddStringToObject(client_info, "name", "xiaozhi-esp32");
    cJSON_AddStringToObject(client_info, "version", "1.0");

    cJSON* init_resp = SendJsonRpc(url, auth_token, session_id, init_req, true);
    cJSON_Delete(init_req);
    if (!init_resp) {
        *out_error = MakeError("Failed to connect to " + url + " (see device log for the HTTP status)");
        return false;
    }
    cJSON* init_error = ExtractResponseError(init_resp);
    if (init_error) {
        *out_error = init_error;
        cJSON_Delete(init_resp);
        return false;
    }
    cJSON_Delete(init_resp);

    // Required by spec: tell the server initialization is complete. This is
    // a notification (no "id"), so there's normally no result worth parsing
    // and a failure here wouldn't usually be fatal — except a 401/403 means
    // the whole session is unauthenticated, and pressing on to tools/list
    // anyway tends to fail in far less useful ways (some servers just hang
    // instead of rejecting quickly). Bail out here instead, with whatever
    // error body the server gave us.
    cJSON* initialized = cJSON_CreateObject();
    cJSON_AddStringToObject(initialized, "jsonrpc", "2.0");
    cJSON_AddStringToObject(initialized, "method", "notifications/initialized");
    int ack_status = 0;
    cJSON* ack = SendJsonRpc(url, auth_token, session_id, initialized, false, &ack_status);
    cJSON_Delete(initialized);
    if (ack_status == 401 || ack_status == 403) {
        cJSON* ack_error = ack ? ExtractResponseError(ack) : nullptr;
        *out_error = ack_error ? ack_error : MakeError("Server rejected the connection (HTTP " + std::to_string(ack_status) + ")");
        if (ack) cJSON_Delete(ack);
        return false;
    }
    if (ack) cJSON_Delete(ack);
    return true;
}

}  // namespace

cJSON* DiscoverMcpServers(const std::string& query) {
    if (query.empty()) {
        return MakeError("Missing query");
    }

    std::string url = std::string(kRegistryUrl) + "?limit=5&search=" + UrlEncode(query);
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    http->SetHeader("Accept", "application/json");
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to reach MCP registry (error 0x%x)", http->GetLastError());
        return MakeError("Failed to reach the MCP registry");
    }
    int status = http->GetStatusCode();
    std::string body = http->ReadAll();
    http->Close();
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "MCP registry returned HTTP %d: %s", status, body.c_str());
        return MakeError("MCP registry returned HTTP " + std::to_string(status));
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return MakeError("Failed to parse registry response");
    }

    cJSON* out_list = cJSON_CreateArray();
    std::set<std::string> seen_names;
    cJSON* servers = cJSON_GetObjectItem(root, "servers");
    cJSON* entry;
    cJSON_ArrayForEach(entry, servers) {
        cJSON* server = cJSON_GetObjectItem(entry, "server");
        if (!server) continue;

        // The registry keeps every published version of a server; skip
        // superseded ones so a query doesn't return the same server twice.
        cJSON* meta = cJSON_GetObjectItem(entry, "_meta");
        cJSON* official = meta ? cJSON_GetObjectItem(meta, "io.modelcontextprotocol.registry/official") : nullptr;
        cJSON* is_latest = official ? cJSON_GetObjectItem(official, "isLatest") : nullptr;
        if (cJSON_IsBool(is_latest) && !cJSON_IsTrue(is_latest)) continue;

        cJSON* name = cJSON_GetObjectItem(server, "name");
        if (!cJSON_IsString(name) || seen_names.count(name->valuestring)) continue;

        // Only remote (HTTP-reachable) servers are usable by
        // ConnectToRemoteMcpServer — a stdio-only entry has nothing to POST to.
        cJSON* remotes = cJSON_GetObjectItem(server, "remotes");
        if (!cJSON_IsArray(remotes) || cJSON_GetArraySize(remotes) == 0) continue;
        cJSON* remote0 = cJSON_GetArrayItem(remotes, 0);
        cJSON* remote_url = cJSON_GetObjectItem(remote0, "url");
        if (!cJSON_IsString(remote_url)) continue;

        seen_names.insert(name->valuestring);

        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", name->valuestring);
        cJSON* title = cJSON_GetObjectItem(server, "title");
        if (cJSON_IsString(title)) cJSON_AddStringToObject(item, "title", title->valuestring);
        cJSON* desc = cJSON_GetObjectItem(server, "description");
        if (cJSON_IsString(desc)) cJSON_AddStringToObject(item, "description", desc->valuestring);
        cJSON_AddStringToObject(item, "url", remote_url->valuestring);
        cJSON* remote_type = cJSON_GetObjectItem(remote0, "type");
        cJSON_AddStringToObject(item, "transport", cJSON_IsString(remote_type) ? remote_type->valuestring : "");
        cJSON_AddItemToArray(out_list, item);
    }
    cJSON_Delete(root);

    cJSON* out = cJSON_CreateObject();
    cJSON_AddItemToObject(out, "servers", out_list);
    return out;
}

cJSON* ConnectToRemoteMcpServer(const std::string& url, const std::string& auth_token) {
    if (url.empty()) {
        return MakeError("Missing url");
    }

    std::string session_id;
    cJSON* init_error = nullptr;
    if (!InitializeMcpSession(url, auth_token, session_id, &init_error)) {
        return init_error;
    }

    cJSON* list_req = cJSON_CreateObject();
    cJSON_AddStringToObject(list_req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(list_req, "id", 2);
    cJSON_AddStringToObject(list_req, "method", "tools/list");
    cJSON_AddObjectToObject(list_req, "params");

    cJSON* list_resp = SendJsonRpc(url, auth_token, session_id, list_req, true);
    cJSON_Delete(list_req);
    if (!list_resp) {
        return MakeError("Connected, but the tools/list request failed (see device log)");
    }
    cJSON* list_error = ExtractResponseError(list_resp);
    if (list_error) {
        cJSON_Delete(list_resp);
        return list_error;
    }
    cJSON* result = cJSON_GetObjectItem(list_resp, "result");
    cJSON* out = result ? cJSON_Duplicate(result, true) : MakeError("Server returned no result for tools/list");
    cJSON_Delete(list_resp);
    return out;
}

cJSON* CallRemoteMcpTool(const std::string& url, const std::string& tool_name,
                          const std::string& arguments_json, const std::string& auth_token) {
    if (url.empty()) {
        return MakeError("Missing url");
    }
    if (tool_name.empty()) {
        return MakeError("Missing tool_name");
    }

    cJSON* arguments = nullptr;
    if (!arguments_json.empty()) {
        arguments = cJSON_Parse(arguments_json.c_str());
        if (!arguments) {
            return MakeError("arguments is not valid JSON: " + arguments_json);
        }
    }

    // Each call re-does the handshake from scratch rather than keeping a
    // session open between calls — simpler and more robust for a device that
    // calls remote tools rarely, at the cost of one extra round trip per call.
    std::string session_id;
    cJSON* init_error = nullptr;
    if (!InitializeMcpSession(url, auth_token, session_id, &init_error)) {
        if (arguments) cJSON_Delete(arguments);
        return init_error;
    }

    cJSON* call_req = cJSON_CreateObject();
    cJSON_AddStringToObject(call_req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(call_req, "id", 2);
    cJSON_AddStringToObject(call_req, "method", "tools/call");
    cJSON* params = cJSON_AddObjectToObject(call_req, "params");
    cJSON_AddStringToObject(params, "name", tool_name.c_str());
    if (arguments) {
        cJSON_AddItemToObject(params, "arguments", arguments);  // ownership moves to call_req
    } else {
        cJSON_AddObjectToObject(params, "arguments");
    }

    cJSON* call_resp = SendJsonRpc(url, auth_token, session_id, call_req, true);
    cJSON_Delete(call_req);
    if (!call_resp) {
        return MakeError("Connected, but the tools/call request failed (see device log)");
    }
    cJSON* call_error = ExtractResponseError(call_resp);
    if (call_error) {
        cJSON_Delete(call_resp);
        return call_error;
    }
    cJSON* result = cJSON_GetObjectItem(call_resp, "result");
    cJSON* out = result ? cJSON_Duplicate(result, true) : MakeError("Server returned no result for tools/call");
    cJSON_Delete(call_resp);
    return out;
}
