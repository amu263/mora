#include "mcp.hpp"
#include <iostream>
namespace mora {
    McpServer::McpServer(Handler h) : invoke(std::move(h)) {}
    std::optional<Json> McpServer::handle(const Json& request) {
        if (!request.is_object() || !request.contains("jsonrpc") ||
            !request["jsonrpc"].is_string() || request["jsonrpc"] != "2.0" ||
            !request.contains("method") || !request["method"].is_string())
            return Json{{"jsonrpc", "2.0"},
                        {"id", nullptr},
                        {"error", Json{{"code", -32600}, {"message", "Invalid JSON-RPC request"}}}};
        auto method = request["method"].get<std::string>();
        if (!request.contains("id")) {
            if (method == "notifications/initialized" && phase == Phase::AwaitInitialized &&
                (!request.contains("params") || request["params"].is_object()))
                phase = Phase::Ready;
            return {};
        }
        auto id = request["id"];
        if (!id.is_string() && !id.is_number_integer())
            return Json{{"jsonrpc", "2.0"},
                        {"id", nullptr},
                        {"error", Json{{"code", -32600}, {"message", "Invalid request ID"}}}};
        Json response = {{"jsonrpc", "2.0"}, {"id", id}};
        auto error = [&](int code, std::string message) {
            response["error"] = Json{{"code", code}, {"message", std::move(message)}};
            return std::optional<Json>(response);
        };
        try {
            if (request.contains("params") && !request["params"].is_object())
                return error(-32602, "params must be an object");
            const auto params = request.value("params", Json::object());
            if (params.contains("_meta") && !params["_meta"].is_object())
                return error(-32602, "_meta must be an object");
            if (method == "initialize") {
                if (phase != Phase::AwaitInitialize)
                    return error(-32600, "This connection has already been initialized");
                if (!params.contains("protocolVersion") || !params["protocolVersion"].is_string() ||
                    params["protocolVersion"].get<std::string>().empty())
                    return error(-32602, "protocolVersion must be a non-empty string");
                if (params.contains("capabilities") && !params["capabilities"].is_object())
                    return error(-32602, "capabilities must be an object");
                if (params.contains("clientInfo")) {
                    const auto& client = params["clientInfo"];
                    if (!client.is_object() || !client.contains("name") ||
                        !client["name"].is_string() || !client.contains("version") ||
                        !client["version"].is_string())
                        return error(-32602, "clientInfo must contain string name and version");
                }
                auto version = params["protocolVersion"].get<std::string>();
                if (version != "2025-11-25" && version != "2025-06-18" && version != "2025-03-26" &&
                    version != "2024-11-05")
                    version = "2025-11-25";
                response["result"] = Json{
                    {"protocolVersion", version},
                    {"capabilities", Json{{"tools", Json{{"listChanged", false}}}}},
                    {"serverInfo", Json{{"name", "mora-music"}, {"version", "0.7.1-preview"}}},
                    {"instructions",
                     "MORA music sub-agent. music_search and music_open_collection return real "
                     "NetEase metadata, never stream URLs or account cookies; open_collection only "
                     "pages a collection and never starts playback. music_play, music_control "
                     "(pause/resume/next/previous/seek/volume/quality) and music_now_playing control "
                     "the one MORA terminal player through a local per-user session pipe and report "
                     "needs_player when no terminal player is attached. music_recommend returns real "
                     "candidates without starting playback; music_play_random starts one selected "
                     "track; music_recommend_queue replaces the queue with N songs in one call "
                     "(source=search with an intent, source=playlists for the account's playlists, "
                     "source=liked for the liked list, source=auto which falls back to the playlists "
                     "only when the search returns nothing) and music_list_likes pages the liked "
                     "songs; music_get_like reads one song's liked state and music_set_like changes "
                     "it. music_queue_list/music_queue_edit inspect and change the one queue "
                     "(one write per call, op=replace takes up to 1000 ids). music_list_playlists, "
                     "music_set_like and music_add_to_playlist use the signed-in player's account; "
                     "writes require an explicit target and are never retried automatically. "
                     "music_get_status reports whether a player is attached and whether the account "
                     "is signed in (nickname only, never a credential). music_now_playing also "
                     "reports the lyric line being sung, and music_get_lyrics returns the bounded "
                     "timed lyrics."}};
                phase = Phase::AwaitInitialized;
            } else if (method == "ping")
                response["result"] = Json::object();
            else if (phase == Phase::AwaitInitialize)
                return error(-32002, "Initialize the MCP server first");
            else if (phase == Phase::AwaitInitialized)
                return error(-32002, "Send notifications/initialized before using tools");
            else if (method == "tools/list") {
                if (params.contains("cursor") &&
                    (!params["cursor"].is_string() || !params["cursor"].get<std::string>().empty()))
                    return error(-32602, "This static tool catalog has no pagination cursor");
                response["result"] = Json{{"tools", musicToolDefinitions()}};
            } else if (method == "tools/call") {
                if (!params.contains("name") || !params["name"].is_string() ||
                    params["name"].get<std::string>().empty())
                    return error(-32602, "tools/call requires a string tool name");
                if (params.contains("arguments") && !params["arguments"].is_object())
                    return error(-32602, "tool arguments must be an object");
                auto name = params["name"].get<std::string>();
                bool known = false;
                for (auto& tool : musicToolDefinitions())
                    if (tool["name"] == name)
                        known = true;
                if (!known)
                    return error(-32602, "Unknown or not-yet-implemented tool: " + name +
                                             ". Use tools/list to discover implemented tools.");
                try {
                    auto result = invoke(name, params.value("arguments", Json::object()));
                    response["result"] = Json{
                        {"content", Json::array({Json{{"type", "text"}, {"text", result.dump()}}})},
                        {"structuredContent", result},
                        {"isError", false}};
                } catch (const ToolError& e) {
                    // Tool-level failure keeps a machine-readable code; stdout still only carries
                    // JSON-RPC and no credentials or stream URLs.
                    response["result"] = Json{
                        {"content",
                         Json::array({Json{{"type", "text"},
                                           {"text", e.code + ": " +
                                                        utf8(sanitize(unutf8(e.what())))}}})},
                        {"structuredContent", Json{{"ok", false},
                                                   {"error", e.code},
                                                   {"message",
                                                    utf8(sanitize(unutf8(e.what())))}}},
                        {"isError", true}};
                } catch (const std::exception& e) {
                    // Anything that is not a ToolError still answers with a structured code, so a
                    // host never has to parse prose to tell "local session is unreadable" from a
                    // protocol failure.
                    auto text = utf8(sanitize(unutf8(e.what())));
                    response["result"] = Json{
                        {"content",
                         Json::array({Json{{"type", "text"}, {"text", "session_error: " + text}}})},
                        {"structuredContent",
                         Json{{"ok", false}, {"error", "session_error"}, {"message", text}}},
                        {"isError", true}};
                }
            } else
                return error(-32601, "Method not found");
        } catch (const std::exception&) {
            return error(-32602, "Invalid request parameters");
        }
        return response;
    }
    int runMcp() {
        hrcheck(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "MCP COM initialization");
        MusicService service;
        // Thin adapter: the terminal player owns the one playback session. Attach lazily so a
        // player started after the host still becomes reachable.
        BridgeClient bridge;
        // Give the terminal player a moment at startup so the first status call is accurate.
        bridge.connect(1500);
        service.attach([&bridge](const std::string& method, const Json& params) {
            if (!bridge.connected())
                bridge.connect(400);
            // Network work goes through the player and can wait for its serialized worker.
            bool network = method == "search" || method == "collection";
            return bridge.call(method, params, network ? 25000 : 4000);
        });
        McpServer server([&](auto& name, auto& args) {
            try {
                auto result = service.invoke(name, args);
                service.releaseBackend();
                return result;
            } catch (...) {
                service.releaseBackend();
                throw;
            }
        });
        for (;;) {
            std::string line;
            bool overflow = false;
            char ch;
            while (std::cin.get(ch)) {
                if (ch == '\n')
                    break;
                if (line.size() < 65536)
                    line += ch;
                else
                    overflow = true;
            }
            if (line.empty() && !std::cin)
                break;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            if (overflow) {
                std::cout << Json{{"jsonrpc", "2.0"},
                                  {"id", nullptr},
                                  {"error", Json{{"code", -32600},
                                                 {"message", "Message exceeds 64 KiB limit"}}}}
                                 .dump()
                          << '\n'
                          << std::flush;
                continue;
            }
            auto request = Json::parse(line, nullptr, false);
            if (request.is_discarded()) {
                std::cout << Json{{"jsonrpc", "2.0"},
                                  {"id", nullptr},
                                  {"error", Json{{"code", -32700}, {"message", "Invalid JSON"}}}}
                                 .dump()
                          << '\n'
                          << std::flush;
                continue;
            }
            auto response = server.handle(request);
            if (response)
                std::cout << response->dump() << '\n' << std::flush;
        }
        CoUninitialize();
        return 0;
    }
} // namespace mora
