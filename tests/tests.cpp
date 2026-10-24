#include "agent.hpp"
#include "api.hpp"
#include "audio.hpp"
#include "bridge.hpp"
#include "commands.hpp"
#include "core.hpp"
#include "mascot.hpp"
#include "mcp.hpp"
#include "persona.hpp"
#include "platform.hpp"
#include "recommend.hpp"
#include "session.hpp"
#include "settings.hpp"
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
    queue.repeat = true;
    CHECK(queue.next() && queue.current()->id == 1);
    CHECK(queue.previous() && queue.current()->id == 2);
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
    CHECK((*listed)["result"]["tools"].size() == 12);
    {
        std::set<std::string> names;
        for (auto& tool : (*listed)["result"]["tools"])
            names.insert(tool["name"].get<std::string>());
        CHECK(names == std::set<std::string>({"music_search", "music_open_collection", "music_play",
                                              "music_control", "music_now_playing",
                                              "music_get_lyrics", "music_recommend",
                                              "music_play_random", "music_list_playlists",
                                              "music_set_like", "music_add_to_playlist",
                                              "music_get_status"}));
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
    }
    // M3: the model adapter shapes tools/messages and stays inside its budgets.
    {
        CHECK(AgentConfig{}.baseUrl ==
              "https://opencode.ai/inference/go/openai/v1/chat/completions");
        auto tools = agentToolsFrom(musicToolDefinitions());
        CHECK(tools.size() == 12);
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
                // 12 music tools plus the terminal-only persona creator.
                CHECK(tools.size() == 13);
                CHECK(tools.back()["function"]["name"] == "mora_create_persona");
                CHECK(tools.back()["function"]["parameters"]["required"] ==
                      Json::array({"id", "name", "voice", "taste"}));
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
        settings.repeat = true;
        settings.motion = false;
        settings.userName = L"阿木的听众";
        settings.personaId = "fox";
        settings.outputDeviceId = 3;
        settings.guideDone = true;
        settings.history = {L"/search 晴天", L"/volume 35"};
        CHECK(settings.save(directory));
        auto loaded = Settings::load(directory);
        CHECK(loaded.userName == L"阿木的听众");
        CHECK(loaded.personaId == "fox");
        CHECK(loaded.outputDeviceId == 3);
        CHECK(loaded.guideDone);
        CHECK(std::abs(loaded.volume - .35) < 1e-9 && loaded.quality == "hires" && loaded.repeat &&
              !loaded.motion);
        CHECK(loaded.history.size() == 2 && loaded.history[0] == L"/search 晴天");
        {
            std::ofstream file(directory / L"settings.json", std::ios::binary | std::ios::trunc);
            file << R"({"volume":9,"quality":"ultra","history":5,"repeat":"yes"})";
        }
        auto recovered = Settings::load(directory);
        CHECK(recovered.volume == 1.0 && recovered.quality == "lossless" && !recovered.repeat &&
              recovered.history.empty());
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
