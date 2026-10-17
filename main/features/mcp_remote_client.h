#ifndef MCP_REMOTE_CLIENT_H
#define MCP_REMOTE_CLIENT_H

#include <cJSON.h>
#include <string>

// Searches the public MCP registry (registry.modelcontextprotocol.io) for
// servers matching a free-text query and returns a short list of candidates
// — name, description, and the URL to hand to ConnectToRemoteMcpServer.
// This only searches; it never contacts the servers it finds.
//
// On success: {"servers": [{"name","title","description","url","transport"}, ...]}.
// On failure: {"error": "..."}.
// Caller owns the returned cJSON object.
cJSON* DiscoverMcpServers(const std::string& query);

// Connects to a specific external MCP server over Streamable HTTP
// (JSON-RPC 2.0: initialize -> notifications/initialized -> tools/list) and
// returns whatever the server's tools/list result was. This is read-only —
// it never calls any of the remote tools, so it doesn't grant new
// capabilities by itself.
//
// On success: the server's tools/list "result" object (typically
// {"tools": [...]}). On failure: {"error": "..."}.
// `auth_token`, if non-empty, is sent as "Authorization: Bearer <token>".
// Caller owns the returned cJSON object.
cJSON* ConnectToRemoteMcpServer(const std::string& url, const std::string& auth_token);

// Calls a specific tool on an external MCP server (JSON-RPC "tools/call").
// Re-does the initialize handshake every time rather than keeping a session
// open between calls — this is a one-shot request, not a persistent
// connection. `arguments_json` is a JSON object encoded as a string (may be
// empty for a tool that takes no arguments).
//
// On success: the remote tool's call result (typically {"content": [...]}).
// On failure: {"error": "..."}.
// Caller owns the returned cJSON object.
cJSON* CallRemoteMcpTool(const std::string& url, const std::string& tool_name,
                          const std::string& arguments_json, const std::string& auth_token);

#endif  // MCP_REMOTE_CLIENT_H
