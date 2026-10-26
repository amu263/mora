#include "service.hpp"
#include <random>

namespace mora {
    namespace {
        // The same five quality levels the terminal exposes, so "switch to lossless" is a real
        // operation for the agent instead of a terminal-only feature.
        constexpr std::array<const char*, 5> qualityLevels{"standard", "higher", "exhigh",
                                                           "lossless", "hires"};
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
        // Bridge replies are data, not code: read them without throwing on a surprising type so a
        // malformed player answer becomes a plain default instead of a protocol-level failure.
        int jsonInt(const Json& object, const char* name, int fallback) {
            if (!object.is_object() || !object.contains(name))
                return fallback;
            const auto& value = object[name];
            if (value.is_number_integer()) {
                if (value.is_number_unsigned()) {
                    auto number = value.get<uint64_t>();
                    return number > 2147483647ULL ? 2147483647 : static_cast<int>(number);
                }
                auto number = value.get<int64_t>();
                return number < -2147483648LL ? -2147483648
                       : number > 2147483647LL ? 2147483647
                                               : static_cast<int>(number);
            }
            if (value.is_number_float())
                return static_cast<int>(value.get<double>());
            return fallback;
        }
        int64_t jsonInt64(const Json& object, const char* name, int64_t fallback) {
            if (!object.is_object() || !object.contains(name))
                return fallback;
            const auto& value = object[name];
            if (!value.is_number_integer())
                return fallback;
            return value.is_number_unsigned() ? static_cast<int64_t>(value.get<uint64_t>())
                                              : value.get<int64_t>();
        }
        std::string jsonText(const Json& object, const char* name, std::string fallback = {}) {
            if (!object.is_object() || !object.contains(name))
                return fallback;
            const auto& value = object[name];
            return value.is_string() ? value.get<std::string>() : std::move(fallback);
        }
        bool jsonBool(const Json& object, const char* name, bool fallback) {
            if (!object.is_object() || !object.contains(name))
                return fallback;
            const auto& value = object[name];
            return value.is_boolean() ? value.get<bool>() : fallback;
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
             "stream URLs or account cookies. Search does not start playback; use music_play. For an "
             "artist, style, scene or mood that has no single album, search with limit 10..30 and "
             "then build the whole queue in one call with music_queue_edit(op=replace, ids=[...]) "
             "using 5..20 ids - queueing only one song for a whole series is a defect."},
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
             "Submit playback into the one running MORA player: kind=song replaces the queue with "
             "that single track, while kind=album or kind=playlist replaces the queue with the whole "
             "collection and starts at startIndex - use those whenever the user asks for an entire "
             "album or playlist. Returns accepted state; query music_now_playing for the real "
             "playback state instead of assuming audio started. Fails with needs_player when no "
             "terminal player is attached."},
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
             "seconds), volume (value 0..100) or quality (level: standard/higher/exhigh/lossless/"
             "hires; it applies from the next track and whether it is really available depends on "
             "the account's permissions). Never creates a second player; fails with needs_player "
             "when no terminal player is attached."},
            {"inputSchema",
             Json{{"type", "object"},
                  {"properties",
                   Json{{"action",
                         Json{{"type", "string"},
                              {"enum", Json::array({"pause", "resume", "next", "previous", "seek",
                                                    "volume", "quality"})}}},
                        {"value", Json{{"type", "number"},
                                       {"description", "seconds for seek, 0..100 for volume"}}},
                        {"level", Json{{"type", "string"},
                                       {"enum", Json::array({"standard", "higher", "exhigh",
                                                             "lossless", "hires"})},
                                       {"description", "Required for action=quality."}}}}},
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
            {"name", "music_recommend_queue"},
            {"title", "按取向或我的歌单批量替换播放队列"},
            {"description",
             "Replace the queue with N songs in one call, including 10, 100 or 1000. "
             "source=search draws from real song search for the given intent; source=playlists "
             "draws from the account's own and subscribed playlists (for '从我的歌单/我的收藏挑 N "
             "首', intent is ignored); source=liked draws from the liked songs ('我喜欢的歌/红心', "
             "intent is ignored as the liked list has no keyword ranking); source=auto searches "
             "first and falls back to the playlists only when the search returns nothing at all - a "
             "thin result is reported honestly instead of being topped up with unrelated tracks. "
             "Selects distinct candidates, replaces the queue once and starts the first song. Do "
             "not call music_play_random repeatedly for a multi-song request, and do not queue "
             "songs one by one. Returns requestedCount, queuedCount and the source actually used; "
             "report any shortfall honestly."},
            {"inputSchema", Json{{"type", "object"},
                                  {"properties", Json{{"intent", Json{{"type", "string"},
                                                                        {"maxLength", 120},
                                                                        {"description",
                                                                         "Style, scene, mood or "
                                                                         "artist; required for "
                                                                         "source=search."}}},
                                                      {"count", Json{{"type", "integer"},
                                                                       {"minimum", 1},
                                                                       {"maximum", 1000},
                                                                       {"default", 30}}},
                                                      {"source", Json{{"type", "string"},
                                                                        {"enum", Json::array(
                                                                             {"auto", "search",
                                                                              "playlists",
                                                                              "liked"})},
                                                                        {"default", "auto"}}},
                                                      {"seed", Json{{"type", "integer"},
                                                                      {"minimum", 1}}}}},
                                  {"additionalProperties", false}}},
            {"annotations", Json{{"readOnlyHint", false}, {"destructiveHint", true},
                                   {"idempotentHint", false}, {"openWorldHint", true}}}});
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
             "status playing only after the player really started audio. Use this when the request "
             "is vague ('play something random') instead of asking the user again."},
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
            {"name", "music_list_likes"},
            {"title", "列出我红心的歌曲"},
            {"description",
             "List the songs this account has hearted ('我喜欢的音乐' / 红心). Paginated and bounded; "
             "returns real ids with metadata when it can be resolved. Use it to answer what is in the "
             "liked list, or to hand ids to music_queue_edit; for 'play my liked songs' prefer "
             "music_recommend_queue(source=liked, count=N) in one call."},
            {"inputSchema",
             Json{{"type", "object"},
                  {"properties",
                   Json{{"limit", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 50},
                                       {"default", 20}}},
                        {"offset", Json{{"type", "integer"}, {"minimum", 0}, {"maximum", 5000},
                                        {"default", 0}}}}},
                  {"additionalProperties", false}}},
            {"annotations", Json{{"readOnlyHint", true},
                                 {"destructiveHint", false},
                                 {"idempotentHint", true},
                                 {"openWorldHint", true}}}});
        tools.push_back(Json{
            {"name", "music_get_like"},
            {"title", "查询一首歌是否已红心"},
            {"description",
             "Read one song's liked state on the signed-in account, so the model never has to guess "
             "or page the whole liked list. Read-only; use music_set_like to change it."},
            {"inputSchema",
             Json{{"type", "object"},
                  {"properties", Json{{"songId", Json{{"type", "integer"}, {"minimum", 1}}}}},
                  {"required", Json::array({"songId"})},
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
        tools.push_back(Json{
            {"name", "music_queue_list"},
            {"title", "查看播放队列"},
            {"description",
             "Read the running player's play queue: the total number of rows, the 0-based index of "
             "the row playing now, the play mode (sequential, repeat_all, repeat_one or shuffle) "
             "and a bounded window of rows with id, name, artist, album and duration. limit is at "
             "most 100 and offset is a 0-based row offset inside that bounded window. Read-only: it "
             "never changes playback, never writes the account and never returns stream URLs, "
             "cookies or keys. Fails with needs_player when no terminal player is attached."},
            {"inputSchema",
             Json{{"type", "object"},
                  {"properties",
                   Json{{"limit", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 100},
                                       {"default", 100}}},
                        {"offset", Json{{"type", "integer"},
                                        {"minimum", 0},
                                        {"maximum", 10000},
                                        {"default", 0},
                                        {"description",
                                         "0-based row offset inside the bounded window."}}}}},
                  {"additionalProperties", false}}},
            {"annotations", Json{{"readOnlyHint", true},
                                 {"destructiveHint", false},
                                 {"idempotentHint", true},
                                 {"openWorldHint", false}}}});
        tools.push_back(Json{
            {"name", "music_queue_edit"},
            {"title", "编辑播放队列与播放模式"},
            {"description",
             "Change the running player's queue, one operation per call: never a second player and "
             "never an automatic retry. op is one of: set_mode (mode: sequential = play through and "
             "stop, repeat_all = loop the whole list, repeat_one = loop the single track, shuffle = "
             "random order; optional seed makes shuffle reproducible), play_index (jump straight to "
             "a queue row), remove (delete one row), move (move a row to another position), clear "
             "(empty the whole queue and stop), replace (swap the whole queue for a new set of songs "
             "and start at the first one - 'change it to this set'), add_next (insert songs right "
             "after the current track, the usual 'play next'), append (add songs to the end and keep "
             "everything already queued). Row indexes are 0-based, exactly like "
             "music_play.startIndex. Out-of-range indexes, unknown ops and unknown fields return "
             "invalid_arguments and change nothing. Returns the new queue state. Choose like a "
             "normal player: for a whole album or playlist use music_play(kind=album or playlist), "
             "which replaces the queue with that collection and plays it (music_open_collection only "
             "lists its tracks and never starts playback); for "
             "an artist, style, scene or mood that has no single album, search first (music_search "
             "with limit 10..30), then build the queue in one call with op=replace and 5..20 ids so "
             "several songs really play - queueing only one song is a defect; when the user says "
             "'add some more', 'queue it next' or 'keep playing more', use append or add_next and "
             "never replace the list that is already playing; when the request is vague ('play "
             "something random'), use music_recommend plus music_play_random instead of asking "
             "again."},
            {"inputSchema",
             Json{{"type", "object"},
                  {"properties",
                   Json{{"op", Json{{"type", "string"},
                                    {"enum", Json::array({"set_mode", "play_index", "remove", "move",
                                                          "clear", "add_next", "append",
                                                          "replace"})}}},
                        {"mode", Json{{"type", "string"},
                                      {"enum", Json::array({"sequential", "repeat_all", "repeat_one",
                                                            "shuffle"})},
                                      {"description", "Required for op=set_mode."}}},
                        {"seed", Json{{"type", "integer"},
                                      {"minimum", 1},
                                      {"description",
                                       "Optional deterministic shuffle seed for op=set_mode."}}},
                        {"index", Json{{"type", "integer"},
                                       {"minimum", 0},
                                       {"maximum", 9999},
                                       {"description",
                                        "0-based queue row for play_index or remove."}}},
                        {"from", Json{{"type", "integer"}, {"minimum", 0}, {"maximum", 9999}}},
                        {"to", Json{{"type", "integer"}, {"minimum", 0}, {"maximum", 9999}}},
                        {"id", Json{{"type", "integer"},
                                    {"minimum", 1},
                                    {"description",
                                     "One song id for replace, add_next or append (id or ids, not "
                                     "both)."}}},
                        {"ids", Json{{"type", "array"},
                                     {"maxItems", 1000},
                                     {"items", Json{{"type", "integer"}, {"minimum", 1}}},
                                     {"description",
                                      "Song ids: up to 1000 for replace (a whole series), up to 50 "
                                      "for add_next or append."}}}}},
                  {"required", Json::array({"op"})},
                  {"additionalProperties", false}}},
            {"annotations", Json{{"readOnlyHint", false},
                                 {"destructiveHint", true},
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
    void MusicService::useTransport(Api::Transport adapter) {
        // The injected transport survives releaseBackend(), so a thin adapter can drop and rebuild
        // its backend between calls and still be exercised by the same deterministic stub. A new
        // transport means a new backend identity, so cached account state must not leak across it.
        transport = std::move(adapter);
        api.reset();
        clearAccountCache();
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
    Api& MusicService::backend() const {
        if (!api)
            api = transport ? std::make_unique<Api>(stateDirectory(), transport)
                            : std::make_unique<Api>();
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
        auto& source = backend();
        source.account();
        cacheAccount(source);
    }
    void MusicService::like(int64_t id, bool value) {
        backend().like(id, value);
        // Keep a warm cache truthful for our own writes instead of dropping it.
        std::lock_guard lock(cacheMutex);
        if (!cachedLikesValid)
            return;
        if (value)
            cachedLikes.insert(id);
        else
            cachedLikes.erase(id);
    }
    void MusicService::save(int64_t song, int64_t playlist) {
        backend().add(song, playlist);
    }
    void MusicService::logout() {
        backend().logout();
        clearAccountCache();
    }
    void MusicService::resume() {
        backend().resume();
    }
    void MusicService::cacheAccount(const Api& source) const {
        std::lock_guard lock(cacheMutex);
        cachedUid = source.uid;
        if (source.uid > 0)
            cachedNickname = source.nickname;
        cachedLikes = source.likes;
        cachedLikesAt = Clock::now();
        cachedLikesValid = true;
    }
    void MusicService::clearAccountCache() const {
        std::lock_guard lock(cacheMutex);
        cachedUid = 0;
        cachedNickname.clear();
        cachedLikes.clear();
        cachedLikesAt = {};
        cachedLikesValid = false;
    }
    void MusicService::invalidateLikes() const {
        // A like written through the attached player is not visible in our cached set, so the next
        // read refreshes instead of serving a stale answer.
        std::lock_guard lock(cacheMutex);
        cachedLikesValid = false;
        cachedLikes.clear();
    }
    std::wstring MusicService::nickname() const {
        // Materialise the backend unless the cache already knows the account: the saved session
        // knows the nickname, and reporting "未登录" before the first network call was a lie after
        // a restart. A damaged session file falls back to the neutral value instead of throwing.
        {
            std::lock_guard lock(cacheMutex);
            if (cachedUid > 0 && !cachedNickname.empty())
                return cachedNickname;
        }
        try {
            auto& source = backend();
            std::lock_guard lock(cacheMutex);
            cachedUid = source.uid;
            if (source.uid > 0)
                cachedNickname = source.nickname;
            return cachedNickname.empty() ? L"未登录" : cachedNickname;
        } catch (const std::exception&) {
            return L"未登录";
        }
    }
    int64_t MusicService::uid() const {
        {
            std::lock_guard lock(cacheMutex);
            if (cachedUid > 0)
                return cachedUid;
        }
        try {
            auto& source = backend();
            std::lock_guard lock(cacheMutex);
            cachedUid = source.uid;
            if (source.uid > 0)
                cachedNickname = source.nickname;
            return cachedUid;
        } catch (const std::exception&) {
            return 0;
        }
    }
    std::set<int64_t> MusicService::likes() const {
        // Liked ids are network state, so the first read hydrates them once; afterwards the cache
        // answers (bounded TTL) and a paging host costs one metadata request per page instead of a
        // full account round trip per tool call.
        constexpr auto ttl = std::chrono::minutes(5);
        {
            std::lock_guard lock(cacheMutex);
            if (cachedLikesValid && Clock::now() - cachedLikesAt < ttl)
                return cachedLikes;
        }
        try {
            auto& source = backend();
            // Only a signed-in session has liked ids to fetch; asking while signed out would spend
            // a request to learn nothing.
            if (source.loggedIn() && (source.uid <= 0 || source.likes.empty()))
                source.account();
            cacheAccount(source);
            std::lock_guard lock(cacheMutex);
            return cachedLikes;
        } catch (const std::exception&) {
            return {};
        }
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
        std::vector<int64_t> unresolved;
        for (size_t i = static_cast<size_t>(std::min<int>(offset, static_cast<int>(items.size())));
             i < items.size() && static_cast<int>(page["items"].size()) < limit; ++i) {
            page["items"].push_back(trackJson(items[i]));
            if (items[i].name.empty() && items[i].id > 0)
                unresolved.push_back(items[i].id);
        }
        // The provider only resolves metadata for the first rows of a collection, so a page beyond
        // that would otherwise come back with real ids but no names. Resolve just this page's
        // unresolved rows (details takes at most 50 ids per call) and ignore a failed lookup, which
        // leaves the bare ids intact.
        if (!unresolved.empty()) {
            std::map<int64_t, Track> known;
            try {
                for (size_t start = 0; start < unresolved.size(); start += 50) {
                    std::vector<int64_t> chunk(
                        unresolved.begin() + static_cast<std::ptrdiff_t>(start),
                        unresolved.begin() + static_cast<std::ptrdiff_t>(
                                                 std::min(unresolved.size(), start + 50)));
                    for (auto& track : details(chunk))
                        known[track.id] = track;
                }
            } catch (const ApiError&) {
            }
            for (auto& item : page["items"]) {
                auto found = known.find(item.value("id", (int64_t)0));
                if (found != known.end())
                    item = trackJson(found->second);
            }
        }
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
        // Resolve the list before reading the account id: myPlaylists() runs account() on its first
        // use, and comparing the creator against a not-yet-loaded id would mark every playlist as
        // "somebody else's" and hide the write actions the user really has.
        auto playlists = library(ownedOnly);
        auto me = uid();
        for (auto& playlist : playlists) {
            auto item = trackJson(playlist);
            item["owner"] = playlist.owner == me;
            item["canWrite"] = playlist.owner == me;
            items.push_back(std::move(item));
        }
        return items;
    }
    Json MusicService::queueState(const Json& data) const {
        // Project the player's queue into the documented shape. Only the fields below ever leave
        // this process: no stream URL, cover URL, cookie or account identifier can leak through.
        Json out = Json::object();
        int total = jsonInt(data, "total", 0);
        int index = jsonInt(data, "index", 0);
        QueueMode mode = QueueMode::Sequential;
        if (!parseQueueMode(jsonText(data, "mode", "sequential"), mode))
            mode = QueueMode::Sequential;
        out["total"] = total < 0 ? 0 : total;
        out["index"] = index < 0 ? 0 : index;
        out["mode"] = queueModeName(mode);
        out["repeat"] = jsonBool(data, "repeat", mode == QueueMode::RepeatAll ||
                                                    mode == QueueMode::RepeatOne);
        Json items = Json::array();
        if (data.is_object() && data.contains("items") && data["items"].is_array()) {
            for (auto& item : data["items"]) {
                if (!item.is_object() || items.size() >= 100)
                    continue;
                int position = jsonInt(item, "position", static_cast<int>(items.size()));
                Json row = {{"position", position < 0 ? static_cast<int>(items.size()) : position},
                            {"id", jsonInt64(item, "id", 0)},
                            {"name", jsonText(item, "name")},
                            {"artist", jsonText(item, "artist")},
                            {"album", jsonText(item, "album")},
                            {"durationSeconds", jsonInt(item, "durationSeconds", 0)}};
                row["current"] = jsonBool(item, "current", row["position"] == out["index"]);
                items.push_back(std::move(row));
            }
        }
        out["items"] = std::move(items);
        return out;
    }
    std::vector<Track> MusicService::queueTracks(const std::vector<int64_t>& ids) {
        std::vector<Track> loaded;
        try {
            loaded = details(ids);
        } catch (const ApiError&) {
            // Metadata is a nicety, never a fake: without it the row still carries a real id and
            // the player fills the rest in through its existing refresh path.
        }
        std::vector<Track> tracks;
        tracks.reserve(ids.size());
        for (auto& track : loaded)
            if (track.id)
                tracks.push_back(std::move(track));
        for (auto id : ids) {
            bool known = false;
            for (auto& track : tracks)
                if (track.id == id)
                    known = true;
            if (!known)
                tracks.push_back(Track{.id = id});
        }
        return tracks;
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
                        // Read-only identity so the model can tell whether "my playlists" and
                        // "my liked songs" are even available, without exposing any credential.
                        {"account", Json{{"signedIn", uid() > 0}, {"nickname", utf8(nickname())}}},
                        {"memoryBudgetMB", 50},
                        {"implemented", Json::array({"metadata_search", "mcp_stdio_handshake",
                                                     "collection_paging",
                                                     "collection_page_metadata",
                                                     "shared_session_playback",
                                                     "playback_control",
                                                     "quality_control",
                                                     "queue_control",
                                                     "playback_modes",
                                                     "deterministic_recommendation",
                                                     "random_playback",
                                                     "batch_queue_from_search_playlists_likes",
                                                     "playlist_listing",
                                                     "liked_listing",
                                                     "like_state_query",
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
            onlyArguments(args, {"action", "value", "level"});
            if (!args.contains("action") || !args["action"].is_string())
                throw ToolError("invalid_arguments", "action must be a string");
            auto action = args["action"].get<std::string>();
            if (action != "pause" && action != "resume" && action != "next" &&
                action != "previous" && action != "seek" && action != "volume" &&
                action != "quality")
                throw ToolError("invalid_arguments",
                                "action must be pause, resume, next, previous, seek, volume or "
                                "quality");
            Json params = {{"action", action}};
            if (args.contains("value")) {
                if (!args["value"].is_number())
                    throw ToolError("invalid_arguments", "value must be a number");
                params["value"] = args["value"].get<double>();
            }
            if ((action == "seek" || action == "volume") && !params.contains("value"))
                throw ToolError("invalid_arguments", action + " needs a value");
            if (action == "quality") {
                if (!args.contains("level") || !args["level"].is_string())
                    throw ToolError("invalid_arguments", "quality needs a level string");
                auto level = args["level"].get<std::string>();
                bool known = false;
                for (auto candidate : qualityLevels)
                    known = known || level == candidate;
                if (!known)
                    throw ToolError("invalid_arguments",
                                    "level must be standard, higher, exhigh, lossless or hires");
                params["level"] = level;
            }
            return session("control", params).data;
        }
        if (name == "music_open_collection") {
            onlyArguments(args, {"kind", "id", "offset", "limit"});
            auto kind = kindName(args, false);
            int64_t id = positiveInteger(args, "id");
            int offset = boundedInteger(args, "offset", 0, 0, 3000);
            int limit = boundedInteger(args, "limit", 50, 1, 100);
            // A provider failure keeps its machine-readable code instead of degrading into a bare
            // text error the host has to parse.
            Json page;
            try {
                page = collectionTool(kind, id, offset, limit);
            } catch (const ApiError& error) {
                throwApiError(error);
            } catch (const std::runtime_error& error) {
                throw ToolError("session_error", error.what());
            }
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
        if (name == "music_recommend_queue") {
            onlyArguments(args, {"intent", "count", "seed", "source"});
            auto source = args.value("source", std::string("auto"));
            if (source != "auto" && source != "search" && source != "playlists" &&
                source != "liked")
                throw ToolError("invalid_arguments",
                                "source must be auto, search, playlists or liked");
            auto intent = args.value("intent", std::string());
            if (intent.size() > 120)
                throw ToolError("invalid_arguments", "intent must be at most 120 characters");
            if (source == "search" && intent.empty())
                throw ToolError("invalid_arguments", "intent is required when source is search");
            int count = boundedInteger(args, "count", 30, 1, 1000);
            auto status = link ? link("status", Json::object())
                               : BridgeReply(false, "needs_player", "no MORA player attached",
                                             Json::object());
            if (!status.ok)
                throw ToolError(status.code.empty() ? "needs_player" : status.code,
                                status.message);
            std::vector<int64_t> ids;
            std::set<int64_t> seen;
            auto collect = [&](const Json& items) {
                for (const auto& item : items) {
                    int64_t id = item.value("id", (int64_t)0);
                    if (id > 0 && seen.insert(id).second)
                        ids.push_back(id);
                }
            };
            auto fillFromSearch = [&]() {
                auto fullQuery = unutf8(intent);
                std::vector<std::wstring> queries{fullQuery};
                // Music search often treats a whole taste sentence as one exact query. Broaden it
                // to its individual style terms only when the specific query has too few hits.
                for (size_t start = 0; start < fullQuery.size();) {
                    auto end = fullQuery.find_first_of(L" \t,，、;/|", start);
                    if (end == std::wstring::npos)
                        end = fullQuery.size();
                    auto term = fullQuery.substr(start, end - start);
                    if (term.size() >= 2 &&
                        std::find(queries.begin(), queries.end(), term) == queries.end())
                        queries.push_back(std::move(term));
                    start = end + 1;
                }
                for (const auto& query : queries) {
                    for (int offset = 0; offset < 3000 && (int)ids.size() < count; offset += 30) {
                        auto page = searchTool(query, Kind::Song, offset, 30);
                        int total = page.value("total", 0);
                        auto items = page.value("items", Json::array());
                        collect(items);
                        if (items.size() < 30 || offset + 30 >= total)
                            break;
                    }
                    if ((int)ids.size() >= count)
                        break;
                }
            };
            auto fillFromPlaylists = [&](uint64_t seed) {
                // The playlists are the source, not a search: read each collection and take its ids,
                // stopping as soon as the requested count is covered. At most four playlists are
                // read, so a large count can never fan out into a request storm.
                auto playlists = libraryTool(false);
                if (playlists.empty())
                    return;
                size_t start = static_cast<size_t>(seed % playlists.size());
                for (size_t used = 0; used < 4 && used < playlists.size() &&
                                      (int)ids.size() < count;
                     ++used) {
                    auto& playlist = playlists[(start + used) % playlists.size()];
                    int64_t playlistId = playlist.value("id", (int64_t)0);
                    if (playlistId <= 0)
                        continue;
                    for (int offset = 0; offset < 1000 && (int)ids.size() < count; offset += 100) {
                        auto page = collectionTool("playlist", playlistId, offset, 100);
                        auto items = page.value("items", Json::array());
                        collect(items);
                        if (items.size() < 100)
                            break;
                    }
                }
            };
            auto fillFromLikes = [&]() {
                // likes() hydrates once and caches afterwards, so this costs no extra account
                // round trip when the host calls the liked tools back to back.
                for (auto id : likes()) {
                    if (id > 0 && seen.insert(id).second)
                        ids.push_back(id);
                    if ((int)ids.size() >= count)
                        break;
                }
            };
            uint64_t seed = args.contains("seed")
                                ? static_cast<uint64_t>(positiveInteger(args, "seed"))
                                : static_cast<uint64_t>(Clock::now().time_since_epoch().count());
            std::string usedSource;
            if (source == "search" || (source == "auto" && !intent.empty())) {
                fillFromSearch();
                usedSource = "search";
            }
            if (source == "liked") {
                fillFromLikes();
                usedSource = "liked";
            }
            // auto falls back to the user's playlists only when the search found nothing at all:
            // topping up a thin search result with unrelated playlist tracks would quietly change
            // what the user asked for, and an honest shortfall is better than a mixed queue.
            size_t afterSearch = ids.size();
            if (source == "playlists" || (source == "auto" && ids.empty())) {
                fillFromPlaylists(seed);
                if (ids.size() > afterSearch)
                    usedSource = usedSource.empty() ? "playlists" : usedSource + "+playlists";
                else if (usedSource.empty())
                    usedSource = "playlists";
            }
            if (ids.empty())
                throw ToolError("no_candidates",
                                source == "playlists"
                                    ? "the signed-in account has no playable playlist tracks"
                                : source == "liked"
                                    ? "the signed-in account has no liked songs"
                                : source == "auto"
                                    ? "no real songs found by search or in the user's playlists"
                                    : "no real songs found for that intent");
            auto availableCount = ids.size();
            std::mt19937_64 random(seed);
            std::shuffle(ids.begin(), ids.end(), random);
            if (ids.size() > static_cast<size_t>(count))
                ids.resize(static_cast<size_t>(count));
            auto reply = link("queue_edit", Json{{"op", "replace"}, {"ids", ids}});
            if (!reply.ok)
                throw ToolError(reply.code.empty() ? "player_error" : reply.code, reply.message);
            return Json{{"requestedCount", count},
                        {"queuedCount", ids.size()},
                        {"availableCount", availableCount},
                        {"source", usedSource},
                        {"searchIntent", intent},
                        {"changed", reply.data.value("changed", false)},
                        {"playback", "submitted"}};
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
            Json items;
            try {
                items = libraryTool(ownedOnly);
            } catch (const ApiError& error) {
                throwApiError(error);
            } catch (const std::runtime_error& error) {
                throw ToolError("session_error", error.what());
            }
            Json page = {{"ownedOnly", ownedOnly},
                         {"offset", offset},
                         {"total", static_cast<int64_t>(items.size())},
                         {"items", Json::array()}};
            for (size_t i = static_cast<size_t>(std::min<int>(offset, static_cast<int>(items.size())));
                 i < items.size() && static_cast<int>(page["items"].size()) < limit; ++i)
                page["items"].push_back(items[i]);
            return page;
        }
        if (name == "music_list_likes") {
            onlyArguments(args, {"limit", "offset"});
            int limit = boundedInteger(args, "limit", 20, 1, 50);
            int offset = boundedInteger(args, "offset", 0, 0, 5000);
            // likes() hydrates the account once (they are network state, not session state) and
            // then answers from the bounded cache, so a host paging the list does not rebuild the
            // account state on every call.
            auto liked = likes();
            // Two very different causes, two codes: a signed-out session is fixable by the user,
            // an empty liked list is simply empty.
            if (liked.empty() && uid() == 0)
                throw ToolError("needs_login",
                                "not signed in to a music account; sign in in the terminal first");
            if (liked.empty())
                throw ToolError("no_likes", "this account has no liked songs yet");
            std::vector<int64_t> page;
            int index = 0;
            for (auto id : liked) {
                if (index++ < offset)
                    continue;
                if (static_cast<int>(page.size()) >= limit)
                    break;
                page.push_back(id);
            }
            // Metadata is a nicety: a failed detail lookup must still return the real ids.
            std::map<int64_t, Track> known;
            try {
                if (!page.empty())
                    for (auto& track : details(page))
                        known[track.id] = track;
            } catch (const ApiError&) {
            }
            Json items = Json::array();
            for (auto id : page) {
                auto found = known.find(id);
                items.push_back(found == known.end() ? Json{{"id", id}}
                                                      : trackJson(found->second));
            }
            return Json{{"total", static_cast<int64_t>(liked.size())},
                        {"offset", offset},
                        {"limit", limit},
                        {"items", items}};
        }
        if (name == "music_get_like") {
            onlyArguments(args, {"songId"});
            int64_t songId = positiveInteger(args, "songId");
            auto liked = likes();
            if (uid() == 0)
                throw ToolError("needs_login",
                                "not signed in to a music account; sign in in the terminal first");
            return Json{{"songId", songId},
                        {"liked", liked.contains(songId)},
                        {"likedTotal", static_cast<int64_t>(liked.size())}};
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
            // The write happened in the player, so our cached liked set is now stale by exactly this
            // one song: refresh it on the next read instead of answering wrongly.
            invalidateLikes();
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
        if (name == "music_queue_list") {
            onlyArguments(args, {"limit", "offset"});
            int limit = boundedInteger(args, "limit", 100, 1, 100);
            int offset = boundedInteger(args, "offset", 0, 0, 10000);
            // Read the bounded head of the one authoritative queue, then page inside it: the
            // projection stays correct even if the player answers the whole window at once.
            auto reply = session("queue_list", Json{{"limit", 100}, {"offset", 0}});
            Json state = queueState(reply.data);
            Json window = Json::array();
            const Json& rows = state["items"];
            for (int row = offset; row < static_cast<int>(rows.size()) &&
                                  static_cast<int>(window.size()) < limit;
                 ++row)
                window.push_back(rows[row]);
            state["items"] = std::move(window);
            state["offset"] = offset;
            state["limit"] = limit;
            return state;
        }
        if (name == "music_queue_edit") {
            onlyArguments(args, {"op", "mode", "seed", "index", "from", "to", "id", "ids"});
            if (!args.contains("op") || !args["op"].is_string())
                throw ToolError("invalid_arguments", "op must be a string");
            auto op = args["op"].get<std::string>();
            Json params = {{"op", op}};
            if (op == "set_mode") {
                onlyArguments(args, {"op", "mode", "seed"});
                if (!args.contains("mode") || !args["mode"].is_string())
                    throw ToolError("invalid_arguments", "set_mode needs a mode string");
                QueueMode mode = QueueMode::Sequential;
                if (!parseQueueMode(args["mode"].get<std::string>(), mode))
                    throw ToolError("invalid_arguments",
                                    "mode must be sequential, repeat_all, repeat_one or shuffle");
                params["mode"] = queueModeName(mode);
                if (args.contains("seed"))
                    params["seed"] = boundedInteger(args, "seed", 1, 1, 2147483647);
            } else if (op == "play_index" || op == "remove") {
                onlyArguments(args, {"op", "index"});
                if (!args.contains("index"))
                    throw ToolError("invalid_arguments", op + " needs an index");
                params["index"] = boundedInteger(args, "index", 0, 0, 9999);
            } else if (op == "move") {
                onlyArguments(args, {"op", "from", "to"});
                if (!args.contains("from") || !args.contains("to"))
                    throw ToolError("invalid_arguments", "move needs from and to");
                params["from"] = boundedInteger(args, "from", 0, 0, 9999);
                params["to"] = boundedInteger(args, "to", 0, 0, 9999);
            } else if (op == "clear") {
                onlyArguments(args, {"op"});
            } else if (op == "add_next" || op == "append" || op == "replace") {
                onlyArguments(args, {"op", "id", "ids"});
                if (args.contains("id") && args.contains("ids"))
                    throw ToolError("invalid_arguments", "pass id or ids, not both");
                // replace builds a fresh series (at most 1000, matching the player); append/add_next
                // extend a queue that is already playing (at most 50, so one call can never become
                // a request storm).
                int maxIds = op == "replace" ? 1000 : 50;
                std::vector<int64_t> ids;
                if (args.contains("id"))
                    ids.push_back(positiveInteger(args, "id"));
                if (args.contains("ids")) {
                    if (!args["ids"].is_array())
                        throw ToolError("invalid_arguments",
                                        "ids must be an array of positive song ids");
                    if (args["ids"].size() > static_cast<size_t>(maxIds))
                        throw ToolError("invalid_arguments",
                                        "ids may hold at most " + std::to_string(maxIds) +
                                            " songs for " + op);
                    for (auto& value : args["ids"]) {
                        if (!value.is_number_integer())
                            throw ToolError("invalid_arguments", "ids must be positive integers");
                        int64_t id = value.is_number_unsigned()
                                         ? static_cast<int64_t>(value.get<uint64_t>())
                                         : value.get<int64_t>();
                        if (id <= 0)
                            throw ToolError("invalid_arguments", "ids must be positive integers");
                        ids.push_back(id);
                    }
                }
                if (ids.empty())
                    throw ToolError("invalid_arguments", op + " needs id or ids");
                // Writes belong to the one player session, so prove it is alive before spending a
                // single network read on metadata. A missing player must never reach details().
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
                auto tracks = queueTracks(ids);
                Json encoded = Json::array();
                bool metadataResolved = false;
                for (auto& track : tracks) {
                    if (!track.name.empty())
                        metadataResolved = true;
                    encoded.push_back(Json{{"id", track.id},
                                           {"name", utf8(track.name)},
                                           {"artist", utf8(track.artist)},
                                           {"album", utf8(track.album)},
                                           {"duration", track.duration}});
                }
                params["ids"] = ids;
                params["tracks"] = std::move(encoded);
                params["metadataResolved"] = metadataResolved;
            } else {
                throw ToolError("invalid_arguments",
                                "op must be set_mode, play_index, remove, move, clear, add_next, "
                                "append or replace");
            }
            auto reply = session("queue_edit", params);
            Json state = queueState(reply.data);
            if (!reply.data.is_object() || !reply.data.contains("items")) {
                // A player that only echoes "accepted" still has to report the new state honestly.
                auto listed = link ? link("queue_list", Json{{"limit", 100}, {"offset", 0}})
                                   : BridgeReply(false, "needs_player", "", Json::object());
                if (listed.ok)
                    state = queueState(listed.data);
            }
            state["op"] = op;
            state["changed"] = jsonBool(reply.data, "changed", true);
            if (op == "add_next" || op == "append" || op == "replace") {
                state["metadataResolved"] = jsonBool(params, "metadataResolved", false);
                state["requestedIds"] = params["ids"];
            }
            return state;
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
