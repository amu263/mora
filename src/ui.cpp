#include "ui.hpp"
#include "agent.hpp"
#include "bridge.hpp"
#include "commands.hpp"
#include "mascot.hpp"
#include "persona.hpp"
#include "qrcodegen.hpp"
#include "settings.hpp"
#include "worker.hpp"
#include "session.hpp"
#include "ui_tool_calls.hpp"
#include <atomic>
#include <fstream>
#include <map>
#include <iostream>
#include <shellapi.h>

namespace mora {
    enum class Modal { None, Login, Qr, Phone, Code, Save, Help, Quality, Guide, Ai, AiKey, AiModel, Agent, Settings, Name,
                       Persona, Device };
    enum Action {
        A_Search = 1,
        A_Songs,
        A_Albums,
        A_Playlists,
        A_Library,
        A_Queue,
        A_Login,
        A_Previous,
        A_Pause,
        A_Next,
        A_Like,
        A_Save,
        A_Quality,
        A_Qr,
        A_Phone,
        A_Logout,
        A_Back,
        A_Close,
        A_Send,
        A_Motion,
        A_Repeat,
        A_Help,
        A_Guide,
        A_GuideNext,
        A_GuidePrevious,
        A_PagePrev,
        A_PageNext,
        A_VolDown,
        A_VolUp,
        A_Home,
        A_AI,
        A_AiKey,
        A_AiModel,
        A_Agent,
        A_Mascot,
        A_Name,
        A_Persona,
        A_Device,
        A_PersonaRow = 50000,
        A_DeviceRow = 60000,
        A_Progress = 80,
        A_Row = 1000,
        A_SaveRow = 20000,
        A_QualityRow = 30000,
        A_Suggestion = 40000
    };

    class App {
        Options options;
        Console console;
        MainThreadToolCalls mainCalls;
        std::atomic<uint64_t> activeChat{0};
        Worker work{&mainCalls, &activeChat};
        std::thread chatThread;
        std::mutex chatMutex;
        std::optional<Reply> chatResult;
        std::atomic<bool> chatRunning{false};
        uint64_t chatSequence = 0;
        Audio audio;
        PlaybackSession session{&audio};
        BridgeServer bridge;
        std::unique_ptr<Canvas> previous;
        Palette colors;
        Cover cover;
        // The active persona drives the homepage character, the UI palette and the model's voice.
        Persona persona = builtinPersonas().front();
        std::vector<Persona> personaList = allPersonas();
        int personaSelected = 0;
        // Lyrics of the track that is playing, fetched once per track by the shared worker.
        Lyrics currentLyrics;
        int64_t lyricsSong = 0;
        uint64_t lyricsTicket = 0;
        // Real-time reactions: opt-in, at most one per track change and rate limited.
        bool reactEnabled = false;
        bool reactPending = false;
        int64_t reactSong = 0;
        Clock::time_point lastReaction{};
        int reactCount = 0;
        std::vector<Track> rows, backRows, saveRows;
        std::set<int64_t> likes;
        std::set<int64_t> detailsAsked;
        std::map<uint64_t, BridgeCompletion> ipcPending;
        uint64_t ipcSequence = 0;
        std::wstring query, input, phone, nickname = L"未登录", title = L"搜索音乐",
                                          status = L"按 / 输入歌名、歌手或专辑；Enter 搜索";
        std::string qrkey;
        int selected = 0, top = 0, total = 0, offset = 0, mode = 1, backMode = 1, backSelected = 0,
            saveSelected = 0, qualitySelected = 3;
        int width = 116, height = 36, listX = 3, listY = 9, listW = 66, listH = 16, progressX = 3,
            progressY = 0, progressW = 80;
        int64_t saveSong = 0;
        bool collection = false, backCollection = false, busy = false, searchFocus = true,
             quit = false, motion = true, home = true, chatView = false;
        size_t inputCursor = 0;
        int suggestion = 0, historyIndex = -1;
        std::vector<std::wstring> history;
        std::vector<std::pair<std::wstring, std::wstring>> chatLog;
        std::wstring userName;
        bool guideDone = false;
        int guidePage = 0;
        int chatScroll = 0;
        bool selecting = false, selectionMoved = false;
        COORD selectionStart{}, selectionEnd{};
        int qrGeneration = 0;
        Modal modal = Modal::None;
        Clock::time_point qrPoll{}, qrStart{}, started = Clock::now();
        double peak = 0, peakPrivate = 0, maxPosition = 0;
        bool audioPlayed = false;
        std::wstring audioError, lastPlayError;
        bool eofHandled = false;
        std::array<double, 24> bars{};
        static constexpr std::array<const char*, 5> qualities{"standard", "higher", "exhigh",
                                                              "lossless", "hires"};
        static constexpr std::array<const wchar_t*, 5> qualityNames{
            L"标准 128K", L"较高 192K", L"极高 320K", L"无损 FLAC", L"Hi-Res"};

