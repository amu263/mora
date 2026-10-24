#include "service.hpp"

namespace mora {
    namespace {
        int boundedInteger(const Json& args, const char* name, int fallback, int minimum,
                           int maximum) {
            if (!args.contains(name))
                return fallback;
            const auto& value = args[name];
            if (!value.is_number_integer())
                throw ToolError("invalid_arguments", std::string(name) + " must be an integer");
            const auto invalidRange = [&] {
                return ToolError("invalid_arguments", std::string(name) + " must be " +
                                                         std::to_string(minimum) + ".." +
                                                         std::to_string(maximum));
            };
            if (value.is_number_unsigned()) {
                auto number = value.get<uint64_t>();
                if (number < static_cast<uint64_t>(minimum) ||
                    number > static_cast<uint64_t>(maximum))
                    throw invalidRange();
                return static_cast<int>(number);
            }
            auto number = value.get<int64_t>();
            if (number < minimum || number > maximum)
                throw invalidRange();
            return static_cast<int>(number);
        }
        int64_t positiveInteger(const Json& args, const char* name) {
            if (!args.contains(name))
                throw ToolError("invalid_arguments", std::string(name) + " is required");
            const auto& value = args[name];
            if (!value.is_number_integer())
                throw ToolError("invalid_arguments", std::string(name) + " must be an integer");
            int64_t number = value.is_number_unsigned() ? static_cast<int64_t>(value.get<uint64_t>())
                                                        : value.get<int64_t>();
            if (number <= 0)
                throw ToolError("invalid_arguments", std::string(name) + " must be positive");
            return number;
        }
        void onlyArguments(const Json& args, std::initializer_list<const char*> allowed) {
            for (auto it = args.begin(); it != args.end(); ++it) {
                bool known = false;
                for (auto name : allowed)
                    if (it.key() == name)
                        known = true;
                if (!known)
                    throw ToolError("invalid_arguments", "Unknown argument: " + it.key());
            }
        }
        bool whitespace(wchar_t c) {
            return c == L' ' || (c >= L'\t' && c <= L'\r') || c == 0xA0 ||
                   (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
                   c == 0x205F || c == 0x3000;
        }
        Kind kindOf(const std::string& kind) {
            return kind == "album" ? Kind::Album : kind == "playlist" ? Kind::Playlist : Kind::Song;
        }
        Track trackFromToolJson(const Json& item) {
            Track track;
            track.id = item.value("id", (int64_t)0);
            track.name = unutf8(item.value("name", std::string()));
            track.artist = unutf8(item.value("artist", std::string()));
            track.album = unutf8(item.value("album", std::string()));
            track.duration = item.value("durationSeconds", 0);
            return track;
        }
        Json candidateJson(const Candidate& candidate) {
            Json item = trackJson(candidate.track);
            item["reason"] = utf8(candidate.reason);
            return item;
        }
        [[noreturn]] void throwApiError(const ApiError& e) {
            throw ToolError(e.code == 301        ? "needs_login"
                            : isRiskCode(e.code) ? "risk_cooldown"
                                                 : "network_unavailable",
                            e.what());
        }
        std::string kindName(const Json& args, bool allowSong) {
            std::string kind = "song";
            if (args.contains("kind")) {
                if (!args["kind"].is_string())
                    throw ToolError("invalid_arguments", allowSong
                                                             ? "kind must be song, album or playlist"
                                                             : "kind must be album or playlist");
                kind = args["kind"].get<std::string>();
            }
            bool known = kind == "album" || kind == "playlist" || (allowSong && kind == "song");
            if (!known)
                throw ToolError("invalid_arguments", allowSong
                                                         ? "kind must be song, album or playlist"
                                                         : "kind must be album or playlist");
            return kind;
        }
    } // namespace
    Json musicToolDefinitions() {
        Json tools = Json::array();
        tools.push_back(Json{
            {"name", "music_search"},
            {"title", "搜索网易云音乐"},
            {"description",
             "Search real NetEase song, album or playlist metadata. Returns IDs and names, never "
             "stream URLs or account cookies. Search does not start playback; use music_play."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"query",
                       Json{{"type", "string"},
                            {"minLength", 1},
                            {"maxLength", 120},
                            {"description", "Song title, artist, album, or a fuzzy combination."}}},
                      {"kind", Json{{"type", "string"},
                                    {"enum", Json::array({"song", "album", "playlist"})},
                                    {"default", "song"}}},
                      {"limit",
                       Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 30}, {"default", 10}}},
                      {"offset", Json{{"type", "integer"},
                                      {"minimum", 0},
                                      {"maximum", 3000},
                                      {"default", 0}}}}},
                 {"required", Json::array({"query"})},
                 {"additionalProperties", false}}},
            {"annotations", Json{{"readOnlyHint", true},
                                 {"destructiveHint", false},
                                 {"idempotentHint", true},
                                 {"openWorldHint", true}}}});
        tools.push_back(Json{
            {"name", "music_open_collection"},
            {"title", "读取专辑或歌单曲目"},
            {"description",
             "Page through real album or playlist metadata. Returns IDs and names only; opening a "
             "collection never starts playback."},
            {"inputSchema",
             Json{{"type", "object"},
                  {"properties",
                   Json{{"kind", Json{{"type", "string"},
                                      {"enum", Json::array({"album", "playlist"})}}},
                        {"id", Json{{"type", "integer"}, {"minimum", 1}}},
                        {"offset", Json{{"type", "integer"},
                                        {"minimum", 0},
                                        {"maximum", 3000},
                                        {"default", 0}}},
                        {"limit", Json{{"type", "integer"},
                                       {"minimum", 1},
                                       {"maximum", 100},
                                       {"default", 50}}}}},
                  {"required", Json::array({"kind", "id"})},
                  {"additionalProperties", false}}},
            {"annotations", Json{{"readOnlyHint", true},
                                 {"destructiveHint", false},
                                 {"idempotentHint", true},
                                 {"openWorldHint", true}}}});
        tools.push_back(Json{
            {"name", "music_play"},
            {"title", "在正在运行的播放器上播放"},
            {"description",
             "Submit playback into the one running MORA player: a song replaces the queue with that "
             "track, an album or playlist replaces it with the collection from startIndex. Returns "
             "accepted state; query music_now_playing for real playback state. Fails with "
             "needs_player when no terminal player is attached."},
            {"inputSchema",
             Json{{"type", "object"},
                  {"properties",
                   Json{{"kind", Json{{"type", "string"},
                                      {"enum", Json::array({"song", "album", "playlist"})},
                                      {"default", "song"}}},
                        {"id", Json{{"type", "integer"}, {"minimum", 1}}},
                        {"startIndex", Json{{"type", "integer"},
                                            {"minimum", 0},
                                            {"maximum", 9999},
                                            {"default", 0}}}}},
                  {"required", Json::array({"id"})},
                  {"additionalProperties", false}}},
            {"annotations", Json{{"readOnlyHint", false},
                                 {"destructiveHint", false},
                                 {"idempotentHint", false},
                                 {"openWorldHint", true}}}});
        tools.push_back(Json{
            {"name", "music_control"},
            {"title", "控制当前播放"},
            {"description",
             "Modify the running player's session: pause, resume, next, previous, seek (value in "
             "seconds) or volume (value 0..100). Never creates a second player; fails with "
             "needs_player when no terminal player is attached."},
            {"inputSchema",
             Json{{"type", "object"},
                  {"properties",
                   Json{{"action",
                         Json{{"type", "string"},
                              {"enum", Json::array({"pause", "resume", "next", "previous", "seek",
                                                    "volume"})}}},
                        {"value", Json{{"type", "number"},
                                       {"description", "seconds for seek, 0..100 for volume"}}}}},
                  {"required", Json::array({"action"})},
                  {"additionalProperties", false}}},
            {"annotations", Json{{"readOnlyHint", false},
                                 {"destructiveHint", false},
                                 {"idempotentHint", false},
                                 {"openWorldHint", false}}}});
        tools.push_back(Json{
            {"name", "music_now_playing"},
            {"title", "查看正在播放"},
            {"description",
             "Read the attached player's real state: track, queue position, pause state, position, "
             "volume, repeat, the quality the account actually received, and a small lyrics block "
             "with the line being sung right now (call music_get_lyrics for the full text). Fails "
             "with needs_player when no terminal player is attached."},
            {"inputSchema", Json{{"type", "object"}, {"additionalProperties", false}}},
            {"annotations", Json{{"readOnlyHint", true},
                                 {"destructiveHint", false},
                                 {"idempotentHint", true},
                                 {"openWorldHint", false}}}});
        tools.push_back(Json{
            {"name", "music_get_lyrics"},
            {"title", "读取歌词"},
            {"description",
             "Read the timed lyrics of the track that is playing (or of an explicit songId). Returns "
             "bounded lines with timestamps, the line that matches the live playback position and, "
             "when the provider ships one, a translated line. Returns instrumental=true when the "
             "track has no lyrics on file. Never returns stream URLs or account cookies."},
            {"inputSchema",
             Json{{"type", "object"},
                  {"properties",
                   Json{{"songId", Json{{"type", "integer"},
                                        {"minimum", 1},
                                        {"description",
                                         "Defaults to the track that is currently playing."}}},
                        {"includeTranslation", Json{{"type", "boolean"}, {"default", true}}},
                        {"maxLines", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 200},
                                          {"default", 60}}},
                        {"fromCurrent", Json{{"type", "boolean"},
                                             {"default", false},
                                             {"description",
                                              "Start at the line matching the live position."}}}}},
                  {"additionalProperties", false}}},
            {"annotations", Json{{"readOnlyHint", true},
                                 {"destructiveHint", false},
                                 {"idempotentHint", true},
                                 {"openWorldHint", true}}}});
        tools.push_back(Json{
            {"name", "music_recommend"},
            {"title", "推荐真实候选"},
            {"description",
             "Return real candidates for an intent: song search hits or tracks from the user's own "
             "playlists. Never starts playback and never changes favorites. Every candidate has a "
             "real id and a reason; keep candidateSetId for music_play_random."},
            {"inputSchema",
             Json{{"type", "object"},
                  {"properties",
                   Json{{"intent", Json{{"type", "string"},
                                        {"maxLength", 120},
                                        {"description",
                                         "What to listen to; required when source is search."}}},
                        {"source", Json{{"type", "string"},
                                        {"enum", Json::array({"auto", "search", "playlists"})},
                                        {"default", "auto"}}},
                        {"limit", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 10},
                                       {"default", 5}}},
                        {"seed", Json{{"type", "integer"},
                                      {"minimum", 1},
                                      {"description", "Optional deterministic seed."}}}}},
                  {"additionalProperties", false}}},
            {"annotations", Json{{"readOnlyHint", true},
                                 {"destructiveHint", false},
                                 {"idempotentHint", false},
                                 {"openWorldHint", true}}}});
        tools.push_back(Json{
            {"name", "music_play_random"},
            {"title", "随机播放一个真实候选"},
            {"description",
             "Pick one candidate from a previous music_recommend set (candidateSetId) or from a fresh "
             "intent search - never the track that is already playing - and submit it to the running "
             "player. Stops and reports instead of skipping through unavailable tracks; returns "
             "status playing only after the player really started audio."},
            {"inputSchema",
             Json{{"type", "object"},
                  {"properties",
                   Json{{"candidateSetId", Json{{"type", "string"},
                                                {"description", "Returned by music_recommend."}}},
                        {"intent", Json{{"type", "string"}, {"maxLength", 120}}},
                        {"seed", Json{{"type", "integer"}, {"minimum", 1}}}}},
                  {"additionalProperties", false}}},
            {"annotations", Json{{"readOnlyHint", false},
                                 {"destructiveHint", false},
                                 {"idempotentHint", false},
                                 {"openWorldHint", true}}}});
        tools.push_back(Json{
            {"name", "music_list_playlists"},
            {"title", "列出用户歌单"},
            {"description",
             "List the signed-in account's playlists with an explicit canWrite flag. Paginated and "
             "bounded; never returns cookies or stream URLs."},
            {"inputSchema",
             Json{{"type", "object"},
                  {"properties",
                   Json{{"ownedOnly", Json{{"type", "boolean"},
                                           {"default", false},
                                           {"description", "Only playlists created by this account."}}},
                        {"limit", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 50},
                                       {"default", 20}}},
                        {"offset", Json{{"type", "integer"}, {"minimum", 0}, {"maximum", 5000},
                                        {"default", 0}}}}},
                  {"additionalProperties", false}}},
            {"annotations", Json{{"readOnlyHint", true},
                                 {"destructiveHint", false},
                                 {"idempotentHint", true},
                                 {"openWorldHint", true}}}});
        tools.push_back(Json{
            {"name", "music_set_like"},
            {"title", "设置红心状态"},
            {"description",
             "Set the signed-in account's liked state for one song to an explicit target value (not "
             "a toggle). Replaying the same call is safe. Requires the running MORA player and a "
             "signed-in account."},
            {"inputSchema",
             Json{{"type", "object"},
                  {"properties",
                   Json{{"songId", Json{{"type", "integer"}, {"minimum", 1}}},
                        {"liked", Json{{"type", "boolean"}}}}},
                  {"required", Json::array({"songId", "liked"})},
                  {"additionalProperties", false}}},
            {"annotations", Json{{"readOnlyHint", false},
                                 {"destructiveHint", false},
                                 {"idempotentHint", true},
                                 {"openWorldHint", true}}}});
        tools.push_back(Json{
            {"name", "music_add_to_playlist"},
            {"title", "把歌曲加入自己的歌单"},
            {"description",
             "Add one song to a playlist this account owns. Ownership is verified before writing; an "
             "already-present song is reported as already_present instead of being retried. "
             "operationId is echoed so the host can deduplicate its own retries."},
            {"inputSchema",
             Json{{"type", "object"},
                  {"properties",
                   Json{{"songId", Json{{"type", "integer"}, {"minimum", 1}}},
                        {"playlistId", Json{{"type", "integer"}, {"minimum", 1}}},
                        {"operationId", Json{{"type", "string"}, {"maxLength", 64}}}}},
                  {"required", Json::array({"songId", "playlistId"})},
                  {"additionalProperties", false}}},
            {"annotations", Json{{"readOnlyHint", false},
                                 {"destructiveHint", false},
                                 {"idempotentHint", false},
                                 {"openWorldHint", true}}}});
        tools.push_back(
            Json{{"name", "music_get_status"},
                 {"title", "查看音乐子 Agent 接入状态"},
                 {"description",
                  "Report implemented capabilities, the attached player session and what is still "
                  "pending. playerSessionAttached is only true when this process shares the "
                  "terminal player's live session."},
                 {"inputSchema", Json{{"type", "object"}, {"additionalProperties", false}}},
                 {"annotations", Json{{"readOnlyHint", true},
                                      {"destructiveHint", false},
                                      {"idempotentHint", true},
                                      {"openWorldHint", false}}}});
        return tools;
    }
    void MusicService::attach(
        std::function<BridgeReply(const std::string&, const Json&)> handler) {
        link = std::move(handler);
    }
    bool MusicService::attached() const {
        return static_cast<bool>(link);
    }
    BridgeReply MusicService::session(const std::string& method, const Json& params) {
        if (!link)
            throw ToolError("needs_player",
                            "no MORA player is attached; run mora in a terminal first");
        auto reply = link(method, params);
        if (!reply.ok)
            throw ToolError(reply.code.empty() ? "player_error" : reply.code,
                            reply.message.empty() ? "the player rejected the request"
                                                  : reply.message);
        return reply;
    }
    void MusicService::releaseBackend() {
        api.reset();
    }
    Api& MusicService::backend() {
        if (!api)
            api = std::make_unique<Api>();
        return *api;
    }
    SearchPage MusicService::search(std::wstring query, Kind kind, int offset) {
        return backend().search(std::move(query), kind, offset);
    }
    std::vector<Track> MusicService::library(bool ownedOnly) {
        return backend().myPlaylists(ownedOnly);
    }
    std::vector<Track> MusicService::album(int64_t id) {
        return backend().album(id);
    }
    std::vector<Track> MusicService::playlist(int64_t id) {
        return backend().playlist(id);
    }
    std::vector<Track> MusicService::details(const std::vector<int64_t>& ids) {
        return backend().details(ids);
    }
    Playback MusicService::playback(const Track& track, const std::string& quality) {
        return backend().playback(track, quality);
    }
    Lyrics MusicService::lyrics(int64_t id) {
        return backend().lyrics(id);
    }
    std::string MusicService::qrKey() {
        return backend().qrKey();
    }
    int MusicService::qrCheck(const std::string& key) {
        return backend().qrCheck(key);
    }
    void MusicService::sendSms(std::string phone) {
        backend().sendSms(std::move(phone));
    }
    void MusicService::phoneLogin(std::string phone, std::string captcha) {
        backend().phoneLogin(std::move(phone), std::move(captcha));
    }
    void MusicService::account() {
        backend().account();
    }
    void MusicService::like(int64_t id, bool value) {
        backend().like(id, value);
    }
    void MusicService::save(int64_t song, int64_t playlist) {
        backend().add(song, playlist);
    }
    void MusicService::logout() {
        backend().logout();
    }
    void MusicService::resume() {
        backend().resume();
    }
    std::wstring MusicService::nickname() const {
        return api ? api->nickname : L"未登录";
    }
    int64_t MusicService::uid() const {
        return api ? api->uid : 0;
    }
    std::set<int64_t> MusicService::likes() const {
        return api ? api->likes : std::set<int64_t>{};
    }
    Json MusicService::searchTool(std::wstring query, Kind kind, int offset, int limit) {
        std::string kindText =
            kind == Kind::Album ? "album" : kind == Kind::Playlist ? "playlist" : "song";
        if (link) {
            auto reply = link("search", Json{{"query", utf8(query)},
                                             {"kind", kindText},
                                             {"offset", offset},
                                             {"limit", limit}});
            if (reply.ok)
                return Json{{"total", reply.data.value("total", 0)},
                            {"items", reply.data.value("items", Json::array())}};
            if (reply.code != "needs_player" && reply.code != "disconnected")
                throw ToolError(reply.code.empty() ? "player_error" : reply.code, reply.message);
        }
        // No attached player: read-only search still works from this process alone.
        auto found = search(std::move(query), kind, offset);
        Json page = {{"total", found.total}, {"items", Json::array()}};
        for (auto& item : found.items) {
            if (static_cast<int>(page["items"].size()) >= limit)
                break;
            page["items"].push_back(trackJson(item));
        }
        return page;
    }
    Json MusicService::collectionTool(const std::string& kind, int64_t id, int offset, int limit) {
        if (link) {
            // A thin adapter borrows the player's account session and network stack instead of
            // keeping a second one alive for the whole host session.
            auto reply = link("collection", Json{{"kind", kind},
                                                 {"id", id},
                                                 {"offset", offset},
                                                 {"limit", limit}});
            if (reply.ok)
                return Json{{"total", reply.data.value("total", 0)},
                            {"items", reply.data.value("items", Json::array())}};
            if (reply.code != "needs_player" && reply.code != "disconnected")
                throw ToolError(reply.code.empty() ? "player_error" : reply.code, reply.message);
        }
        auto items = kind == "album" ? album(id) : playlist(id);
        Json page = {{"total", static_cast<int>(items.size())}, {"items", Json::array()}};
        for (size_t i = static_cast<size_t>(std::min<int>(offset, static_cast<int>(items.size())));
             i < items.size() && static_cast<int>(page["items"].size()) < limit; ++i)
            page["items"].push_back(trackJson(items[i]));
        return page;
    }
    Json MusicService::libraryTool(bool ownedOnly) {
        if (link) {
            auto reply = link("playlists", Json{{"ownedOnly", ownedOnly}});
            if (reply.ok)
                return reply.data.value("items", Json::array());
            if (reply.code != "needs_player" && reply.code != "disconnected")
                throw ToolError(reply.code.empty() ? "player_error" : reply.code, reply.message);
        }
        Json items = Json::array();
        auto me = uid();
        for (auto& playlist : library(ownedOnly)) {
            auto item = trackJson(playlist);
            item["owner"] = playlist.owner == me;
            item["canWrite"] = playlist.owner == me;
            items.push_back(std::move(item));
        }
        return items;
    }
    Json MusicService::lyricsTool(int64_t songId, bool includeTranslation, int maxLines) {
        // The attached player owns the account session and knows the live position, so it answers
        // when it can; a bare adapter still reads lyrics from its own read-only network path.
        if (link) {
            auto reply = link("lyrics", Json{{"songId", songId},
                                             {"includeTranslation", includeTranslation},
                                             {"maxLines", maxLines}});
            if (reply.ok)
                return reply.data;
            if (reply.code != "needs_player" && reply.code != "disconnected")
                throw ToolError(reply.code.empty() ? "player_error" : reply.code, reply.message);
        }
        auto loaded = lyrics(songId);
        return lyricsJson(loaded, songId, nullptr, 0, includeTranslation,
                          static_cast<size_t>(std::clamp(maxLines, 1, 200)));
    }
    std::string MusicService::rememberCandidates(std::vector<Candidate> picked) {
        std::string id = "rec-" + std::to_string(++candidateSerial);
        candidateSets[id] = std::move(picked);
        candidateOrder.push_back(id);
        while (candidateOrder.size() > 8) {
            candidateSets.erase(candidateOrder.front());
            candidateOrder.pop_front();
        }
        return id;
    }
    Json MusicService::invoke(const std::string& name, const Json& args) {
        if (!args.is_object())
            throw ToolError("invalid_arguments", "arguments must be an object");
        if (name == "music_get_status") {
            if (!args.empty())
                throw ToolError("invalid_arguments", "music_get_status takes no arguments");
            Json view = nullptr;
            bool live = false;
            if (link) {
                auto reply = link("status", Json::object());
                if (reply.ok) {
                    view = std::move(reply.data);
                    live = true;
                }
            }
            return Json{{"stage", "m4-session-bridge"},
                        {"playerSessionAttached", live},
                        {"session", view},
                        {"memoryBudgetMB", 50},
                        {"implemented", Json::array({"metadata_search", "mcp_stdio_handshake",
                                                     "collection_paging",
                                                     "shared_session_playback",
                                                     "playback_control",
                                                     "deterministic_recommendation",
                                                     "random_playback",
                                                     "playlist_listing",
                                                     "account_write_tools",
                                                     "integration_status"})},
                        {"separateTerminalPlayerAvailable", true},
                        {"pending", Json::array({"agent_settings_form"})}};
        }
        if (name == "music_now_playing") {
            if (!args.empty())
                throw ToolError("invalid_arguments", "music_now_playing takes no arguments");
            return session("status", Json::object()).data;
        }
        if (name == "music_play") {
            onlyArguments(args, {"kind", "id", "startIndex"});
            auto kind = kindName(args, true);
            int64_t id = positiveInteger(args, "id");
            int start = boundedInteger(args, "startIndex", 0, 0, 9999);
            return session("play", Json{{"kind", kind}, {"id", id}, {"startIndex", start}}).data;
        }
        if (name == "music_control") {
            onlyArguments(args, {"action", "value"});
            if (!args.contains("action") || !args["action"].is_string())
                throw ToolError("invalid_arguments", "action must be a string");
            auto action = args["action"].get<std::string>();
            if (action != "pause" && action != "resume" && action != "next" &&
                action != "previous" && action != "seek" && action != "volume")
                throw ToolError("invalid_arguments",
                                "action must be pause, resume, next, previous, seek or volume");
            Json params = {{"action", action}};
            if (args.contains("value")) {
                if (!args["value"].is_number())
                    throw ToolError("invalid_arguments", "value must be a number");
                params["value"] = args["value"].get<double>();
            }
            if ((action == "seek" || action == "volume") && !params.contains("value"))
                throw ToolError("invalid_arguments", action + " needs a value");
            return session("control", params).data;
        }
        if (name == "music_open_collection") {
            onlyArguments(args, {"kind", "id", "offset", "limit"});
            auto kind = kindName(args, false);
            int64_t id = positiveInteger(args, "id");
            int offset = boundedInteger(args, "offset", 0, 0, 3000);
            int limit = boundedInteger(args, "limit", 50, 1, 100);
            auto page = collectionTool(kind, id, offset, limit);
            return Json{{"kind", kind},
                        {"id", id},
                        {"offset", offset},
                        {"total", page.value("total", 0)},
                        {"items", page.value("items", Json::array())}};
        }
        if (name == "music_get_lyrics") {
            onlyArguments(args, {"songId", "includeTranslation", "maxLines", "fromCurrent"});
            int64_t songId = args.contains("songId") ? positiveInteger(args, "songId") : 0;
            bool includeTranslation = args.value("includeTranslation", true);
            int maxLines = boundedInteger(args, "maxLines", 60, 1, 200);
            Json page = lyricsTool(songId, includeTranslation, maxLines);
            if (page.value("unavailable", false))
                throw ToolError("not_found", "the provider has no lyrics on file for this song");
            if (!songId && page.value("songId", (int64_t)0) == 0)
                throw ToolError("needs_player",
                                "no track is playing; pass songId or start playback first");
            return page;
        }
        if (name == "music_recommend") {
            onlyArguments(args, {"intent", "source", "limit", "seed"});
            auto source = args.value("source", std::string("auto"));
            if (source != "auto" && source != "search" && source != "playlists")
                throw ToolError("invalid_arguments", "source must be auto, search or playlists");
            auto intent = args.value("intent", std::string());
            if (intent.size() > 120)
                throw ToolError("invalid_arguments", "intent must be at most 120 characters");
            int limit = boundedInteger(args, "limit", 5, 1, 10);
            uint64_t seed = args.contains("seed")
                                ? static_cast<uint64_t>(positiveInteger(args, "seed"))
                                : static_cast<uint64_t>(Clock::now().time_since_epoch().count());
            if (source == "auto")
                source = intent.empty() ? "playlists" : "search";
            std::vector<Candidate> pool;
            std::string queryText;
            try {
                if (source == "search") {
                    if (intent.empty())
                        throw ToolError("invalid_arguments",
                                        "intent is required when source is search");
                    auto page = searchTool(unutf8(intent), Kind::Song, 0, 30);
                    int rank = 0;
                    for (auto& item : page["items"]) {
                        Candidate candidate;
                        candidate.track = trackFromToolJson(item);
                        candidate.reason = L"搜索「" + unutf8(intent) + L"」第 " +
                                           std::to_wstring(++rank) + L" 条命中";
                        pool.push_back(std::move(candidate));
                    }
                    queryText = intent;
                } else {
                    auto listed = libraryTool(true);
                    if (listed.empty())
                        throw ToolError("not_found",
                                        "the signed-in account has no playlist to draw from");
                    size_t used = 0;
                    for (auto& playlist : listed) {
                        if (used >= 2 || pool.size() >= 100)
                            break;
                        int64_t playlistId = playlist.value("id", (int64_t)0);
                        if (!playlistId)
                            continue;
                        ++used;
                        auto page = collectionTool("playlist", playlistId, 0, 100);
                        auto name = unutf8(playlist.value("name", std::string()));
                        for (auto& item : page["items"]) {
                            Candidate candidate;
                            candidate.track = trackFromToolJson(item);
                            candidate.reason = L"来自你创建的歌单《" + name + L"》";
                            pool.push_back(std::move(candidate));
                        }
                    }
                }
            } catch (const ApiError& error) {
                throwApiError(error);
            }
            auto picked = selectCandidates(std::move(pool), static_cast<size_t>(limit), seed, 0);
            if (picked.empty())
                throw ToolError("no_candidates", "no real candidate was found for that intent");
            auto setId = rememberCandidates(picked);
            Json candidates = Json::array();
            for (auto& candidate : picked)
                candidates.push_back(candidateJson(candidate));
            return Json{{"candidateSetId", setId},
                        {"source", source},
                        {"query", queryText},
                        {"seed", seed},
                        {"count", static_cast<int>(picked.size())},
                        {"candidates", candidates},
                        {"playbackStarted", false}};
        }
        if (name == "music_play_random") {
            onlyArguments(args, {"candidateSetId", "intent", "seed"});
            auto setId = args.value("candidateSetId", std::string());
            auto intent = args.value("intent", std::string());
            if (setId.empty() && intent.empty())
                throw ToolError("invalid_arguments", "candidateSetId or intent is required");
            // The tool must change real audio, so require a live player before choosing anything.
            auto current = link ? link("status", Json::object())
                                : BridgeReply(false, "needs_player", "no MORA player is attached",
                                              Json::object());
            if (!current.ok)
                throw ToolError(current.code.empty() || current.code == "disconnected"
                                    ? std::string("needs_player")
                                    : current.code,
                                current.message.empty() ? "no MORA player is attached"
                                                        : current.message);
            std::vector<Candidate> pool;
            if (!setId.empty()) {
                auto found = candidateSets.find(setId);
                if (found == candidateSets.end())
                    throw ToolError("not_found",
                                    "unknown candidateSetId; call music_recommend first");
                pool = found->second;
            } else {
                try {
                    auto page = searchTool(unutf8(intent), Kind::Song, 0, 30);
                    int rank = 0;
                    for (auto& item : page["items"]) {
                        Candidate candidate;
                        candidate.track = trackFromToolJson(item);
                        candidate.reason = L"搜索「" + unutf8(intent) + L"」第 " +
                                           std::to_wstring(++rank) + L" 条命中";
                        pool.push_back(std::move(candidate));
                    }
                } catch (const ApiError& error) {
                    throwApiError(error);
                }
            }
            if (pool.empty())
                throw ToolError("no_candidates", "no real candidate was found for that intent");
            int64_t avoid = 0;
            if (current.data.is_object()) {
                auto track = current.data.value("track", Json::object());
                if (track.is_object())
                    avoid = track.value("id", (int64_t)0);
            }
            uint64_t seed = args.contains("seed")
                                ? static_cast<uint64_t>(positiveInteger(args, "seed"))
                                : static_cast<uint64_t>(Clock::now().time_since_epoch().count());
            auto picked = selectCandidates(std::move(pool), 1, seed, avoid);
            if (picked.empty())
                throw ToolError("no_candidates", avoid ? "the only candidate is already playing"
                                                       : "no candidate was found");
            const Candidate& candidate = picked.front();
            auto submitted = link("play", Json{{"kind", "song"},
                                               {"id", candidate.track.id},
                                               {"startIndex", 0}});
            if (!submitted.ok)
                throw ToolError(submitted.code.empty() ? "player_error" : submitted.code,
                                submitted.message);
            // Report the player's real state, but never skip through unavailable tracks.
            bool playing = false;
            std::string failure;
            for (int attempt = 0; attempt < 20; ++attempt) {
                Sleep(200);
                auto view = link("status", Json::object());
                if (!view.ok || !view.data.is_object())
                    break;
                auto track = view.data.value("track", Json::object());
                playing = view.data.value("playing", false) && track.is_object() &&
                          track.value("id", (int64_t)0) == candidate.track.id;
                // A decoder/device failure is an environment problem, not a silent stall: surface it
                // with the play request error instead of letting the adapter guess.
                failure = view.data.value("lastError", std::string());
                if (failure.empty())
                    failure = view.data.value("audioError", std::string());
                if (playing || !failure.empty())
                    break;
            }
            if (!failure.empty())
                throw ToolError("playback_unavailable", failure);
            return Json{{"status", playing ? "playing" : "accepted"},
                        {"playing", playing},
                        {"candidate", candidateJson(candidate)},
                        {"candidateSetId", setId}};
        }
        if (name == "music_list_playlists") {
            onlyArguments(args, {"ownedOnly", "limit", "offset"});
            bool ownedOnly = false;
            if (args.contains("ownedOnly")) {
                if (!args["ownedOnly"].is_boolean())
                    throw ToolError("invalid_arguments", "ownedOnly must be a boolean");
                ownedOnly = args["ownedOnly"].get<bool>();
            }
            int limit = boundedInteger(args, "limit", 20, 1, 50);
            int offset = boundedInteger(args, "offset", 0, 0, 5000);
            auto items = libraryTool(ownedOnly);
            Json page = {{"ownedOnly", ownedOnly},
                         {"offset", offset},
                         {"total", static_cast<int64_t>(items.size())},
                         {"items", Json::array()}};
            for (size_t i = static_cast<size_t>(std::min<int>(offset, static_cast<int>(items.size())));
                 i < items.size() && static_cast<int>(page["items"].size()) < limit; ++i)
                page["items"].push_back(items[i]);
            return page;
        }
        if (name == "music_set_like") {
            onlyArguments(args, {"songId", "liked"});
            int64_t songId = positiveInteger(args, "songId");
            if (!args.contains("liked") || !args["liked"].is_boolean())
                throw ToolError("invalid_arguments", "liked must be a boolean");
            bool liked = args["liked"].get<bool>();
            auto reply = link ? link("like", Json{{"id", songId}, {"liked", liked}})
                              : BridgeReply(false, "needs_player",
                                            "no MORA player is attached; start mora first",
                                            Json::object());
            if (!reply.ok)
                throw ToolError(reply.code.empty() ? "player_error" : reply.code,
                                reply.message.empty() ? "the player rejected the like request"
                                                      : reply.message);
            return Json{{"songId", songId},
                        {"liked", liked},
                        {"state", liked ? "liked" : "unliked"}};
        }
        if (name == "music_add_to_playlist") {
            onlyArguments(args, {"songId", "playlistId", "operationId"});
            int64_t songId = positiveInteger(args, "songId");
            int64_t playlistId = positiveInteger(args, "playlistId");
            auto operationId = args.value("operationId", std::string());
            if (operationId.size() > 64)
                throw ToolError("invalid_arguments", "operationId must be at most 64 characters");
            // Writes must go through the one player session, so prove it is alive before any
            // network work (the read-only fallback must never be used for a write).
            auto current = link ? link("status", Json::object())
                                : BridgeReply(false, "needs_player",
                                              "no MORA player is attached; start mora first",
                                              Json::object());
            if (!current.ok)
                throw ToolError(current.code.empty() || current.code == "disconnected"
                                    ? std::string("needs_player")
                                    : current.code,
                                current.message.empty() ? "no MORA player is attached"
                                                        : current.message);
            // Only playlists this account owns are writable: check before touching the account.
            auto items = libraryTool(true);
            bool owned = false;
            for (auto& item : items)
                if (item.is_object() && item.value("id", (int64_t)0) == playlistId &&
                    item.value("canWrite", false))
                    owned = true;
            if (!owned)
                throw ToolError("no_permission",
                                "playlist is not owned by the signed-in account; only own playlists "
                                "are writable");
            if (!link)
                throw ToolError("needs_player",
                                "no MORA player is attached; start mora first");
            auto reply = link("add", Json{{"songId", songId}, {"playlistId", playlistId}});
            if (!reply.ok && reply.code == "already_present")
                return Json{{"songId", songId},
                            {"playlistId", playlistId},
                            {"operationId", operationId},
                            {"status", "already_present"}};
            if (!reply.ok)
                throw ToolError(reply.code.empty() ? "player_error" : reply.code,
                                reply.message.empty() ? "the player rejected the request"
                                                      : reply.message);
            return Json{{"songId", songId},
                        {"playlistId", playlistId},
                        {"operationId", operationId},
                        {"status", "added"}};
        }
        if (name != "music_search")
            throw std::invalid_argument("Unknown music tool");
        onlyArguments(args, {"query", "kind", "limit", "offset"});
        if (!args.contains("query") || !args["query"].is_string())
            throw ToolError("invalid_arguments", "query must be a string");
        auto query = unutf8(args["query"].get<std::string>());
        if (query.empty() || query.size() > 120)
            throw ToolError("invalid_arguments", "query length must be 1..120 characters");
        if (std::all_of(query.begin(), query.end(), whitespace))
            throw ToolError("invalid_arguments", "query must contain visible text");
        if (std::any_of(query.begin(), query.end(),
                        [](wchar_t c) { return c < 32 || (c >= 127 && c <= 159); }))
            throw ToolError("invalid_arguments", "query must not contain control characters");
        auto kind = kindName(args, true);
        int limit = boundedInteger(args, "limit", 10, 1, 30);
        int offset = boundedInteger(args, "offset", 0, 0, 3000);
        auto page = searchTool(query, kindOf(kind), offset, limit);
        return Json{{"query", utf8(query)},
                    {"kind", kind},
                    {"total", page.value("total", 0)},
                    {"offset", offset},
                    {"items", page.value("items", Json::array())}};
    }
} // namespace mora
