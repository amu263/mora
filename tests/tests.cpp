#include "agent.hpp"
#include "api.hpp"
#include "audio.hpp"
#include "bridge.hpp"
#include "commands.hpp"
#include "context.hpp"
#include "core.hpp"
#include "mascot.hpp"
#include "mcp.hpp"
#include "persona.hpp"
#include "platform.hpp"
#include "recommend.hpp"
#include "session.hpp"
#include "settings.hpp"
#include "stats.hpp"
#include "ui.hpp"
#include "ui_tool_calls.hpp"
#include "worker.hpp"
#include <cmath>
#include <fstream>
#include <iostream>

static int failures = 0;
static int checks = 0;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            std::cerr << "FAIL line " << __LINE__ << ": " #x "\n";                                 \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)
using namespace mora;

int main() {
    // A model call runs on the worker, while playback and control stay on the UI thread.
    {
        MainThreadToolCalls calls;
        const auto owner = std::this_thread::get_id();
        BridgeReply result;
        std::thread worker([&] {
            result = calls.call("play", Json{{"id", 7}});
        });
        for (int i = 0; i < 1000 && calls.pump([&](uint64_t requestOwner, const std::string& method,
                                                     const Json& params) {
                 CHECK(requestOwner == 0);
                 CHECK(std::this_thread::get_id() == owner);
                 CHECK(method == "play" && params["id"] == 7);
                 return BridgeReply(true, "", "", Json{{"accepted", true}});
             }) == 0; ++i)
            Sleep(1);
        worker.join();
        CHECK(result.ok && result.data.value("accepted", false));
        calls.close();
        auto closed = calls.call("control", Json::object());
        CHECK(!closed.ok && closed.code == "cancelled");
    }
    {
        std::atomic<uint64_t> active{0};
        Worker worker(nullptr, &active);
        bool cancelled = false;
        try {
            worker.callTool(1, "music_control", Json{{"action", "volume"}, {"value", 20}});
        } catch (const ToolError& error) {
            cancelled = error.code == "cancelled";
        }
        CHECK(cancelled);
    }
    CHECK(unutf8(utf8(L"晴天 · 周杰伦")) == L"晴天 · 周杰伦");
    CHECK(displayWidth(L"晴天 abc") == 8);
    CHECK(displayWidth(truncate(L"晴天abc", 5)) <= 5);
    CHECK(sanitize(L"song\x1b[31m\r\n") == L"song[31m  ");
    CHECK(editDistance(L"周杰论", L"周杰伦") == 1);
    Track good{.id = 1, .name = L"晴天", .artist = L"周杰伦", .album = L"叶惠美"};
    Track other{.id = 2, .name = L"光年之外", .artist = L"邓紫棋"};
    CHECK(matchScore(good, L"周杰论 晴天") > matchScore(other, L"周杰论 晴天"));
    CHECK(matchScore(good, L"叶惠美") > matchScore(other, L"叶惠美"));
    auto parsed = parseTrack(Json::parse(
        R"({"id":123,"name":"晴天","ar":[{"name":"周杰伦"}],"al":{"id":22,"name":"叶惠美","picUrl":"https://p1.music.126.net/a.jpg"},"dt":269000})"));
    CHECK(parsed.id == 123 && parsed.duration == 269 && parsed.artist == L"周杰伦");
    auto partial = parseTrack(Json::parse(
        R"({"id":9,"name":null,"dt":null,"ar":null,"al":{"name":null,"picUrl":null}})"));
    CHECK(partial.id == 9 && partial.name.empty() && partial.duration == 0);
    CHECK(!playable(Json::parse(R"({"url":null,"code":200})")));
    CHECK(!playable(Json::parse(R"({"url":"https://x","freeTrialInfo":{"start":0,"end":30}})")));
    CHECK(playable(Json::parse(R"({"url":"https://x","freeTrialInfo":null})")));
    CHECK(isRiskCode(405) && isRiskCode(429) && isRiskCode(-462));
    CHECK(!isRiskCode(200) && !isRiskCode(801));
    Queue queue;
    CHECK(!queue.next());
    queue.assign({good, other}, 1);
    CHECK(queue.current()->id == 2);
    CHECK(!queue.next());
    queue.setRepeat(true);
    CHECK(queue.next() && queue.current()->id == 1);
    CHECK(queue.previous() && queue.current()->id == 2);
    // M8: explicit queue modes, and the queue edits a music player is expected to expose.
    {
        // Every mode name round-trips, and a listener's own words are accepted too.
        for (auto mode : queueModes()) {
            QueueMode parsed{};
            CHECK(parseQueueMode(queueModeName(mode), parsed) && parsed == mode);
        }
        QueueMode parsed{};
        CHECK(parseQueueMode("random", parsed) && parsed == QueueMode::Shuffle);
        CHECK(parseQueueMode("loop_one", parsed) && parsed == QueueMode::RepeatOne);
        CHECK(!parseQueueMode("nonsense", parsed));

        Queue sequential;
        sequential.assign({good, other, Track{.id = 3}}, 0);
        CHECK(sequential.next() && sequential.current()->id == 2);
        CHECK(sequential.next() && sequential.current()->id == 3);
        CHECK(!sequential.next() && sequential.current()->id == 3); // the end stops the queue

        Queue loop;
        loop.assign({good, other, Track{.id = 3}}, 2);
        loop.setMode(QueueMode::RepeatAll);
        CHECK(loop.next() && loop.current()->id == 1); // wraps to the first row
        CHECK(loop.previous() && loop.current()->id == 3);

        Queue single;
        single.assign({good, other}, 0);
        single.setMode(QueueMode::RepeatOne);
        CHECK(single.next() && single.current()->id == 1); // same row: the caller replays it
        CHECK(single.previous() && single.current()->id == 1);

        // Shuffle is reproducible for a given seed and never stops on a non-empty queue.
        Queue one, two;
        one.assign({good, other, Track{.id = 3}, Track{.id = 4}}, 0);
        two.assign({good, other, Track{.id = 3}, Track{.id = 4}}, 0);
        one.setMode(QueueMode::Shuffle, 7);
        two.setMode(QueueMode::Shuffle, 7);
        std::vector<int64_t> orderOne, orderTwo;
        for (int i = 0; i < 6; ++i) {
            one.next();
            two.next();
            orderOne.push_back(one.current()->id);
            orderTwo.push_back(two.current()->id);
        }
        CHECK(orderOne == orderTwo);
        CHECK(orderOne.size() == 6 && one.current()->id != 0);
        // Enabling shuffle on a playing track keeps that track as the first pick.
        Queue mid;
        mid.assign({good, other, Track{.id = 3}}, 1);
        mid.setMode(QueueMode::Shuffle, 3);
        CHECK(mid.current()->id == 2);
        CHECK(mid.next() && mid.current()->id != 2);
        // A full shuffle pass wraps over the same order, so stepping back is an exact inverse: a
        // failed playback can roll one step back without changing the order the caller asked for.
        Queue wrap;
        wrap.assign({good, other, Track{.id = 3}}, 0);
        wrap.setMode(QueueMode::Shuffle, 9);
        std::vector<int64_t> pass;
        for (int i = 0; i < 3; ++i) {
            wrap.next();
            pass.push_back(wrap.current()->id);
        }
        wrap.next(); // the pass ended and wrapped
        CHECK(wrap.current()->id == pass.front());
        CHECK(wrap.previous() && wrap.current()->id == pass.back());
        // The seed is authoritative: asking for shuffle again with the same seed must reproduce the
        // same walk even when the mode was already shuffle (an early return here would silently keep
        // the previous permutation).
        PlaybackSession reshuffle;
        reshuffle.assign({good, other, Track{.id = 3}, Track{.id = 4}}, 0);
        reshuffle.setMode(QueueMode::Shuffle, 21);
        reshuffle.setMode(QueueMode::Shuffle, 22);
        reshuffle.setMode(QueueMode::Shuffle, 21);
        std::vector<int64_t> seededWalk;
        for (int i = 0; i < 4; ++i) {
            reshuffle.move(1);
            seededWalk.push_back(reshuffle.currentTrack()->id);
        }
        PlaybackSession replays;
        replays.assign({good, other, Track{.id = 3}, Track{.id = 4}}, 0);
        replays.setMode(QueueMode::Shuffle, 21);
        std::vector<int64_t> replayWalk;
        for (int i = 0; i < 4; ++i) {
            replays.move(1);
            replayWalk.push_back(replays.currentTrack()->id);
        }
        CHECK(seededWalk == replayWalk && seededWalk.size() == 4);

        // Jump, remove and reorder all keep the listener on the row they are hearing.
        Queue edit;
        edit.assign({good, other, Track{.id = 3}}, 1);
        CHECK(edit.current()->id == 2);
        CHECK(edit.removeAt(0) && edit.current()->id == 2 && edit.tracks.size() == 2);
        CHECK(!edit.removeAt(9));
        CHECK(edit.moveTrack(1, 0) && edit.current()->id == 2); // follows the row, not the slot
        CHECK(!edit.moveTrack(0, 0) && !edit.moveTrack(0, 5));
        CHECK(edit.setIndex(0) && edit.current()->id == 3);
        CHECK(!edit.setIndex(0) && !edit.setIndex(9));
        CHECK(edit.append({Track{.id = 0}, Track{.id = 4}}) && edit.tracks.size() == 3);
        edit.assign({good, other}, 0);
        CHECK(edit.insertNext({Track{.id = 9}}) && edit.tracks.size() == 3);
        CHECK(edit.current()->id == 1 && edit.next() && edit.current()->id == 9);
        edit.clear();
        CHECK(edit.tracks.empty() && !edit.next() && !edit.previous() && !edit.current());
        // The session exposes the same edits and reports the mode in its snapshot.
        PlaybackSession modes;
        modes.assign({good, other, Track{.id = 3}}, 2);
        modes.setMode(QueueMode::Shuffle, 5);
        CHECK(modes.mode() == QueueMode::Shuffle && modes.snapshot().mode == QueueMode::Shuffle);
        CHECK(!modes.snapshot().repeat);
        CHECK(modes.playIndex(0) && modes.queueIndex() == 0);
        CHECK(modes.removeIndex(2) && modes.queueSize() == 2);
        CHECK(modes.moveTrack(0, 1) && modes.queueIndex() == 1);
        CHECK(modes.playNext({Track{.id = 8}}) && modes.queueSize() == 3);
        CHECK(modes.enqueue({Track{.id = 7}}) && modes.queueSize() == 4);
        modes.clearQueue();
        CHECK(modes.queueEmpty() && modes.queueSize() == 0);
    }
    // M1-A: one authoritative playback session instead of a private copy per entry point.
    {
        PlaybackSession headless;
        CHECK(headless.snapshot() == SessionSnapshot{});
        CHECK(!headless.move(1) && !headless.currentTrack());
        headless.assign({good, other}, 1);
        CHECK(headless.currentTrack() && headless.currentTrack()->id == 2);
        CHECK(headless.tracks().size() == 2 && headless.index() == 1);
        headless.setVolume(.4);
        headless.setQuality("hires");
        headless.setRepeat(true);
        CHECK(headless.move(1) && headless.currentTrack()->id == 1);
        auto first = headless.snapshot(), second = headless.snapshot();
        CHECK(first == second);
        CHECK(first.ids == std::vector<int64_t>({1, 2}) && first.index == 0 && first.repeat);
        CHECK(first.volume == .4 && first.requestedQuality == "hires" && first.revision > 0);
        // A partial or repeated metadata answer merges non-empty fields and never restarts a load.
        headless.assign({Track{.id = 3}}, 0);
        auto empty = headless.snapshot();
        headless.refresh(Track{.id = 3});
        CHECK(headless.snapshot() == empty);
        Track rich{.id = 3, .name = L"夜曲", .artist = L"周杰伦", .album = L"十一月的萧邦"};
        rich.duration = 227;
        headless.refresh(rich);
        CHECK(headless.tracks()[0].name == L"夜曲" && headless.tracks()[0].duration == 227);
        auto loaded = headless.snapshot();
        headless.refresh(rich);
        CHECK(headless.snapshot() == loaded);
        // A selection recorded without touching the device still merges into the queue.
        headless.assignId(42);
        headless.setCurrent(Track{.id = 42, .name = L"待播放"});
        CHECK(headless.snapshot().currentId == 42 && headless.actualQuality().empty());
        CHECK(headless.tracks().size() == 1 && headless.tracks()[0].name == L"待播放");
        // Request identity: a reply from a superseded request can never overwrite newer state.
        uint64_t older = headless.beginRequest();
        CHECK(headless.owns(older));
        uint64_t newer = headless.beginRequest();
        CHECK(!headless.owns(older) && headless.owns(newer) && !headless.owns(0));
        headless.cancelRequests();
        CHECK(!headless.owns(newer) && headless.snapshot().request > newer);
    }
    {
        Audio device;
        PlaybackSession live(&device);
        live.setVolume(.25);
        for (int i = 0; i < 200 && std::abs(device.state().volume - .25) > 1e-9; ++i)
            Sleep(10);
        CHECK(std::abs(device.state().volume - .25) < 1e-9);
        live.adjustVolume(.1);
        CHECK(std::abs(live.volume() - .35) < 1e-9);
        live.setVolume(4);
        CHECK(live.volume() == 1);
        for (int i = 0; i < 200 && std::abs(live.audioState().volume - 1) > 1e-9; ++i)
            Sleep(10);
        CHECK(std::abs(live.audioState().volume - 1) < 1e-9);
    }
    CHECK(urlEncode("a b+&") == "a%20b%2B%26");
    CHECK(hex(hash("abc", BCRYPT_MD5_ALGORITHM)) == "900150983cd24fb0d6963f7d28e17f72");
    auto plain = std::string("test encryption boundary \xe6\x99\xb4\xe5\xa4\xa9");
    auto encrypted = aes(bytes(plain), bytes("e82ckenh8dichen8"), true);
    CHECK(str(aes(encrypted, bytes("e82ckenh8dichen8"), false)) == plain);
    CHECK(base64(bytes("hello")) == "aGVsbG8=");
    CHECK(str(unbase64("aGVsbG8=")) == "hello");
    auto secret = bytes("a sensitive session");
    CHECK(unprotect(protect(secret)) == secret);
    CHECK(xeapiSign("123", "456") == "Zxlhz1h703SrGx8LpTse0SvvmmYaON97kpSllPu61Vs=");
    auto e = eapi("/api/test", "{\"a\":1}");
    CHECK(e == "4DC723619A991588865191FD2F319BAD0918BC9C604E1E84A5C3578922E3A7E8810405B5500AF5BEABA"
               "2DEAB687471586CE47DE62C9D523E260A0250C7F3AC2802B572BD7B95623F10A1D55EF99B9A8C");
    CHECK(webRsa(bytes("0123456789abcdef")).size() == 256);
    CHECK(anonymousUsername("0123456789abcdef") ==
          "MDEyMzQ1Njc4OWFiY2RlZiBGZEd0TGNER0VRL2JBUyttdVhYMDVBPT0=");
    std::array<uint8_t, 32> privateKey{};
    privateKey[0] = 1;
    CHECK(x25519Public(privateKey).size() == 32);
    Cover cover;
    cover.pixels.assign(16 * 16 * 3, 0);
    cover.width = cover.height = 16;
    for (size_t i = 0; i < cover.pixels.size(); i += 3) {
        cover.pixels[i] = 60;
        cover.pixels[i + 1] = 120;
        cover.pixels[i + 2] = 180;
    }
    auto theme = palette(cover);
    CHECK(theme.accent.b > theme.accent.r);
    CHECK(contrast(theme.text, theme.background) >= 4.5);
    Canvas canvas(80, 24, theme);
    canvas.text(78, 2, L"晴天abc", theme.text);
    CHECK(canvas.width() == 80 && canvas.height() == 24);
    CHECK(canvas.plain().size() > 0);
    CHECK(canvas.hit(-1, 0) == 0);
    canvas.button(2, 3, 10, L"PLAY", 7);
    CHECK(canvas.hit(5, 3) == 7 && canvas.hit(12, 3) == 0);
    Canvas selected(12, 3, theme);
    selected.text(1, 0, L"你好 A", theme.text);
    selected.text(1, 1, L"第二行", theme.text);
    CHECK(selected.selectedText(1, 0, 6, 0) == L"你好 A");
    CHECK(selected.selectedText(2, 0, 2, 0) == L"你");
    CHECK(selected.selectedText(1, 1, 1, 0).find(L"你好 A") != std::wstring::npos);
    selected.highlight(1, 0, 6, 0);
    CHECK(selected.selectedText(1, 0, 6, 0) == L"你好 A");
    auto invocation = parseCommand(L"  /SEARCH   夜的钢琴曲 石进  ");
    CHECK(invocation.verb == L"search" && invocation.args == L"夜的钢琴曲 石进");
    CHECK(parseCommand(L"晴天 周杰伦").verb == L"chat");
    CHECK(commandSuggestions(L"/vol").size() == 1 &&
          commandSuggestions(L"/vol")[0].name == L"volume");
    CHECK(commandSuggestions(L"/search abc").empty());
    CHECK(commandSuggestions(L"/unknown").empty());
    CHECK(commandSuggestions(L"/").size() > 15);
    CHECK(mascotPixel(15, 12, false).has_value());
    CHECK(!mascotPixel(-1, 0, false).has_value());
    int invoked = 0;
    McpServer mcp([&](const std::string& name, const Json& args) {
        ++invoked;
        // A damaged local session surfaces as a plain runtime_error; the host must still receive a
        // structured code instead of prose.
        if (name == "music_list_likes")
            throw std::runtime_error("Session file too large");
        return Json{{"name", name}, {"args", args}};
    });
    auto init = mcp.handle(Json{{"jsonrpc", "2.0"},
                                {"id", 1},
                                {"method", "initialize"},
                                {"params", Json{{"protocolVersion", "2025-11-25"}}}});
    CHECK(init.has_value() && (*init)["result"]["protocolVersion"] == "2025-11-25");
    CHECK((*init)["result"]["instructions"].get<std::string>().find("not implemented yet") ==
          std::string::npos);
    CHECK(!mcp.handle(Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}));
    auto listed = mcp.handle(Json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
    // The registry is the single source of truth, so adding a tool never needs a magic number here.
    CHECK((*listed)["result"]["tools"].size() == musicToolDefinitions().size());
    {
        std::set<std::string> names;
        for (auto& tool : (*listed)["result"]["tools"])
            names.insert(tool["name"].get<std::string>());
        std::set<std::string> expected;
        for (auto& tool : musicToolDefinitions())
            expected.insert(tool["name"].get<std::string>());
        CHECK(names == expected);
        CHECK(names.contains("music_queue_list") && names.contains("music_queue_edit"));
        // A tool's advertised effect must match what it does: music_open_collection only pages
        // through metadata, so no description may tell the model that it starts playback.
        for (auto& tool : musicToolDefinitions()) {
            auto description = tool.value("description", std::string());
            auto name = tool.value("name", std::string());
            if (name == "music_open_collection") {
                CHECK(tool["annotations"]["readOnlyHint"] == true);
                CHECK(description.find("never starts playback") != std::string::npos);
                continue;
            }
            if (description.find("music_open_collection") == std::string::npos)
                continue;
            CHECK(description.find("only lists") != std::string::npos ||
                  description.find("never starts playback") != std::string::npos);
        }
    }
    auto called = mcp.handle(
        Json{{"jsonrpc", "2.0"},
             {"id", 3},
             {"method", "tools/call"},
             {"params", Json{{"name", "music_get_status"}, {"arguments", Json::object()}}}});
    CHECK(invoked == 1 && !(*called)["result"]["isError"].get<bool>());
    auto unknown = mcp.handle(Json{{"jsonrpc", "2.0"},
                                   {"id", 4},
                                   {"method", "tools/call"},
                                   {"params", Json{{"name", "music_not_a_tool"}}}});
    CHECK((*unknown)["error"]["code"] == -32602 && invoked == 1);
    auto failedSession = mcp.handle(
        Json{{"jsonrpc", "2.0"},
             {"id", 5},
             {"method", "tools/call"},
             {"params", Json{{"name", "music_list_likes"}, {"arguments", Json::object()}}}});
    CHECK((*failedSession)["result"]["isError"] == true);
    CHECK((*failedSession)["result"]["structuredContent"]["error"] == "session_error");
    // M4-A: tool layer requires the attached session for playback control.
    {
        MusicService service;
        CHECK(!service.attached());
        bool needsPlayer = false;
        try {
            service.invoke("music_play", Json{{"id", 1}});
        } catch (const ToolError& e) {
            needsPlayer = e.code == "needs_player";
        }
        CHECK(needsPlayer);
        bool invalidAction = false;
        try {
            service.invoke("music_control", Json{{"action", "explode"}});
        } catch (const ToolError& e) {
            invalidAction = e.code == "invalid_arguments";
        }
        CHECK(invalidAction);
        bool missingValue = false;
        try {
            service.invoke("music_control", Json{{"action", "seek"}});
        } catch (const ToolError& e) {
            missingValue = e.code == "invalid_arguments";
        }
        CHECK(missingValue);
        service.attach([](const std::string& method, const Json& params) {
            return BridgeReply(true, "", "", Json{{"method", method}, {"params", params}});
        });
        CHECK(service.attached());
        auto played = service.invoke("music_play", Json{{"id", 7}});
        CHECK(played["method"] == "play" && played["params"]["kind"] == "song" &&
              played["params"]["id"] == 7);
        auto status = service.invoke("music_get_status", Json::object());
        CHECK(status["playerSessionAttached"] == true);
        CHECK(status["pending"] == Json::array({"agent_settings_form"}));
        // Identity is reported read-only so the model can tell whether "my playlists" exist.
        CHECK(status["account"].is_object() && status["account"]["signedIn"].is_boolean() &&
              status["account"]["nickname"].is_string());
        // Quality is a real listening operation, not a terminal-only setting.
        auto quality = service.invoke(
            "music_control", Json{{"action", "quality"}, {"level", "lossless"}});
        CHECK(quality["method"] == "control" && quality["params"]["action"] == "quality" &&
              quality["params"]["level"] == "lossless");
        bool badQuality = false;
        try {
            service.invoke("music_control", Json{{"action", "quality"}, {"level", "ultra"}});
        } catch (const ToolError& e) {
            badQuality = e.code == "invalid_arguments";
        }
        CHECK(badQuality);
        bool missingLevel = false;
        try {
            service.invoke("music_control", Json{{"action", "quality"}});
        } catch (const ToolError& e) {
            missingLevel = e.code == "invalid_arguments";
        }
        CHECK(missingLevel);
        // A whole series can be replaced in one call up to the player's own 1000-row limit; the
        // append family keeps its smaller 50-row bound so one call can never become a storm.
        Json thousand = Json::array();
        for (int i = 1; i <= 1000; ++i)
            thousand.push_back(i);
        thousand.push_back(1001);
        bool tooMany = false;
        try {
            service.invoke("music_queue_edit", Json{{"op", "replace"}, {"ids", thousand}});
        } catch (const ToolError& e) {
            tooMany = e.code == "invalid_arguments";
        }
        CHECK(tooMany);
        Json fiftyOne = Json::array();
        for (int i = 1; i <= 51; ++i)
            fiftyOne.push_back(i);
        bool appendTooMany = false;
        try {
            service.invoke("music_queue_edit", Json{{"op", "append"}, {"ids", fiftyOne}});
        } catch (const ToolError& e) {
            appendTooMany = e.code == "invalid_arguments";
        }
        CHECK(appendTooMany);
        int searches = 0, replacements = 0;
        MusicService series;
        series.attach([&](const std::string& method, const Json& params) {
            if (method == "status")
                return BridgeReply(true, "", "", Json::object());
            if (method == "search") {
                ++searches;
                int offset = params.value("offset", 0);
                Json items = Json::array();
                for (int i = offset; i < std::min(offset + 30, 1000); ++i)
                    items.push_back(Json{{"id", i + 1}});
                return BridgeReply(true, "", "", Json{{"total", 1000}, {"items", items}});
            }
            CHECK(method == "queue_edit");
            ++replacements;
            CHECK(params["op"] == "replace");
            return BridgeReply(true, "", "", Json{{"changed", true}});
        });
        for (int count : {10, 100, 1000}) {
            searches = replacements = 0;
            auto queued = series.invoke("music_recommend_queue",
                                        Json{{"intent", "测试风格"}, {"count", count},
                                             {"seed", 7}});
            CHECK(queued["requestedCount"] == count && queued["queuedCount"] == count);
            CHECK(replacements == 1 && searches >= 1);
        }
        CHECK(personaSearchSeed(resolvePersona("devil")).find(L"日语流行") !=
              std::wstring::npos);
        CHECK(usePersonaSearchSeed(L"随机播放十首歌"));
        CHECK(usePersonaSearchSeed(L"按你的品味，随机播放二十首歌"));
        CHECK(!usePersonaSearchSeed(L"随机播放十首周杰伦的歌"));
        int broadened = 0;
        MusicService broadSearch;
        broadSearch.attach([&](const std::string& method, const Json& params) {
            if (method == "status")
                return BridgeReply(true, "", "", Json::object());
            if (method == "search") {
                auto query = params.value("query", std::string());
                if (query == "日语流行") {
                    ++broadened;
                    return BridgeReply(true, "", "", Json{{"total", 20},
                        {"items", Json::array({Json{{"id", 1}}, Json{{"id", 2}}})}});
                }
                return BridgeReply(true, "", "", Json{{"total", 0},
                                                      {"items", Json::array()}});
            }
            // The auto fallback asks the player for the user's playlists when search came up
            // short; this stub has none, so the queue keeps exactly the two search hits.
            if (method == "playlists")
                return BridgeReply(true, "", "", Json{{"items", Json::array()}});
            CHECK(method == "queue_edit" && params["ids"].size() == 2);
            return BridgeReply(true, "", "", Json{{"changed", true}});
        });
        auto broadenedQueue = broadSearch.invoke(
            "music_recommend_queue", Json{{"intent", "日语流行 动画 J-ROCK"},
                                          {"count", 10}, {"seed", 7}});
        CHECK(broadenedQueue["queuedCount"] == 2 && broadened >= 1);
    }
    // Multi-song queueing must be able to draw from the user's own playlists, not only from a
    // keyword search: "从我账号歌单随机挑三十首" used to fail with zero candidates.
    {
        MusicService fromPlaylists;
        std::vector<int64_t> queuedIds;
        fromPlaylists.attach([&](const std::string& method, const Json& params) {
            if (method == "status")
                return BridgeReply(true, "", "", Json::object());
            if (method == "playlists") {
                CHECK(params["ownedOnly"] == false);
                return BridgeReply(true, "", "",
                                   Json{{"items", Json::array({Json{{"id", 11}},
                                                               Json{{"id", 12}}})}});
            }
            if (method == "search")
                return BridgeReply(true, "", "",
                                   Json{{"total", 0}, {"items", Json::array()}});
            if (method == "collection") {
                CHECK(params["kind"] == "playlist");
                auto base = params["id"].get<int64_t>() * 100;
                Json items = Json::array();
                for (int i = 0; i < 100; ++i)
                    items.push_back(Json{{"id", base + i}});
                return BridgeReply(true, "", "", Json{{"total", 100}, {"items", items}});
            }
            CHECK(method == "queue_edit" && params["op"] == "replace");
            for (auto& id : params["ids"])
                queuedIds.push_back(id.get<int64_t>());
            return BridgeReply(true, "", "", Json{{"changed", true}});
        });
        auto queued = fromPlaylists.invoke(
            "music_recommend_queue", Json{{"source", "playlists"}, {"count", 30}, {"seed", 3}});
        CHECK(queued["queuedCount"] == 30 && queued["source"] == "playlists" &&
              queuedIds.size() == 30);
        for (auto id : queuedIds)
            CHECK(id > 100); // every id came from a playlist collection, never from a search
        queuedIds.clear();
        // No count named: the documented default applies instead of an error.
        auto defaulted =
            fromPlaylists.invoke("music_recommend_queue", Json{{"source", "playlists"}});
        CHECK(defaulted["requestedCount"] == 30 && queuedIds.size() == 30);
        // auto searches first and only falls back to the playlists when the search found nothing,
        // which is the case that used to answer "没找到" for a playlist request.
        queuedIds.clear();
        auto autoFallback = fromPlaylists.invoke(
            "music_recommend_queue", Json{{"intent", "无结果关键词"}, {"count", 5}});
        CHECK(autoFallback["queuedCount"] == 5 &&
              autoFallback["source"] == "search+playlists" && queuedIds.size() == 5);
    }
    // Liked songs are a listening source too: they are account state, so a cold service refreshes
    // them once and can then queue or page them.
    {
        auto previousDir = []() {
            std::wstring buffer(1024, L'\0');
            auto size = GetEnvironmentVariableW(L"MORA_DATA_DIR", buffer.data(), 1024);
            return size > 0 ? std::wstring(buffer.data(), size) : std::wstring();
        }();
        auto temporary = std::filesystem::temp_directory_path() / "mora-likes-test";
        std::filesystem::remove_all(temporary);
        SetEnvironmentVariableW(L"MORA_DATA_DIR", temporary.wstring().c_str());
        auto transport = [](std::wstring url, std::string, std::wstring, size_t,
                            bool) -> Response {
            Response response;
            response.status = 200;
            if (url.find(L"qrcode/unikey") != std::wstring::npos)
                response.body = R"({"code":200,"unikey":"likes-key"})";
            else if (url.find(L"client/login") != std::wstring::npos) {
                response.body = R"({"code":803})";
                response.cookies = {"MUSIC_U=fixture-secret; Path=/; HttpOnly; Secure"};
            } else if (url.find(L"account/get") != std::wstring::npos)
                response.body = R"({"code":200,"profile":{"userId":42,"nickname":"fixture-me"}})";
            else if (url.find(L"like/get") != std::wstring::npos)
                response.body = R"({"code":200,"ids":[101,102,103,104,105]})";
            else
                response.body = R"({"code":200})";
            return response;
        };
        {
            Api seed(temporary, transport);
            seed.qrKey();
            CHECK(seed.qrCheck("likes-key") == 803 && seed.likes.size() == 5);
        }
        MusicService fromLikes;
        fromLikes.useTransport(transport);
        std::vector<int64_t> queuedIds;
        fromLikes.attach([&](const std::string& method, const Json& params) {
            if (method == "status")
                return BridgeReply(true, "", "", Json::object());
            CHECK(method == "queue_edit");
            for (auto& id : params["ids"])
                queuedIds.push_back(id.get<int64_t>());
            return BridgeReply(true, "", "", Json{{"changed", true}});
        });
        auto queued = fromLikes.invoke("music_recommend_queue",
                                       Json{{"source", "liked"}, {"count", 3}, {"seed", 5}});
        CHECK(queued["queuedCount"] == 3 && queued["source"] == "liked" && queuedIds.size() == 3);
        for (auto id : queuedIds)
            CHECK(id >= 101 && id <= 105);
        auto page = fromLikes.invoke("music_list_likes", Json{{"limit", 2}, {"offset", 0}});
        CHECK(page["total"] == 5 && page["items"].size() == 2);
        auto tail = fromLikes.invoke("music_list_likes", Json{{"limit", 50}, {"offset", 4}});
        CHECK(tail["items"].size() == 1);
        // One read answers "is this song hearted", so the model never has to page the whole list.
        auto hearted = fromLikes.invoke("music_get_like", Json{{"songId", 103}});
        CHECK(hearted["liked"] == true && hearted["likedTotal"] == 5);
        auto stranger = fromLikes.invoke("music_get_like", Json{{"songId", 999}});
        CHECK(stranger["liked"] == false);
        std::filesystem::remove_all(temporary);
        SetEnvironmentVariableW(L"MORA_DATA_DIR", previousDir.empty() ? nullptr
                                                                     : previousDir.c_str());
    }
    // M3: the model adapter shapes tools/messages and stays inside its budgets.
    {
        CHECK(AgentConfig{}.baseUrl ==
              "https://opencode.ai/inference/go/openai/v1/chat/completions");
        auto tools = agentToolsFrom(musicToolDefinitions());
        CHECK(tools.size() == musicToolDefinitions().size());
        CHECK(tools[0]["type"] == "function" && tools[0]["function"]["name"].is_string());
        CHECK(tools[0]["function"]["parameters"]["type"] == "object");
        Json response = {{"choices", Json::array({Json{{"message", Json{{"content", "你好"}}}}})}};
        auto reply = parseAssistant(response);
        CHECK(reply.text == L"你好" && reply.calls.empty());
        Json calling = {{"choices",
                         Json::array({Json{{"message",
                                            Json{{"content", nullptr},
                                                 {"tool_calls",
                                                  Json::array({Json{{"id", "call-1"},
                                                                    {"type", "function"},
                                                                    {"function",
                                                                     Json{{"name", "music_search"},
                                                                          {"arguments",
                                                                           "{\"query\":\"夜的钢琴曲\"}"}}}}})}}}}})}};
        auto parsed = parseAssistant(calling);
        CHECK(parsed.calls.size() == 1 && parsed.calls[0].name == "music_search");
        CHECK(parsed.calls[0].arguments["query"] == "夜的钢琴曲");
        bool providerError = false;
        try {
            parseAssistant(Json{{"error", Json{{"message", "bad key"}}}});
        } catch (const AgentError& error) {
            providerError = error.code == "provider_error";
        }
        CHECK(providerError);
        // The loop must respect the tool budget and feed results back as tool messages.
        auto previousDir = []() {
            std::wstring buffer(1024, L'\0');
            auto size = GetEnvironmentVariableW(L"MORA_DATA_DIR", buffer.data(), 1024);
            return size > 0 ? std::wstring(buffer.data(), size) : std::wstring();
        }();
        auto temporary = std::filesystem::temp_directory_path() / "mora-agent-test";
        SetEnvironmentVariableW(L"MORA_DATA_DIR", temporary.wstring().c_str());
        CHECK(!agentKeyPresent());
        auto noKey = runChatLoop(AgentConfig{}, "播放一首歌", [](const std::string&, const Json&) {
            return Json::object();
        });
        CHECK(!noKey.ok && noKey.code == "missing_key");
        saveAgentKey("test-key-not-a-secret");
        CHECK(agentKeyPresent() && loadAgentKey() == "test-key-not-a-secret");
        clearAgentKey();
        CHECK(!agentKeyPresent() && !std::filesystem::exists(temporary / "agent.bin"));
        saveAgentKey("test-key-not-a-secret");
        int modelCalls = 0, executed = 0;
        auto outcome = runChatLoop(
            AgentConfig{}, "找一首夜的钢琴曲",
            [&executed](const std::string& name, const Json& arguments) {
                ++executed;
                CHECK(name == "music_search");
                CHECK(arguments["query"] == "夜的钢琴曲");
                return Json{{"results", Json::array({Json{{"id", 149229}}})}};
            },
            [&modelCalls](const AgentConfig&, const Json& messages, const Json& tools) {
                ++modelCalls;
                // A search request receives read-only tools, not playback tools.
                CHECK(!tools.empty());
                for (const auto& tool : tools)
                    CHECK(tool["function"]["name"] != "music_play" &&
                          tool["function"]["name"] != "mora_create_persona");
                CHECK(messages.size() >= 2 && messages[0]["role"] == "system" &&
                      messages[1]["role"] == "user");
                if (modelCalls == 1)
                    return Json{{"choices",
                                 Json::array({Json{{"message",
                                                    Json{{"content", nullptr},
                                                         {"tool_calls",
                                                          Json::array({Json{{"id", "call-1"},
                                                                            {"type", "function"},
                                                                            {"function",
                                                                             Json{{"name",
                                                                                   "music_search"},
                                                                                  {"arguments",
                                                                                   "{\"query\":\"夜的钢琴曲\"}"}}}}})}}}}})}};
                CHECK(messages.back()["role"] == "tool" &&
                      messages.back()["tool_call_id"] == "call-1");
                CHECK(messages.back()["content"].get<std::string>().find("149229") !=
                      std::string::npos);
                return Json{{"choices",
                             Json::array({Json{{"message", Json{{"content", "找到《夜的钢琴曲》"}}}}})}};
            });
        CHECK(outcome.ok && outcome.toolCalls == 1 && executed == 1 && modelCalls == 2);
        CHECK(outcome.text == L"找到《夜的钢琴曲》");
        CHECK(outcome.trace.size() == 1 && outcome.trace[0]["tool"] == "music_search");
        // A failing tool must be reported to the model, never retried silently.
        int failCalls = 0;
        auto failed = runChatLoop(
            AgentConfig{}, "播放", [](const std::string&, const Json&) -> Json {
                throw ToolError("needs_player", "no MORA player is attached");
            },
            [&failCalls](const AgentConfig&, const Json& messages, const Json&) {
                ++failCalls;
                if (failCalls == 1)
                    return Json{{"choices",
                                 Json::array({Json{{"message",
                                                    Json{{"content", nullptr},
                                                         {"tool_calls",
                                                          Json::array({Json{{"id", "c"},
                                                                            {"type", "function"},
                                                                            {"function",
                                                                             Json{{"name", "music_play"},
                                                                                  {"arguments", "{}"}}}}})}}}}})}};
                CHECK(messages.back()["content"].get<std::string>().find("needs_player") !=
                      std::string::npos);
                return Json{{"choices",
                             Json::array({Json{{"message", Json{{"content", "先运行 mora"}}}}})}};
            });
        CHECK(failed.ok && failed.trace[0]["resultSummary"].get<std::string>().find(
                                "needs_player") != std::string::npos);
        // A large multi-byte tool result must never be truncated mid-character: the trace is dumped.
        auto wide = runChatLoop(
            AgentConfig{}, "长结果",
            [](const std::string&, const Json&) {
                std::string payload;
                for (int i = 0; i < 80; ++i)
                    payload += "夜的钢琴曲" + std::to_string(i) + "、";
                return Json{{"items", payload}};
            },
            [](const AgentConfig&, const Json&, const Json&) {
                return Json{{"choices",
                             Json::array({Json{{"message",
                                                Json{{"content", "好的"}}}}})}};
            });
        CHECK(wide.ok && wide.trace.size() == 0);
        auto withTool = runChatLoop(
            AgentConfig{}, "长结果",
            [](const std::string&, const Json&) {
                std::string payload;
                for (int i = 0; i < 80; ++i)
                    payload += "夜的钢琴曲" + std::to_string(i) + "、";
                return Json{{"items", payload}};
            },
            [](const AgentConfig&, const Json& messages, const Json&) {
                if (messages.size() <= 2)
                    return Json{{"choices",
                                 Json::array({Json{{"message",
                                                    Json{{"content", nullptr},
                                                         {"tool_calls",
                                                          Json::array({Json{{"id", "c1"},
                                                                            {"type", "function"},
                                                                            {"function",
                                                                             Json{{"name", "music_search"},
                                                                                  {"arguments", "{}"}}}}})}}}}})}};
                return Json{{"choices",
                             Json::array({Json{{"message", Json{{"content", "好的"}}}}})}};
            });
        CHECK(withTool.ok && withTool.trace.size() == 1);
        CHECK(withTool.trace.dump().find("resultSummary") != std::string::npos);
        AgentConfig shortBudget;
        shortBudget.maxRounds = 1;
        int finalCalls = 0;
        auto playedAtLimit = runChatLoop(
            shortBudget, "播放歌曲",
            [](const std::string& name, const Json&) {
                CHECK(name == "music_play");
                return Json{{"accepted", true}};
            },
            [&finalCalls](const AgentConfig&, const Json& messages, const Json& tools) {
                ++finalCalls;
                if (!tools.empty())
                    return Json{{"choices", Json::array({Json{{"message", Json{
                        {"content", nullptr}, {"tool_calls", Json::array({Json{
                            {"id", "play-1"}, {"type", "function"},
                            {"function", Json{{"name", "music_play"}, {"arguments", "{}"}}}}})}}}}})}};
                CHECK(messages.back()["role"] == "tool");
                return Json{{"choices", Json::array({Json{{"message", Json{{"content", "已提交播放。"}}}}})}};
            });
        CHECK(playedAtLimit.ok && playedAtLimit.code.empty() &&
              playedAtLimit.text == L"已提交播放。" && finalCalls == 2 &&
              playedAtLimit.toolCalls == 1);
        auto toolRequest = [](const std::string& id, const std::string& name) {
            return Json{{"choices", Json::array({Json{{"message", Json{
                {"content", nullptr}, {"tool_calls", Json::array({Json{
                    {"id", id}, {"type", "function"},
                    {"function", Json{{"name", name}, {"arguments", "{}"}}}}})}}}}})}};
        };
        for (bool contextLimit : {false, true}) {
            AgentConfig limit;
            limit.maxRounds = 3;
            limit.maxToolCalls = contextLimit ? 8 : 1;
            limit.maxContextMessages = contextLimit ? 4 : 64;
            int writes = 0, requests = 0;
            auto recovered = runChatLoop(
                limit, "播放一首歌",
                [&writes](const std::string&, const Json&) {
                    ++writes;
                    return Json{{"accepted", true}};
                },
                [&](const AgentConfig&, const Json& messages, const Json& tools) {
                    ++requests;
                    if (requests == 1)
                        return toolRequest("first", "music_queue_edit");
                    CHECK(messages.back()["role"] == "tool");
                    if (!tools.empty())
                        return toolRequest("extra", "music_queue_edit");
                    return Json{{"choices", Json::array({Json{{"message", Json{{"content", "已换好十首。"}}}}})}};
                });
            CHECK(recovered.ok && recovered.code.empty() &&
                  recovered.text == L"已换好十首。" && writes == 1 && requests == 2);
        }
        int markupRequests = 0;
        auto markup = runChatLoop(
            AgentConfig{}, "换一首歌",
            [](const std::string&, const Json&) {
                return Json{{"changed", true}};
            },
            [&](const AgentConfig&, const Json&, const Json& tools) {
                ++markupRequests;
                if (markupRequests == 1)
                    return Json{{"choices", Json::array({Json{{"message", Json{
                        {"content", nullptr}, {"tool_calls", Json::array({Json{
                            {"id", "queue"}, {"type", "function"},
                            {"function", Json{{"name", "music_queue_edit"},
                                               {"arguments", "{\"op\":\"replace\",\"ids\":[1,2,3]}"}}}}})}}}}})}};
                CHECK(tools.empty() || markupRequests == 2);
                return Json{{"choices", Json::array({Json{{"message", Json{
                    {"content", "<|DSML|calls><|DSML|invoke name=\"music_play\">"}}}}})}};
            });
        CHECK(markup.ok && markupRequests == 2 &&
              markup.text == L"已替换播放队列，共 3 首歌。");
        int seriesModelCalls = 0, seriesToolCalls = 0;
        auto seriesAnswer = runChatLoop(
            AgentConfig{}, "按你的品味随机播放一千首歌曲",
            [&](const std::string& name, const Json& args) {
                ++seriesToolCalls;
                CHECK(name == "music_recommend_queue" && args["count"] == 1000);
                return Json{{"requestedCount", 1000}, {"queuedCount", 987},
                            {"changed", true}};
            },
            [&](const AgentConfig&, const Json&, const Json& tools) {
                ++seriesModelCalls;
                if (!tools.empty()) {
                    // The one-call queue tool, one batch queue edit for ids the model already has,
                    // and single-shot read-only context: never a per-song playback loop.
                    std::set<std::string> names;
                    for (const auto& tool : tools)
                        names.insert(tool["function"]["name"].get<std::string>());
                    CHECK(names.contains("music_recommend_queue"));
                    CHECK(names.contains("music_queue_edit"));
                    CHECK(!names.contains("music_play") && !names.contains("music_play_random") &&
                          !names.contains("music_search"));
                    return Json{{"choices", Json::array({Json{{"message", Json{
                        {"content", nullptr}, {"tool_calls", Json::array({Json{
                            {"id", "series"}, {"type", "function"},
                            {"function", Json{{"name", "music_recommend_queue"},
                                               {"arguments", "{\"intent\":\"日语流行\",\"count\":1000}"}}}}})}}}}})}};
                }
                return Json{{"choices", Json::array({Json{{"message", Json{{"content", "已排入987首，其余没有找到。"}}}}})}};
            });
        CHECK(seriesAnswer.ok && seriesModelCalls == 2 && seriesToolCalls == 1 &&
              seriesAnswer.text.find(L"987") != std::wstring::npos);
        int chatToolCalls = 0, chatModelCalls = 0;
        auto opinion = runChatLoop(
            AgentConfig{}, "随便聊聊你对现在播放的歌的看法",
            [&](const std::string&, const Json&) {
                ++chatToolCalls;
                return Json::object();
            },
            [&](const AgentConfig&, const Json&, const Json& tools) {
                ++chatModelCalls;
                CHECK(tools.empty());
                return Json{{"choices", Json::array({Json{{"message", Json{{"content", "这首歌的节奏很有冲劲。"}}}}})}};
            });
        CHECK(opinion.ok && chatModelCalls == 1 && chatToolCalls == 0);
        int statusOnlyCalls = 0;
        auto currentSongQuestion = runChatLoop(
            AgentConfig{}, "现在播放的是什么歌？",
            [&](const std::string&, const Json&) {
                ++statusOnlyCalls;
                return Json::object();
            },
            [&](const AgentConfig&, const Json&, const Json& tools) {
                for (const auto& tool : tools)
                    CHECK(tool["function"]["name"] != "music_play" &&
                          tool["function"]["name"] != "music_recommend_queue");
                return Json{{"choices", Json::array({Json{{"message", Json{{"content", "我来看看当前曲目。"}}}}})}};
            });
        CHECK(currentSongQuestion.ok && statusOnlyCalls == 0);
        int illegalCalls = 0;
        auto illegalPlayback = runChatLoop(
            AgentConfig{}, "聊聊这首歌",
            [&](const std::string&, const Json&) {
                ++illegalCalls;
                return Json::object();
            },
            [&](const AgentConfig&, const Json&, const Json&) {
                static int rounds = 0;
                if (++rounds == 1)
                    return toolRequest("unexpected", "music_play");
                return Json{{"choices", Json::array({Json{{"message", Json{{"content", "我们聊聊它的旋律。"}}}}})}};
            });
        CHECK(illegalPlayback.ok && illegalCalls == 0);
        int stubbornCalls = 0, stubbornWrites = 0;
        auto stubborn = runChatLoop(
            AgentConfig{}, "播放一首歌",
            [&](const std::string&, const Json&) {
                ++stubbornWrites;
                return Json{{"accepted", true}};
            },
            [&](const AgentConfig&, const Json&, const Json& tools) {
                ++stubbornCalls;
                CHECK(stubbornCalls == 1 || tools.empty());
                return toolRequest("again", "music_play");
            });
        CHECK(stubborn.ok && stubbornWrites == 1 && stubbornCalls == 2 &&
              stubborn.text == L"已提交播放请求。");
        int failedSeriesCalls = 0;
        auto failedSeries = runChatLoop(
            AgentConfig{}, "随机播放二十首歌",
            [&](const std::string&, const Json&) -> Json {
                ++failedSeriesCalls;
                throw ToolError("no_candidates", "no songs available");
            },
            [&](const AgentConfig&, const Json&, const Json& tools) {
                CHECK(!tools.empty());
                return toolRequest("series-failed", "music_recommend_queue");
            });
        CHECK(failedSeries.ok && failedSeriesCalls == 1 &&
              failedSeries.text.find(L"没找到") != std::wstring::npos);
        // A recommendation the user wants to listen to is an operation even without the word
        // "播放": this exact phrasing used to be classified as small talk and lost every queue tool.
        int recommendCalls = 0, recommendToolCalls = 0;
        auto recommended = runChatLoop(
            AgentConfig{}, "推荐三十首歌我听听，并及时锐评",
            [&](const std::string& name, const Json& args) {
                ++recommendToolCalls;
                CHECK(name == "music_recommend_queue" && args["count"] == 30);
                return Json{{"requestedCount", 30}, {"queuedCount", 30}, {"changed", true}};
            },
            [&](const AgentConfig&, const Json&, const Json& tools) {
                ++recommendCalls;
                std::set<std::string> names;
                for (const auto& tool : tools)
                    names.insert(tool["function"]["name"].get<std::string>());
                if (recommendCalls == 1) {
                    CHECK(names.contains("music_recommend_queue"));
                    CHECK(!names.contains("music_play") && !names.contains("music_play_random"));
                    return Json{{"choices", Json::array({Json{{"message", Json{
                        {"content", nullptr}, {"tool_calls", Json::array({Json{
                            {"id", "queue-thirty"}, {"type", "function"},
                            {"function", Json{{"name", "music_recommend_queue"},
                                               {"arguments", "{\"intent\":\"日语流行\",\"count\":30}"}}}}})}}}}})}};
                }
                CHECK(names.empty());
                return Json{{"choices", Json::array({Json{{"message", Json{
                    {"content", "三十首都排好了，先听前几首。"}}}}})}};
            });
        CHECK(recommended.ok && recommendToolCalls == 1 && recommendCalls == 2 &&
              recommended.text.find(L"三十首") != std::wstring::npos);
        std::filesystem::remove_all(temporary);
        SetEnvironmentVariableW(L"MORA_DATA_DIR", previousDir.empty() ? nullptr
                                                                     : previousDir.c_str());
    }
    // M4-B: playlist ownership must be decided from a resolved account id. A fresh service used to
    // compare the creator against a not-yet-loaded uid (0), so every playlist - including the
    // user's own - was reported as somebody else's and lost its write actions.
    {
        auto previousDir = []() {
            std::wstring buffer(1024, L'\0');
            auto size = GetEnvironmentVariableW(L"MORA_DATA_DIR", buffer.data(), 1024);
            return size > 0 ? std::wstring(buffer.data(), size) : std::wstring();
        }();
        auto temporary = std::filesystem::temp_directory_path() / "mora-owned-test";
        std::filesystem::remove_all(temporary);
        SetEnvironmentVariableW(L"MORA_DATA_DIR", temporary.wstring().c_str());
        auto transport = [](std::wstring url, std::string, std::wstring, size_t,
                            bool) -> Response {
            Response response;
            response.status = 200;
            if (url.find(L"qrcode/unikey") != std::wstring::npos)
                response.body = R"({"code":200,"unikey":"owned-key"})";
            else if (url.find(L"client/login") != std::wstring::npos) {
                response.body = R"({"code":803})";
                response.cookies = {"MUSIC_U=fixture-secret; Path=/; HttpOnly; Secure"};
            } else if (url.find(L"account/get") != std::wstring::npos)
                response.body =
                    R"({"code":200,"profile":{"userId":42,"nickname":"fixture-me"}})";
            else if (url.find(L"like/get") != std::wstring::npos)
                response.body = R"({"code":200,"ids":[123]})";
            else if (url.find(L"user/playlist") != std::wstring::npos)
                response.body = R"({"code":200,"playlist":[{"id":11,"name":"mine","creator":{"userId":42,"nickname":"me"}},{"id":12,"name":"other","creator":{"userId":99,"nickname":"other"}}]})";
            else
                response.body = R"({"code":200})";
            return response;
        };
        // Seed a real saved session through the public login path, then drop the cached account id:
        // a cold start can hold a valid cookie before account() has ever run, and that is exactly
        // the state in which the ordering of "list first, then read uid" matters.
        {
            Api seed(temporary, transport);
            seed.qrKey();
            CHECK(seed.qrCheck("owned-key") == 803 && seed.loggedIn() && seed.uid == 42);
            std::ifstream in(temporary / L"session.bin", std::ios::binary);
            Bytes blob((std::istreambuf_iterator<char>(in)), {});
            auto stored = Json::parse(str(unprotect(blob)));
            CHECK(stored.is_object() && stored.contains("cookies"));
            stored.erase("uid");
            stored.erase("nickname");
            auto clear = bytes(stored.dump());
            auto encrypted = protect(clear);
            std::ofstream out(temporary / L"session.bin", std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(encrypted.data()),
                      static_cast<std::streamsize>(encrypted.size()));
        }
        MusicService owned;
        owned.useTransport(transport);
        CHECK(owned.uid() == 0); // nothing cached yet: the cookie alone carries no account id
        auto mine = owned.invoke("music_list_playlists",
                                 Json{{"ownedOnly", true}, {"limit", 50}});
        CHECK(mine["total"] == 1 && mine["items"].size() == 1);
        CHECK(mine["items"][0]["id"] == 11 && mine["items"][0]["owner"] == true &&
              mine["items"][0]["canWrite"] == true);
        CHECK(owned.uid() == 42 && owned.nickname() == L"fixture-me");
        auto all = owned.invoke("music_list_playlists", Json{{"ownedOnly", false}});
        CHECK(all["total"] == 2 && all["items"].size() == 2);
        CHECK(all["items"][0]["owner"] == true && all["items"][0]["canWrite"] == true);
        CHECK(all["items"][1]["id"] == 12 && all["items"][1]["owner"] == false &&
              all["items"][1]["canWrite"] == false);
        // The identity survives releaseBackend(): a thin adapter drops the backend between calls
        // and must still know who it is talking to.
        owned.releaseBackend();
        CHECK(owned.uid() == 42 && owned.nickname() == L"fixture-me");
        std::filesystem::remove_all(temporary);
        SetEnvironmentVariableW(L"MORA_DATA_DIR", previousDir.empty() ? nullptr
                                                                     : previousDir.c_str());
    }
    // Personas: five built-ins, a validated custom store, and one avatar box shared by all.
    {
        auto previousDir = []() {
            std::wstring buffer(1024, L'\0');
            auto size = GetEnvironmentVariableW(L"MORA_DATA_DIR", buffer.data(), 1024);
            return size > 0 ? std::wstring(buffer.data(), size) : std::wstring();
        }();
        auto temporary = std::filesystem::temp_directory_path() / "mora-persona-test";
        std::filesystem::remove_all(temporary);
        SetEnvironmentVariableW(L"MORA_DATA_DIR", temporary.wstring().c_str());

        const auto& builtins = builtinPersonas();
        CHECK(builtins.size() == 5);
        CHECK(builtins.front().id == "amu");
        CHECK(resolvePersona("fox").name == L"小灰狐娘");
        CHECK(resolvePersona("does-not-exist").id == "amu");
        CHECK(personaHasDrawnAvatar("amu"));
        // All five built-ins ship drawn art; the generator pipeline writes these directories.
        for (const auto& persona : builtins)
            CHECK(personaHasDrawnAvatar(persona.id));
        CHECK(personaAvatarPixel("fox", 24, 26).has_value());
        CHECK(!personaAvatarPixel("does-not-exist", 4, 4).has_value());
        CHECK(!personaAvatarPixel("amu", -1, 0).has_value());
        CHECK(!personaAvatarPixel("amu", 0, personaAvatarHeight).has_value());

        auto prompt = personaSystemPrompt(resolvePersona("daidai"), L"阿木");
        CHECK(prompt.find(L"小宅女呆呆") != std::wstring::npos);
        CHECK(prompt.find(L"阿木") != std::wstring::npos);
        CHECK(!personaSearchSeed(resolvePersona("mushroom")).empty());
        // A generated avatar exists for a persona without drawn art, and stays inside the box.
        CHECK(personaGeneratedPixel(resolvePersona("custom-x"), 24, 25).has_value());
        CHECK(!personaGeneratedPixel(resolvePersona("custom-x"), -1, 0).has_value());
        CHECK(!personaGeneratedPixel(resolvePersona("custom-x"), 0, personaAvatarHeight)
                   .has_value());

        CHECK(loadCustomPersonas().empty());
        bool badId = false;
        try {
            createCustomPersona("amu", L"冒名", L"x", L"y", L"z", L"");
        } catch (const std::invalid_argument&) {
            badId = true;
        }
        CHECK(badId);
        bool badChars = false;
        try {
            createCustomPersona("bad id!", L"名字", L"x", L"y", L"z", L"");
        } catch (const std::invalid_argument&) {
            badChars = true;
        }
        CHECK(badChars);
        bool emptyVoice = false;
        try {
            createCustomPersona("quiet", L"安静人", L"x", L"   ", L"z", L"");
        } catch (const std::invalid_argument&) {
            emptyVoice = true;
        }
        CHECK(emptyVoice);

        auto created = createCustomPersona("nightowl", L"夜猫子", L"深夜推荐官",
                                           L"慵懒低声，爱在凌晨分享", L"氛围电子与后摇", L"夜里好。");
        CHECK(!created.builtin && created.name == L"夜猫子");
        // The palette is fixed per id, so the same persona keeps the same colours.
        CHECK(created.accent.r == resolvePersona("nightowl").accent.r);
        auto reloaded = loadCustomPersonas();
        CHECK(reloaded.size() == 1 && reloaded.front().id == "nightowl");
        CHECK(allPersonas().size() == 6);
        bool duplicate = false;
        try {
            createCustomPersona("nightowl", L"重复", L"", L"x", L"y", L"");
        } catch (const std::invalid_argument&) {
            duplicate = true;
        }
        CHECK(duplicate);
        // A hostile name is flattened rather than stored with control characters.
        auto squashed = createCustomPersona("squash", L"a\r\nb\tc", L"", L"voice", L"taste", L"");
        CHECK(squashed.name == L"a  b c");
        CHECK(loadCustomPersonas().size() == 2);

        std::filesystem::remove_all(temporary);
        SetEnvironmentVariableW(L"MORA_DATA_DIR", previousDir.empty() ? nullptr
                                                                     : previousDir.c_str());
    }
    // M4-B: account writes are explicit state, verified before touching the account.
    {
        MusicService service;
        bool noPlayer = false;
        try {
            service.invoke("music_set_like", Json{{"songId", 7}, {"liked", true}});
        } catch (const ToolError& e) {
            noPlayer = e.code == "needs_player";
        }
        CHECK(noPlayer);
        bool noPlayerAdd = false;
        try {
            service.invoke("music_add_to_playlist", Json{{"songId", 7}, {"playlistId", 42}});
        } catch (const ToolError& e) {
            noPlayerAdd = e.code == "needs_player";
        }
        CHECK(noPlayerAdd);
        bool missingLiked = false;
        try {
            service.invoke("music_set_like", Json{{"songId", 7}});
        } catch (const ToolError& e) {
            missingLiked = e.code == "invalid_arguments";
        }
        CHECK(missingLiked);
        std::vector<std::pair<std::string, Json>> calls;
        service.attach([&calls](const std::string& method, const Json& params) {
            calls.push_back({method, params});
            if (method == "playlists")
                return BridgeReply(true, "", "",
                                   Json{{"total", 1},
                                        {"items", Json::array({Json{{"id", 42},
                                                                    {"name", "测试歌单"},
                                                                    {"owner", true},
                                                                    {"canWrite", true}}})}});
            if (method == "like")
                return BridgeReply(true, "", "", Json{{"songId", params["id"]}});
            return BridgeReply(true, "", "", Json{{"status", "ok"}});
        });
        auto liked = service.invoke("music_set_like", Json{{"songId", 7}, {"liked", true}});
        CHECK(liked["state"] == "liked" && liked["songId"] == 7);
        CHECK(calls.back().first == "like" && calls.back().second["liked"] == true);
        auto added = service.invoke("music_add_to_playlist",
                                    Json{{"songId", 7}, {"playlistId", 42},
                                         {"operationId", "op-1"}});
        CHECK(added["status"] == "added" && added["operationId"] == "op-1");
        CHECK(calls.back().first == "add" && calls.back().second["playlistId"] == 42);
        // A playlist this account does not own must be refused before any write.
        bool notOwned = false;
        try {
            service.invoke("music_add_to_playlist", Json{{"songId", 7}, {"playlistId", 43}});
        } catch (const ToolError& e) {
            notOwned = e.code == "no_permission";
        }
        CHECK(notOwned && calls.back().first == "playlists");
        auto listed = service.invoke("music_list_playlists", Json::object());
        CHECK(listed["total"] == 1 && listed["items"][0]["canWrite"] == true);
    }
    // M6: user settings survive a restart and bad values never break the player.
    {
        auto directory = std::filesystem::temp_directory_path() /
                         (L"mora-settings-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::remove_all(directory);
        auto defaults = Settings::load(directory);
        CHECK(defaults.volume == .65 && defaults.quality == "lossless" && defaults.motion &&
              !defaults.repeat && defaults.history.empty() && !defaults.guideDone);
        Settings settings;
        settings.volume = .35;
        settings.quality = "hires";
        settings.queueMode = QueueMode::RepeatAll; // authoritative; `repeat` is only the legacy mirror
        settings.motion = false;
        settings.userName = L"阿木的听众";
        settings.personaId = "fox";
        settings.outputDeviceId = 3;
        settings.guideDone = true;
        settings.reactEnabled = true;
        settings.reactMinSeconds = 120;
        settings.reactMaxChars = 40;
        settings.contextLimitTokens = 65536;
        settings.autoCompress = false;
        settings.reviewChars = 1200;
        settings.history = {L"/search 晴天", L"/volume 35"};
        CHECK(settings.save(directory));
        auto loaded = Settings::load(directory);
        CHECK(loaded.userName == L"阿木的听众");
        CHECK(loaded.personaId == "fox");
        CHECK(loaded.outputDeviceId == 3);
        CHECK(loaded.guideDone);
        CHECK(loaded.queueMode == QueueMode::RepeatAll && loaded.repeat);
        CHECK(loaded.reactEnabled && loaded.reactMinSeconds == 120 && loaded.reactMaxChars == 40);
        CHECK(loaded.contextLimitTokens == 65536 && !loaded.autoCompress && loaded.reviewChars == 1200);
        CHECK(std::abs(loaded.volume - .35) < 1e-9 && loaded.quality == "hires" && !loaded.motion);
        CHECK(loaded.history.size() == 2 && loaded.history[0] == L"/search 晴天");
        {
            std::ofstream file(directory / L"settings.json", std::ios::binary | std::ios::trunc);
            file << R"({"volume":9,"quality":"ultra","history":5,"repeat":"yes"})";
        }
        auto recovered = Settings::load(directory);
        CHECK(recovered.volume == 1.0 && recovered.quality == "lossless" && !recovered.repeat &&
              recovered.history.empty());
        // Out-of-range reaction and context values fall back to safe defaults instead of sticking.
        CHECK(!recovered.reactEnabled && recovered.reactMinSeconds == 45 &&
              recovered.reactMaxChars == 25);
        CHECK(recovered.contextLimitTokens == 0 && recovered.autoCompress &&
              recovered.queueMode == QueueMode::Sequential);
        {
            std::ofstream file(directory / L"settings.json", std::ios::binary | std::ios::trunc);
            file << R"({"repeat":true,"reactMinSeconds":9000,"reactMaxChars":1})";
        }
        auto legacy = Settings::load(directory);
        // The pre-mode `repeat` flag still decides the mode, and absurd values are clamped.
        CHECK(legacy.queueMode == QueueMode::RepeatAll && legacy.repeat);
        CHECK(legacy.reactMinSeconds == 600 && legacy.reactMaxChars == 10);
        // A persona id the player cannot resolve still falls back to the first built-in.
        CHECK(recovered.personaId == "amu" && recovered.outputDeviceId == -1);
        CHECK(!recovered.guideDone);
        {
            std::ofstream file(directory / L"settings.json", std::ios::binary | std::ios::trunc);
            file << R"({"userName":"已有用户"})";
        }
        CHECK(Settings::load(directory).guideDone);
        CHECK(resolvePersona(recovered.personaId).id == "amu");
        std::filesystem::remove_all(directory);
    }
    // Conversation memory: it survives a restart, stays bounded, and compacts instead of forgetting.
    {
        auto directory = std::filesystem::temp_directory_path() /
                         (L"mora-context-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::remove_all(directory);
        CHECK(loadConversation(directory).empty());
        Conversation conversation;
        conversation.append("user", "播放夜的钢琴曲");
        conversation.append("assistant", "正在为你播放《夜的钢琴曲》");
        conversation.append("user", "再推荐几首类似的");
        CHECK(conversation.turns.size() == 3 && conversation.turns[0].role == "user");
        CHECK(saveConversation(directory, conversation));
        auto restored = loadConversation(directory);
        CHECK(restored.turns.size() == 3 && restored.turns[2].content == "再推荐几首类似的");
        // The projection carries the checkpoint as its own system message, then the newest turns.
        // The projection keeps the newest `limit` messages in order, oldest first.
        auto history = conversationHistory(restored, 2);
        CHECK(history.size() == 2 && history[0].role == "assistant" &&
              history[1].content == "再推荐几首类似的");
        restored.summary = "用户喜欢安静的钢琴曲";
        history = conversationHistory(restored, 2);
        CHECK(history.size() == 3 && history[0].role == "system" &&
              history[0].content.find("安静的钢琴曲") != std::string::npos);
        // Compacting folds the older turns into the checkpoint and keeps the newest ones verbatim.
        compactConversation(restored, "用户要求更多类似推荐", 2);
        CHECK(restored.turns.size() == 2 && restored.compressedTurns == 1);
        CHECK(restored.summary.find("安静的钢琴曲") != std::string::npos &&
              restored.summary.find("类似推荐") != std::string::npos);
        // A second compaction merges instead of replacing the earlier checkpoint.
        compactConversation(restored, "用户最终接受了推荐", 1);
        CHECK(restored.compressedTurns == 2);
        CHECK(restored.summary.find("安静的钢琴曲") != std::string::npos &&
              restored.summary.find("接受了推荐") != std::string::npos);
        // Empty content never becomes a turn, and the ring buffer keeps the newest entries.
        Conversation bounded;
        for (int i = 0; i < 60; ++i)
            bounded.append("user", "第" + std::to_string(i) + "条");
        CHECK(bounded.turns.size() == 40 && bounded.turns.back().content == "第59条");
        bounded.append("assistant", "");
        CHECK(bounded.turns.size() == 40);
        // A corrupt or oversized file must not break the player or surface as memory.
        {
            std::ofstream file(directory / L"chat-memory.json", std::ios::binary | std::ios::trunc);
            file << R"({"summary":42,"turns":"not-an-array","compressedTurns":"x"})";
        }
        auto corrupt = loadConversation(directory);
        CHECK(corrupt.empty() && corrupt.compressedTurns == 0);
        {
            std::ofstream file(directory / L"chat-memory.json", std::ios::binary | std::ios::trunc);
            file << R"({"turns":[{"role":"tool","content":"nope"},{"role":"user","content":"有效"}]})";
        }
        auto filtered = loadConversation(directory);
        CHECK(filtered.turns.size() == 1 && filtered.turns[0].content == "有效");
        std::filesystem::remove_all(directory);
    }
    // Listen history: local day/month/year reports, bounded projection, no account material.
    {
        auto directory = std::filesystem::temp_directory_path() /
                         (L"mora-stats-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        ListenPeriod period{};
        CHECK(parseListenPeriod("day", period) && period == ListenPeriod::Day);
        CHECK(parseListenPeriod("month", period) && period == ListenPeriod::Month);
        CHECK(parseListenPeriod("year", period) && period == ListenPeriod::Year);
        CHECK(!parseListenPeriod("decade", period));
        CHECK(std::string(listenPeriodName(ListenPeriod::Year)) == "year");
        {
            ListenHistory history(directory);
            Track listened{.id = 11, .name = L"晴天", .artist = L"周杰伦", .album = L"叶惠美"};
            listened.duration = 269;
            history.onTrackStart(listened, "search");
            history.onTrackEnd(listened, 268.0, true);
            Track skipped{.id = 22, .name = L"夜曲", .artist = L"周杰伦"};
            skipped.duration = 227;
            history.onTrackStart(skipped, "playlist");
            history.onTrackEnd(skipped, 10.0, false);
            // finalize-once: repeated flushes must not duplicate the finished listens.
            history.flush();
            history.flush();
            auto day = history.aggregate(ListenPeriod::Day);
            CHECK(day.period == ListenPeriod::Day && !day.key.empty());
            CHECK(day.plays == 2 && day.completedPlays == 1 && day.skippedPlays == 1);
            CHECK(day.uniqueSongs == 2 && day.uniqueArtists == 1);
            CHECK(day.listenedSeconds >= 270 && day.byHour.size() == 24);
            CHECK(day.firstStart > 0 && day.lastStart >= day.firstStart);
            CHECK(day.topSongs.size() == 2 && day.topSongs[0].label == "晴天" &&
                  day.topSongs[0].artist == "周杰伦" && day.topSongs[0].plays == 1);
            CHECK(day.topArtists.size() == 1 && day.topArtists[0].plays == 2);
            CHECK(!day.availableKeys.empty() && day.availableKeys.front() == day.key);
            // Day, month and year all see the same two listens; only the bucket key differs.
            CHECK(history.aggregate(ListenPeriod::Month).plays == 2);
            CHECK(history.aggregate(ListenPeriod::Year).plays == 2);
            CHECK(history.aggregate(ListenPeriod::Day, "1999-01-01").plays == 0);
            auto projection = listenStatsJson(day);
            auto text = projection.dump();
            CHECK(projection["period"] == "day" && projection["plays"] == 2);
            CHECK(projection["timezone"] == "local" && projection["topSongs"].is_array());
            CHECK(projection["hourHistogram"].size() == 24);
            CHECK(text.size() <= 8192);
            // The projection is model input, so it must never carry account or stream material.
            for (const char* forbidden : {"cookie", "MUSIC_U", "http://", "https://", "uid", "session"})
                CHECK(text.find(forbidden) == std::string::npos);
            CHECK(!history.entries().empty());
        }
        std::filesystem::remove_all(directory);
    }
    // Lyrics: LRC parsing, the live index and the bounded projection the agent reads.
    {
        const std::string plain = "[ti:Test]\n[ar:Nobody]\n[00:01.00]first\n"
                                  "[00:05.50][00:20.00]chorus\n\n[00:12.25]second\n";
        const std::string translated = "[00:01.00]1st\n[00:12.25]2nd\n";
        auto lines = parseLrcTimed(plain, translated);
        CHECK(lines.size() == 4); // metadata tags skipped, repeated chorus expanded
        CHECK(lines[0].seconds == 1.0 && lines[0].text == L"first");
        CHECK(lines[0].translation == L"1st");
        CHECK(lines[1].seconds == 5.5 && lines[1].text == L"chorus");
        CHECK(lines[2].seconds == 12.25 && lines[2].text == L"second");
        CHECK(lines[2].translation == L"2nd");
        CHECK(lines[3].seconds == 20.0 && lines[3].text == L"chorus");
        CHECK(parseLrcTimed("", "").empty());
        CHECK(parseLrcTimed("[00:03.00]\n", "").empty()); // a stamp with no text is not a line

        Lyrics lyrics;
        lyrics.songId = 42;
        lyrics.lines = lines;
        CHECK(lyricIndexAt(lyrics, 0.0) == -1);
        CHECK(lyricIndexAt(lyrics, 1.0) == 0);
        CHECK(lyricIndexAt(lyrics, 4.0) == 0);
        CHECK(lyricIndexAt(lyrics, 5.5) == 1);
        CHECK(lyricIndexAt(lyrics, 12.5) == 2);
        CHECK(lyricIndexAt(lyrics, 9999.0) == 3);

        Track track{.id = 42, .name = L"测试曲", .artist = L"某人", .duration = 60};
        auto json = lyricsJson(lyrics, 42, &track, 12.5, true, 2);
        CHECK(json["songId"] == 42 && json["title"] == "测试曲");
        CHECK(json["currentIndex"] == 2 && json["positionSeconds"] == 12.5);
        CHECK(json["lineCount"] == 4 && json["lines"].size() == 2);
        CHECK(json["truncated"] == true);
        auto withoutTranslation = lyricsJson(lyrics, 42, &track, 1.0, false, 10);
        CHECK(withoutTranslation["lines"].size() == 4);
        CHECK(withoutTranslation["hasTranslation"] == false);
        for (auto& line : withoutTranslation["lines"])
            CHECK(!line.contains("translation"));
        Lyrics instrumental;
        instrumental.songId = 7;
        instrumental.instrumental = true;
        auto empty = lyricsJson(instrumental, 7, nullptr, 0, true, 10);
        CHECK(empty["instrumental"] == true && empty["lines"].empty());
        CHECK(empty["currentIndex"] == -1);
    }
    // M5: deterministic, bounded candidates and the guards around random playback.
    {
        std::vector<Candidate> pool;
        for (int64_t id = 1; id <= 5; ++id) {
            Candidate candidate;
            candidate.track.id = id;
            candidate.track.name = L"候选" + std::to_wstring(id);
            candidate.reason = L"测试";
            pool.push_back(candidate);
        }
        auto first = selectCandidates(pool, 3, 42);
        auto again = selectCandidates(pool, 3, 42);
        CHECK(first.size() == 3 && again.size() == 3);
        CHECK(first[0].track.id == again[0].track.id && first[1].track.id == again[1].track.id &&
              first[2].track.id == again[2].track.id);
        auto avoiding = selectCandidates(pool, 5, 7, first[0].track.id);
        CHECK(avoiding.size() == 4);
        for (auto& candidate : avoiding)
            CHECK(candidate.track.id != first[0].track.id);
        auto duplicated = pool;
        duplicated.push_back(pool.front());
        CHECK(selectCandidates(duplicated, 10, 3).size() == pool.size());
        CHECK(selectCandidates({}, 5, 1).empty());
        CHECK(selectCandidates(pool, 0, 1).empty());
        CHECK(selectCandidates(pool, 5, 9, pool[0].track.id).size() == 4);
    }
    {
        MusicService service;
        bool invalidSource = false;
        try {
            service.invoke("music_recommend", Json{{"source", "library"}});
        } catch (const ToolError& e) {
            invalidSource = e.code == "invalid_arguments";
        }
        CHECK(invalidSource);
        bool missingIntent = false;
        try {
            service.invoke("music_recommend", Json{{"source", "search"}});
        } catch (const ToolError& e) {
            missingIntent = e.code == "invalid_arguments";
        }
        CHECK(missingIntent);
        bool refusedRandom = false;
        try {
            service.invoke("music_play_random", Json{{"candidateSetId", "rec-404"}});
        } catch (const ToolError& e) {
            refusedRandom = e.code == "needs_player";
        }
        CHECK(refusedRandom);
    }
    // M4-A: one current-user named pipe, one player instance, bounded request/reply.
    {
        std::wstring pipe = L"\\\\.\\pipe\\mora-test-" +
                            std::to_wstring(GetCurrentProcessId());
        CHECK(bridgePipeName().starts_with(L"\\\\.\\pipe\\mora-player-"));
        BridgeServer server;
        std::string error;
        bool pipeStarted = server.start(pipe, error);
        if (!pipeStarted)
            std::cerr << "bridge start failed: " << error << "\n";
        CHECK(pipeStarted);
        CHECK(server.running() && !server.attached());
        BridgeServer second;
        std::string secondError;
        CHECK(!second.start(pipe, secondError) && !secondError.empty());
        std::atomic<bool> stop{false};
        std::mutex deferredMutex;
        std::vector<BridgeCompletion> deferred;
        std::thread pump([&] {
            while (!stop) {
                std::vector<BridgeCompletion> ready;
                {
                    std::lock_guard lock(deferredMutex);
                    ready.swap(deferred);
                }
                for (auto& completion : ready)
                    completion(BridgeReply(true, "", "", Json{{"deferred", true}}));
                server.pump([&](const std::string& method, const Json& params,
                                BridgeCompletion done) {
                    if (method == "fail") {
                        done(BridgeReply(false, "no_permission", "denied by policy",
                                         Json::object()));
                        return;
                    }
                    if (method == "defer") {
                        // Answering later must still reach the waiting adapter frame.
                        std::lock_guard lock(deferredMutex);
                        deferred.push_back(std::move(done));
                        return;
                    }
                    done(BridgeReply(true, "", "", Json{{"method", method}, {"echo", params}}));
                });
                Sleep(5);
            }
        });
        BridgeClient client(pipe);
        CHECK(client.connect(2000) && client.connected());
        auto view = client.call("status", Json::object(), 4000);
        CHECK(view.ok && view.data["method"] == "status");
        auto echo = client.call("play", Json{{"id", 42}}, 4000);
        CHECK(echo.ok && echo.data["echo"]["id"] == 42);
        auto denied = client.call("fail", Json::object(), 4000);
        CHECK(!denied.ok && denied.code == "no_permission");
        auto later = client.call("defer", Json::object(), 4000);
        CHECK(later.ok && later.data["deferred"] == true);
        client.disconnect();
        auto offline = client.call("status", Json::object(), 1000);
        CHECK(!offline.ok && offline.code == "needs_player");
        stop = true;
        pump.join();
        server.stop();
        CHECK(!server.running());
    }
    auto fixtureDir = std::filesystem::current_path() / L"build" /
                      (L"fixture-" + std::to_wstring(GetCurrentProcessId()));
    int requests = 0, writes = 0;
    auto transport = [&](std::wstring url, std::string body, std::wstring headers, size_t,
                         bool) -> Response {
        ++requests;
        CHECK(url.starts_with(L"https://"));
        CHECK(headers.find(L"X-Forwarded-For") == std::wstring::npos);
        Response response;
        response.status = 200;
        if (url.find(L"unikey") != std::wstring::npos)
            response.body = R"({"code":200,"unikey":"test-key"})";
        else if (url.find(L"client/login") != std::wstring::npos) {
            response.body = R"({"code":803})";
            response.cookies = {"MUSIC_U=fixture-secret; Path=/; HttpOnly; Secure"};
        } else if (url.find(L"account/get") != std::wstring::npos)
            response.body = R"({"code":200,"profile":{"userId":42,"nickname":"fixture-user"}})";
        else if (url.find(L"song/like/get") != std::wstring::npos)
            response.body = R"({"code":200,"ids":[123]})";
        else if (url.find(L"radio/like") != std::wstring::npos) {
            ++writes;
            response.body = R"({"code":200})";
        } else if (url.find(L"manipulate/tracks") != std::wstring::npos) {
            ++writes;
            auto decrypted = str(aes(unhex(body.substr(7)), bytes("e82ckenh8dichen8"), false));
            CHECK(decrypted.find("\"op\":\"add\"") != std::string::npos);
            CHECK(decrypted.find("\"trackIds\":\"[456]\"") != std::string::npos);
            response.body = R"({"code":512})";
        } else if (url.find(L"user/playlist") != std::wstring::npos)
            response.body =
                R"({"code":200,"playlist":[{"id":11,"name":"mine","creator":{"userId":42,"nickname":"me"}},{"id":12,"name":"other","creator":{"userId":99,"nickname":"other"}}]})";
        else
            response.body = R"({"code":200})";
        return response;
    };
    {
        Api api(fixtureDir, transport);
        bool denied = false;
        try {
            api.like(123, true);
        } catch (const ApiError& e) {
            denied = e.code == 301;
        }
        CHECK(denied && requests == 0);
        CHECK(api.qrKey() == "test-key");
        CHECK(api.qrCheck("test-key") == 803);
        CHECK(api.loggedIn() && api.uid == 42 && api.likes.contains(123));
        api.like(456, true);
        CHECK(api.likes.contains(456));
        auto mine = api.myPlaylists(true);
        CHECK(mine.size() == 1 && mine[0].id == 11);
        int before = requests;
        bool rejected = false;
        try {
            api.add(456, 11);
        } catch (const ApiError& e) {
            rejected = e.code == 512;
        }
        CHECK(rejected && requests == before + 1 && writes == 2);
    }
    {
        Api restored(fixtureDir, transport);
        CHECK(restored.loggedIn() && restored.uid == 42);
        std::ifstream f(fixtureDir / L"session.bin", std::ios::binary);
        std::string blob((std::istreambuf_iterator<char>(f)), {});
        CHECK(blob.find("fixture-secret") == std::string::npos &&
              blob.find("fixture-user") == std::string::npos);
        f.close();
        restored.logout();
        CHECK(!restored.loggedIn());
        CHECK(!std::filesystem::exists(fixtureDir / L"session.bin"));
    }
    {
        // A session file that still carries an identity but has lost its cookie must not be
        // presented as a signed-in account: account() clears the stale fields without a request.
        auto staleDir = fixtureDir / L"stale";
        std::filesystem::create_directories(staleDir);
        Json stale = {{"device", "stale-device"},
                      {"cookies", Json::object()},
                      {"uid", 42},
                      {"nickname", "stale-user"}};
        auto clear = bytes(stale.dump());
        auto encrypted = protect(clear);
        {
            std::ofstream out(staleDir / L"session.bin", std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(encrypted.data()),
                      static_cast<std::streamsize>(encrypted.size()));
        }
        int staleRequests = 0;
        Api orphaned(staleDir, [&](std::wstring, std::string, std::wstring, size_t, bool) {
            ++staleRequests;
            return Response{.body = R"({"code":200})", .status = 200};
        });
        CHECK(!orphaned.loggedIn() && orphaned.uid == 42 && orphaned.nickname == L"stale-user");
        orphaned.account();
        CHECK(orphaned.uid == 0 && orphaned.nickname == L"未登录" && orphaned.likes.empty());
        CHECK(staleRequests == 0);
        std::filesystem::remove_all(staleDir);
    }
    int riskCalls = 0;
    Api risk(fixtureDir, [&](std::wstring, std::string, std::wstring, size_t, bool) {
        ++riskCalls;
        return Response{.body = R"({"code":429})", .status = 429};
    });
    for (int i = 0; i < 2; ++i) {
        bool blocked = false;
        try {
            risk.search(L"test", Kind::Song);
        } catch (const ApiError& e) {
            blocked = e.code == 429;
        }
        CHECK(blocked);
    }
    CHECK(riskCalls == 1);
    std::filesystem::remove(fixtureDir / L"session.bin");
    std::filesystem::remove(fixtureDir);
    // A temporary local WAV exercises the real Windows decoder and audio device.
    auto path = std::filesystem::temp_directory_path() / L"mora-test-tone.wav";
    writeTestTone(path, 2);
    {
        Audio audio;
        auto outputs = outputDevices();
        CHECK(!outputs.empty() && outputs.front().id == -1);
        CHECK(!audio.selectOutputDevice(-2));
        CHECK(audio.selectOutputDevice(-1));
        CHECK(audio.outputDevice() == -1);
        audio.play(path.wstring());
        for (int i = 0; i < 100 && audio.state().position < .1 && audio.state().error.empty(); ++i)
            Sleep(50);
        CHECK(audio.state().error.empty());
        CHECK(audio.state().position >= .1);
        CHECK(audio.state().sampleRate == 44100 && audio.state().sampleBits == 16);
        CHECK(audio.selectOutputDevice(-1));
        for (int i = 0; i < 40 && audio.state().position < .2 && audio.state().error.empty(); ++i)
            Sleep(25);
        CHECK(audio.state().error.empty());
        audio.pause();
        Sleep(100);
        CHECK(audio.state().paused);
        audio.seek(1.0);
        Sleep(150);
        audio.pause();
        Sleep(150);
        CHECK(audio.state().position >= .9);
        audio.stop();
    }
    // M1-A: the session, not the UI, drives the device and keeps the resolved metadata.
    {
        Audio device;
        PlaybackSession live(&device);
        Track local{.id = 7, .name = L"本地音测试"};
        live.assign({local}, 0);
        live.startTrack(local, "standard", path.wstring());
        for (int i = 0; i < 100 && device.state().position < .1 &&
                        device.state().error.empty();
             ++i)
            Sleep(50);
        CHECK(device.state().error.empty() && device.state().position >= .1);
        CHECK(live.snapshot().currentId == 7 && live.actualQuality() == "standard");
        live.pause();
        Sleep(150);
        CHECK(live.audioState().paused);
        live.stop();
        for (int i = 0; i < 200 && live.audioState().position > 0; ++i)
            Sleep(10);
        CHECK(live.audioState().position == 0);
    }
    std::filesystem::remove(path);
    auto hiresPath = std::filesystem::temp_directory_path() / L"mora-test-hires.wav";
    writeTestTone(hiresPath, 2, 96000, 24);
    {
        Audio audio;
        audio.play(hiresPath.wstring());
        for (int i = 0; i < 100 && audio.state().position < .1 && audio.state().error.empty(); ++i)
            Sleep(50);
        CHECK(audio.state().error.empty());
        CHECK(audio.state().sampleRate == 96000 && audio.state().sampleBits == 24);
    }
    std::filesystem::remove(hiresPath);
    std::cout << (failures ? "FAILED " : "PASS ") << checks << " checks, " << failures
              << " failures\n";
    return failures ? 1 : 0;
}