        void message(std::wstring text) {
            status = sanitize(std::move(text));
        }
        bool setUserName(std::wstring name) {
            auto first = name.find_first_not_of(L" \t\r\n");
            if (first == std::wstring::npos)
                return false;
            name = name.substr(first, name.find_last_not_of(L" \t\r\n") - first + 1);
            if (name.size() > 24 || name.find_first_of(L"\r\n\t") != std::wstring::npos)
                return false;
            userName = std::move(name);
            if (!options.demo && options.seconds <= 0 && options.snapshot.empty()) {
                auto settings = Settings::load(stateDirectory());
                settings.userName = userName;
                settings.save(stateDirectory());
            }
            return true;
        }
        void editName() {
            modal = Modal::Name;
            fillInput(userName);
        }
        void finishGuide() {
            guideDone = true;
            modal = Modal::None;
            if (!options.demo && options.seconds <= 0 && options.snapshot.empty()) {
                auto settings = Settings::load(stateDirectory());
                settings.guideDone = true;
                settings.save(stateDirectory());
            }
            message(L"准备好了：输入 /search 歌名 开始，随时用 /guide 重看引导");
        }
        // The persona owns the base palette, so switching persona visibly repaints the terminal.
        void applyPersonaColors() {
            colors.background = persona.background;
            colors.surface = persona.surface;
            colors.accent = persona.accent;
            colors.muted = persona.muted;
            colors.text = RGB{235, 233, 238};
        }
        void refreshPersonas() {
            personaList = allPersonas();
            personaSelected = 0;
            for (size_t i = 0; i < personaList.size(); ++i)
                if (personaList[i].id == persona.id)
                    personaSelected = static_cast<int>(i);
        }
        bool selectPersona(const std::string& id, bool announce) {
            const Persona* found = nullptr;
            for (const auto& candidate : personaList)
                if (candidate.id == id)
                    found = &candidate;
            if (!found)
                for (const auto& candidate : builtinPersonas())
                    if (candidate.id == id)
                        found = &candidate;
            if (!found)
                return false;
            persona = *found;
            refreshPersonas();
            applyPersonaColors();
            if (!options.demo && options.seconds <= 0 && options.snapshot.empty()) {
                auto settings = Settings::load(stateDirectory());
                settings.personaId = persona.id;
                settings.save(stateDirectory());
            }
            if (announce) {
                message(L"人设已切换 · " + persona.name + L" · " + persona.tagline);
                if (chatView && !chatLog.empty())
                    chatLog.push_back({L"", persona.greeting});
            }
            return true;
        }
        void openPersonaPicker() {
            refreshPersonas();
            modal = Modal::Persona;
        }
        // Reasoning strength mirrors DST's ladder; `off` sends no reasoning field at all.
        static constexpr std::array<const char*, 8> effortLevels{"",     "off",  "minimal", "low",
                                                                 "medium", "high", "xhigh",   "max"};
        std::wstring effortLabel(const std::string& level) const {
            return level.empty() ? L"provider 默认" : unutf8(level);
        }
        bool setReasoningEffort(const std::string& level) {
            if (std::find(effortLevels.begin(), effortLevels.end(), level) == effortLevels.end())
                return false;
            auto config = loadAgentConfig();
            config.reasoningEffort = level;
            saveAgentConfig(config);
            message(L"思考强度 · " + effortLabel(level));
            return true;
        }
        // The Windows output device list is read on demand; selecting one takes effect immediately
        // and is remembered for later runs.
        std::vector<OutputDevice> outputList = outputDevices();
        int deviceSelected = 0;
        void refreshOutputDevices() {
            outputList = outputDevices();
            deviceSelected = 0;
            auto current = audio.outputDevice();
            for (size_t i = 0; i < outputList.size(); ++i)
                if (outputList[i].id == current)
                    deviceSelected = static_cast<int>(i);
        }
        void openDevicePicker() {
            refreshOutputDevices();
            modal = Modal::Device;
        }
        bool selectOutputDevice(int index) {
            if (index < 0 || index >= (int)outputList.size())
                return false;
            if (!audio.selectOutputDevice(outputList[(size_t)index].id))
                return false;
            if (!options.demo && options.seconds <= 0 && options.snapshot.empty()) {
                auto settings = Settings::load(stateDirectory());
                settings.outputDeviceId = outputList[(size_t)index].id;
                settings.save(stateDirectory());
            }
            message(L"音频输出 · " + outputList[(size_t)index].name);
            return true;
        }
        void selectOutputDeviceByNameOrIndex(const std::wstring& args) {
            refreshOutputDevices();
            if (args == L"default") {
                selectOutputDevice(0);
                return;
            }
            try {
                size_t consumed = 0;
                int index = std::stoi(args, &consumed);
                if (consumed != args.size() || !selectOutputDevice(index))
                    throw std::runtime_error("range");
            } catch (...) {
                message(L"请用 /audio-device 查看列表，或 /audio-device <序号>、/audio-device default");
            }
        }
        bool submit(std::string name, std::function<Reply(MusicService&)> task) {
            if (options.demo) {
                message(L"演示模式 · 联网操作请退出后运行 mora");
                return false;
            }
            if (busy) {
                message(L"正在处理上一项操作，请稍候");
                return false;
            }
            uint64_t request = session.beginRequest();
            busy = true;
            work.post(request, std::move(name), std::move(task));
            return true;
        }
        void fillInput(std::wstring text) {
            input = std::move(text);
            inputCursor = input.size();
            searchFocus = true;
            suggestion = 0;
            historyIndex = -1;
        }
        void cancelPending() {
            session.cancelRequests();
            activeChat = 0;
            work.cancel();
            busy = false;
            qrkey.clear();
        }
        void startChat(const std::wstring& prompt, const std::wstring& label = {}) {
            if (options.demo) {
                message(L"演示模式 · 模型请求请退出后运行 mora");
                return;
            }
            if (busy || chatRunning) {
                message(L"正在处理上一项操作，请稍候");
                return;
            }
            if (chatThread.joinable())
                chatThread.join();
            uint64_t request = ++chatSequence;
            activeChat = request;
            chatRunning = true;
            home = false;
            chatView = true;
            searchFocus = true;
            chatLog.push_back({label.empty() ? prompt : label, L"正在联系模型…"});
            chatScroll = 0;
            if (chatLog.size() > 8)
                chatLog.erase(chatLog.begin());
            message(L"模型正在处理…");
            // The persona and the user's chosen name must be captured on the UI thread: the chat
            // worker may not read mutable UI state while the main thread repaints.
            auto systemPrompt = personaSystemPrompt(persona, userName) + L"\n" + playbackContext() +
                                L"聊天时可以自然带入当前播放的歌曲或这一句歌词，但不要编造歌词或播放"
                                L"状态；需要完整歌词时先调用 music_get_lyrics。";
            auto personaId = persona.id;
            chatThread = std::thread([this, request, text = utf8(prompt),
                                      systemPrompt = std::move(systemPrompt),
                                      personaId = std::move(personaId)] {
                Reply reply;
                reply.request = request;
                reply.type = "chat";
                try {
                    Json confirmedLikes = Json::array();
                    Json createdPersonas = Json::array();
                    auto outcome = runChatLoop(
                        loadAgentConfig(), text,
                        [this, request, &confirmedLikes,
                         &createdPersonas](const std::string& name, const Json& args) {
                            // Persona creation stays in the terminal process: it is the player's
                            // own store, not a music-account action, and it never goes through MCP.
                            if (name == "mora_create_persona") {
                                if (!args.is_object())
                                    throw ToolError("invalid_arguments",
                                                    "persona arguments must be an object");
                                auto created = createCustomPersona(
                                    args.value("id", std::string()),
                                    unutf8(args.value("name", std::string())),
                                    unutf8(args.value("tagline", std::string())),
                                    unutf8(args.value("voice", std::string())),
                                    unutf8(args.value("taste", std::string())),
                                    unutf8(args.value("greeting", std::string())));
                                Json result = {{"id", created.id},
                                               {"name", utf8(created.name)},
                                               {"tagline", utf8(created.tagline)},
                                               {"status", "created"}};
                                createdPersonas.push_back(result);
                                return result;
                            }
                            auto result = work.callTool(request, name, args);
                            if (name == "music_set_like" && result.is_object() &&
                                result.contains("songId") && result.contains("liked"))
                                confirmedLikes.push_back(Json{{"songId", result["songId"]},
                                                              {"liked", result["liked"]}});
                            if (name == "mora_create_persona" && result.is_object() &&
                                result.contains("id"))
                                createdPersonas.push_back(result);
                            return result;
                        },
                        {}, systemPrompt);
                    reply.data = Json{{"ok", outcome.ok},
                                      {"code", outcome.code},
                                      {"error", outcome.error},
                                      {"toolCalls", outcome.toolCalls},
                                      {"trace", outcome.trace},
                                      {"personaId", personaId},
                                      {"confirmedLikes", confirmedLikes},
                                      {"createdPersonas", createdPersonas}};
                    reply.text = utf8(outcome.text);
                } catch (const std::exception& e) {
                    reply.type = "error";
                    reply.text = e.what();
                }
                {
                    std::lock_guard lock(chatMutex);
                    chatResult = std::move(reply);
                }
                chatRunning = false;
            });
        }
        // Ambience: one short voluntary comment when a track changes while the user is already in
        // the conversation. Bounded by the caller (rate limit, per-session cap, one per track).
        void reactToTrack(const Track& track) {
            std::wstring prompt = L"（系统提示）刚刚切到《" + track.name + L"》";
            if (!track.artist.empty())
                prompt += L" - " + track.artist;
            prompt += L"。请用当前人设的口吻，就这首歌给一句不超过 25 字、像在场一起听的反应；";
            prompt += L"不要调用工具，不要复述这条系统提示。";
            auto state = session.audioState();
            if (!currentLyrics.instrumental && currentLyrics.songId == track.id) {
                int index = lyricIndexAt(currentLyrics, state.position);
                if (index >= 0 && index < (int)currentLyrics.lines.size())
                    prompt += L"此刻歌词是「" + currentLyrics.lines[(size_t)index].text + L"」。";
            }
            startChat(prompt, L"♪ " + (track.name.empty() ? std::wstring(L"换歌") : track.name));
        }
        void goHome() {
            home = true;
            chatView = false;
            searchFocus = true;
            modal = Modal::None;
            input.clear();
            inputCursor = 0;
            ++qrGeneration;
            cancelPending();
            detailsAsked.clear();
            collection = false;
            mode = 1;
        }
        void search() {
            if (query.empty()) {
                message(L"请输入歌名、歌手、专辑，或它们的组合");
                return;
            }
            home = chatView = false;
            if (options.demo) {
                rows = session.tracks();
                selected = top = 0;
                total = (int)rows.size();
                title = L"离线界面演示";
                message(L"DEMO · 仅显示样例曲目，不是实际搜索结果");
                return;
            }
            auto q = query;
            Kind k = (Kind)(mode == 2 ? 10 : mode == 3 ? 1000 : 1);
            int page = offset;
            if (submit("search", [=](MusicService& s) {
                    auto p = s.search(q, k, page);
                    Reply r;
                    r.type = "search";
                    r.items = std::move(p.items);
                    r.total = p.total;
                    return r;
                }))
                message(L"正在搜索…");
        }
        void executeInput() {
            auto submitted = input;
            auto invocation = parseCommand(input);
            auto verb = invocation.verb, args = invocation.args;
            if (verb.empty())
                return;
            if (chatRunning && verb != L"home" && verb != L"quit") {
                message(L"模型仍在处理上一条消息，请稍候或用 /home 取消");
                return;
            }
            if (verb == L"search" || verb == L"albums" || verb == L"playlists") {
                if (args.empty()) {
                    fillInput(L"/" + verb + L" ");
                    message(L"在命令后输入关键词，再按 Enter");
                    return;
                }
                if (busy) {
                    message(L"正在处理上一项操作，请稍候");
                    return;
                }
                mode = verb == L"albums" ? 2 : verb == L"playlists" ? 3 : 1;
                query = args;
                offset = 0;
                collection = false;
                searchFocus = false;
                search();
            } else if (verb == L"chat") {
                if (args.empty()) {
                    fillInput(L"");
                    message(L"输入你想聊的音乐话题；模型适配器见 /ai");
                    return;
                }
                startChat(args);
            } else if (verb == L"home")
                goHome();
            else if (verb == L"login") {
                if (args == L"qr")
                    startQr();
                else if (args == L"phone") {
                    modal = Modal::Phone;
                    fillInput(phone);
                } else
                    action(A_Login);
            } else if (verb == L"library")
                chooseMode(4);
            else if (verb == L"queue")
                chooseMode(5);
            else if (verb == L"play") {
                if (!args.empty()) {
                    try {
                        size_t consumed = 0;
                        int n = std::stoi(args, &consumed);
                        if (consumed != args.size() || n < 1 || n > (int)rows.size())
                            throw std::runtime_error("range");
                        selected = n - 1;
                    } catch (...) {
                        message(L"请使用结果中的有效序号，例如 /play 1");
                        return;
                    }
                }
                if (rows.empty()) {
                    message(L"先用 /search、/albums 或 /playlists 获取曲目");
                    return;
                }
                home = chatView = false;
                searchFocus = false;
                activate();
            } else if (verb == L"pause")
                action(A_Pause);
            else if (verb == L"next")
                action(A_Next);
            else if (verb == L"prev")
                action(A_Previous);
            else if (verb == L"like")
                action(A_Like);
            else if (verb == L"add")
                action(A_Save);
            else if (verb == L"quality") {
                if (args.empty())
                    action(A_Quality);
                else {
                    auto it = std::find_if(qualities.begin(), qualities.end(),
                                           [&](auto q) { return unutf8(q) == args; });
                    if (it == qualities.end()) {
                        message(L"可用音质：standard / higher / exhigh / lossless / hires");
                        return;
                    }
                    action(A_QualityRow + (int)(it - qualities.begin()));
                }
            } else if (verb == L"volume" || verb == L"seek") {
                try {
                    size_t n = 0;
                    double value = std::stod(args, &n);
                    if (n != args.size() || !std::isfinite(value))
                        throw std::runtime_error("number");
                    if (verb == L"volume") {
                        if (value < 0 || value > 100)
                            throw std::runtime_error("range");
                        session.setVolume(value / 100);
                    } else {
                        bool relative = !args.empty() && (args[0] == L'+' || args[0] == L'-');
                        session.seek(relative ? session.audioState().position + value : value);
                    }
                } catch (...) {
                    message(verb == L"volume" ? L"格式：/volume 0–100"
                                              : L"格式：/seek 90 或 /seek +10");
                    return;
                }
            } else if (verb == L"repeat" || verb == L"motion") {
                if (!args.empty() && args != L"on" && args != L"off") {
                    message(L"参数使用 on 或 off");
                    return;
                }
                if (verb == L"repeat") {
                    if (!args.empty())
                        session.setRepeat(args == L"off");
                    action(A_Repeat);
                } else {
                    if (!args.empty())
                        motion = args == L"off";
                    action(A_Motion);
                }
            } else if (verb == L"ai")
                modal = Modal::Ai;
            else if (verb == L"name") {
                if (args.empty())
                    editName();
                else if (setUserName(args))
                    message(L"聊天称呼已设为 " + userName);
                else {
                    message(L"名字须为 1–24 个字符，不能包含换行");
                    return;
                }
            } else if (verb == L"settings")
                modal = Modal::Settings;
            else if (verb == L"guide") {
                guidePage = 0;
                modal = Modal::Guide;
            }
            else if (verb == L"effort") {
                auto config = loadAgentConfig();
                if (args.empty()) {
                    std::wstring list;
                    for (auto level : effortLevels) {
                        if (!list.empty())
                            list += L" / ";
                        list += level[0] ? unutf8(level) : L"default";
                    }
                    message(L"当前思考强度 " + effortLabel(config.reasoningEffort) + L" · 可用 " +
                            list);
                } else if (!setReasoningEffort(utf8(args)))
                    message(L"可用档位：default / off / minimal / low / medium / high / xhigh / "
                            L"max");
            } else if (verb == L"audio-device") {
                if (args.empty())
                    openDevicePicker();
                else
                    selectOutputDeviceByNameOrIndex(args);
            } else if (verb == L"persona") {
                if (args.empty())
                    openPersonaPicker();
                else if (selectPersona(utf8(args), true))
                    refreshPersonas();
                else
                    message(L"未知人设；输入 /persona 查看可用列表");
            } else if (verb == L"react") {
                if (args == L"on") {
                    reactEnabled = true;
                    message(L"实时反应已开启 · 换歌时自动说一句（每首一次，间隔≥45 秒）");
                } else if (args == L"off") {
                    reactEnabled = false;
                    reactPending = false;
                    message(L"实时反应已关闭");
                } else if (args == L"now") {
                    if (session.playing().id)
                        reactToTrack(session.playing());
                    else
                        message(L"先播放一首歌");
                } else
                    message(L"用法：/react on | off | now");
            } else if (verb == L"lyrics") {
                const Track& track = session.playing();
                if (!track.id) {
                    message(L"先播放一首歌");
                    return;
                }
                if (currentLyrics.songId != track.id || currentLyrics.lines.empty()) {
                    message(currentLyrics.instrumental ? L"这首歌是纯音乐，没有歌词"
                                                       : L"歌词还在加载，请稍后再试");
                    return;
                }
                int index = lyricIndexAt(currentLyrics, session.audioState().position);
                message(L"♪ " + (index >= 0 && index < (int)currentLyrics.lines.size()
                                     ? currentLyrics.lines[(size_t)index].text
                                     : std::wstring(L"（前奏）")));
            } else if (verb == L"agent")
                modal = Modal::Agent;
            else if (verb == L"help")
                action(A_Help);
            else if (verb == L"quit")
                quit = true;
            else if (verb == L"resume")
                submit("resume", [](MusicService& s) {
                    s.resume();
                    Reply r;
                    r.type = "resumed";
                    return r;
                });
            else {
                message(L"未知命令，输入 / 查看列表，或 /help 获取帮助");
                return;
            }
            if (verb != L"login" && verb != L"ai") {
                if (history.empty() || history.back() != submitted)
                    history.push_back(submitted);
                if (history.size() > 40)
                    history.erase(history.begin());
            }
            if (modal != Modal::Phone && modal != Modal::Code && modal != Modal::Name) {
                input.clear();
                inputCursor = 0;
                suggestion = 0;
                historyIndex = -1;
            }
        }
        void chooseMode(int m) {
            if (busy)
                return;
            home = chatView = false;
            searchFocus = false;
            mode = m;
            collection = false;
            selected = top = offset = 0;
            rows.clear();
            title = m == 1   ? L"单曲"
                    : m == 2 ? L"专辑"
                    : m == 3 ? L"歌单"
                    : m == 4 ? L"我的音乐"
                             : L"播放队列";
            if (m == 4)
                submit("library", [](MusicService& s) {
                    Reply r;
                    r.type = "library";
                    r.items = s.library();
                    r.nickname = s.nickname();
                    r.uid = s.uid();
                    return r;
                });
            else if (m == 5) {
                rows = session.tracks();
                selected = (int)session.index();
                top = std::max(0, selected - listH / 2);
                total = (int)rows.size();
            } else if (!query.empty())
                search();
        }
        bool playCurrent() {
            if (!session.currentTrack())
                return false;
            Track t = *session.currentTrack();
            lastPlayError.clear();
            if (options.demo) {
                session.setCurrent(t);
                message(L"演示模式 · 可使用 --local 文件名 试听本地音频");
                return true;
            }
            auto level = session.quality();
            if (!submit("play", [t, level](MusicService& s) {
                    Reply r;
                    r.type = "play";
                    r.playback = s.playback(t, level);
                    r.nickname = s.nickname();
                    r.uid = s.uid();
                    return r;
                }))
                return false;
            message(L"正在获取账号授权的播放地址…");
            return true;
        }
        void activate() {
            if (rows.empty() || selected < 0 || selected >= (int)rows.size())
                return;
            auto t = rows[selected];
            if (t.kind == Kind::Song) {
                // The queue must not move for a request that cannot start: otherwise the reply of
                // the still-running request would overwrite the newer selection.
                if (busy) {
                    message(L"正在处理上一项操作，请稍候");
                    return;
                }
                session.assign(rows, selected);
                playCurrent();
            } else {
                backRows = rows;
                backMode = mode;
                backSelected = selected;
                backCollection = collection;
                submit("collection", [t](MusicService& s) {
                    Reply r;
                    r.type = "collection";
                    r.items = t.kind == Kind::Album ? s.album(t.id) : s.playlist(t.id);
                    r.nickname = t.name;
                    return r;
                });
            }
        }
        void navigate(int amount) {
            selected = std::clamp(selected + amount, 0, std::max(0, (int)rows.size() - 1));
            if (selected < top)
                top = selected;
            if (selected >= top + listH)
                top = selected - listH + 1;
        }
        const Track* target() {
            if (!rows.empty() && selected < (int)rows.size() && rows[selected].kind == Kind::Song)
                return &rows[selected];
            if (session.playing().id)
                return &session.playing();
            return nullptr;
        }
        void like() {
            auto t = target();
            if (!t) {
                message(L"先选择一首歌曲");
                return;
            }
            int64_t id = t->id;
            bool value = !likes.contains(id);
            submit("like", [id, value](MusicService& s) {
                s.like(id, value);
                Reply r;
                r.type = "like";
                r.id = id;
                r.code = value;
                return r;
            });
        }
        void save() {
            auto t = target();
            if (!t) {
                message(L"先选择一首歌曲");
                return;
            }
            saveSong = t->id;
            saveSelected = 0;
            submit("save-list", [](MusicService& s) {
                Reply r;
                r.type = "save-list";
                r.items = s.library(true);
                return r;
            });
        }
        void saveTo(int index) {
            if (index < 0 || index >= (int)saveRows.size())
                return;
            auto song = saveSong, pid = saveRows[index].id;
            submit("save", [song, pid](MusicService& s) {
                s.save(song, pid);
                Reply r;
                r.type = "saved";
                return r;
            });
        }
        void startQr() {
            ++qrGeneration;
            int gen = qrGeneration;
            qrkey.clear();
            qrStart = Clock::now();
            modal = Modal::Qr;
            if (!submit("qr-key", [gen](MusicService& s) {
                    Reply r;
                    r.type = "qr-key";
                    r.text = s.qrKey();
                    r.code = gen;
                    return r;
                })) {
                modal = Modal::None;
                ++qrGeneration;
            }
        }
        void sms() {
            std::string p = utf8(phone);
            if (p.size() < 6 || p.size() > 18 ||
                !std::all_of(p.begin(), p.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                message(L"请输入有效手机号（默认国家区号 86）");
                return;
            }
            submit("sms", [p](MusicService& s) {
                s.sendSms(p);
                Reply r;
                r.type = "sms";
                return r;
            });
        }
        void loginPhone() {
            auto p = utf8(phone), code = utf8(input);
            if (code.size() < 4 || code.size() > 8 ||
                !std::all_of(code.begin(), code.end(),
                             [](char c) { return c >= '0' && c <= '9'; })) {
                message(L"请输入短信中的数字验证码");
                return;
            }
            submit("phone", [p, code](MusicService& s) {
                s.phoneLogin(p, code);
                Reply r;
                r.type = "account";
                r.nickname = s.nickname();
                r.uid = s.uid();
                r.likes = s.likes();
                return r;
            });
        }
        void back() {
            if (modal != Modal::None) {
                if (modal == Modal::Guide) {
                    finishGuide();
                    return;
                }
                if (modal == Modal::AiKey || modal == Modal::AiModel) {
                    modal = Modal::Ai;
                    input.assign(input.size(), L'\0');
                    input.clear();
                    inputCursor = 0;
                    return;
                }
                modal = Modal::None;
                ++qrGeneration;
                qrkey.clear();
                input.clear();
                inputCursor = 0;
                searchFocus = home || chatView;
                return;
            }
            if (searchFocus) {
                input.clear();
                inputCursor = 0;
                searchFocus = home || chatView;
                return;
            }
            if (collection) {
                rows = std::move(backRows);
                selected = backSelected;
                top = std::max(0, selected - listH / 2);
                mode = backMode;
                collection = backCollection;
                title = L"搜索结果";
                detailsAsked.clear();
                return;
            }
            goHome();
        }
        void page(int direction) {
            if (collection || mode == 5) {
                navigate(direction * 50);
                return;
            }
            if (mode > 3)
                return;
            int next = offset + direction * 30;
            if (next < 0 || (total && next >= total))
                return;
            offset = next;
            search();
        }
        void action(int a) {
            if (a >= A_Suggestion) {
                auto suggestions = commandSuggestions(input);
                int index = a - A_Suggestion;
                if (index >= 0 && index < (int)suggestions.size())
                    fillInput(L"/" + suggestions[index].name + L" ");
                return;
            }
            if (a >= A_QualityRow) {
                int i = a - A_QualityRow;
                if (i >= 0 && i < 5) {
                    session.setQuality(qualities[i]);
                    qualitySelected = i;
                    modal = Modal::None;
                    message(L"音质将在下一首歌生效；是否可用由账号权限决定");
                }
                return;
            }
            if (a >= A_SaveRow) {
                saveTo(a - A_SaveRow);
                return;
            }
            if (a >= A_Row) {
                selected = std::clamp(top + a - A_Row, 0, std::max(0, (int)rows.size() - 1));
                return;
            }
            switch (a) {
            case A_Search:
                fillInput(L"/search ");
                break;
            case A_Songs:
                chooseMode(1);
                break;
            case A_Albums:
                chooseMode(2);
                break;
            case A_Playlists:
                chooseMode(3);
                break;
            case A_Library:
                chooseMode(4);
                break;
            case A_Queue:
                chooseMode(5);
                break;
            case A_Login:
                modal = Modal::Login;
                searchFocus = false;
                break;
            case A_Previous:
                if (!busy && session.move(-1))
                    playCurrent();
                break;
            case A_Next:
                if (!busy && session.move(1))
                    playCurrent();
                break;
            case A_Pause:
                session.pause();
                break;
            case A_Like:
                like();
                break;
            case A_Save:
                save();
                break;
            case A_Quality:
                modal = Modal::Quality;
                break;
            case A_Qr:
                startQr();
                break;
            case A_Phone:
                modal = Modal::Phone;
                input = phone;
                break;
            case A_Logout:
                submit("logout", [](MusicService& s) {
                    s.logout();
                    Reply r;
                    r.type = "logout";
                    return r;
                });
                break;
            case A_Back:
            case A_Close:
                back();
                break;
            case A_Send:
                if (modal == Modal::Phone) {
                    phone = input;
                    sms();
                } else if (modal == Modal::Code)
                    loginPhone();
                break;
            case A_Motion:
                motion = !motion;
                message(motion ? L"音频动效已开启" : L"音频动效已关闭");
                break;
            case A_Repeat:
                session.setRepeat(!session.repeat());
                message(session.repeat() ? L"循环播放已开启" : L"顺序播放");
                break;
            case A_Help:
                modal = Modal::Help;
                break;
            case A_Guide:
                guidePage = 0;
                modal = Modal::Guide;
                break;
            case A_GuideNext:
                if (guidePage < 2) ++guidePage;
                else finishGuide();
                break;
            case A_GuidePrevious:
                guidePage = std::max(0, guidePage - 1);
                break;
            case A_PagePrev:
                page(-1);
                break;
            case A_PageNext:
                page(1);
                break;
            case A_VolDown:
                session.adjustVolume(-.05);
                break;
            case A_VolUp:
                session.adjustVolume(.05);
                break;
            case A_Home:
                goHome();
                break;
            case A_AI:
                modal = Modal::Ai;
                break;
            case A_AiKey:
                modal = Modal::AiKey;
                input.clear();
                inputCursor = 0;
                break;
            case A_AiModel:
                modal = Modal::AiModel;
                input = unutf8(loadAgentConfig().model);
                inputCursor = input.size();
                break;
            case A_Name:
                editName();
                break;
            case A_Persona:
                openPersonaPicker();
                break;
            case A_Device:
                openDevicePicker();
                break;
            case A_DeviceRow + 0:
            case A_DeviceRow + 1:
            case A_DeviceRow + 2:
            case A_DeviceRow + 3:
            case A_DeviceRow + 4:
            case A_DeviceRow + 5:
            case A_DeviceRow + 6:
            case A_DeviceRow + 7:
            case A_DeviceRow + 8:
            case A_DeviceRow + 9:
            case A_DeviceRow + 10:
            case A_DeviceRow + 11:
            case A_DeviceRow + 12:
            case A_DeviceRow + 13:
            case A_DeviceRow + 14:
            case A_DeviceRow + 15:
                selectOutputDevice(a - A_DeviceRow);
                modal = Modal::None;
                break;
            case A_PersonaRow + 0:
            case A_PersonaRow + 1:
            case A_PersonaRow + 2:
            case A_PersonaRow + 3:
            case A_PersonaRow + 4:
            case A_PersonaRow + 5:
            case A_PersonaRow + 6:
            case A_PersonaRow + 7:
            case A_PersonaRow + 8:
            case A_PersonaRow + 9:
            case A_PersonaRow + 10:
            case A_PersonaRow + 11:
            case A_PersonaRow + 12:
            case A_PersonaRow + 13:
            case A_PersonaRow + 14:
            case A_PersonaRow + 15:
                personaSelected = a - A_PersonaRow;
                if (personaSelected < (int)personaList.size()) {
                    selectPersona(personaList[(size_t)personaSelected].id, true);
                    modal = Modal::None;
                }
                break;
            case A_Agent:
                modal = Modal::Agent;
                break;
            case A_Mascot:
                message(persona.name + L" · " + persona.tagline + L"。");
                break;
            }
        }
        // Adapter read-only network work runs on the shared worker so the MCP process never
        // needs its own HTTP stack or a second account session.
        void startIpc(std::string method, const Json& params, BridgeCompletion done) {
            uint64_t ticket = ++ipcSequence;
            auto query = params.value("query", std::string());
            auto kind = params.value("kind", std::string("song"));
            int offset = params.value("offset", 0);
            int limit = params.value("limit", 10);
            int64_t id = params.value("id", (int64_t)0);
            bool liked = params.value("liked", false);
            int64_t songId = params.value("songId", (int64_t)0);
            int64_t playlistId = params.value("playlistId", (int64_t)0);
            bool includeTranslation = params.value("includeTranslation", true);
            int maxLines = std::clamp(params.value("maxLines", 60), 1, 200);
            // Lyrics need the live track and position, which only this thread owns. Serve the cached
            // copy when it is the same song; otherwise fetch once on the worker.
            Track lyricTrack;
            double lyricPosition = 0;
            if (method == "lyrics") {
                lyricTrack = session.playing();
                if (songId <= 0)
                    songId = lyricTrack.id;
                if (songId <= 0) {
                    done(BridgeReply(false, "not_playing", "no track is playing", Json::object()));
                    return;
                }
                if (songId == lyricTrack.id)
                    lyricPosition = session.audioState().position;
                if (songId == lyricTrack.id && currentLyrics.songId == songId &&
                    !currentLyrics.lines.empty()) {
                    done(BridgeReply(true, "", "",
                                     lyricsJson(currentLyrics, songId, &lyricTrack, lyricPosition,
                                                includeTranslation,
                                                static_cast<size_t>(maxLines))));
                    return;
                }
            }
            ipcPending[ticket] = std::move(done);
            auto posted = work.postAdapter(
                ticket, "ipc-" + method,
                [method, query, kind, offset, limit, id, liked, songId, playlistId,
                 includeTranslation, maxLines, lyricTrack, lyricPosition,
                 ticket](MusicService& s) {
                    Reply r;
                    r.type = "ipc-" + method;
                    r.id = (int64_t)ticket;
                    if (method == "lyrics") {
                        auto loaded = s.lyrics(songId);
                        r.data = lyricsJson(loaded, songId,
                                            songId == lyricTrack.id ? &lyricTrack : nullptr,
                                            lyricPosition, includeTranslation,
                                            static_cast<size_t>(maxLines));
                        return r;
                    }
                    if (method == "playlists") {
                        // The player knows whose playlists these are: mark writability here so the
                        // adapter never has to guess with a possibly stale uid.
                        auto uid = s.uid();
                        Json items = Json::array();
                        for (auto& playlist : s.library(true)) {
                            auto item = trackJson(playlist);
                            item["owner"] = playlist.owner == uid;
                            item["canWrite"] = playlist.owner == uid;
                            items.push_back(std::move(item));
                        }
                        r.data = Json{{"total", (int)items.size()}, {"items", items}};
                        return r;
                    }
                    if (method == "like") {
                        s.like(id, liked);
                        r.data = Json{{"songId", id}, {"liked", liked}};
                        return r;
                    }
                    if (method == "add") {
                        s.save(songId, playlistId);
                        r.data = Json{{"songId", songId}, {"playlistId", playlistId}};
                        return r;
                    }
                    if (method == "search") {
                        auto page = s.search(unutf8(query),
                                             kind == "album"        ? Kind::Album
                                             : kind == "playlist"   ? Kind::Playlist
                                                                    : Kind::Song,
                                             offset);
                        r.total = page.total;
                        r.items = std::move(page.items);
                    } else {
                        r.items = kind == "album" ? s.album(id) : s.playlist(id);
                        r.total = (int)r.items.size();
                    }
                    if (limit > 0 && (int)r.items.size() > limit)
                        r.items.resize((size_t)limit);
                    return r;
                });
            if (!posted) {
                auto it = ipcPending.find(ticket);
                if (it != ipcPending.end()) {
                    auto reply = BridgeReply(false, "busy",
                                             "the terminal player is finishing another request",
                                             Json::object());
                    auto completion = std::move(it->second);
                    ipcPending.erase(it);
                    completion(std::move(reply));
                }
            }
        }
        void completeIpc(const Reply& r) {
            // The worker stamps every adapter reply with its ticket, including error replies, so a
            // failed request still answers its waiting frame instead of timing out.
            auto it = ipcPending.find(r.ticket);
            if (it == ipcPending.end())
                return;
            auto done = std::move(it->second);
            ipcPending.erase(it);
            if (r.type == "error") {
                auto code = r.code == 301        ? "needs_login"
                            : r.code == 502      ? "already_present"
                            : isRiskCode(r.code) ? "risk_cooldown"
                                                 : "network_unavailable";
                done(BridgeReply(false, code, r.text, Json::object()));
                return;
            }
            if (!r.data.is_null()) {
                done(BridgeReply(true, "", "", std::move(r.data)));
                return;
            }
            Json items = Json::array();
            for (auto& t : r.items)
                items.push_back(trackJson(t));
            done(BridgeReply(true, "", "", Json{{"total", r.total}, {"items", items}}));
        }
        Json sessionView() const {
            auto state = session.audioState();
            const Track& track = session.playing();
            Json current = nullptr;
            if (track.id)
                current = Json{{"id", track.id},
                               {"name", utf8(track.name)},
                               {"artist", utf8(track.artist)},
                               {"album", utf8(track.album)},
                               {"durationSeconds", track.duration}};
            return Json{
                {"playing", state.playing},
                {"paused", state.paused},
                {"finished", state.finished},
                {"positionSeconds", state.position},
                {"durationSeconds", state.duration},
                {"volumePercent", (int)std::lround(session.volume() * 100)},
                {"quality", session.actualQuality().empty() ? session.quality()
                                                            : session.actualQuality()},
                {"sampleRate", state.sampleRate},
                {"sampleBits", state.sampleBits},
                {"repeat", session.repeat()},
                {"queueIndex", (int)session.index()},
                {"queueSize", (int)session.tracks().size()},
                {"lastError", utf8(lastPlayError)},
                {"audioError", utf8(audioError)},
                {"lyrics", lyricsView(track, state.position)},
                {"track", current}};
        }
        // A small, bounded projection of the cached lyrics so the agent knows what is being sung
        // right now without shipping a whole song into every model turn.
        Json lyricsView(const Track& track, double position) const {
            if (!track.id || currentLyrics.songId != track.id)
                return Json{{"available", false}};
            if (currentLyrics.instrumental)
                return Json{{"available", false}, {"instrumental", true}};
            int index = lyricIndexAt(currentLyrics, position);
            Json view = {{"available", !currentLyrics.lines.empty()},
                         {"instrumental", currentLyrics.instrumental},
                         {"lineCount", (int64_t)currentLyrics.lines.size()},
                         {"currentIndex", index}};
            if (index >= 0 && index < (int)currentLyrics.lines.size()) {
                view["currentLine"] = utf8(currentLyrics.lines[(size_t)index].text);
                view["currentLineSeconds"] = currentLyrics.lines[(size_t)index].seconds;
                if (!currentLyrics.lines[(size_t)index].translation.empty())
                    view["currentTranslation"] =
                        utf8(currentLyrics.lines[(size_t)index].translation);
                if (index + 1 < (int)currentLyrics.lines.size())
                    view["nextLine"] = utf8(currentLyrics.lines[(size_t)index + 1].text);
            }
            return view;
        }
        // One line for the chat prompt: what is playing, how far in, and what is being sung.
        std::wstring playbackContext() const {
            const Track& track = session.playing();
            if (!track.id)
                return L"当前没有在播放歌曲。";
            auto state = session.audioState();
            int minutes = (int)(state.position / 60), seconds = (int)state.position % 60;
            std::wstring text = L"当前正在播放《" + track.name + L"》";
            if (!track.artist.empty())
                text += L" - " + track.artist;
            text += state.paused ? L"（已暂停）" : L"";
            text += L"，进度 " + std::to_wstring(minutes) + L":" +
                    (seconds < 10 ? L"0" : L"") + std::to_wstring(seconds);
            if (track.duration > 0)
                text += L" / " + std::to_wstring(track.duration / 60) + L":" +
                        (track.duration % 60 < 10 ? L"0" : L"") +
                        std::to_wstring(track.duration % 60);
            if (!currentLyrics.instrumental && currentLyrics.songId == track.id) {
                int index = lyricIndexAt(currentLyrics, state.position);
                if (index >= 0 && index < (int)currentLyrics.lines.size())
                    text += L"；此刻歌词是「" + currentLyrics.lines[(size_t)index].text + L"」";
            }
            return text + L"。";
        }
        // Runs on the UI thread through BridgeServer::pump: adapters only ever describe intent,
        // the session stays owned here.
        BridgeReply control(const std::string& method, const Json& params) {
            auto busyReply = [] {
                return BridgeReply(false, "busy",
                                   "the terminal player is finishing another request",
                                   Json::object());
            };
            if (method == "hello")
                return BridgeReply(true, "", "",
                                   Json{{"version", 1},
                                        {"player", "tui"},
                                        {"pid", (int64_t)GetCurrentProcessId()}});
            if (method == "status")
                return BridgeReply(true, "", "", sessionView());
            if (method == "play") {
                if (busy)
                    return busyReply();
                auto kind = params.value("kind", std::string("song"));
                int64_t id = params.value("id", (int64_t)0);
                if (id <= 0)
                    return BridgeReply(false, "invalid_arguments", "id must be a positive id",
                                       Json::object());
                if (kind == "song") {
                    session.assignId(id);
                    if (!playCurrent())
                        return busyReply();
                    return BridgeReply(true, "", "",
                                       Json{{"status", "accepted"}, {"kind", kind}, {"id", id}});
                }
                if (kind == "album" || kind == "playlist") {
                    lastPlayError.clear();
                    int start = params.value("startIndex", 0);
                    if (start < 0 || start > 9999)
                        return BridgeReply(false, "invalid_arguments",
                                           "startIndex must be 0..9999", Json::object());
                    if (!submit("play-collection", [kind, id, start](MusicService& s) {
                            Reply r;
                            r.type = "play-collection";
                            r.items = kind == "album" ? s.album(id) : s.playlist(id);
                            r.code = start;
                            return r;
                        }))
                        return busyReply();
                    return BridgeReply(true, "", "",
                                       Json{{"status", "accepted"},
                                            {"kind", kind},
                                            {"id", id},
                                            {"startIndex", start}});
                }
                return BridgeReply(false, "invalid_arguments",
                                   "kind must be song, album or playlist", Json::object());
            }
            if (method == "control") {
                auto action = params.value("action", std::string());
                if (action == "pause" || action == "resume") {
                    session.setPaused(action == "pause");
                    return BridgeReply(true, "", "", sessionView());
                }
                if (action == "next" || action == "previous") {
                    if (busy)
                        return busyReply();
                    if (!session.move(action == "next" ? 1 : -1))
                        return BridgeReply(false, "queue_boundary",
                                           "no other track in the queue", Json::object());
                    playCurrent();
                    return BridgeReply(true, "", "", Json{{"status", "accepted"}});
                }
                if (action == "seek" || action == "volume") {
                    if (!params.contains("value") || !params["value"].is_number())
                        return BridgeReply(false, "invalid_arguments", "value must be a number",
                                           Json::object());
                    double value = params["value"].get<double>();
                    if (action == "seek") {
                        if (!(value >= 0))
                            return BridgeReply(false, "invalid_arguments",
                                               "seek value must be >= 0 seconds", Json::object());
                        session.seek(value);
                    } else {
                        if (!(value >= 0 && value <= 100))
                            return BridgeReply(false, "invalid_arguments",
                                               "volume value must be 0..100", Json::object());
                        session.setVolume(value / 100);
                    }
                    return BridgeReply(true, "", "", sessionView());
                }
                return BridgeReply(false, "invalid_arguments",
                                   "action must be pause, resume, next, previous, seek or volume",
                                   Json::object());
            }
            return BridgeReply(false, "unknown_method", "unknown session method: " + method,
                               Json::object());
        }
        void key(const KEY_EVENT_RECORD& k) {
            if (!k.bKeyDown)
                return;
            auto c = k.uChar.UnicodeChar;
            bool ctrl = k.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED);
            if (ctrl && k.wVirtualKeyCode == 'C') {
                cancelPending();
                quit = true;
                return;
            }
            if (k.wVirtualKeyCode == VK_ESCAPE) {
                back();
                return;
            }
            if (ctrl && k.wVirtualKeyCode == 'K' && modal == Modal::None) {
                fillInput(L"/");
                return;
            }
            if (modal == Modal::Login) {
                if (c == L'1' || k.wVirtualKeyCode == VK_RETURN)
                    action(A_Qr);
                if (c == L'2')
                    action(A_Phone);
                if (k.wVirtualKeyCode == 'D')
                    action(A_Logout);
                return;
            }
            if (modal == Modal::Qr)
                return;
            if (modal == Modal::Save) {
                if (k.wVirtualKeyCode == VK_UP)
                    saveSelected = std::max(0, saveSelected - 1);
                if (k.wVirtualKeyCode == VK_DOWN)
                    saveSelected =
                        std::min(std::max(0, (int)saveRows.size() - 1), saveSelected + 1);
                if (k.wVirtualKeyCode == VK_RETURN)
                    saveTo(saveSelected);
                return;
            }
            if (modal == Modal::Quality) {
                if (k.wVirtualKeyCode == VK_UP)
                    qualitySelected = std::max(0, qualitySelected - 1);
                if (k.wVirtualKeyCode == VK_DOWN)
                    qualitySelected = std::min(4, qualitySelected + 1);
                if (k.wVirtualKeyCode == VK_RETURN)
                    action(A_QualityRow + qualitySelected);
                return;
            }
            if (modal == Modal::Name) {
                if (k.wVirtualKeyCode == VK_RETURN) {
                    if (setUserName(input)) {
                        modal = guideDone ? Modal::None : Modal::Guide;
                        guidePage = 0;
                        input.clear();
                        inputCursor = 0;
                        message(L"聊天称呼已设为 " + userName);
                    } else
                        message(L"名字须为 1–24 个字符，不能包含换行");
                    return;
                }
                if (k.wVirtualKeyCode == VK_BACK) {
                    if (inputCursor) {
                        input.erase(inputCursor - 1, 1);
                        --inputCursor;
                    }
                    return;
                }
                if (c >= L' ' && input.size() < 24) {
                    input.insert(inputCursor, 1, c);
                    ++inputCursor;
                }
                return;
            }
            if (modal == Modal::Guide) {
                if (k.wVirtualKeyCode == VK_RETURN || k.wVirtualKeyCode == VK_RIGHT)
                    action(A_GuideNext);
                else if (k.wVirtualKeyCode == VK_LEFT)
                    action(A_GuidePrevious);
                else if (k.wVirtualKeyCode == 'S')
                    finishGuide();
                return;
            }
            if (modal == Modal::Ai) {
                if (k.wVirtualKeyCode == 'K') action(A_AiKey);
                else if (k.wVirtualKeyCode == 'M') action(A_AiModel);
                return;
            }
            if (modal == Modal::AiKey || modal == Modal::AiModel) {
                if (k.wVirtualKeyCode == VK_RETURN) {
                    try {
                        if (modal == Modal::AiKey) {
                            if (input.empty()) { message(L"密钥不能为空"); return; }
                            saveAgentKey(utf8(input));
                            message(L"模型密钥已安全保存");
                        } else {
                            if (input.empty() || input.size() > 128 ||
                                input.find_first_of(L" \t\r\n") != std::wstring::npos) {
                                message(L"模型 ID 须为 1–128 个非空白字符");
                                return;
                            }
                            auto config = loadAgentConfig();
                            config.model = utf8(input);
                            saveAgentConfig(config);
                            message(L"模型 ID 已保存");
                        }
                        input.assign(input.size(), L'\0');
                        input.clear();
                        inputCursor = 0;
                        modal = Modal::Ai;
                    } catch (const std::exception&) {
                        message(L"保存模型配置失败，请检查本机配置目录");
                    }
                    return;
                }
                if (k.wVirtualKeyCode == VK_BACK) {
                    if (inputCursor) { input.erase(inputCursor - 1, 1); --inputCursor; }
                    return;
                }
                if (c >= L' ' && c != 127 && input.size() < 512) {
                    input.insert(inputCursor, 1, c);
                    ++inputCursor;
                }
                return;
            }
            if (modal == Modal::Persona) {
                if (k.wVirtualKeyCode == VK_UP)
                    personaSelected = std::max(0, personaSelected - 1);
                if (k.wVirtualKeyCode == VK_DOWN)
                    personaSelected =
                        std::min(std::max(0, (int)personaList.size() - 1), personaSelected + 1);
                if (k.wVirtualKeyCode == VK_RETURN && personaSelected < (int)personaList.size()) {
                    selectPersona(personaList[(size_t)personaSelected].id, true);
                    modal = Modal::None;
                }
                if (k.wVirtualKeyCode == VK_ESCAPE)
                    modal = Modal::None;
                return;
            }
            if (modal == Modal::Device) {
                if (k.wVirtualKeyCode == VK_UP)
                    deviceSelected = std::max(0, deviceSelected - 1);
                if (k.wVirtualKeyCode == VK_DOWN)
                    deviceSelected =
                        std::min(std::max(0, (int)outputList.size() - 1), deviceSelected + 1);
                if (k.wVirtualKeyCode == VK_RETURN && deviceSelected < (int)outputList.size()) {
                    selectOutputDevice(deviceSelected);
                    modal = Modal::None;
                }
                if (k.wVirtualKeyCode == VK_ESCAPE)
                    modal = Modal::None;
                return;
            }
            if (modal == Modal::Help || modal == Modal::Agent ||
                modal == Modal::Settings)
                return;
            if (modal == Modal::Phone || modal == Modal::Code) {
                if (k.wVirtualKeyCode == VK_RETURN) {
                    action(A_Send);
                    return;
                }
                if (k.wVirtualKeyCode == VK_BACK) {
                    if (!input.empty())
                        input.pop_back();
                    return;
                }
                if (c >= L'0' && c <= L'9' && input.size() < 18)
                    input += c;
                return;
            }
            if (searchFocus) {
                auto suggestions = commandSuggestions(input);
                if ((k.wVirtualKeyCode == VK_UP || k.wVirtualKeyCode == VK_DOWN) &&
                    !suggestions.empty()) {
                    suggestion = std::clamp(suggestion + (k.wVirtualKeyCode == VK_UP ? -1 : 1), 0,
                                            (int)suggestions.size() - 1);
                    return;
                }
                if (k.wVirtualKeyCode == VK_TAB) {
                    if (!suggestions.empty()) {
                        suggestion = std::clamp(suggestion, 0, (int)suggestions.size() - 1);
                        fillInput(L"/" + suggestions[suggestion].name + L" ");
                    } else if (!home && !chatView)
                        searchFocus = false;
                    else
                        fillInput(L"/");
                    return;
                }
                if (k.wVirtualKeyCode == VK_RETURN) {
                    if (!suggestions.empty()) {
                        suggestion = std::clamp(suggestion, 0, (int)suggestions.size() - 1);
                        auto spec = suggestions[suggestion];
                        if (parseCommand(input).verb != spec.name) {
                            fillInput(L"/" + spec.name);
                            if (!spec.usage.empty() && spec.usage[0] == L'<') {
                                fillInput(input + L" ");
                                return;
                            }
                        }
                    }
                    executeInput();
                    return;
                }
                if (k.wVirtualKeyCode == VK_LEFT) {
                    if (inputCursor)
                        --inputCursor;
                    return;
                }
                if (k.wVirtualKeyCode == VK_RIGHT) {
                    if (inputCursor < input.size())
                        ++inputCursor;
                    return;
                }
                if (k.wVirtualKeyCode == VK_HOME) {
                    inputCursor = 0;
                    return;
                }
                if (k.wVirtualKeyCode == VK_END) {
                    inputCursor = input.size();
                    return;
                }
                if (k.wVirtualKeyCode == VK_BACK) {
                    if (inputCursor) {
                        input.erase(inputCursor - 1, 1);
                        --inputCursor;
                    }
                    suggestion = 0;
                    return;
                }
                if (k.wVirtualKeyCode == VK_DELETE) {
                    if (inputCursor < input.size())
                        input.erase(inputCursor, 1);
                    return;
                }
                if (ctrl && k.wVirtualKeyCode == 'U') {
                    input.clear();
                    inputCursor = 0;
                    return;
                }
                if (k.wVirtualKeyCode == VK_UP || k.wVirtualKeyCode == VK_DOWN) {
                    if (history.empty())
                        return;
                    if (historyIndex < 0)
                        historyIndex = (int)history.size();
                    historyIndex = std::clamp(historyIndex + (k.wVirtualKeyCode == VK_UP ? -1 : 1),
                                              0, (int)history.size());
                    input = historyIndex == (int)history.size() ? L"" : history[historyIndex];
                    inputCursor = input.size();
                    return;
                }
                if (c >= 32 && input.size() < 256) {
                    input.insert(inputCursor, 1, c);
                    ++inputCursor;
                    suggestion = 0;
                }
                return;
            }
            switch (k.wVirtualKeyCode) {
            case VK_UP:
                navigate(-1);
                return;
            case VK_DOWN:
                navigate(1);
                return;
            case VK_PRIOR:
                navigate(-listH);
                return;
            case VK_NEXT:
                navigate(listH);
                return;
            case VK_HOME:
                navigate(-(int)rows.size());
                return;
            case VK_END:
                navigate((int)rows.size());
                return;
            case VK_RETURN:
                activate();
                return;
            case VK_SPACE:
                action(A_Pause);
                return;
            case VK_LEFT:
                session.seek(session.audioState().position - 5);
                return;
            case VK_RIGHT:
                session.seek(session.audioState().position + 5);
                return;
            case VK_TAB:
                fillInput(L"");
                return;
            }
            if (c == L'/') {
                fillInput(L"/");
                return;
            }
            if (c == L'?') {
                action(A_Help);
                return;
            }
            if (c == L'[') {
                page(-1);
                return;
            }
            if (c == L']') {
                page(1);
                return;
            }
            switch (k.wVirtualKeyCode) {
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
                chooseMode(k.wVirtualKeyCode - '0');
                break;
            case 'L':
                action(A_Login);
                break;
            case 'H':
                action(A_Like);
                break;
            case 'A':
                action(A_Save);
                break;
            case 'N':
                action(A_Next);
                break;
            case 'P':
                action(A_Previous);
                break;
            case 'V':
                action(A_Quality);
                break;
            case 'M':
                action(A_Motion);
                break;
            case 'C':
                action(A_Repeat);
                break;
            case 'Q':
                quit = true;
                break;
            case VK_OEM_PLUS:
                action(A_VolUp);
                break;
            case VK_OEM_MINUS:
                action(A_VolDown);
                break;
            default:
                if (c >= 32)
                    fillInput(std::wstring(1, c));
                break;
            }
        }
        void receive() {
            for (auto& r : work.poll()) {
                if (r.ticket && r.ticket == lyricsTicket) {
                    lyricsTicket = 0;
                    if (r.type != "error" && r.lyrics.songId == lyricsSong)
                        currentLyrics = std::move(r.lyrics);
                    continue;
                }
                if (r.ticket) {
                    completeIpc(r); // adapter work is answered by its own ticket
                    continue;
                }
                if (!session.owns(r.request))
                    continue; // 被取代的回复不得改动状态
                busy = false;
                if (r.type == "error") {
                    // A failed metadata load stays retryable by re-entering the list; it never
                    // retries by itself, so one failure cannot become a request storm.
                    message(r.origin == "details"
                                ? unutf8(r.text) + L" · 重新进入列表可重试"
                                : unutf8(r.text));
                    if (r.origin == "play" || r.origin == "play-collection")
                        lastPlayError = sanitize(unutf8(r.text));
                    if (r.origin == "qr-check")
                        qrkey.clear();
                    continue;
                }
                if (r.type == "search") {
                    rows = std::move(r.items);
                    detailsAsked.clear();
                    selected = top = 0;
                    collection = false;
                    total = r.total;
                    title = mode == 1 ? L"单曲" : mode == 2 ? L"专辑" : L"歌单";
                    message(rows.empty() ? L"没有找到结果，试试歌名与歌手的组合"
                                         : L"Enter 播放单曲 / 打开专辑与歌单 · [ ] 翻页");
                } else if (r.type == "library") {
                    rows = std::move(r.items);
                    detailsAsked.clear();
                    total = (int)rows.size();
                    nickname = r.nickname;
                    message(L"我的歌单 · Enter 打开，选择曲目后 Enter 连续播放");
                } else if (r.type == "collection") {
                    rows = std::move(r.items);
                    detailsAsked.clear();
                    selected = top = 0;
                    total = (int)rows.size();
                    collection = true;
                    title = r.nickname;
                    message(L"Enter 从选中曲目连续播放全部 · Esc 返回 · [ ] 跳转 50 首");
                } else if (r.type == "play-collection") {
                    // Adapter-requested playback of a whole album or playlist: replace the queue and
                    // start from the requested index without touching the visible result list.
                    session.assign(std::move(r.items),
                                   static_cast<size_t>(std::max(0, r.code)));
                    playCurrent();
                } else if (r.type == "details") {
                    for (const auto& t : r.items) {
                        session.refresh(t);
                        for (auto& row : rows) {
                            if (row.id != t.id)
                                continue;
                            if (!t.name.empty())
                                row.name = t.name;
                            if (!t.artist.empty())
                                row.artist = t.artist;
                            if (!t.album.empty())
                                row.album = t.album;
                            if (!t.cover.empty())
                                row.cover = t.cover;
                            if (t.duration > 0)
                                row.duration = t.duration;
                            if (t.owner)
                                row.owner = t.owner;
                        }
                    }
                } else if (r.type == "play") {
                    cover = std::move(r.playback.cover);
                    // The cover still tints the player, but only the accent: the persona keeps its
                    // own background, surface and muted colours so identity survives playback.
                    colors.accent = palette(cover).accent;
                    nickname = r.nickname;
                    session.startTrack(r.playback.track, r.playback.quality,
                                       std::move(r.playback.url));
                    audioError.clear();
                    lastPlayError.clear();
                    message(L"正在播放 · H 红心 · A 收藏到歌单");
                } else if (r.type == "qr-key") {
                    if (r.code == qrGeneration && modal == Modal::Qr) {
                        qrkey = r.text;
                        qrStart = Clock::now();
                        qrPoll = Clock::now();
                        message(L"用网易云音乐 App 扫码，并在手机上确认登录");
                    }
                } else if (r.type == "qr-check") {
                    if (r.key != qrkey || modal != Modal::Qr)
                        continue;
                    if (r.code == 800) {
                        qrkey.clear();
                        message(L"二维码已过期，Esc 返回后重新生成");
                    } else if (r.code == 802)
                        message(L"已扫码，请在手机上确认");
                    else if (r.code == 803) {
                        qrkey.clear();
                        modal = Modal::None;
                        nickname = r.nickname;
                        likes = std::move(r.likes);
                        message(L"已登录 · " + nickname);
                    }
                } else if (r.type == "sms") {
                    modal = Modal::Code;
                    input.clear();
                    message(L"验证码已发送；输入验证码后 Enter 登录");
                } else if (r.type == "account") {
                    nickname = r.nickname;
                    likes = std::move(r.likes);
                    modal = Modal::None;
                    message(r.uid ? L"已登录 · " + nickname : L"当前为游客模式 · 按 L 登录");
                } else if (r.type == "logout") {
                    nickname = L"未登录";
                    likes.clear();
                    modal = Modal::None;
                    message(L"本机登录会话已清除");
                } else if (r.type == "like") {
                    if (r.code)
                        likes.insert(r.id);
                    else
                        likes.erase(r.id);
                    message(r.code ? L"已添加红心" : L"已取消红心");
                } else if (r.type == "save-list") {
                    saveRows = std::move(r.items);
                    modal = Modal::Save;
                    message(saveRows.empty() ? L"没有可写入的歌单，请先在官方客户端创建"
                                             : L"选择你创建的歌单，Enter 收藏");
                } else if (r.type == "saved") {
                    modal = Modal::None;
                    message(L"已收藏到歌单");
                } else if (r.type == "resumed")
                    message(L"冷却已结束，可以手动重试");
            }
            std::optional<Reply> completed;
            {
                std::lock_guard lock(chatMutex);
                completed.swap(chatResult);
            }
            if (completed && completed->request == activeChat && activeChat != 0) {
                activeChat = 0;
                if (completed->data.is_object()) {
                    for (const auto& item : completed->data.value("confirmedLikes", Json::array())) {
                        auto id = item.value("songId", (int64_t)0);
                        if (item.value("liked", false))
                            likes.insert(id);
                        else
                            likes.erase(id);
                    }
                    // A persona the model created during this turn becomes selectable, and is
                    // adopted immediately so the user can see the new voice at work.
                    auto created = completed->data.value("createdPersonas", Json::array());
                    if (!created.empty()) {
                        refreshPersonas();
                        auto id = created.back().value("id", std::string());
                        if (!id.empty())
                            selectPersona(id, false);
                    }
                }
                if (completed->type == "error") {
                    if (!chatLog.empty())
                        chatLog.back().second = L"模型请求失败：" + sanitize(unutf8(completed->text));
                    message(L"模型请求失败");
                } else if (completed->data.value("ok", false)) {
                    auto answer = unutf8(completed->text);
                    if (!chatLog.empty())
                        chatLog.back().second = answer.empty() ? L"模型没有返回文字" : answer;
                    message(L"模型已回复 · 音乐工具调用 " +
                            std::to_wstring(completed->data.value("toolCalls", 0)) + L" 次");
                } else {
                    auto code = completed->data.value("code", std::string());
                    auto error = completed->data.value("error", std::string());
                    if (!chatLog.empty())
                        chatLog.back().second = L"模型请求失败 [" + unutf8(code) + L"]：" +
                                                sanitize(unutf8(error));
                    message(L"模型请求失败 · " + unutf8(code));
                }
            }
        }
        void tick() {
            receive();
            mainCalls.pump([this](uint64_t owner, const std::string& method,
                                  const Json& params) {
                if (!owner || owner != activeChat)
                    return BridgeReply(false, "cancelled", "the chat request was cancelled",
                                       Json::object());
                return control(method, params);
            });
            bridge.pump([this](const std::string& method, const Json& params,
                               BridgeCompletion done) {
                if (method == "search" || method == "collection" || method == "playlists" ||
                    method == "like" || method == "add" || method == "lyrics")
                    startIpc(method, params, std::move(done));
                else
                    done(control(method, params));
            });
            auto now = Clock::now();
            if (modal == Modal::Qr && !qrkey.empty() && !busy) {
                if (now - qrStart > std::chrono::seconds(120)) {
                    qrkey.clear();
                    message(L"二维码超时，请返回后重新生成");
                } else if (now - qrPoll >= std::chrono::seconds(3)) {
                    qrPoll = now;
                    auto key = qrkey;
                    submit("qr-check", [key](MusicService& s) {
                        Reply r;
                        r.type = "qr-check";
                        r.code = s.qrCheck(key);
                        r.key = key;
                        if (r.code == 803) {
                            r.nickname = s.nickname();
                            r.likes = s.likes();
                        }
                        return r;
                    });
                }
            }
            auto state = session.audioState();
            // The decoder resets AudioState asynchronously when it starts the next track, so the
            // EOF latch is cleared only once the old "finished" state is really gone. Clearing it
            // in the play reply would re-enter the EOF branch in the same tick and skip a track.
            if (!state.finished)
                eofHandled = false;
            audioPlayed = audioPlayed || state.position > .1;
            maxPosition = std::max(maxPosition, state.position);
            // At most one lyrics request per track, on the shared worker, never per frame.
            {
                const Track& playing = session.playing();
                if (playing.id && playing.id != lyricsSong && !lyricsTicket) {
                    lyricsSong = playing.id;
                    currentLyrics = Lyrics{};
                    lyricsTicket = ++ipcSequence;
                    auto ticket = lyricsTicket;
                    if (!work.postAdapter(ticket, "lyrics", [id = playing.id](MusicService& s) {
                            Reply reply;
                            reply.type = "lyrics";
                            reply.id = id;
                            reply.lyrics = s.lyrics(id);
                            return reply;
                        }))
                        lyricsTicket = 0;
                }
                if (!playing.id && lyricsSong) {
                    lyricsSong = 0;
                    currentLyrics = Lyrics{};
                }
            }
            // Real-time reaction: one per track change, only inside an open conversation, with a
            // minimum gap and a per-session cap so a queue can never become a request storm.
            {
                const Track& playing = session.playing();
                auto now = Clock::now();
                if (playing.id != reactSong) {
                    reactSong = playing.id;
                    reactPending = reactEnabled && !options.demo && playing.id && chatView &&
                                   !chatRunning && !busy && state.playing &&
                                   now - lastReaction >= std::chrono::seconds(45) &&
                                   reactCount < 40;
                    if (reactPending)
                        lastReaction = now;
                }
                if (!state.playing)
                    reactPending = false;
                if (reactPending && state.position > .8 && !chatRunning && !busy) {
                    reactPending = false;
                    ++reactCount;
                    reactToTrack(playing);
                }
            }
            if (!state.error.empty() && state.error != audioError) {
                audioError = state.error;
                message(state.error);
            }
            if (state.finished && !eofHandled && !busy) {
                if (!session.move(1)) {
                    eofHandled = true;
                    message(L"队列播放完毕");
                } else {
                    // Consume this EOF event immediately: a failed or cancelled reply must leave
                    // the player on the current track instead of auto-skipping the whole queue.
                    // tick() re-arms the flag once the decoder leaves the finished state.
                    eofHandled = true;
                    if (!playCurrent()) {
                        session.move(-1); // refused submit: stay put and retry on a later tick
                        eofHandled = false;
                    }
                }
            }
            for (int i = 0; i < 24; ++i)
                bars[i] = std::max(state.spectrum[i], bars[i] * .72);
            if (!busy && !home && !chatView && (collection || mode == 5) && !rows.empty() &&
                modal == Modal::None) {
                std::vector<int64_t> ids;
                for (int i = top; i < std::min((int)rows.size(), top + listH) && ids.size() < 30;
                     ++i) {
                    if (rows[i].name.empty() && !detailsAsked.contains(rows[i].id))
                        ids.push_back(rows[i].id);
                }
                if (!ids.empty() && submit("details", [ids](MusicService& s) {
                        Reply r;
                        r.type = "details";
                        r.items = s.details(ids);
                        return r;
                    }))
                    detailsAsked.insert(ids.begin(), ids.end());
            }
            peak = std::max(peak, peakMemoryMB());
            peakPrivate = std::max(peakPrivate, peakPrivateMB());
            if (options.seconds > 0 && now - started >= std::chrono::seconds(options.seconds))
                quit = true;
        }
        void drawCover(Canvas& c, int x, int y, int w, int h) {
            if (cover.pixels.empty()) {
                c.rect(x, y, w, h, colors.surface);
                for (int row = 0; row < h; ++row) {
                    std::wstring line;
                    for (int col = 0; col < w; ++col) {
                        double dx = (col - w / 2.0) / 2, dy = row - h / 2.0;
                        double d = sqrt(dx * dx + dy * dy);
                        line += d < 1.4                                           ? L'●'
                                : d > 2 && d < h * .43 && (int)(d * 1.6) % 2 == 0 ? L'·'
                                                                                  : L' ';
                    }
                    c.text(x, y + row, line, colors.muted, w);
                }
                return;
            }
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < w; ++i) {
                    int sx = i * cover.width / w, sy = j * 2 * cover.height / (h * 2),
                        sy2 = (j * 2 + 1) * cover.height / (h * 2);
                    size_t a = (sy * cover.width + sx) * 3, b = (sy2 * cover.width + sx) * 3;
                    c.pixel(x + i, y + j, L'▀',
                            {cover.pixels[a], cover.pixels[a + 1], cover.pixels[a + 2]},
                            {cover.pixels[b], cover.pixels[b + 1], cover.pixels[b + 2]});
                }
        }
        void drawHome(Canvas& c) {
            bool compact = height < 34 || width < 104;
            bool shortWindow = height < 28;
            int sw = compact ? (shortWindow ? 24 : 32) : personaAvatarWidth;
            int sh = compact ? (shortWindow ? 26 : 34) : personaAvatarHeight;
            int x = compact ? 4 : std::max(4, (width - 91) / 2);
            int y = 2, right = x + sw + (compact ? 5 : 7);
            int textY = compact ? y : y + 3;
            for (int row = 0; row < sh; row += 2)
                for (int col = 0; col < sw; ++col) {
                    auto avatar = [&](int py) {
                        int ax = col * personaAvatarWidth / sw, ay = py * personaAvatarHeight / sh;
                        if (auto drawn = personaAvatarPixel(persona.id, ax, ay))
                            return drawn;
                        return personaGeneratedPixel(persona, ax, ay);
                    };
                    auto a = avatar(row), b = avatar(row + 1);
                    if (a || b)
                        c.pixel(x + col, y + row / 2, L'▀', a.value_or(colors.background),
                                b.value_or(colors.background), A_Mascot);
                }
            const std::array<std::wstring, 5> logo = {
                L"█   █   ███   ████    ███ ", L"██ ██  █   █  █   █  █   █",
                L"█ █ █  █   █  ████   █████", L"█   █  █   █  █  █   █   █",
                L"█   █   ███   █   █  █   █"};
            for (int i = 0; i < 5; ++i)
                c.text(right, textY + 1 + i, logo[i], colors.accent, width - right - 3);
            c.text(right, textY + 7, L"云间 · " + persona.name + L"的音乐终端", colors.text,
                   width - right - 3);
            c.text(right, textY + 9, persona.greeting, colors.muted, width - right - 3);
            if (!compact)
                c.text(right, textY + 11, L"输入音乐需求 · / 查看全部命令", colors.muted,
                       width - right - 3);
            int quickY = compact ? textY + 12 : textY + 14;
            if (quickY < height - 8)
                c.button(right, quickY, std::min(36, width - right - 3), L"/search   搜索一首歌",
                         A_Search);
            if (quickY + 2 < height - 8)
                c.button(right, quickY + 2, std::min(36, width - right - 3), L"/persona  切换人设",
                         A_Persona);
            if (quickY + 4 < height - 8)
                c.button(right, quickY + 4, std::min(36, width - right - 3),
                         L"/agent    接入外部 Agent", A_Agent);
            if (!compact)
                c.text(x + 13, y + sh / 2 + 1, persona.name, colors.muted, 30);
            auto state = session.audioState();
            if (state.playing)
                c.text(4, height - 9,
                       L"♫ " + truncate(session.playing().name, width - 30) +
                           (state.paused ? L"  ·  PAUSED" : L"  ·  PLAYING"),
                       colors.accent, width - 8);
        }
        void drawChat(Canvas& c) {
            c.text(4, 5, persona.name + L" · " + persona.tagline, colors.accent, width - 8);
            struct ChatLine { std::wstring text; bool fromUser; };
            std::vector<ChatLine> lines;
            auto append = [&](const std::wstring& value, bool fromUser, int lineWidth) {
                std::wstring remaining = value;
                while (!remaining.empty()) {
                    size_t n = 0;
                    int used = 0;
                    while (n < remaining.size() && remaining[n] != L'\n') {
                        int cw = displayWidth(std::wstring_view(&remaining[n], 1));
                        if (used + cw > lineWidth)
                            break;
                        used += cw;
                        ++n;
                    }
                    if (!n && remaining[0] != L'\n')
                        n = 1;
                    lines.push_back({remaining.substr(0, n), fromUser});
                    remaining.erase(0, n);
                    if (!remaining.empty() && remaining[0] == L'\n')
                        remaining.erase(0, 1);
                }
            };
            for (const auto& [prompt, answer] : chatLog) {
                append((userName.empty() ? L"用户" : userName) + L"  " + prompt, true,
                       width - 8);
                lines.push_back({L"", false});
                append(answer.empty() ? L"模型没有返回文字" : answer, false, width - 12);
                lines.push_back({L"", false});
                lines.push_back({L"", false});
            }
            int visible = std::max(1, height - 18);
            chatScroll = std::clamp(chatScroll, 0, std::max(0, (int)lines.size() - visible));
            int begin = std::max(0, (int)lines.size() - visible - chatScroll);
            int end = std::min((int)lines.size(), begin + visible);
            int y = height - 10 - (end - begin);
            for (int i = begin; i < end; ++i, ++y)
                c.text(lines[i].fromUser ? 4 : 6, y, lines[i].text,
                       lines[i].fromUser ? colors.text : colors.muted,
                       lines[i].fromUser ? width - 8 : width - 12);
        }
        void drawComposer(Canvas& c) {
            int y = height - 6;
            RGB border = searchFocus ? colors.accent : colors.surface;
            c.line(4, y, width - 8, border);
            c.line(4, y + 2, width - 8, border);
            c.pixel(3, y, L'╭', border, colors.background);
            c.pixel(width - 4, y, L'╮', border, colors.background);
            c.pixel(3, y + 2, L'╰', border, colors.background);
            c.pixel(width - 4, y + 2, L'╯', border, colors.background);
            c.text(5, y + 1, L"❯", colors.accent);
            inputCursor = std::min(inputCursor, input.size());
            std::wstring composerInput = modal == Modal::AiKey
                                             ? std::wstring(input.size(), L'•') : input;
            size_t start = 0;
            while (start < inputCursor && displayWidth(std::wstring_view(composerInput).substr(
                                              start, inputCursor - start)) > width - 15)
                ++start;
            c.text(7, y + 1, std::wstring_view(composerInput).substr(start), colors.text, width - 13);
            c.mark(3, y, width - 6, A_Row - 1);
            c.mark(3, y + 1, width - 6, A_Row - 1);
            c.mark(3, y + 2, width - 6, A_Row - 1);
            c.text(4, y - 1, (busy ? L"◌ " : L"· ") + status, colors.muted, width - 8);
            c.text(4, height - 2,
                   searchFocus ? L"Enter 发送  ·  / 命令  ·  Tab 补全  ·  ↑↓ 历史  ·  Esc 返回"
                               : L"↑↓ 选择  ·  Enter 播放  ·  / 命令  ·  Tab 输入  ·  /home 首页",
                   colors.muted, width - 20);
            c.text(width - 14, height - 2,
                   std::to_wstring((int)(session.volume() * 100)) + L"% VOL", colors.accent,
                   10);
            auto suggestions = searchFocus && modal == Modal::None ? commandSuggestions(input)
                                                                   : std::vector<CommandSpec>{};
            if (!suggestions.empty()) {
                suggestion = std::clamp(suggestion, 0, (int)suggestions.size() - 1);
                int count = std::min(5, (int)suggestions.size()),
                    startIndex = std::clamp(suggestion - 2, 0, (int)suggestions.size() - count),
                    panelY = y - count - 3, panelW = std::min(width - 8, 84);
                c.rect(4, panelY, panelW, count + 2, colors.surface);
                c.text(6, panelY,
                       L"╭─ 命令 · " + std::to_wstring(suggestions.size()) +
                           L" 项  ↑↓ 选择 / Tab 补全",
                       colors.accent, panelW - 4);
                for (int i = 0; i < count; ++i) {
                    int index = startIndex + i;
                    auto& spec = suggestions[index];
                    c.text(6, panelY + 1 + i,
                           (index == suggestion ? L"❯ /" : L"  /") + pad(spec.name, 12) + L"  " +
                               spec.description,
                           index == suggestion ? colors.accent : colors.muted, panelW - 4);
                    c.mark(4, panelY + 1 + i, panelW, A_Suggestion + index);
                }
                c.text(6, panelY + count + 1, L"╰─ " + suggestions[suggestion].usage, colors.muted,
                       panelW - 4);
            }
        }
        void drawModal(Canvas& c) {
            int x = std::max(2, (width - 62) / 2), w = std::min(62, width - 4),
                y = modal == Modal::Qr ? 3 : 5,
                h = std::max(8, height - (modal == Modal::Qr ? 6 : 11));
            c.rect(x, y, w, h, colors.surface);
            c.button(x + w - 8, y + 1, 6, L"Esc", A_Close);
            auto txt = [&](int row, std::wstring_view s) {
                c.text(x + 3, y + row, s, colors.text, w - 6);
            };
            if (modal == Modal::Login) {
                txt(2, L"账号 · 网易云音乐");
                txt(4, nickname);
                c.button(x + 3, y + 6, w - 6, L"1  扫码登录  /  Enter", A_Qr, true);
                c.button(x + 3, y + 8, w - 6, L"2  手机号与短信验证码", A_Phone);
                if (nickname != L"未登录")
                    c.button(x + 3, y + 10, w - 6, L"D  退出本机账号", A_Logout);
                txt(13, L"优先扫码；验证挑战请在官方客户端完成。");
            } else if (modal == Modal::Qr) {
                txt(2, L"扫码登录");
                if (qrkey.empty()) {
                    txt(5, busy ? L"正在生成二维码…" : L"二维码不可用，请 Esc 返回后重试。");
                } else {
                    auto qr = qrcodegen::QrCode::encodeText(
                        ("https://music.163.com/login?codekey=" + qrkey).c_str(),
                        qrcodegen::QrCode::Ecc::LOW);
                    int size = qr.getSize() + 8;
                    int qx = x + (w - size) / 2, qy = y + 4;
                    if (size > w - 4 || (size + 1) / 2 > h - 6) {
                        txt(5, L"窗口太小，请放大终端以显示完整二维码。");
                    } else {
                        for (int row = 0; row < size; row += 2)
                            for (int col = 0; col < size; ++col) {
                                bool a = qr.getModule(col - 4, row - 4),
                                     b = qr.getModule(col - 4, row + 1 - 4);
                                RGB fg = a ? RGB{0, 0, 0} : RGB{255, 255, 255},
                                    bg = b ? RGB{0, 0, 0} : RGB{255, 255, 255};
                                c.pixel(qx + col, qy + row / 2, L'▀', fg, bg);
                            }
                        txt(5 + (size + 1) / 2, L"用网易云音乐 App 扫码，并在手机确认");
                    }
                }
            } else if (modal == Modal::Name) {
                txt(2, L"怎么称呼你？");
                txt(4, L"首次启动请设置名字；之后可用 /name 或 /settings 修改。");
                c.rect(x + 3, y + 7, w - 6, 1, colors.background);
                c.text(x + 4, y + 7, input + L"▏", colors.accent, w - 8);
                txt(10, L"Enter 保存 · 1–24 个字符");
            } else if (modal == Modal::Settings) {
                txt(2, L"个人设置");
                txt(5, L"聊天称呼：" + (userName.empty() ? L"尚未设置" : userName));
                c.button(x + 3, y + 7, w - 6, L"修改我的名字", A_Name);
                c.button(x + 3, y + 9, w - 6, L"切换音乐 Agent 人设", A_Persona);
                c.button(x + 3, y + 11, w - 6, L"重看新手引导", A_Guide);
            } else if (modal == Modal::Persona) {
                txt(2, L"选择音乐 Agent 人设");
                int rows = std::min((int)personaList.size(), std::max(1, h - 9));
                int from = std::clamp(personaSelected - rows + 1, 0,
                                      std::max(0, (int)personaList.size() - rows));
                for (int i = 0; i < rows; ++i) {
                    int index = from + i;
                    if (index >= (int)personaList.size())
                        break;
                    const auto& candidate = personaList[(size_t)index];
                    bool active = candidate.id == persona.id;
                    std::wstring label = (active ? L"● " : L"○ ") + candidate.name + L"  ·  " +
                                         candidate.tagline;
                    if (!candidate.builtin)
                        label += L"  (自定义)";
                    c.button(x + 3, y + 5 + i * 2, w - 6, truncate(label, w - 10),
                             A_PersonaRow + index, index == personaSelected);
                }
            } else if (modal == Modal::Device) {
                txt(2, L"选择音频输出设备");
                int rows = std::min((int)outputList.size(), std::max(1, h - 9));
                int from = std::clamp(deviceSelected - rows + 1, 0,
                                      std::max(0, (int)outputList.size() - rows));
                for (int i = 0; i < rows; ++i) {
                    int index = from + i;
                    if (index >= (int)outputList.size())
                        break;
                    bool active = outputList[(size_t)index].id == audio.outputDevice();
                    std::wstring label = (active ? L"● " : L"○ ") + outputList[(size_t)index].name;
                    c.button(x + 3, y + 5 + i * 2, w - 6, truncate(label, w - 10),
                             A_DeviceRow + index, index == deviceSelected);
                }
            } else if (modal == Modal::Phone || modal == Modal::Code) {
                txt(2, modal == Modal::Phone ? L"手机号登录 · 国家区号 86" : L"输入短信验证码");
                txt(5, modal == Modal::Phone ? L"手机号" : L"验证码");
                c.rect(x + 3, y + 7, w - 6, 1, colors.background);
                c.text(x + 4, y + 7,
                       (modal == Modal::Code ? std::wstring(input.size(), L'•') : input) + L"▏",
                       colors.accent, w - 8);
                c.button(x + 3, y + 10, w - 6,
                         modal == Modal::Phone ? L"Enter  发送一次验证码" : L"Enter  登录", A_Send,
                         true);
                txt(13, L"发送间隔 60 秒；不保存手机号或验证码。");
            } else if (modal == Modal::Save) {
                txt(2, L"收藏到歌单");
                int visible = std::max(1, h - 6), begin = std::max(0, saveSelected - visible + 1);
                if (saveRows.empty())
                    txt(5, L"没有可写入的歌单。");
                for (int i = begin; i < std::min((int)saveRows.size(), begin + visible); ++i)
                    c.button(x + 3, y + 4 + i - begin, w - 6, truncate(saveRows[i].name, w - 10),
                             A_SaveRow + i, i == saveSelected);
            } else if (modal == Modal::Quality) {
                txt(2, L"音质 · 下一首歌生效");
                for (int i = 0; i < 5; ++i)
                    c.button(x + 3, y + 5 + i * 2, w - 6, qualityNames[i], A_QualityRow + i,
                             i == qualitySelected);
                txt(16, L"显示实际返回音质；VIP 与 Hi-Res 依账号权限。");
            } else if (modal == Modal::Help) {
                txt(2, L"MORA · 操作指南");
                std::vector<std::wstring> help = {L"/search <关键词>  搜索单曲",
                                                  L"/albums /playlists <关键词>  专辑与歌单",
                                                  L"/login [qr|phone]  登录网易云",
                                                  L"/play [序号] /pause /next /prev",
                                                  L"/like 红心   /add 收藏   /quality 音质",
                                                  L"/queue 队列   /library 我的歌单",
                                                  L"/home 首页   /ai 模型接口   /agent MCP",
                                                  L"/persona 人设   /effort 思考强度   /name 称呼",
                                                  L"/audio-device 音频输出   /settings 个人设置",
                                                  L"/guide 重看新手引导",
                                                  L"Tab 补全命令 · ↑↓ 建议 / 历史",
                                                  L"结果页 Tab 进入输入；Esc 回到列表或首页",
                                                  L"鼠标选择、双击播放，进度条可点击跳转"};
                for (size_t i = 0; i < help.size() && 4 + (int)i < h - 1; ++i)
                    txt(4 + i, help[i]);
            } else if (modal == Modal::Guide) {
                txt(2, L"新手引导 · " + std::to_wstring(guidePage + 1) + L" / 3");
                if (guidePage == 0) {
                    txt(4, L"【先听一首】输入 /search 歌名或歌手，再选序号播放。");
                    txt(6, L"播放时可用 Space 暂停、N 下一首、H 收藏红心。");
                    txt(8, L"也可以直接点击首页的“搜索一首歌”。");
                } else if (guidePage == 1) {
                    txt(4, L"【按需登录】游客可先搜索；登录后可用个人歌单与红心。");
                    txt(6, L"输入 /login qr，用网易云音乐 App 扫码并确认。");
                    txt(8, L"只能播放账号实际有权限的音乐；稍后登录也可以。");
                } else {
                    txt(4, L"【可选：音乐 Agent】想用 /chat 聊天时，再配置模型。");
                    txt(6, L"输入 /ai，按 K 填密钥、按 M 选择模型 ID。");
                    txt(8, L"密钥保存在当前 Windows 用户的加密文件中。");
                }
                c.button(x + 3, y + 10, w - 6,
                         guidePage == 2 ? L"Enter  完成并开始使用" : L"Enter  下一步",
                         A_GuideNext, true);
                txt(11, L"← 上一步  ·  S 或 Esc 跳过  ·  /guide 随时重看");
            } else if (modal == Modal::Ai) {
                auto config = loadAgentConfig();
                txt(2, L"音乐 Agent · 模型设置");
                txt(4, L"人设：" + persona.name + L" · " + persona.tagline);
                txt(6, L"思考强度：" + effortLabel(config.reasoningEffort) +
                           L" · /effort 可改（off→max）");
                txt(8, L"模型：" + truncate(unutf8(config.model), w - 12));
                txt(10, agentKeyPresent() ? L"密钥：已配置（DPAPI 保护）" : L"密钥：未配置");
                c.button(x + 3, y + 12, w - 6, L"K  输入或替换模型密钥", A_AiKey);
                c.button(x + 3, y + 14, w - 6, L"M  修改模型 ID", A_AiModel);
                txt(17, L"/chat 开始对话；环境变量可覆盖当前模型设置。");
            } else if (modal == Modal::AiKey || modal == Modal::AiModel) {
                bool secret = modal == Modal::AiKey;
                txt(2, secret ? L"输入模型密钥" : L"输入模型 ID");
                txt(4, secret ? L"密钥仅保存到当前用户的 DPAPI 加密文件。" : L"模型 ID 保存到 agent.json。");
                c.rect(x + 3, y + 7, w - 6, 1, colors.background);
                c.text(x + 4, y + 7, truncate(secret ? std::wstring(input.size(), L'•') : input, w - 10) + L"▏",
                       colors.accent, w - 8);
                txt(10, L"Enter 保存 · Esc 取消");
            } else if (modal == Modal::Agent) {
                txt(2, L"外部 Agent · MCP stdio（已接通）");
                txt(5, L"启动入口：mora --mcp");
                txt(7, L"工具：搜索/集合/播放/控制/推荐/随机/歌单/红心/收藏/状态");
                txt(9, L"播放类工具要求先在本机运行 mora（单实例）。");
                txt(11, L"外部 Agent 不需要播放器内的模型 Key。");
                txt(13, L"写工具只作用于用户明确给出的目标，不自动重试。");
            }
        }
        std::unique_ptr<Canvas> render() {
            auto dims = console.size();
            width = dims.first;
            height = dims.second;
            auto c = std::make_unique<Canvas>(width, height, colors);
            if (width < 78 || height < 24) {
                c->text(3, 3, L"MORA 云间", colors.accent);
                c->text(3, 6, L"请将终端放大至至少 78 × 24", colors.text);
                c->text(3, 8, L"建议 116 × 36 · Cascadia Mono / 等距更纱黑体", colors.muted);
                return c;
            }
            auto state = session.audioState();
            bool sidePanel = width >= 104 && height >= 32;
            listW = sidePanel ? width * 59 / 100 : width - 6;
            listX = 3;
            listY = 8;
            listH = std::max(2, height - 22);
            c->text(3, 1, L"M O R A", colors.accent);
            c->text(15, 1, L"云间  /  STEREO TERMINAL", colors.muted);
            std::wstring mem = std::to_wstring((int)memoryMB()) + L" MB";
            c->text(width - 26, 1, options.demo ? L"DEMO · 离线演示" : mem, colors.muted, 22);
            if (home) {
                drawHome(*c);
                drawComposer(*c);
                if (modal != Modal::None)
                    drawModal(*c);
                return c;
            }
            c->button(3, 3, 10, L"1 单曲", A_Songs, mode == 1 && !chatView);
            c->button(14, 3, 10, L"2 专辑", A_Albums, mode == 2 && !chatView);
            c->button(25, 3, 10, L"3 歌单", A_Playlists, mode == 3 && !chatView);
            c->button(36, 3, 12, L"4 我的音乐", A_Library, mode == 4 && !chatView);
            c->button(49, 3, 10, L"5 队列", A_Queue, mode == 5 && !chatView);
            if (width >= 100)
                c->button(width - 34, 3, 14, L"/home 首页", A_Home);
            c->button(width - 18, 3, 15, truncate(nickname, 10), A_Login);
            if (chatView) {
                drawChat(*c);
                drawComposer(*c);
                if (modal != Modal::None)
                    drawModal(*c);
                return c;
            }
            c->text(3, 6, truncate(title + (query.empty() ? L"" : L" / " + query), listW - 22),
                    colors.accent);
            c->text(std::max(20, listW - 14), 6,
                    std::to_wstring(rows.size()) + L" / " + std::to_wstring(total), colors.muted,
                    18);
            for (int i = 0; i < listH && top + i < (int)rows.size(); ++i) {
                int index = top + i;
                auto& t = rows[index];
                int y = listY + i;
                bool sel = index == selected;
                c->rect(listX, y, listW, 1, sel ? colors.surface : colors.background);
                c->text(4, y,
                        session.playing().id && t.id == session.playing().id
                            ? L"▸"
                            : std::to_wstring(offset + index + 1),
                        sel ? colors.accent : colors.muted, 4);
                int nameWidth = listW * 50 / 100;
                auto name = t.name.empty() ? L"载入曲目… #" + std::to_wstring(t.id) : t.name;
                c->text(9, y, name, sel ? colors.accent : colors.text, nameWidth - 2);
                c->text(9 + nameWidth, y, t.artist, colors.muted, listW - nameWidth - 14);
                c->text(listX + listW - 5, y,
                        t.kind == Kind::Song ? (likes.contains(t.id) ? L"♥" : timeText(t.duration))
                                             : L"↵",
                        likes.contains(t.id) ? colors.accent : colors.muted, 5);
            }
            if (rows.empty()) {
                c->text(7, 13, L"从一首歌开始。", colors.text, listW - 12);
                c->text(7, 15, L"/ 输入「歌名 歌手」搜索元数据", colors.muted, listW - 12);
                c->text(7, 17, L"2 搜专辑 · 3 搜歌单 · L 登录账号", colors.muted, listW - 12);
            }
            if (sidePanel) {
                int x = listW + 7, w = width - x - 4, artH = std::clamp(height - 28, 4, 12),
                    artW = std::min(artH * 2, w);
                drawCover(*c, x + (w - artW) / 2, 8, artW, artH);
                int y = 9 + artH;
                c->text(x, y, session.playing().id ? session.playing().name : L"尚未播放",
                        colors.text, w);
                c->text(x, y + 2, session.playing().artist, colors.muted, w);
                c->text(x, y + 3, session.playing().album, colors.muted, w);
                std::wstring format = state.sampleRate
                                          ? L" · " + std::to_wstring(state.sampleRate / 1000) +
                                                L"k / " + std::to_wstring(state.sampleBits) + L"bit"
                                          : L"";
                c->text(x, y + 4,
                        session.actualQuality().empty()
                            ? L"READY / 按 Enter 播放"
                            : unutf8(session.actualQuality()) + format,
                        colors.accent, w);
            }
            int controlY = height - 12;
            c->line(3, controlY - 2, width - 6, colors.surface);
            c->text(3, controlY - 1,
                    (state.paused    ? L"PAUSED"
                     : state.playing ? L"PLAYING"
                                     : L"READY") +
                        std::wstring(L"  ·  ") + truncate(session.playing().name, width / 2 - 16),
                    colors.accent, width / 2);
            if (motion) {
                int x = width - 30;
                for (int i = 0; i < 24; ++i) {
                    int n = std::clamp((int)std::round(bars[i] * 7), 0, 7);
                    static const wchar_t* levels = L"▁▂▃▄▅▆▇█";
                    c->pixel(x + i, controlY - 1, levels[n], n ? colors.accent : colors.surface,
                             colors.background);
                }
            }
            c->button(3, controlY + 1, 7, L"P |<", A_Previous);
            c->button(11, controlY + 1, 12, state.paused ? L"Space >" : L"Space II", A_Pause, true);
            c->button(24, controlY + 1, 7, L"N >|", A_Next);
            c->button(33, controlY + 1, 10, L"H 红心", A_Like);
            c->button(44, controlY + 1, 10, L"A 收藏", A_Save);
            c->button(55, controlY + 1, 10, L"V 音质", A_Quality);
            c->button(66, controlY + 1, 9, session.repeat() ? L"C 循环" : L"C 顺序", A_Repeat);
            if (width >= 94) {
                c->button(width - 15, controlY + 1, 5, L"-", A_VolDown);
                c->button(width - 9, controlY + 1, 5, L"+", A_VolUp);
            }
            progressY = controlY + 3;
            progressX = 10;
            progressW = width - 22;
            c->text(3, progressY, timeText(state.position), colors.muted, 6);
            double duration = state.duration > 0 ? state.duration : session.playing().duration;
            int filled = duration > 0 ? (int)(progressW * state.position / duration) : 0;
            for (int i = 0; i < progressW; ++i)
                c->pixel(progressX + i, progressY, i == filled ? L'●' : L'─',
                         i <= filled ? colors.accent : colors.surface, colors.background,
                         A_Progress);
            c->text(width - 9, progressY, timeText(duration), colors.muted, 6);
            drawComposer(*c);
            if (modal != Modal::None)
                drawModal(*c);
            return c;
        }
        bool copySelection(const std::wstring& selected) {
            if (selected.empty() || !OpenClipboard(nullptr))
                return false;
            bool ok = false;
            if (EmptyClipboard()) {
                size_t bytes = (selected.size() + 1) * sizeof(wchar_t);
                HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (block) {
                    if (void* target = GlobalLock(block)) {
                        memcpy(target, selected.c_str(), bytes);
                        GlobalUnlock(block);
                        if (SetClipboardData(CF_UNICODETEXT, block))
                            ok = true;
                    }
                    if (!ok)
                        GlobalFree(block);
                }
            }
            CloseClipboard();
            return ok;
        }
        void mouseClick(const MOUSE_EVENT_RECORD& m) {
            int x = m.dwMousePosition.X, y = m.dwMousePosition.Y;
            if (!previous)
                return;
            int a = previous->hit(x, y);
            if (modal != Modal::None) {
                bool allowed =
                    a == A_Close ||
                    (modal == Modal::Login && (a == A_Qr || a == A_Phone || a == A_Logout)) ||
                    ((modal == Modal::Phone || modal == Modal::Code) && a == A_Send) ||
                    (modal == Modal::Save && a >= A_SaveRow && a < A_QualityRow) ||
                    (modal == Modal::Settings && (a == A_Name || a == A_Persona)) ||
                    (modal == Modal::Persona && a >= A_PersonaRow && a < A_DeviceRow) ||
                    (modal == Modal::Device && a >= A_DeviceRow) ||
                    (modal == Modal::Quality && a >= A_QualityRow);
                if (allowed)
                    action(a);
                return;
            }
            if (a == A_Row - 1) {
                searchFocus = true;
                inputCursor = input.size();
                return;
            }
            if (a == A_Progress) {
                auto s = session.audioState();
                if (s.duration > 0)
                    session.seek(s.duration *
                                 std::clamp(double(x - progressX) / progressW, 0.0, 1.0));
                return;
            }
            if (a) {
                action(a);
                return;
            }
            if (modal == Modal::None && !home && !chatView && x >= listX && x < listX + listW &&
                y >= listY && y < listY + listH) {
                int index = top + y - listY;
                if (index < (int)rows.size()) {
                    selected = index;
                    searchFocus = false;
                    if (m.dwEventFlags == DOUBLE_CLICK)
                        activate();
                }
            }
        }
        void mouse(const MOUSE_EVENT_RECORD& m) {
            int x = m.dwMousePosition.X, y = m.dwMousePosition.Y;
            if (m.dwEventFlags == MOUSE_WHEELED) {
                int step = SHORT(HIWORD(m.dwButtonState)) > 0 ? -3 : 3;
                if (modal == Modal::Save)
                    saveSelected = std::clamp(saveSelected + step, 0,
                                              std::max(0, (int)saveRows.size() - 1));
                else if (modal == Modal::None && chatView)
                    chatScroll = std::max(0, chatScroll - step);
                else if (modal == Modal::None)
                    navigate(step);
                return;
            }
            bool down = m.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED;
            if (down && !selecting) {
                selecting = true;
                selectionMoved = false;
                selectionStart = m.dwMousePosition;
                selectionEnd = selectionStart;
            } else if (down && selecting) {
                selectionEnd = m.dwMousePosition;
                selectionMoved |= selectionEnd.X != selectionStart.X ||
                                  selectionEnd.Y != selectionStart.Y;
            } else if (!down && selecting) {
                selectionEnd = m.dwMousePosition;
                selectionMoved |= selectionEnd.X != selectionStart.X ||
                                  selectionEnd.Y != selectionStart.Y;
                if (selectionMoved && previous) {
                    auto selected = previous->selectedText(selectionStart.X, selectionStart.Y,
                                                           selectionEnd.X, selectionEnd.Y);
                    message(copySelection(selected) ? L"已复制选中文字" : L"复制失败或选区为空");
                } else {
                    MOUSE_EVENT_RECORD click = m;
                    click.dwMousePosition = selectionStart;
                    mouseClick(click);
                }
                selecting = false;
            }
        }

