#include "ui.hpp"
#include "agent.hpp"
#include "bridge.hpp"
#include "commands.hpp"
#include "context.hpp"
#include "mascot.hpp"
#include "persona.hpp"
#include "qrcodegen.hpp"
#include "settings.hpp"
#include "stats.hpp"
#include "worker.hpp"
#include "session.hpp"
#include "ui_tool_calls.hpp"
#include <atomic>
#include <fstream>
#include <map>
#include <iostream>
#include <shellapi.h>

namespace mora {
    enum class Modal { None, SavedSession, Login, Qr, Phone, Code, Save, Help, Quality, Guide, Ai, AiKey, AiModel, AiModels, Agent, Settings, Name,
                       Persona, Device, Stats };
    enum Action {
        A_Search = 1,
        A_Songs,
        A_Albums,
        A_Playlists,
        A_Library,
        A_Queue,
        A_Login,
        A_UseSavedSession,
        A_ForgetSavedSession,
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
        A_AiClearKey,
        A_Agent,
        A_Mascot,
        A_Name,
        A_Persona,
        A_Device,
        A_Mode,
        A_Detail,
        A_ReactToggle,
        A_ReactSlower,
        A_ReactFaster,
        A_ReactCharsDown,
        A_ReactCharsUp,
        A_ReviewCharsDown,
        A_ReviewCharsUp,
        A_CompressToggle,
        A_AiDiscover,
        A_AiCached,
        A_StatsDay,
        A_StatsMonth,
        A_StatsYear,
        A_ModelRow = 70000,
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
        // User-tunable reaction budget and review length, persisted through settings.json.
        int reactMinSeconds = 45, reactMaxChars = 25, reviewChars = 1000;
        bool autoCompress = true;
        int contextLimitTokens = 0;
        // Song detail page: a read-only overlay over the page that opened it, so Esc returns there.
        bool detail = false, detailFollow = true;
        int detailScroll = 0;
        // While a model request is in flight the conversation rotates the persona's waiting lines.
        int waitLineIndex = -1;
        Clock::time_point waitLineSince{};
        bool chatWaiting = false;
        // Conversation memory (context.hpp) and the listen log (stats.hpp). Both stay in the user's
        // own data directory and are only created for real interactive runs.
        Conversation conversation;
        std::unique_ptr<ListenHistory> listenHistory;
        ListenPeriod statsPeriod = ListenPeriod::Day;
        ListenStats statsReport;
        bool statsLoaded = false;
        int statsScroll = 0;
        size_t lastPromptTokens = 0, lastContextTokens = 0;
        // Model discovery runs off the UI thread; tick() picks the catalog up when it lands.
        std::thread discoverThread;
        std::mutex discoverMutex;
        std::optional<ModelCatalog> discoverResult;
        std::atomic<bool> discoverRunning{false};
        std::vector<ModelCapability> modelList;
        int modelSelected = 0;
        bool modelFromCache = false;
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
        void showWelcomeIfNeeded() {
            if (options.demo || options.seconds > 0 || !options.snapshot.empty())
                return;
            if (userName.empty()) editName();
            else if (!guideDone) modal = Modal::Guide;
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
        // Queue modes are shared with the MCP adapter and the model loop; the UI only adds a
        // readable label and keeps the canonical name available for commands and diagnostics.
        static std::wstring modeLabel(QueueMode mode) {
            switch (mode) {
            case QueueMode::RepeatAll:
                return L"列表循环";
            case QueueMode::RepeatOne:
                return L"单曲循环";
            case QueueMode::Shuffle:
                return L"随机";
            default:
                return L"顺序";
            }
        }
        std::string modeName() const { return queueModeName(session.mode()); }
        std::wstring qualityLabel() const {
            for (size_t i = 0; i < qualities.size(); ++i)
                if (session.quality() == qualities[i])
                    return qualityNames[i];
            return unutf8(session.quality());
        }
        // The requested quality is a preference; the actual one is what the account really got.
        std::wstring actualQualityLabel() const {
            const auto& actual = session.actualQuality();
            if (actual.empty())
                return qualityLabel();
            for (size_t i = 0; i < qualities.size(); ++i)
                if (actual == qualities[i])
                    return qualityNames[i];
            return unutf8(actual);
        }
        // Persist one preference right away so a crash cannot silently lose a UI setting. Demo and
        // measurement runs never touch the user's own settings file.
        void persistSetting(const std::function<void(Settings&)>& change) {
            if (options.demo || options.seconds > 0 || !options.snapshot.empty())
                return;
            auto settings = Settings::load(stateDirectory());
            change(settings);
            settings.save(stateDirectory());
        }
        static std::wstring durationText(int64_t seconds) {
            int64_t hours = seconds / 3600, minutes = (seconds % 3600) / 60;
            if (hours > 0)
                return std::to_wstring(hours) + L" 小时 " + std::to_wstring(minutes) + L" 分";
            return std::to_wstring(minutes) + L" 分 " + std::to_wstring(seconds % 60) + L" 秒";
        }
        void announceMode() {
            message(L"队列模式 · " + modeLabel(session.mode()) + L" (" + unutf8(modeName()) + L")");
        }
        void applyQueueMode(QueueMode mode) {
            session.setMode(mode);
            persistSetting([mode](Settings& s) { s.queueMode = mode; });
            announceMode();
        }
        void cycleQueueMode() {
            const auto& modes = queueModes();
            size_t index = 0;
            for (size_t i = 0; i < modes.size(); ++i)
                if (modes[i] == session.mode())
                    index = i;
            applyQueueMode(modes[(index + 1) % modes.size()]);
        }
        // The detail page is an overlay: it remembers whether it was opened from the homepage or
        // from a result/queue page, so Esc always returns to the same place.
        void openDetail() {
            detail = true;
            detailFollow = true;
            detailScroll = 0;
            chatView = false;
            searchFocus = false;
            modal = Modal::None;
        }
        // Model discovery is one bounded request off the UI thread; tick() publishes the result. A
        // failure is reported with its code instead of inventing a model list.
        void startDiscovery(bool forceFetch) {
            if (options.demo) {
                message(L"演示模式 · 模型发现请退出后运行 mora");
                return;
            }
            if (discoverRunning) {
                message(L"正在从端点发现模型…");
                return;
            }
            if (!forceFetch) {
                auto cached = cachedModelCatalog();
                if (cached.models.empty()) {
                    message(L"还没有缓存的模型目录 · 按 F 从端点发现");
                    return;
                }
                modelList = cached.models;
                modelFromCache = true;
                modelSelected = 0;
                auto config = loadAgentConfig();
                for (size_t i = 0; i < modelList.size(); ++i)
                    if (modelList[i].id == config.model)
                        modelSelected = (int)i;
                modal = Modal::AiModels;
                message(L"已缓存 " + std::to_wstring(modelList.size()) +
                        L" 个模型 · Enter 采用 · F 重新发现");
                return;
            }
            if (discoverThread.joinable())
                discoverThread.join();
            discoverRunning = true;
            message(L"正在从端点发现模型…");
            discoverThread = std::thread([this] {
                ModelCatalog catalog;
                try {
                    catalog = fetchModelCatalog(loadAgentConfig());
                } catch (const std::exception& e) {
                    catalog.ok = false;
                    catalog.code = "exception";
                    catalog.error = e.what();
                }
                if (catalog.ok)
                    cacheModelCatalog(catalog);
                std::lock_guard lock(discoverMutex);
                discoverResult = std::move(catalog);
                discoverRunning = false;
            });
        }
        void handleDiscoveryResult() {
            std::optional<ModelCatalog> finished;
            {
                std::lock_guard lock(discoverMutex);
                finished.swap(discoverResult);
            }
            if (!finished)
                return;
            if (!finished->ok) {
                auto code = finished->code.empty() ? std::string("unknown") : finished->code;
                message(L"模型发现失败 [" + unutf8(code) + L"]：" +
                        sanitize(unutf8(finished->error.empty() ? std::string("端点没有返回详情")
                                                                : finished->error)));
                return;
            }
            if (finished->models.empty()) {
                message(L"端点没有返回可用模型 · 可按 M 手填模型 ID");
                return;
            }
            modelList = finished->models;
            modelFromCache = false;
            modelSelected = 0;
            auto config = loadAgentConfig();
            for (size_t i = 0; i < modelList.size(); ++i)
                if (modelList[i].id == config.model)
                    modelSelected = (int)i;
            modal = Modal::AiModels;
            message(L"发现 " + std::to_wstring(modelList.size()) +
                    L" 个模型 · ↑↓ 选择，Enter 采用");
        }
        // Adopting a discovered model stores its id, its reported context window and its modalities.
        // Modalities are only ever reported, never chosen by the listener.
        void adoptModel(int index) {
            if (index < 0 || index >= (int)modelList.size())
                return;
            const auto& capability = modelList[(size_t)index];
            auto config = loadAgentConfig();
            config.model = capability.id;
            config.contextTokens = capability.contextTokens;
            config.modelModalities = capability.inputModalities;
            saveAgentConfig(config);
            contextLimitTokens = capability.contextTokens;
            persistSetting([this](Settings& s) { s.contextLimitTokens = contextLimitTokens; });
            message(L"已采用模型 " + unutf8(capability.id) + L" · 上下文 " +
                    (capability.contextTokens
                         ? std::to_wstring(capability.contextTokens) + L" tokens"
                         : std::wstring(L"端点未报告上下文")));
            modal = Modal::Ai;
        }
        // Listen reports come from the local log only; a period with no rows is reported as empty
        // instead of showing a blank panel.
        void loadStats(ListenPeriod period) {
            statsPeriod = period;
            statsLoaded = false;
            statsScroll = 0;
            if (!listenHistory) {
                message(L"演示模式不写入听歌记录 · 请退出后运行 mora");
                return;
            }
            statsReport = listenHistory->aggregate(period);
            statsLoaded = true;
            modal = Modal::Stats;
            if (statsReport.plays == 0)
                message(L"这个周期还没有听歌记录 · 播放几首后再看");
        }
        // The review is written by the model from the real local aggregate. With no data, or when
        // the model fails, the player says so and never fabricates a comment.
        void startReview() {
            if (!listenHistory) {
                message(L"演示模式不能写锐评 · 请退出后运行 mora");
                return;
            }
            auto stats = listenHistory->aggregate(statsPeriod);
            if (stats.plays <= 0) {
                message(L"还没有听歌记录，先播放几首再来写锐评");
                return;
            }
            int target = std::clamp(reviewChars, 200, 4000);
            std::wstring prompt =
                L"下面是我真实的听歌统计（由本机播放器记录，只包含这些数据）：\n" +
                unutf8(listenStatsJson(stats).dump()) + L"\n请据此写一份约 " +
                std::to_wstring(target) +
                L" 字的听歌锐评，必须覆盖：歌品与性格画像、特殊歌曲、高频曲目、听歌风格。";
            prompt += L"只能引用统计里真实出现的歌曲、歌手与数字；数据不足就如实指出不足，"
                      L"不要编造任何记录。";
            std::wstring extra =
                L"这一次是长文锐评任务，请写约 " + std::to_wstring(target) +
                L" 字，可以超过上面 120 字的限制；不要调用会改变播放或账号状态的工具。";
            startChat(prompt, L"请为我的听歌记录写一份锐评", true, extra);
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
        // `remember` keeps automatic track reactions out of the stored memory, and `extraSystem`
        // carries a task-specific instruction (the review) without changing the persona contract.
        void startChat(const std::wstring& prompt, const std::wstring& label = {},
                       bool remember = true, const std::wstring& extraSystem = {}) {
            if (options.demo) {
                message(L"演示模式 · 模型请求请退出后运行 mora");
                return;
            }
            if (busy || chatRunning) {
                message(L"正在处理上一件事，请稍候");
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
            chatLog.push_back({label.empty() ? prompt : label, personaWaitLine(persona, 0)});
            chatScroll = 0;
            waitLineIndex = 0;
            waitLineSince = Clock::now();
            chatWaiting = true;
            if (chatLog.size() > 8)
                chatLog.erase(chatLog.begin());
            message(L"模型正在处理…");
            // The persona, the user's chosen name and the stored memory must be captured on the UI
            // thread: the chat worker may not read mutable UI state while the main thread repaints.
            auto systemPrompt = personaSystemPrompt(persona, userName) + L"\n" + playbackContext() +
                                L"聊天时可以自然带入当前播放的歌曲或这一句歌词，但不要编造歌词或播放"
                                L"状态；需要完整歌词时先调用 music_get_lyrics。";
            if (!extraSystem.empty())
                systemPrompt += L"\n" + extraSystem;
            auto personaId = persona.id;
            auto memory = conversationHistory(conversation, 24);
            bool compress = autoCompress;
            int contextLimit = contextLimitTokens;
            chatThread = std::thread([this, request, text = utf8(prompt), remember,
                                      systemPrompt = std::move(systemPrompt),
                                      personaId = std::move(personaId),
                                      memory = std::move(memory), compress, contextLimit] {
                Reply reply;
                reply.request = request;
                reply.type = "chat";
                try {
                    Json confirmedLikes = Json::array();
                    Json createdPersonas = Json::array();
                    auto config = loadAgentConfig();
                    config.autoCompress = compress;
                    if (contextLimit > 0)
                        config.contextTokens = contextLimit;
                    auto outcome = runChatLoop(
                        config, text,
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
                        {}, systemPrompt, memory);
                    reply.data = Json{{"ok", outcome.ok},
                                      {"code", outcome.code},
                                      {"error", outcome.error},
                                      {"toolCalls", outcome.toolCalls},
                                      {"trace", outcome.trace},
                                      {"personaId", personaId},
                                      {"confirmedLikes", confirmedLikes},
                                      {"createdPersonas", createdPersonas},
                                      {"remember", remember},
                                      {"user", text},
                                      {"promptTokens", outcome.promptTokens},
                                      {"contextLimitTokens", outcome.contextLimitTokens},
                                      {"compressedMessages", outcome.compressedMessages},
                                      {"compressionDegraded", outcome.compressionDegraded},
                                      {"compressionCode", outcome.compressionCode}};
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
            prompt += L"。请用当前人设的口吻，就这首歌给一句不超过 " +
                      std::to_wstring(std::clamp(reactMaxChars, 10, 200)) +
                      L" 字、像在场一起听的反应；";
            prompt += L"不要调用工具，不要复述这条系统提示。";
            auto state = session.audioState();
            if (!currentLyrics.instrumental && currentLyrics.songId == track.id) {
                int index = lyricIndexAt(currentLyrics, state.position);
                if (index >= 0 && index < (int)currentLyrics.lines.size())
                    prompt += L"此刻歌词是「" + currentLyrics.lines[(size_t)index].text + L"」。";
            }
            startChat(prompt, L"♪ " + (track.name.empty() ? std::wstring(L"换歌") : track.name),
                      false);
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
                    // Backward compatible switch: on maps to repeat_all, off to sequential, and the
                    // bare form still toggles between those two.
                    if (args.empty())
                        applyQueueMode(session.repeat() ? QueueMode::Sequential
                                                        : QueueMode::RepeatAll);
                    else
                        applyQueueMode(args == L"off" ? QueueMode::Sequential
                                                      : QueueMode::RepeatAll);
                } else {
                    if (!args.empty())
                        motion = args == L"off";
                    action(A_Motion);
                }
            } else if (verb == L"mode") {
                if (args.empty()) {
                    cycleQueueMode();
                } else {
                    QueueMode parsed;
                    if (!parseQueueMode(utf8(args), parsed)) {
                        message(L"可用模式：sequential / repeat_all / repeat_one / shuffle");
                        return;
                    }
                    applyQueueMode(parsed);
                }
            } else if (verb == L"detail")
                openDetail();
            else if (verb == L"stats") {
                ListenPeriod period = statsPeriod;
                if (!args.empty() && !parseListenPeriod(utf8(args), period)) {
                    message(L"用法：/stats [day | month | year]");
                    return;
                }
                loadStats(period);
            } else if (verb == L"review") {
                ListenPeriod period = statsPeriod;
                if (!args.empty() && !parseListenPeriod(utf8(args), period)) {
                    message(L"用法：/review [day | month | year]");
                    return;
                }
                statsPeriod = period;
                startReview();
            } else if (verb == L"models")
                startDiscovery(false);
            else if (verb == L"ai")
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
                session.setSource(collection ? (backMode == 2 ? "album" : "playlist")
                                             : mode == 4 ? "library"
                                             : mode == 5 ? "queue" : "search");
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
                if (modal == Modal::SavedSession) {
                    quit = true;
                    return;
                }
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
            if (detail) {
                detail = false;
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
            if (a >= A_ModelRow && a < A_ModelRow + 64) {
                modelSelected = a - A_ModelRow;
                adoptModel(modelSelected);
                return;
            }
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
            case A_UseSavedSession:
                if (!busy)
                    submit("account", [](MusicService& s) {
                        s.account();
                        Reply r;
                        r.type = "account";
                        r.nickname = s.nickname();
                        r.uid = s.uid();
                        r.likes = s.likes();
                        return r;
                    });
                break;
            case A_ForgetSavedSession:
                if (!busy)
                    action(A_Logout);
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
            case A_AiClearKey:
                try {
                    clearAgentKey();
                    message(L"本机模型密钥已清除；普通听歌仍可使用");
                } catch (const std::exception&) {
                    message(L"清除密钥失败，请检查本机配置目录");
                }
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
            case A_Repeat:
                applyQueueMode(session.repeat() ? QueueMode::Sequential : QueueMode::RepeatAll);
                break;
            case A_Mode:
                cycleQueueMode();
                break;
            case A_Detail:
                openDetail();
                break;
            case A_ReactToggle:
                reactEnabled = !reactEnabled;
                if (!reactEnabled)
                    reactPending = false;
                persistSetting([this](Settings& s) { s.reactEnabled = reactEnabled; });
                if (reactEnabled)
                    message(L"自发评价已开启 · 换歌时按设定间隔说一句（每首最多一次）");
                else
                    message(L"自发评价已关闭");
                break;
            case A_ReactSlower:
                reactMinSeconds = std::min(600, reactMinSeconds + 5);
                persistSetting([this](Settings& s) { s.reactMinSeconds = reactMinSeconds; });
                message(L"评价间隔 · " + std::to_wstring(reactMinSeconds) + L" 秒");
                break;
            case A_ReactFaster:
                reactMinSeconds = std::max(10, reactMinSeconds - 5);
                persistSetting([this](Settings& s) { s.reactMinSeconds = reactMinSeconds; });
                message(L"评价间隔 · " + std::to_wstring(reactMinSeconds) + L" 秒");
                break;
            case A_ReactCharsUp:
                reactMaxChars = std::min(200, reactMaxChars + 5);
                persistSetting([this](Settings& s) { s.reactMaxChars = reactMaxChars; });
                message(L"评价字数 · 最多 " + std::to_wstring(reactMaxChars) + L" 字");
                break;
            case A_ReactCharsDown:
                reactMaxChars = std::max(10, reactMaxChars - 5);
                persistSetting([this](Settings& s) { s.reactMaxChars = reactMaxChars; });
                message(L"评价字数 · 最多 " + std::to_wstring(reactMaxChars) + L" 字");
                break;
            case A_ReviewCharsUp:
                reviewChars = std::min(4000, reviewChars + 100);
                persistSetting([this](Settings& s) { s.reviewChars = reviewChars; });
                message(L"锐评字数 · 约 " + std::to_wstring(reviewChars) + L" 字");
                break;
            case A_ReviewCharsDown:
                reviewChars = std::max(200, reviewChars - 100);
                persistSetting([this](Settings& s) { s.reviewChars = reviewChars; });
                message(L"锐评字数 · 约 " + std::to_wstring(reviewChars) + L" 字");
                break;
            case A_CompressToggle:
                autoCompress = !autoCompress;
                persistSetting([this](Settings& s) { s.autoCompress = autoCompress; });
                message(autoCompress ? L"对话记忆自动压缩已开启" : L"对话记忆自动压缩已关闭");
                break;
            case A_AiDiscover:
                startDiscovery(true);
                break;
            case A_AiCached:
                startDiscovery(false);
                break;
            case A_StatsDay:
                loadStats(ListenPeriod::Day);
                break;
            case A_StatsMonth:
                loadStats(ListenPeriod::Month);
                break;
            case A_StatsYear:
                loadStats(ListenPeriod::Year);
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
            if (modal == Modal::SavedSession) {
                done(BridgeReply(false, "account_confirmation_required",
                                 "confirm the saved account in the terminal first", Json::object()));
                return;
            }
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
        // Bounded projection of the authoritative queue. `position` is the absolute 0-based queue
        // index so an adapter can act on a row without guessing, and `current` marks the row the
        // player is on. Empty queues are a normal answer, not an error.
        Json queueView(int offset, int limit) const {
            const auto& tracks = session.tracks();
            size_t begin = std::min<size_t>(static_cast<size_t>(std::max(0, offset)),
                                            tracks.size());
            size_t end = std::min<size_t>(tracks.size(), begin + static_cast<size_t>(limit));
            Json items = Json::array();
            for (size_t i = begin; i < end; ++i) {
                const Track& track = tracks[i];
                items.push_back(Json{{"position", (int)i},
                                     {"id", track.id},
                                     {"name", utf8(track.name)},
                                     {"artist", utf8(track.artist)},
                                     {"album", utf8(track.album)},
                                     {"durationSeconds", track.duration},
                                     {"current", i == session.index()}});
            }
            return Json{{"total", (int)tracks.size()},
                        {"index", (int)session.index()},
                        {"mode", queueModeName(session.mode())},
                        {"repeat", session.repeat()},
                        {"offset", std::max(0, offset)},
                        {"limit", limit},
                        {"items", items}};
        }
        // queue_edit: exactly one write per call. Out-of-range indices are rejected without
        // modifying the queue, and a no-op (same index, same mode, from == to) reports
        // changed:false instead of failing.
        BridgeReply queueEdit(const Json& params) {
            auto invalid = [](const std::string& text) {
                return BridgeReply(false, "invalid_arguments", text, Json::object());
            };
            auto integerAt = [&params](const char* key, int fallback, bool& ok) {
                if (!params.contains(key))
                    return fallback;
                if (!params[key].is_number_integer() || params[key].is_boolean()) {
                    ok = false;
                    return fallback;
                }
                return params[key].get<int>();
            };
            if (!params.is_object())
                return invalid("queue_edit requires an object");
            auto op = params.value("op", std::string());
            bool ok = true;
            bool changed = false;
            if (op == "set_mode") {
                if (!params.contains("mode") || !params["mode"].is_string())
                    return invalid("mode is required");
                QueueMode mode;
                if (!parseQueueMode(params["mode"].get<std::string>(), mode))
                    return invalid("mode must be sequential, repeat_all, repeat_one or shuffle");
                bool seedOk = true;
                bool seedGiven = params.contains("seed");
                int seedValue = integerAt("seed", 1, seedOk);
                if (!seedOk || seedValue < 1)
                    return invalid("seed must be a positive integer");
                QueueMode before = session.mode();
                session.setMode(mode, static_cast<uint64_t>(seedValue));
                // `changed` also reports an explicit-seed re-shuffle: the projection (mode/total/
                // index) may look identical while the audible order really was rebuilt.
                bool reshuffled =
                    seedGiven && mode == QueueMode::Shuffle && session.queueSize() > 1;
                changed = before != session.mode() || reshuffled;
                if (before != session.mode())
                    persistSetting([mode](Settings& s) { s.queueMode = mode; });
            } else if (op == "play_index" || op == "remove" || op == "move") {
                if (session.queueEmpty())
                    return BridgeReply(false, "queue_boundary", "the queue is empty",
                                       Json::object());
                size_t size = session.queueSize();
                if (op == "move") {
                    int from = integerAt("from", -1, ok), to = integerAt("to", -1, ok);
                    if (!ok || from < 0 || to < 0 || (size_t)from >= size || (size_t)to >= size)
                        return invalid("from and to must be 0-based indices inside the queue");
                    if (from != to) {
                        session.moveTrack((size_t)from, (size_t)to);
                        changed = true;
                    }
                } else {
                    int index = integerAt("index", -1, ok);
                    if (!ok || index < 0 || (size_t)index >= size)
                        return invalid("index must be a 0-based index inside the queue");
                    if (op == "play_index") {
                        if ((size_t)index != session.index()) {
                            session.playIndex((size_t)index);
                            changed = true;
                        }
                    } else {
                        session.removeIndex((size_t)index);
                        changed = true;
                    }
                }
            } else if (op == "clear") {
                changed = !session.queueEmpty();
                session.clearQueue();
            } else if (op == "add_next" || op == "append" || op == "replace") {
                // replace swaps the whole queue for the new set and starts its first row; append and
                // add_next only add, so they can never throw away what is already queued.
                size_t maxTracks = op == "replace" ? 100 : 50;
                std::vector<Track> incoming;
                if (params.contains("tracks")) {
                    if (!params["tracks"].is_array())
                        return invalid("tracks must be an array");
                    for (const auto& item : params["tracks"]) {
                        if (!item.is_object())
                            return invalid("every track must be an object");
                        int64_t id = item.value("id", (int64_t)0);
                        if (id <= 0)
                            return invalid("every track needs a positive id");
                        Track track;
                        track.id = id;
                        track.name = unutf8(item.value("name", std::string()));
                        track.artist = unutf8(item.value("artist", std::string()));
                        track.album = unutf8(item.value("album", std::string()));
                        track.duration = item.value("duration", 0);
                        incoming.push_back(std::move(track));
                    }
                } else if (params.contains("ids")) {
                    if (!params["ids"].is_array())
                        return invalid("ids must be an array");
                    for (const auto& item : params["ids"]) {
                        if (!item.is_number_integer() || item.is_boolean())
                            return invalid("ids must be positive integers");
                        int64_t id = item.get<int64_t>();
                        if (id <= 0)
                            return invalid("ids must be positive integers");
                        // Reuse the metadata of a row already in the queue; an unknown id still
                        // enters as a placeholder and is filled by the normal details path.
                        Track track{.id = id};
                        for (const auto& row : session.tracks())
                            if (row.id == id) {
                                track = row;
                                break;
                            }
                        incoming.push_back(std::move(track));
                    }
                } else
                    return invalid("ids or tracks are required");
                if (incoming.empty() || incoming.size() > maxTracks)
                    return invalid(op == "replace" ? "1..100 tracks are required"
                                                   : "1..50 tracks are required");
                if (op == "replace") {
                    session.assign(std::move(incoming), 0);
                    changed = !session.queueEmpty();
                    playCurrent();
                } else
                    changed =
                        op == "add_next" ? session.playNext(incoming) : session.enqueue(incoming);
            } else
                return BridgeReply(false, "unknown_method", "unknown queue op: " + op,
                                   Json::object());
            Json view = queueView(0, 100);
            view["applied"] = op;
            view["changed"] = changed;
            if (params.contains("metadataResolved"))
                view["metadataResolved"] = params["metadataResolved"];
            return BridgeReply(true, "", "", std::move(view));
        }
        // Runs on the UI thread through BridgeServer::pump: adapters only ever describe intent,
        // the session stays owned here.
        BridgeReply control(const std::string& method, const Json& params) {
            if (modal == Modal::SavedSession && method != "hello")
                return BridgeReply(false, "account_confirmation_required",
                                   "confirm the saved account in the terminal first",
                                   Json::object());
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
            // Queue bridge for the MCP adapter and the model loop. Read-only listing never checks
            // `busy`; edits are single operations that never retry and never touch the network.
            if (method == "queue_list") {
                int limit = 100, offset = 0;
                if (params.contains("limit") && params["limit"].is_number_integer())
                    limit = std::clamp(params["limit"].get<int>(), 1, 100);
                if (params.contains("offset") && params["offset"].is_number_integer())
                    offset = std::max(0, params["offset"].get<int>());
                return BridgeReply(true, "", "", queueView(offset, limit));
            }
            if (method == "queue_edit") {
                if (busy)
                    return busyReply();
                return queueEdit(params);
            }
            if (method == "play") {
                if (busy)
                    return busyReply();
                auto kind = params.value("kind", std::string("song"));
                int64_t id = params.value("id", (int64_t)0);
                if (id <= 0)
                    return BridgeReply(false, "invalid_arguments", "id must be a positive id",
                                       Json::object());
                if (kind == "song") {
                    session.setSource("agent");
                    session.assignId(id);
                    if (!playCurrent())
                        return busyReply();
                    return BridgeReply(true, "", "",
                                       Json{{"status", "accepted"}, {"kind", kind}, {"id", id}});
                }
                if (kind == "album" || kind == "playlist") {
                    lastPlayError.clear();
                    session.setSource("agent");
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
            if (modal == Modal::SavedSession) {
                if (k.wVirtualKeyCode == VK_RETURN)
                    action(A_UseSavedSession);
                else if (k.wVirtualKeyCode == 'D')
                    action(A_ForgetSavedSession);
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
                else if (k.wVirtualKeyCode == 'D') action(A_AiClearKey);
                else if (k.wVirtualKeyCode == 'F') action(A_AiDiscover);
                else if (k.wVirtualKeyCode == 'L') startDiscovery(false);
                return;
            }
            if (modal == Modal::AiModels) {
                if (k.wVirtualKeyCode == VK_UP)
                    modelSelected = std::max(0, modelSelected - 1);
                if (k.wVirtualKeyCode == VK_DOWN)
                    modelSelected =
                        std::min(std::max(0, (int)modelList.size() - 1), modelSelected + 1);
                if (k.wVirtualKeyCode == VK_RETURN)
                    adoptModel(modelSelected);
                else if (k.wVirtualKeyCode == 'F')
                    startDiscovery(true);
                else if (k.wVirtualKeyCode == 'M')
                    action(A_AiModel);
                return;
            }
            if (modal == Modal::Stats) {
                if (k.wVirtualKeyCode == VK_UP)
                    statsScroll = std::max(0, statsScroll - 1);
                else if (k.wVirtualKeyCode == VK_DOWN)
                    ++statsScroll;
                else if (k.wVirtualKeyCode == VK_PRIOR)
                    statsScroll = std::max(0, statsScroll - 5);
                else if (k.wVirtualKeyCode == VK_NEXT)
                    statsScroll += 5;
                else if (k.wVirtualKeyCode == VK_LEFT)
                    loadStats((ListenPeriod)std::max(0, (int)statsPeriod - 1));
                else if (k.wVirtualKeyCode == VK_RIGHT)
                    loadStats((ListenPeriod)std::min(2, (int)statsPeriod + 1));
                else if (k.wVirtualKeyCode == '1')
                    loadStats(ListenPeriod::Day);
                else if (k.wVirtualKeyCode == '2')
                    loadStats(ListenPeriod::Month);
                else if (k.wVirtualKeyCode == '3')
                    loadStats(ListenPeriod::Year);
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
            if (modal == Modal::Settings) {
                // One keystroke per preference, so the panel needs no nested selection state.
                if (k.wVirtualKeyCode == 'R')
                    action(A_ReactToggle);
                else if (c == L'[')
                    action(A_ReactFaster);
                else if (c == L']')
                    action(A_ReactSlower);
                else if (c == L'-')
                    action(A_ReactCharsDown);
                else if (c == L'+' || c == L'=' || k.wVirtualKeyCode == VK_OEM_PLUS)
                    action(A_ReactCharsUp);
                else if (c == L',')
                    action(A_ReviewCharsDown);
                else if (c == L'.')
                    action(A_ReviewCharsUp);
                else if (k.wVirtualKeyCode == 'C')
                    action(A_CompressToggle);
                return;
            }
            if (modal == Modal::Help || modal == Modal::Agent)
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
            if (detail) {
                // The detail page owns scroll keys; everything else stays with the player so Esc,
                // Space and the volume keys keep working while lyrics are open.
                switch (k.wVirtualKeyCode) {
                case VK_UP:
                    detailFollow = false;
                    detailScroll = std::max(0, detailScroll - 1);
                    return;
                case VK_DOWN:
                    detailFollow = false;
                    detailScroll += 1;
                    return;
                case VK_PRIOR:
                    detailFollow = false;
                    detailScroll = std::max(0, detailScroll - 5);
                    return;
                case VK_NEXT:
                    detailFollow = false;
                    detailScroll += 5;
                    return;
                case VK_HOME:
                    detailFollow = false;
                    detailScroll = 0;
                    return;
                case VK_END:
                    detailFollow = false;
                    detailScroll = (int)currentLyrics.lines.size();
                    return;
                case VK_RETURN:
                    detailFollow = true;
                    return;
                case VK_SPACE:
                    session.pause();
                    return;
                case VK_OEM_PLUS:
                    session.adjustVolume(.05);
                    return;
                case VK_OEM_MINUS:
                    session.adjustVolume(-.05);
                    return;
                default:
                    return;
                }
            }
            // One rule for every advertised page shortcut: while the composer is empty, the letters
            // and digits printed on the frame act as the actions they name; as soon as there is any
            // text, every printable character belongs to the composer. The homepage only honours the
            // two shortcuts it actually advertises (1–5 and D) so a chat message typed there keeps
            // working, while a browse/queue page offers its whole control row.
            bool pageShortcut = !chatView && !detail && modal == Modal::None && input.empty() &&
                                (home || !searchFocus);
            if (pageShortcut) {
                wchar_t letter = c ? c : (wchar_t)k.wVirtualKeyCode;
                if (letter >= L'a' && letter <= L'z')
                    letter = (wchar_t)(letter - L'a' + L'A');
                if (k.wVirtualKeyCode == VK_SPACE) {
                    action(A_Pause);
                    return;
                }
                if (letter == L'D') {
                    openDetail();
                    return;
                }
                if (letter >= L'1' && letter <= L'5') {
                    chooseMode(letter - L'0');
                    return;
                }
                if (!home) {
                    switch (letter) {
                    case L'L':
                        action(A_Login);
                        return;
                    case L'H':
                        action(A_Like);
                        return;
                    case L'A':
                        action(A_Save);
                        return;
                    case L'N':
                        action(A_Next);
                        return;
                    case L'P':
                        action(A_Previous);
                        return;
                    case L'V':
                        action(A_Quality);
                        return;
                    case L'M':
                        action(A_Motion);
                        return;
                    case L'C':
                        action(A_Mode);
                        return;
                    case L'Q':
                        quit = true;
                        return;
                    case L'?':
                        action(A_Help);
                        return;
                    case L'+':
                    case L'=':
                        action(A_VolUp);
                        return;
                    case L'-':
                        action(A_VolDown);
                        return;
                    default:
                        break;
                    }
                }
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
            case 'D':
                openDetail();
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
                    showWelcomeIfNeeded();
                } else if (r.type == "logout") {
                    nickname = L"未登录";
                    likes.clear();
                    modal = Modal::None;
                    message(L"本机登录会话已清除");
                    showWelcomeIfNeeded();
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
                chatWaiting = false;
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
                    // Remember the turn only after a real answer, and only for requests the player
                    // itself started (automatic reactions are not part of the user's memory).
                    if (completed->data.value("remember", false)) {
                        conversation.append("user", completed->data.value("user", std::string()));
                        conversation.append("assistant", completed->text);
                        if (!options.demo && options.seconds <= 0 && options.snapshot.empty())
                            saveConversation(stateDirectory(), conversation);
                    }
                    lastPromptTokens = completed->data.value("promptTokens", (size_t)0);
                    lastContextTokens = completed->data.value("contextLimitTokens", (size_t)0);
                    int compressed = completed->data.value("compressedMessages", 0);
                    // The degradation flag comes straight from the loop's own outcome; it is never
                    // inferred from the summary text and never shown when no compression happened.
                    bool degraded = completed->data.value("compressionDegraded", false);
                    std::string degradeCode = completed->data.value("compressionCode", std::string());
                    if (compressed > 0) {
                        std::wstring note = L"（已压缩 " + std::to_wstring(compressed) +
                                            L" 条早前对话 · 上下文 " +
                                            std::to_wstring(lastPromptTokens) + L"/" +
                                            std::to_wstring(lastContextTokens) + L" tokens）";
                        if (degraded && !degradeCode.empty())
                            note += L"（压缩降级：" + unutf8(degradeCode) + L"，已折叠 " +
                                    std::to_wstring(compressed) + L" 条）";
                        chatLog.push_back({L"", note});
                        if (chatLog.size() > 8)
                            chatLog.erase(chatLog.begin());
                        message(L"对话记忆已压缩 " + std::to_wstring(compressed) +
                                L" 条早期消息 · 请求 " + std::to_wstring(lastPromptTokens) +
                                L"/" + std::to_wstring(lastContextTokens) + L" tokens" +
                                (degraded && !degradeCode.empty()
                                     ? L" · 压缩降级：" + unutf8(degradeCode)
                                     : L""));
                    } else
                        message(L"模型已回复 · 音乐工具调用 " +
                                std::to_wstring(completed->data.value("toolCalls", 0)) + L" 次" +
                                (lastContextTokens
                                     ? L" · 上下文 " + std::to_wstring(lastPromptTokens) + L"/" +
                                           std::to_wstring(lastContextTokens)
                                     : L""));
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
            handleDiscoveryResult();
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
                // The gap and the reply length come from the settings panel; the per-session cap is
                // derived from the gap, so a listener can never turn the reaction into an
                // unbounded request stream.
                int gapSeconds = std::clamp(reactMinSeconds, 10, 600);
                int sessionCap = std::max(4, 3600 / gapSeconds);
                if (playing.id != reactSong) {
                    reactSong = playing.id;
                    reactPending = reactEnabled && !options.demo && playing.id && chatView &&
                                   !chatRunning && !busy && state.playing &&
                                   now - lastReaction >= std::chrono::seconds(gapSeconds) &&
                                   reactCount < sessionCap;
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
            // Rotate the persona's waiting line while a model request is in flight instead of
            // leaving one generic "contacting the model" string on screen.
            if (chatWaiting && chatRunning && !chatLog.empty()) {
                int index = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      Clock::now() - waitLineSince)
                                      .count() /
                                  1500);
                if (index != waitLineIndex) {
                    waitLineIndex = index;
                    chatLog.back().second = personaWaitLine(persona, index);
                }
            }
            // The queue page mirrors the authoritative queue, so its entries and current row stay
            // truthful even when playback advanced while the page stayed open.
            if (mode == 5 && !home && !chatView && !detail && modal == Modal::None) {
                rows = session.tracks();
                total = (int)rows.size();
                selected = std::clamp(selected, 0, std::max(0, total - 1));
                if (selected < top)
                    top = selected;
                if (selected >= top + listH)
                    top = selected - listH + 1;
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
        // Homepage layout works from named bands instead of fixed rows: the composer owns the
        // bottom of the screen, the now-playing card sits above it, the browse launchpad above
        // that, and the character plus the right-hand column fill whatever is left. That is what
        // keeps 78x24 and a wide window both readable without overlap.
        void drawHome(Canvas& c) {
            bool compact = height < 34 || width < 104;
            int footerTop = height - 8;
            constexpr int cardH = 4;
            int cardTop = footerTop - cardH;
            int browseY = cardTop - 2;
            int contentBottom = browseY - 1;
            int y = 2;
            int maxRows = std::max(3, contentBottom - y + 1);
            int desiredSh = compact ? (height < 28 ? 26 : 34) : personaAvatarHeight;
            int sh = std::clamp(std::min(desiredSh, maxRows * 2), 8, personaAvatarHeight);
            int sw = std::clamp(sh * personaAvatarWidth / personaAvatarHeight, 10,
                                personaAvatarWidth);
            int x = compact ? 4 : std::max(4, (width - 91) / 2);
            int right = x + sw + (compact ? 5 : 7);
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
            int ry = compact ? y + 1 : y + 3;
            for (int i = 0; i < 5; ++i)
                c.text(right, ry + i, logo[i], colors.accent, std::max(0, width - right - 3));
            int nextY = ry + 6;
            if (nextY <= contentBottom) {
                c.text(right, nextY, L"云间 · " + persona.name + L"的音乐终端", colors.text,
                       std::max(0, width - right - 3));
                nextY += 2;
            }
            if (nextY <= contentBottom) {
                c.text(right, nextY, persona.greeting, colors.muted,
                       std::max(0, width - right - 3));
                ++nextY;
            }
            if (nextY <= contentBottom && !compact) {
                c.text(right, nextY, L"输入音乐需求 · / 查看全部命令", colors.muted,
                       std::max(0, width - right - 3));
                ++nextY;
            }
            const std::array<std::pair<std::wstring, int>, 3> quick{
                std::pair{L"/search   搜索一首歌", A_Search},
                std::pair{L"/persona  切换人设", A_Persona},
                std::pair{L"/agent    接入外部 Agent", A_Agent}};
            for (const auto& [label, action] : quick) {
                if (nextY > contentBottom)
                    break;
                c.button(right, nextY, std::min(36, std::max(0, width - right - 3)), label, action);
                nextY += 2;
            }
            // Browse launchpad: one compact row, clickable and reachable with 1-5 on the keyboard.
            if (browseY >= 5) {
                c.text(4, browseY, L"浏览", colors.muted, 4);
                const std::array<std::pair<std::wstring, int>, 5> entries{
                    std::pair{L"1 单曲", A_Songs},      std::pair{L"2 专辑", A_Albums},
                    std::pair{L"3 歌单", A_Playlists}, std::pair{L"4 我的音乐", A_Library},
                    std::pair{L"5 队列", A_Queue}};
                int bx = 10;
                for (const auto& [label, action] : entries) {
                    int w = displayWidth(label) + 2;
                    if (bx + w > width - 16)
                        break;
                    c.button(bx, browseY, w, label, action);
                    bx += w + 2;
                }
                if (width >= 96)
                    c.button(width - 16, browseY, 12, L"D 详情", A_Detail);
                else if (bx + 12 <= width - 2)
                    c.button(bx, browseY, 12, L"D 详情", A_Detail);
                if (width >= 100)
                    c.button(width - 32, browseY, 14, L"/search 搜索", A_Search);
            }
            // Now-playing card: cover, title, artist/album, progress, quality and queue mode. With
            // no track it still explains what will appear here instead of going blank.
            auto state = session.audioState();
            if (cardTop >= 6) {
                c.rect(4, cardTop, width - 8, cardH, colors.surface);
                drawCover(c, 4, cardTop, 8, cardH);
                int tx = 14;
                int tw = std::max(8, width - tx - 4);
                const Track& track = session.playing();
                if (track.id) {
                    c.text(tx, cardTop, L"♪ " + (track.name.empty() ? L"载入曲目…" : track.name),
                           colors.accent, tw);
                    std::wstring sub = track.artist;
                    if (!track.album.empty())
                        sub += (sub.empty() ? L"" : L" · ") + track.album;
                    c.text(tx, cardTop + 1, sub.empty() ? L"—" : sub, colors.text, tw);
                    double duration =
                        state.duration > 0 ? state.duration : (double)track.duration;
                    c.text(tx, cardTop + 2,
                           timeText(state.position) + L" / " + timeText(duration) + L" · " +
                               actualQualityLabel() + L" · " + modeLabel(session.mode()),
                           colors.muted, tw);
                    int pw = std::min(tw, 40);
                    if (pw > 4) {
                        double ratio = duration > 0 ? std::clamp(state.position / duration, 0.0, 1.0)
                                                    : 0.0;
                        int filled = (int)(pw * ratio);
                        for (int i = 0; i < pw; ++i)
                            c.pixel(tx + i, cardTop + 3, i == filled ? L'●' : L'─',
                                    i <= filled ? colors.accent : colors.muted, colors.surface);
                    }
                } else {
                    c.text(tx, cardTop, L"♪ 还没有正在播放的歌曲", colors.accent, tw);
                    c.text(tx, cardTop + 1, L"Enter 播放 · /search 搜一首，或 1–5 浏览",
                           colors.text, tw);
                    c.text(tx, cardTop + 2,
                           L"音质 " + qualityLabel() + L" · " + modeLabel(session.mode()),
                           colors.muted, tw);
                }
            }
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
            std::wstring hint =
                detail ? L"↑↓ 滚动歌词  ·  Enter 回到当前行  ·  Space 暂停  ·  Esc 返回"
                : searchFocus
                    ? L"Enter 发送  ·  / 命令  ·  Tab 补全  ·  ↑↓ 历史  ·  Esc 返回"
                    : L"↑↓ 选择  ·  Enter 播放  ·  / 命令  ·  Tab 输入  ·  /home 首页";
            c.text(4, height - 2, hint, colors.muted, width - 20);
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
        // Two compact rows shared by every browse/queue page: a small cover plus the live track,
        // duration, quality and queue mode. It stays truthful when nothing is playing.
        void drawRouteNowPlaying(Canvas& c) {
            auto state = session.audioState();
            const Track& track = session.playing();
            drawCover(c, 3, 4, 8, 2);
            int tx = 13, tw = std::max(10, width - tx - 4);
            if (track.id) {
                std::wstring title = L"♪ " + (track.name.empty() ? L"载入曲目…" : track.name);
                if (!track.artist.empty())
                    title += L" — " + track.artist;
                c.text(tx, 4, title, colors.accent, tw);
                double duration = state.duration > 0 ? state.duration : (double)track.duration;
                std::wstring detail;
                if (!track.album.empty())
                    detail = track.album + L" · ";
                detail += timeText(state.position) + L" / " + timeText(duration) + L" · " +
                          actualQualityLabel() + L" · " + modeLabel(session.mode());
                c.text(tx, 5, detail, colors.muted, tw);
            } else {
                c.text(tx, 4, L"♪ 还没有正在播放的歌曲", colors.accent, tw);
                c.text(tx, 5, L"音质 " + qualityLabel() + L" · " + modeLabel(session.mode()),
                       colors.muted, tw);
            }
        }
        // Song detail page: cover, metadata and scrollable lyrics. Instrumental, not-yet-loaded and
        // unavailable lyrics each say what they are instead of showing an empty box.
        void drawDetail(Canvas& c) {
            const Track& track = session.playing();
            auto state = session.audioState();
            c.text(4, 3, L"歌曲详情", colors.accent, 20);
            c.button(width - 13, 3, 10, L"D 关闭", A_Back);
            int coverW = std::clamp(width / 7, 10, 22), coverH = std::clamp(height / 4, 3, 10);
            drawCover(c, 4, 5, coverW, coverH);
            int tx = 4 + coverW + 3, tw = std::max(10, width - tx - 4);
            if (track.id) {
                c.text(tx, 5, track.name.empty() ? L"载入曲目…" : track.name, colors.accent, tw);
                c.text(tx, 6, track.artist.empty() ? L"—" : track.artist, colors.text, tw);
                c.text(tx, 7, track.album.empty() ? L"—" : track.album, colors.muted, tw);
                double duration = state.duration > 0 ? state.duration : (double)track.duration;
                std::wstring format =
                    state.sampleRate ? L" · " + std::to_wstring(state.sampleRate / 1000) + L"k/" +
                                           std::to_wstring(state.sampleBits) + L"bit"
                                     : L"";
                c.text(tx, 8, L"时长 " + timeText(duration) + L" · " + actualQualityLabel() + format,
                       colors.muted, tw);
                c.text(tx, 9,
                       L"队列 第 " + std::to_wstring((int)session.index() + 1) + L"/" +
                           std::to_wstring((int)session.tracks().size()) + L" 首 · " +
                           modeLabel(session.mode()),
                       colors.muted, tw);
            } else {
                c.text(tx, 5, L"还没有正在播放的歌曲", colors.accent, tw);
                c.text(tx, 6, L"按 /search 搜一首，Enter 播放后这里会显示封面与歌词。", colors.text,
                       tw);
                c.text(tx, 8, L"音质 " + qualityLabel() + L" · " + modeLabel(session.mode()),
                       colors.muted, tw);
            }
            enum class LyricState { Ready, Loading, Instrumental, Unavailable, Empty };
            LyricState ls = LyricState::Loading;
            if (!track.id)
                ls = LyricState::Empty;
            else if (currentLyrics.instrumental)
                ls = LyricState::Instrumental;
            else if (currentLyrics.unavailable)
                ls = LyricState::Unavailable;
            else if (currentLyrics.songId == track.id && !currentLyrics.lines.empty())
                ls = LyricState::Ready;
            int lyricTop = std::max(5 + coverH + 1, 12);
            int bottom = std::max(lyricTop + 2, height - 8);
            std::wstring header = L"歌词";
            if (ls == LyricState::Instrumental)
                header += L" · 纯音乐，没有歌词";
            else if (ls == LyricState::Unavailable)
                header += L" · 歌词暂不可用";
            else if (ls == LyricState::Loading)
                header += L" · 歌词尚未加载";
            else if (ls == LyricState::Ready)
                header += L" · 当前行高亮 · ↑↓ 滚动";
            c.line(4, lyricTop, width - 8, colors.surface);
            c.text(4, lyricTop, L" " + header + L" ", colors.accent, width - 10);
            int first = lyricTop + 1, visible = std::max(1, bottom - first);
            if (ls == LyricState::Ready) {
                int lines = (int)currentLyrics.lines.size();
                int live = lyricIndexAt(currentLyrics, state.position);
                if (detailFollow && live >= 0)
                    detailScroll = std::max(0, live - visible / 2);
                detailScroll = std::clamp(detailScroll, 0, std::max(0, lines - visible));
                for (int i = 0; i < visible; ++i) {
                    int index = detailScroll + i;
                    if (index >= lines)
                        break;
                    bool current = index == live;
                    if (current)
                        c.rect(4, first + i, width - 8, 1, colors.surface);
                    c.text(4, first + i,
                           (current ? L"▶ " : L"  ") + currentLyrics.lines[(size_t)index].text,
                           current ? colors.accent : colors.muted, width - 8);
                    if (current && !currentLyrics.lines[(size_t)index].translation.empty() &&
                        width >= 108)
                        c.text(width / 2 + 2, first + i,
                               currentLyrics.lines[(size_t)index].translation, colors.text,
                               width / 2 - 8);
                }
            } else if (ls == LyricState::Loading)
                c.text(4, first, L"歌词正在加载，稍等一下就回来。", colors.muted, width - 10);
            else if (ls == LyricState::Instrumental)
                c.text(4, first, L"这首歌是纯音乐，没有歌词。", colors.muted, width - 10);
            else if (ls == LyricState::Unavailable)
                c.text(4, first, L"接口没有返回歌词（可能受版权限制），这里如实说明。", colors.muted,
                       width - 10);
            else
                c.text(4, first, L"先播一首歌，再回来看歌词。", colors.muted, width - 10);
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
            if (modal == Modal::SavedSession) {
                txt(2, L"这台电脑保存过网易云音乐登录");
                txt(4, L"会话属于当前 Windows 用户，未包含在下载包中。");
                txt(6, L"请选择是否继续使用；不会在此之前访问账号。");
                c.button(x + 3, y + 8, w - 6, L"Enter  继续使用本机账号",
                         A_UseSavedSession, true);
                c.button(x + 3, y + 10, w - 6, L"D  清除本机登录，改用游客模式",
                         A_ForgetSavedSession);
                txt(11, L"Esc 退出程序");
            } else if (modal == Modal::Login) {
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
                txt(1, L"个人设置");
                // One row per preference with inline buttons, so the panel still fits a 78x24
                // window instead of pushing half the settings off the bottom.
                auto setting = [&](int row, std::wstring_view label, const std::wstring& value,
                                   RGB color) {
                    c.text(x + 3, y + row, label, colors.muted, w - 22);
                    c.text(x + 3 + displayWidth(label), y + row, value, color, w - 22);
                };
                setting(2, L"聊天称呼：", userName.empty() ? L"尚未设置" : userName, colors.text);
                c.button(x + w - 12, y + 2, 9, L"修改", A_Name);
                setting(3, L"人设：", persona.name, colors.text);
                c.button(x + w - 12, y + 3, 9, L"切换", A_Persona);
                setting(4, L"自发评价：", reactEnabled ? L"已开启" : L"已关闭",
                        reactEnabled ? colors.accent : colors.muted);
                c.button(x + w - 12, y + 4, 9, L"R 切换", A_ReactToggle);
                setting(5, L"评价间隔：", std::to_wstring(reactMinSeconds) + L" 秒", colors.text);
                c.button(x + w - 17, y + 5, 7, L"更慢", A_ReactSlower);
                c.button(x + w - 8, y + 5, 7, L"更快", A_ReactFaster);
                setting(6, L"评价字数：", std::to_wstring(reactMaxChars) + L" 字", colors.text);
                c.button(x + w - 17, y + 6, 7, L"更短", A_ReactCharsDown);
                c.button(x + w - 8, y + 6, 7, L"更长", A_ReactCharsUp);
                setting(7, L"锐评字数：", std::to_wstring(reviewChars) + L" 字", colors.text);
                c.button(x + w - 17, y + 7, 7, L"更短", A_ReviewCharsDown);
                c.button(x + w - 8, y + 7, 7, L"更长", A_ReviewCharsUp);
                setting(8, L"记忆压缩：", autoCompress ? L"自动" : L"关闭",
                        autoCompress ? colors.accent : colors.muted);
                c.button(x + w - 12, y + 8, 9, L"C 切换", A_CompressToggle);
                setting(9, L"上下文上限：",
                        contextLimitTokens > 0 ? std::to_wstring(contextLimitTokens) + L" tokens"
                                               : L"自动（按端点报告）",
                        colors.muted);
                c.button(x + w - 12, y + 10, 9, L"引导", A_Guide);
                c.text(x + 3, y + 10, L"重看新手引导", colors.muted, w - 20);
                c.text(x + 3, y + h - 1, L"R 评价开关 · [ ] 间隔 · - + 字数 · , . 锐评 · C 压缩",
                       colors.muted, w - 6);
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
                // Preview the exact line this persona says while a model request is in flight.
                int previewY = y + h - 3;
                int lastButton = y + 5 + (rows - 1) * 2;
                if (personaSelected < (int)personaList.size() && previewY > lastButton + 1)
                    c.text(x + 3, previewY,
                           truncate(L"等待语：" +
                                        personaWaitLine(personaList[(size_t)personaSelected], 0),
                                    w - 6),
                           colors.muted, w - 6);
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
                                                  L"/mode 队列模式   /detail 歌曲详情与歌词",
                                                  L"/stats 听歌统计   /review 听歌锐评   /models 模型目录",
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
                    txt(6, L"输入 /ai，按 K 填密钥、按 M 选模型，或按 F 从端点发现模型。");
                    txt(8, L"密钥保存在当前 Windows 用户的加密文件中。");
                }
                c.button(x + 3, y + 10, w - 6,
                         guidePage == 2 ? L"Enter  完成并开始使用" : L"Enter  下一步",
                         A_GuideNext, true);
                txt(11, L"← 上一步  ·  S 或 Esc 跳过  ·  /guide 随时重看");
            } else if (modal == Modal::Ai) {
                auto config = loadAgentConfig();
                txt(2, L"音乐 Agent · 模型设置");
                txt(3, L"人设：" + persona.name + L" · " + persona.tagline);
                txt(4, L"思考强度：" + effortLabel(config.reasoningEffort));
                txt(5, L"模型：" + truncate(unutf8(config.model), w - 12));
                txt(6, config.contextTokens
                           ? L"上下文：" + std::to_wstring(config.contextTokens) + L" tokens" +
                                 (contextLimitTokens ? L"（本地设定同值）" : L"")
                           : L"上下文：端点未报告（按回退值）");
                txt(7, agentKeyPresent() ? L"密钥：本机已配置（DPAPI 保护）" : L"密钥：本机未配置");
                if (!config.modelModalities.empty()) {
                    std::wstring modes;
                    for (const auto& mode : config.modelModalities) {
                        if (!modes.empty())
                            modes += L" / ";
                        modes += unutf8(mode);
                    }
                    txt(8, L"模态（端点报告，不可手选）：" + truncate(modes, w - 30));
                }
                if (lastContextTokens)
                    txt(9, L"上次请求：" + std::to_wstring(lastPromptTokens) + L"/" +
                              std::to_wstring(lastContextTokens) + L" tokens");
                int half = (w - 8) / 2;
                c.button(x + 3, y + 10, half, L"K  设置模型密钥", A_AiKey);
                c.button(x + 5 + half, y + 10, half, L"M  修改模型 ID", A_AiModel);
                c.button(x + 3, y + 11, half, L"D  清除本机密钥", A_AiClearKey);
                c.button(x + 5 + half, y + 11, half, L"F  从端点发现模型", A_AiDiscover);
                c.button(x + 3, y + 12, w - 6, L"L  查看已缓存的模型目录", A_AiCached);
                if (h > 16)
                    txt(14, L"/chat 对话；模态只按端点报告展示，用户不能手选。");
            } else if (modal == Modal::AiModels) {
                txt(1, modelFromCache ? L"已缓存的模型目录" : L"端点发现的模型");
                txt(2, L"共 " + std::to_wstring(modelList.size()) +
                           L" 个 · ↑↓ 选择，Enter 采用（模态只报告）");
                int rows = std::max(1, h - 6);
                int from = std::clamp(modelSelected - rows + 1, 0,
                                      std::max(0, (int)modelList.size() - rows));
                for (int i = 0; i < rows; ++i) {
                    int index = from + i;
                    if (index >= (int)modelList.size())
                        break;
                    const auto& capability = modelList[(size_t)index];
                    std::wstring label = (index == modelSelected ? L"❯ " : L"  ") +
                                         unutf8(capability.id);
                    label += L"  ·  ctx " + (capability.contextTokens
                                                  ? std::to_wstring(capability.contextTokens)
                                                  : std::wstring(L"?"));
                    label += L" · out " + (capability.maxOutputTokens
                                               ? std::to_wstring(capability.maxOutputTokens)
                                               : std::wstring(L"?"));
                    if (!capability.inputModalities.empty()) {
                        label += L" · ";
                        for (size_t m = 0; m < capability.inputModalities.size(); ++m) {
                            if (m)
                                label += L"+";
                            label += unutf8(capability.inputModalities[m]);
                        }
                    }
                    if (capability.reasoning)
                        label += L" · 推理";
                    c.button(x + 3, y + 4 + i, w - 6, truncate(label, w - 10),
                             A_ModelRow + index, index == modelSelected);
                }
                txt(h - 2, L"Enter 采用 · F 重新发现 · M 手填 · Esc 返回");
            } else if (modal == Modal::Stats) {
                std::wstring periodName = statsPeriod == ListenPeriod::Day      ? L"日"
                                          : statsPeriod == ListenPeriod::Month  ? L"月"
                                                                                : L"年";
                txt(1, L"听歌统计 · " + periodName +
                           (statsReport.key.empty() ? L"" : L"（" + unutf8(statsReport.key) + L"）"));
                std::vector<std::wstring> lines;
                if (statsReport.plays == 0) {
                    lines.push_back(L"这个周期还没有听歌记录。");
                    lines.push_back(L"播放几首歌后，这里会显示次数、时长、Top 歌曲与歌手。");
                } else {
                    lines.push_back(L"播放 " + std::to_wstring(statsReport.plays) + L" 次 · 时长 " +
                                    durationText(statsReport.listenedSeconds) + L" · 完整 " +
                                    std::to_wstring(statsReport.completedPlays) + L" · 跳过 " +
                                    std::to_wstring(statsReport.skippedPlays));
                    lines.push_back(L"独立歌曲 " + std::to_wstring(statsReport.uniqueSongs) +
                                    L" · 独立歌手 " + std::to_wstring(statsReport.uniqueArtists));
                    lines.push_back(L"");
                    if (!statsReport.topSongs.empty()) {
                        lines.push_back(L"Top 歌曲");
                        size_t rank = 1;
                        for (const auto& row : statsReport.topSongs)
                            lines.push_back(L"  " + std::to_wstring(rank++) + L". " +
                                            unutf8(row.label) + L"  ·  " +
                                            std::to_wstring(row.plays) + L" 次");
                    }
                    if (!statsReport.topArtists.empty()) {
                        lines.push_back(L"");
                        lines.push_back(L"Top 歌手");
                        size_t rank = 1;
                        for (const auto& row : statsReport.topArtists)
                            lines.push_back(L"  " + std::to_wstring(rank++) + L". " +
                                            unutf8(row.label) + L"  ·  " +
                                            std::to_wstring(row.plays) + L" 次");
                    }
                    if (!statsReport.byHour.empty()) {
                        lines.push_back(L"");
                        lines.push_back(L"时段分布");
                        int64_t peak = 0;
                        for (const auto& hour : statsReport.byHour)
                            peak = std::max(peak, hour.plays);
                        std::wstring bars;
                        static const wchar_t* levels = L"▁▂▃▄▅▆▇█";
                        for (const auto& hour : statsReport.byHour) {
                            int level = peak > 0 ? (int)(hour.plays * 7 / peak) : 0;
                            bars += levels[std::clamp(level, 0, 7)];
                        }
                        lines.push_back(L"  0 时 " + bars + L" 23 时");
                    }
                }
                int rows = std::max(1, h - 7);
                statsScroll = std::clamp(statsScroll, 0,
                                         std::max(0, (int)lines.size() - rows));
                for (int i = 0; i < rows; ++i) {
                    int index = statsScroll + i;
                    if (index >= (int)lines.size())
                        break;
                    c.text(x + 3, y + 4 + i, truncate(lines[(size_t)index], w - 6),
                           index == 0 ? colors.text : colors.muted, w - 6);
                }
                int third = (w - 10) / 3;
                c.button(x + 3, y + h - 3, third, L"1 日", A_StatsDay,
                         statsPeriod == ListenPeriod::Day);
                c.button(x + 5 + third, y + h - 3, third, L"2 月", A_StatsMonth,
                         statsPeriod == ListenPeriod::Month);
                c.button(x + 7 + third * 2, y + h - 3, third, L"3 年", A_StatsYear,
                         statsPeriod == ListenPeriod::Year);
                txt(h - 2, L"↑↓ 滚动 · ← → 切换周期 · Esc 返回");
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
                txt(7, L"工具：搜索/集合/播放/控制/队列/推荐/随机/歌单/红心/收藏/状态");
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
            listH = std::max(2, height - 23);
            c->text(3, 1, L"M O R A", colors.accent);
            c->text(15, 1, L"云间  /  STEREO TERMINAL", colors.muted);
            std::wstring mem = std::to_wstring((int)memoryMB()) + L" MB";
            c->text(width - 26, 1, options.demo ? L"DEMO · 离线演示" : mem, colors.muted, 22);
            if (detail) {
                drawDetail(*c);
                drawComposer(*c);
                if (modal != Modal::None)
                    drawModal(*c);
                return c;
            }
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
            // Now-playing band, then the list title and the list itself. The band is always drawn,
            // so an empty queue still says what is playing (or that nothing is) instead of blank.
            drawRouteNowPlaying(*c);
            c->text(3, 6, truncate(title + (query.empty() ? L"" : L" / " + query), listW - 22),
                    colors.accent);
            c->text(std::max(20, listW - 14), 6,
                    std::to_wstring(rows.size()) + L" / " + std::to_wstring(total), colors.muted,
                    18);
            if (mode == 5) {
                std::wstring summary = L"队列 " + std::to_wstring(total) + L" 首";
                if (total > 0)
                    summary += L" · 当前 " + std::to_wstring((int)session.index() + 1) + L"/" +
                               std::to_wstring(total);
                summary += L" · " + modeLabel(session.mode());
                c->text(3, 7, truncate(summary, width - 20), colors.accent, width - 20);
            } else
                c->text(3, 7, L"Enter 播放 · D 歌曲详情与歌词 · [ ] 翻页", colors.muted, width - 20);
            c->button(width - 13, 7, 10, L"D 详情", A_Detail);
            for (int i = 0; i < listH && top + i < (int)rows.size(); ++i) {
                int index = top + i;
                auto& t = rows[index];
                int y = listY + i;
                bool sel = index == selected;
                c->rect(listX, y, listW, 1, sel ? colors.surface : colors.background);
                bool current = mode == 5 ? (int)session.index() == index
                                         : (session.playing().id && t.id == session.playing().id);
                c->text(4, y, current ? L"▸" : std::to_wstring(offset + index + 1),
                        sel || current ? colors.accent : colors.muted, 4);
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
                std::vector<std::wstring> empty;
                if (mode == 5)
                    empty = {L"队列里还没有歌曲", L"/search 搜一首，Enter 播放后会进入队列",
                             L"也可以 2 专辑 / 3 歌单 / 4 我的音乐"};
                else if (mode == 4)
                    empty = {L"我的音乐还没有载入", L"按 L 登录网易云账号后重试",
                             L"登录后这里会列出你创建和收藏的歌单"};
                else
                    empty = {L"还没有内容", L"/ 输入「歌名 歌手」搜索", L"1 单曲 · 2 专辑 · 3 歌单"};
                for (size_t i = 0; i < empty.size() && (int)i < listH; ++i)
                    c->text(7, listY + (int)i, truncate(empty[i], listW - 12),
                            i == 0 ? colors.text : colors.muted, listW - 12);
            }
            int controlY = height - 12;
            if (sidePanel) {
                int x = listW + 7, w = width - x - 4, artH = std::clamp(height - 28, 4, 12),
                    artW = std::min(artH * 2, w);
                drawCover(*c, x + (w - artW) / 2, 8, artW, artH);
                int y = 9 + artH;
                const Track& track = session.playing();
                c->text(x, y, track.id ? track.name : L"尚未播放", colors.text, w);
                c->text(x, y + 2, track.id && !track.artist.empty() ? track.artist : L"—",
                        colors.muted, w);
                c->text(x, y + 3, track.id && !track.album.empty() ? track.album : L"—",
                        colors.muted, w);
                std::wstring format =
                    state.sampleRate ? L" · " + std::to_wstring(state.sampleRate / 1000) + L"k/" +
                                           std::to_wstring(state.sampleBits) + L"bit"
                                     : L"";
                c->text(x, y + 4,
                        actualQualityLabel() + format + L" · " + modeLabel(session.mode()),
                        colors.accent, w);
                std::wstring lyric;
                if (track.id && currentLyrics.songId == track.id) {
                    if (currentLyrics.instrumental)
                        lyric = L"♪ 纯音乐，没有歌词";
                    else {
                        int index = lyricIndexAt(currentLyrics, state.position);
                        if (index >= 0 && index < (int)currentLyrics.lines.size())
                            lyric = L"♪ " + currentLyrics.lines[(size_t)index].text;
                    }
                }
                if (!lyric.empty() && y + 6 < controlY - 2)
                    c->text(x, y + 6, truncate(lyric, w), colors.accent, w);
            }
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
            c->button(66, controlY + 1, 9, L"C " + modeLabel(session.mode()), A_Mode);
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
                    (modal == Modal::SavedSession &&
                     (a == A_UseSavedSession || a == A_ForgetSavedSession)) ||
                    (modal == Modal::Login && (a == A_Qr || a == A_Phone || a == A_Logout)) ||
                    ((modal == Modal::Phone || modal == Modal::Code) && a == A_Send) ||
                    (modal == Modal::Save && a >= A_SaveRow && a < A_QualityRow) ||
                    (modal == Modal::Settings &&
                     (a == A_Name || a == A_Persona || a == A_Guide || a == A_ReactToggle ||
                      a == A_ReactSlower || a == A_ReactFaster || a == A_ReactCharsDown ||
                      a == A_ReactCharsUp || a == A_ReviewCharsDown || a == A_ReviewCharsUp ||
                      a == A_CompressToggle)) ||
                    (modal == Modal::Ai &&
                     (a == A_AiKey || a == A_AiModel || a == A_AiClearKey || a == A_AiDiscover ||
                      a == A_AiCached)) ||
                    (modal == Modal::AiModels && a >= A_ModelRow && a < A_ModelRow + 64) ||
                    (modal == Modal::Stats &&
                     (a == A_StatsDay || a == A_StatsMonth || a == A_StatsYear)) ||
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
                // The queue mode is authoritative; `repeat` is only the legacy mirror.
                session.setMode(settings.queueMode);
                reactEnabled = settings.reactEnabled;
                reactMinSeconds = settings.reactMinSeconds;
                reactMaxChars = settings.reactMaxChars;
                reviewChars = settings.reviewChars;
                autoCompress = settings.autoCompress;
                contextLimitTokens = settings.contextLimitTokens;
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
                // Conversation memory and the listen log live in the same user directory. The sink
                // is attached before any track can start, so no listen is lost.
                conversation = loadConversation(stateDirectory());
                listenHistory = std::make_unique<ListenHistory>(stateDirectory());
                session.attachListenSink(listenHistory.get());
                session.setSource("search");
            }
            applyPersonaColors();
            status = L"预览版 0.7.0 · /search 搜歌 · /login 登录 · /persona 人设 · /ai 模型接口";
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
                session.setSource("local");
                session.startTrack(local, "",
                                   std::filesystem::absolute(options.localAudio).wstring());
            }
        }
        ~App() {
            mainCalls.close();
            if (chatThread.joinable())
                chatThread.join();
            if (discoverThread.joinable())
                discoverThread.join();
            if (listenHistory)
                listenHistory->flush();
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
            bool interactiveRun = !options.demo && options.seconds <= 0 && options.snapshot.empty();
            bool savedSession = !options.demo && options.localAudio.empty() &&
                                std::filesystem::exists(stateDirectory() / L"session.bin");
            if (interactiveRun && savedSession && !options.playId)
                modal = Modal::SavedSession;
            else
                showWelcomeIfNeeded();
            if (options.playId) {
                home = false;
                searchFocus = false;
                session.assignId(options.playId);
                playCurrent();
            } else if (savedSession && !interactiveRun)
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
                settings.queueMode = session.mode();
                settings.reactEnabled = reactEnabled;
                settings.reactMinSeconds = reactMinSeconds;
                settings.reactMaxChars = reactMaxChars;
                settings.reviewChars = reviewChars;
                settings.autoCompress = autoCompress;
                settings.contextLimitTokens = contextLimitTokens;
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
