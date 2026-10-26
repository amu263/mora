#include "agent.hpp"
#include "bridge.hpp"
#include "context.hpp"
#include "mcp.hpp"
#include "qrcodegen.hpp"
#include "ui.hpp"
#include <fstream>
#include <iostream>
#include <shellapi.h>

using namespace mora;
int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    int count = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    Options options;
    std::wstring probe;
    Kind kind = Kind::Song;
    int64_t probePlay = 0;
    std::string probeQuality = "standard";
    bool mcp = false, loginQr = false, accountCheck = false, setAgentKey = false,
         agentCheck = false, listModels = false;
    std::wstring chatText;
    try {
        for (int i = 1; i < count; ++i) {
            std::wstring a = args[i];
            auto value = [&]() {
                if (i + 1 >= count)
                    throw std::runtime_error("Missing argument value");
                return std::wstring(args[++i]);
            };
            if (a == L"--demo")
                options.demo = true;
            else if (a == L"--mcp")
                mcp = true;
            else if (a == L"--login-qr")
                loginQr = true;
            else if (a == L"--account")
                accountCheck = true;
            else if (a == L"--set-agent-key")
                setAgentKey = true;
            else if (a == L"--agent-check")
                agentCheck = true;
            else if (a == L"--models")
                listModels = true;
            else if (a == L"--chat")
                chatText = value();
            else if (a == L"--no-motion")
                options.motion = false;
            else if (a == L"--seconds")
                options.seconds = std::stoi(value());
            else if (a == L"--report")
                options.report = value();
            else if (a == L"--snapshot")
                options.snapshot = value();
            else if (a == L"--local")
                options.localAudio = value();
            else if (a == L"--play")
                options.playId = std::stoll(value());
            else if (a == L"--data-dir") {
                auto dir = std::filesystem::absolute(value());
                SetEnvironmentVariableW(L"MORA_DATA_DIR", dir.c_str());
            } else if (a == L"--probe")
                probe = value();
            else if (a == L"--probe-play")
                probePlay = std::stoll(value());
            else if (a == L"--quality")
                probeQuality = utf8(value());
            else if (a == L"--kind") {
                auto k = value();
                kind = k == L"album" ? Kind::Album : k == L"playlist" ? Kind::Playlist : Kind::Song;
            } else if (a == L"--version") {
                std::cout << "MORA 0.7.2-preview / Windows x64 native\n";
                LocalFree(args);
                return 0;
            } else if (a == L"--help") {
                std::cout
                    << "MORA / yun jian - NetEase music terminal player\n\n  mora                 "
                       "Start terminal player\n  mora --demo          Offline UI demo\n  mora "
                       "--no-motion     Disable visualizer\n  mora --local FILE    Play a local "
                       "MP3/FLAC/WAV for diagnostics\n  mora --probe QUERY [--kind "
                       "song|album|playlist]\n  mora --seconds N --report FILE   Measure "
                       "memory\n  mora --mcp          MCP stdio framework\n  mora --login-qr    "
                       "Headless QR login (stores DPAPI session)\n  mora --account      Account "
                       "diagnostics (nickname/uid/likes/playlists)\n  mora --set-agent-key  Read a "
                       "model key from stdin into the DPAPI vault\n  mora --models        List the "
                       "models the configured endpoint advertises, with the context window and "
                       "modalities it reports (never the key)\n  mora --agent-check   Show model "
                       "endpoint/model/key presence and the cached context window (never the key)\n  mora --chat TEXT      One "
                       "bounded model tool loop with chat-memory.json history (attach a running "
                       "player for playback)\n\n"
                       "Commands: /search QUERY, /albums QUERY, /playlists QUERY, /login\n"
                       "          /queue, /play 1, /pause, /home, /help, /quit\n"
                       "Input: / command suggestions, Tab complete, Enter execute\n"
                       "Results: Up/Down select, Enter play, Space pause, Tab input\n";
                LocalFree(args);
                return 0;
            } else
                throw std::runtime_error("Unknown argument: " + utf8(a));
        }
        LocalFree(args);
        args = nullptr;
        if (mcp)
            return runMcp();
        // M3: the key is read from stdin so it never appears in a command line or a log.
        if (setAgentKey) {
            std::string key;
            char buffer[256];
            while (std::cin.good()) {
                std::cin.read(buffer, sizeof(buffer));
                key.append(buffer, static_cast<size_t>(std::cin.gcount()));
                if (key.size() > 4096)
                    throw std::runtime_error("model key is unreasonably long");
            }
            while (!key.empty() && (key.back() == '\n' || key.back() == '\r' || key.back() == ' ' ||
                                    key.back() == '\t' || key.back() == '"'))
                key.pop_back();
            while (!key.empty() && (key.front() == ' ' || key.front() == '\t' || key.front() == '"'))
                key.erase(key.begin());
            saveAgentKey(key);
            auto config = loadAgentConfig();
            std::cout << Json{{"keyStored", true},
                              {"keyLength", static_cast<int64_t>(key.size())},
                              {"model", config.model},
                              {"baseUrl", config.baseUrl}}
                             .dump(2)
                      << '\n';
            return 0;
        }
        if (listModels) {
            auto config = loadAgentConfig();
            // Read-only discovery of the sibling /models route of the configured chat endpoint.
            // Modalities and the context window are reported for information; a request never
            // carries a modality and the key is never printed.
            auto catalog = fetchModelCatalog(config);
            if (catalog.ok)
                cacheModelCatalog(catalog);
            Json models = Json::array();
            for (auto& model : catalog.models)
                models.push_back(Json{{"id", model.id},
                                      {"configured", model.id == config.model},
                                      {"inputModalities", model.inputModalities},
                                      {"outputModalities", model.outputModalities},
                                      {"contextTokens", model.contextTokens},
                                      {"maxOutputTokens", model.maxOutputTokens},
                                      {"reasoning", model.reasoning}});
            auto cached = cachedModelCatalog();
            Json result = {{"baseUrl", config.baseUrl},
                           {"model", config.model},
                           {"keyPresent", agentKeyPresent()},
                           {"endpoint", catalog.endpoint},
                           {"ok", catalog.ok},
                           {"reportOnly", true},
                           {"models", models},
                           {"cached", {{"ok", cached.ok},
                                       {"endpoint", cached.endpoint},
                                       {"modelCount", (int64_t)cached.models.size()}}}};
            if (!catalog.ok) {
                result["code"] = catalog.code;
                result["error"] = catalog.error;
            }
            std::cout << result.dump(2) << '\n';
            return catalog.ok ? 0 : 5;
        }
        if (agentCheck) {
            auto config = loadAgentConfig();
            auto cached = cachedModelCatalog();
            Json cachedModel = Json(nullptr);
            for (auto& model : cached.models) {
                if (model.id != config.model)
                    continue;
                cachedModel = Json{{"id", model.id},
                                   {"contextTokens", model.contextTokens},
                                   {"maxOutputTokens", model.maxOutputTokens},
                                   {"inputModalities", model.inputModalities},
                                   {"outputModalities", model.outputModalities},
                                   {"reasoning", model.reasoning}};
                break;
            }
            std::cout << Json{{"baseUrl", config.baseUrl},
                              {"model", config.model},
                              {"keyPresent", agentKeyPresent()},
                              {"maxToolCalls", config.maxToolCalls},
                              {"maxRounds", config.maxRounds},
                              {"contextTokens", config.contextTokens},
                              {"contextLimitTokens", config.contextTokens > 0
                                                         ? config.contextTokens
                                                         : config.contextFallbackTokens},
                              {"contextFallbackTokens", config.contextFallbackTokens},
                              {"compressAt", config.compressAt},
                              {"autoCompress", config.autoCompress},
                              {"keepRecentMessages", (int64_t)config.keepRecentMessages},
                              {"modelModalities", config.modelModalities},
                              {"cachedCatalog", {{"ok", cached.ok},
                                                 {"code", cached.code},
                                                 {"endpoint", cached.endpoint},
                                                 {"modelCount", (int64_t)cached.models.size()}}},
                              {"cachedModel", cachedModel},
                              {"dataDir", utf8(stateDirectory().wstring())}}
                             .dump(2)
                      << '\n';
            return 0;
        }
        if (!chatText.empty()) {
            hrcheck(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM initialization");
            MusicService service;
            BridgeClient bridge;
            bridge.connect(600);
            service.attach([&bridge](const std::string& method, const Json& params) {
                if (!bridge.connected())
                    bridge.connect(600);
                return bridge.call(method, params, 25000);
            });
            auto config = loadAgentConfig();
            auto directory = stateDirectory();
            // The remembered conversation (checkpoint plus newest turns) is handed to the loop,
            // which compresses further when the estimated prompt crosses the window.
            auto conversation = loadConversation(directory);
            auto outcome = runChatLoop(config, utf8(chatText),
                                       [&service](const std::string& tool, const Json& arguments) {
                                           return service.invoke(tool, arguments);
                                       },
                                       {}, {}, conversationHistory(conversation, 24));
            Json result = {{"model", config.model},
                           {"baseUrl", config.baseUrl},
                           {"playerAttached", bridge.connected()},
                           {"ok", outcome.ok},
                           {"toolCalls", outcome.toolCalls},
                           {"promptTokens", (int64_t)outcome.promptTokens},
                           {"contextLimitTokens", (int64_t)outcome.contextLimitTokens},
                           {"compressedMessages", outcome.compressedMessages},
                           {"compressionDegraded", outcome.compressionDegraded},
                           {"compressionCode", outcome.compressionCode},
                           {"answer", utf8(outcome.text)},
                           {"trace", outcome.trace}};
            if (!outcome.ok) {
                result["code"] = outcome.code;
                result["error"] = outcome.error;
            } else if (!outcome.text.empty()) {
                conversation.append("user", utf8(chatText));
                conversation.append("assistant", utf8(outcome.text));
                result["memorySaved"] = saveConversation(directory, conversation);
            }
            std::cout << result.dump(2) << '\n';
            CoUninitialize();
            return outcome.ok ? 0 : 5;
        }
        // Headless QR login: prints the real login QR the server issued, polls every 3 s for at
        // most 120 s and stores the DPAPI session on success. No cookie or codekey is logged.
        if (loginQr) {
            hrcheck(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM initialization");
            Api api;
            auto key = api.qrKey();
            auto text = "https://music.163.com/login?codekey=" + key;
            auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::QUARTILE);
            const int border = 2;
            for (int y = -border; y < qr.getSize() + border; ++y) {
                for (int x = -border; x < qr.getSize() + border; ++x) {
                    bool dark = x >= 0 && y >= 0 && x < qr.getSize() && y < qr.getSize() &&
                                qr.getModule(x, y);
                    std::cout << (dark ? "██" : "  ");
                }
                std::cout << '\n';
            }
            std::cout << "waiting for scan (120s, 3s poll)...\n" << std::flush;
            int last = 0;
            for (int attempt = 0; attempt < 40; ++attempt) {
                Sleep(3000);
                int code = api.qrCheck(key);
                if (code == 803) {
                    std::cout << "logged in as " << utf8(api.nickname) << "\n";
                    CoUninitialize();
                    return 0;
                }
                if (code == 800) {
                    std::cout << "qr expired\n";
                    CoUninitialize();
                    return 3;
                }
                if (code != last && (code == 801 || code == 802)) {
                    std::cout << (code == 802 ? "scanned, confirm in the phone app\n"
                                              : "waiting for scan\n")
                              << std::flush;
                    last = code;
                }
            }
            std::cout << "login timed out\n";
            CoUninitialize();
            return 4;
        }
        // Account diagnostics for M2 acceptance: nickname, uid, likes and playlist count only.
        if (accountCheck) {
            hrcheck(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM initialization");
            Api api;
            Json result = {{"nickname", utf8(api.nickname)}, {"uid", api.uid}};
            try {
                api.account();
                result["nickname"] = utf8(api.nickname);
                result["uid"] = api.uid;
                result["likes"] = (int64_t)api.likes.size();
            } catch (const ApiError& e) {
                result["accountError"] = e.what();
            }
            try {
                auto playlists = api.myPlaylists(false);
                result["playlists"] = (int64_t)playlists.size();
                Json names = Json::array();
                for (size_t i = 0; i < playlists.size() && i < 10; ++i)
                    names.push_back(Json{{"id", playlists[i].id},
                                         {"name", utf8(playlists[i].name)},
                                         // Ownership is this account's id, not "some creator exists":
                                         // the sample also contains playlists other people own.
                                         {"owner", playlists[i].owner == api.uid}});
                result["playlistSample"] = names;
            } catch (const ApiError& e) {
                result["playlistsError"] = e.what();
            }
            std::cout << result.dump(2) << '\n';
            CoUninitialize();
            return 0;
        }
        if (!probe.empty() || probePlay) {
            hrcheck(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM initialization");
            Api api;
            Json result = Json::object();
            if (!probe.empty()) {
                auto page = api.search(probe, kind);
                result["total"] = page.total;
                result["results"] = Json::array();
                for (auto& t : page.items)
                    result["results"].push_back(Json{{"id", t.id},
                                                     {"name", utf8(t.name)},
                                                     {"artist", utf8(t.artist)},
                                                     {"album", utf8(t.album)}});
            }
            if (probePlay) {
                auto p = api.playback(Track{.id = probePlay}, probeQuality);
                result["playback"] = {{"id", p.track.id},
                                      {"name", utf8(p.track.name)},
                                      {"quality", p.quality},
                                      {"coverDecoded", !p.cover.pixels.empty()}};
                if (options.seconds > 0) {
                    Audio audio;
                    audio.play(p.url);
                    double peak = 0, peakPrivate = 0;
                    bool played = false;
                    auto until = Clock::now() + std::chrono::seconds(options.seconds);
                    while (Clock::now() < until) {
                        auto state = audio.state();
                        if (!state.error.empty())
                            throw std::runtime_error(utf8(state.error));
                        played = played || state.position > .1;
                        peak = std::max(peak, memoryMB());
                        peakPrivate = std::max(peakPrivate, privateMB());
                        Sleep(100);
                    }
                    result["audioPlayed"] = played;
                    result["peakWorkingSetMB"] = peak;
                    result["peakPrivateMB"] = peakPrivate;
                    result["budgetPassed"] = peak <= 50 && peakPrivate <= 50;
                }
            }
            result["workingSetMB"] = memoryMB();
            result["privateMB"] = privateMB();
            std::cout << result.dump(2) << '\n';
            if (!options.report.empty()) {
                std::ofstream file(std::filesystem::path(options.report));
                file << result.dump(2) << '\n';
            }
            CoUninitialize();
            return 0;
        }
        return runApp(options);
    } catch (const std::exception& e) {
        if (args)
            LocalFree(args);
        std::cerr << "MORA: " << e.what() << '\n';
        return 1;
    }
}