      public:
        explicit App(Options opts) : options(std::move(opts)), motion(options.motion) {
            if (!options.demo) {
                // Volume, quality, repeat, motion and the last commands survive a restart. The
                // explicit --no-motion flag still wins over a stored preference.
                auto settings = Settings::load(stateDirectory());
                motion = options.motion && settings.motion;
                session.setVolume(settings.volume);
                session.setQuality(settings.quality);
                session.setRepeat(settings.repeat);
                history = std::move(settings.history);
                userName = std::move(settings.userName);
                guideDone = settings.guideDone;
                for (int index = 0; index < 5; ++index)
                    if (qualities[index] == settings.quality)
                        qualitySelected = index;
                refreshPersonas();
                selectPersona(settings.personaId, false);
                personaList = allPersonas();
                refreshPersonas();
                // Restore the remembered output device before any track can start.
                refreshOutputDevices();
                if (auto found = std::find_if(outputList.begin(), outputList.end(),
                                              [&](const OutputDevice& d) {
                                                  return d.id == settings.outputDeviceId;
                                              });
                    found != outputList.end())
                    audio.selectOutputDevice(settings.outputDeviceId);
            }
            applyPersonaColors();
            status = L"预览版 0.6.0 · /search 搜歌 · /login 登录 · /persona 人设 · /ai 模型接口";
            if (options.demo) {
                std::array<std::array<std::wstring, 3>, 8> songs{
                    {{L"晴天", L"周杰伦", L"叶惠美"},
                     {L"七里香", L"周杰伦", L"七里香"},
                     {L"夜曲", L"周杰伦", L"十一月的萧邦"},
                     {L"搁浅", L"周杰伦", L"七里香"},
                     {L"一路向北", L"周杰伦", L"十一月的萧邦"},
                     {L"稻香", L"周杰伦", L"魔杰座"},
                     {L"安静", L"周杰伦", L"范特西"},
                     {L"枫", L"周杰伦", L"十一月的萧邦"}}};
                int n = 1;
                for (auto& song : songs)
                    rows.push_back(Track{.id = n++,
                                         .name = song[0],
                                         .artist = song[1],
                                         .album = song[2],
                                         .duration = 269});
                total = (int)rows.size();
                title = L"离线界面演示";
                query = L"周杰伦";
                likes.insert(1);
                session.assign(rows);
                session.setCurrent(rows.front());
                cover.width = cover.height = 48;
                cover.pixels.resize(48 * 48 * 3);
                for (int y = 0; y < 48; ++y)
                    for (int x = 0; x < 48; ++x) {
                        size_t i = (y * 48 + x) * 3;
                        bool moon = (x - 32) * (x - 32) + (y - 14) * (y - 14) < 64;
                        cover.pixels[i] = moon ? 218 : 32 + x;
                        cover.pixels[i + 1] = moon ? 207 : 55 + y;
                        cover.pixels[i + 2] = moon ? 159 : 73 + y / 2;
                    }
                colors.accent = palette(cover).accent;
                message(L"离线演示 · 界面样例，不代表真实搜索结果");
            }
            if (!options.localAudio.empty()) {
                Track local{.id = -1,
                            .name =
                                std::filesystem::absolute(options.localAudio).filename().wstring(),
                            .artist = L"本地音频验收"};
                session.assign({local});
                session.startTrack(local, "",
                                   std::filesystem::absolute(options.localAudio).wstring());
            }
        }
        ~App() {
            mainCalls.close();
            if (chatThread.joinable())
                chatThread.join();
        }
        int run() {
            if (!console.interactive() && options.seconds <= 0 && options.snapshot.empty()) {
                std::cerr << "Please run mora in CMD, PowerShell or Windows Terminal.\n";
                return 1;
            }
            // Exactly one player owns the music session pipe. MCP adapters (and a future in-process
            // agent) attach to this instance instead of starting a second audio core.
            if (!options.demo) {
                std::string error;
                if (!bridge.start(error)) {
                    std::cerr << "MORA: " << error << "\n"
                              << "Only one MORA player can own the music session; close the "
                                 "running player and try again.\n";
                    return 2;
                }
            }
            if (!options.demo && options.seconds <= 0 && options.snapshot.empty()) {
                if (userName.empty()) editName();
                else if (!guideDone) modal = Modal::Guide;
            }
            if (options.playId) {
                home = false;
                searchFocus = false;
                session.assignId(options.playId);
                playCurrent();
            } else if (!options.demo && options.localAudio.empty() &&
                       std::filesystem::exists(stateDirectory() / L"session.bin"))
                submit("account", [](MusicService& s) {
                    s.account();
                    Reply r;
                    r.type = "account";
                    r.nickname = s.nickname();
                    r.uid = s.uid();
                    r.likes = s.likes();
                    return r;
                });
            while (!quit) {
                tick();
                for (auto& e : console.events()) {
                    if (e.EventType == KEY_EVENT)
                        key(e.Event.KeyEvent);
                    else if (e.EventType == MOUSE_EVENT)
                        mouse(e.Event.MouseEvent);
                }
                auto next = render();
                if (selecting && selectionMoved)
                    next->highlight(selectionStart.X, selectionStart.Y, selectionEnd.X,
                                    selectionEnd.Y);
                console.write(next->ansi(previous.get()));
                previous = std::move(next);
                if (searchFocus && modal == Modal::None && width >= 78 && height >= 24) {
                    size_t start = 0;
                    while (start < inputCursor && displayWidth(std::wstring_view(input).substr(
                                                      start, inputCursor - start)) > width - 15)
                        ++start;
                    console.cursor(7 + displayWidth(std::wstring_view(input).substr(
                                           start, inputCursor - start)),
                                   height - 5, true);
                } else
                    console.cursor(0, 0, false);
                if (!options.snapshot.empty()) {
                    std::filesystem::path path(options.snapshot);
                    std::ofstream file(path, std::ios::binary);
                    file << utf8(path.extension() == L".ansi" ? previous->ansi()
                                                              : previous->plain());
                    if (options.seconds <= 0)
                        break;
                }
                Sleep(motion ? 100 : 250);
            }
            auto finalAudio = session.audioState();
            session.stop();
            // Only a real interactive session persists user state: demo and measurement runs must
            // not overwrite someone's saved volume, quality or history.
            if (!options.demo && options.seconds <= 0 && options.snapshot.empty()) {
                Settings settings;
                settings.volume = session.volume();
                settings.quality = session.quality();
                settings.repeat = session.repeat();
                settings.motion = motion;
                settings.history = std::move(history);
                settings.userName = userName;
                settings.guideDone = guideDone;
                settings.personaId = persona.id;
                settings.outputDeviceId = audio.outputDevice();
                settings.save(stateDirectory());
            }
            if (!options.report.empty()) {
                bool expectedAudio = !options.localAudio.empty() || options.playId;
                Json j = {
                    {"peakWorkingSetMB", peak},
                    {"peakPrivateMB", peakPrivate},
                    {"budgetMB", 50},
                    {"passed", peak <= 50 && peakPrivate <= 50 && (!expectedAudio || audioPlayed) &&
                                   audioError.empty()},
                    {"seconds", std::chrono::duration<double>(Clock::now() - started).count()},
                    {"mode", options.demo ? "demo" : "live"},
                    {"audio", expectedAudio},
                    {"audioPlayed", audioPlayed},
                    {"maxPositionSeconds", maxPosition},
                    {"audioError", utf8(audioError)},
                    {"actualQuality", session.actualQuality()},
                    {"sampleRate", finalAudio.sampleRate},
                    {"sampleBits", finalAudio.sampleBits}};
                std::ofstream f(std::filesystem::path(options.report), std::ios::binary);
                f << j.dump(2) << '\n';
            }
            return 0;
        }
    };
    constexpr std::array<const char*, 5> App::qualities;
    constexpr std::array<const wchar_t*, 5> App::qualityNames;
    int runApp(const Options& opts) {
        App app(opts);
        return app.run();
    }
} // namespace mora
