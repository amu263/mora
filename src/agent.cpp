#include "agent.hpp"
#include "platform.hpp"
#include "service.hpp"
#include <cctype>
#include <ctime>
#include <fstream>
#include <sstream>

namespace mora {
    namespace {
        std::filesystem::path agentFile(const char* name) {
            auto dir = stateDirectory();
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            return dir / name;
        }
        std::string readFile(const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return {};
            std::ostringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }
        Json readJson(const std::filesystem::path& path) {
            auto text = readFile(path);
            if (text.empty())
                return Json(nullptr);
            auto parsed = Json::parse(text, nullptr, false);
            return parsed.is_discarded() ? Json(nullptr) : parsed;
        }
        std::string env(const char* name) {
            char buffer[512] = {};
            auto size = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
            return size > 0 && size < sizeof(buffer) ? std::string(buffer, size) : std::string();
        }
        // The same ladder DST exposes: `off` means "send no reasoning field at all", which is why
        // it is valid but never written into the request body.
        bool validReasoningEffort(const std::string& value) {
            return value.empty() || value == "off" || value == "minimal" || value == "low" ||
                   value == "medium" || value == "high" || value == "xhigh" || value == "max";
        }
        // Truncating at a byte offset can split a UTF-8 sequence and make the whole JSON
        // undumpable later, so cut on a character boundary.
        std::string summarize(const std::string& text, size_t limit) {
            if (text.size() <= limit)
                return text;
            size_t end = limit;
            while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
                --end;
            return text.substr(0, end);
        }
        std::string trimmed(std::string value) {
            auto blank = [](char c) {
                return c == ' ' || c == '\t' || c == '\r' || c == '\n';
            };
            while (!value.empty() && blank(value.back()))
                value.pop_back();
            size_t begin = 0;
            while (begin < value.size() && blank(value[begin]))
                ++begin;
            return value.substr(begin);
        }
        std::string lowered(std::string value) {
            for (auto& c : value)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return value;
        }
        bool writeJson(const std::filesystem::path& path, const Json& data) {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
                return false;
            file << data.dump(2) << '\n';
            return static_cast<bool>(file);
        }
        std::string nowIso() {
            auto now = std::time(nullptr);
            std::tm utc{};
            gmtime_s(&utc, &now);
            char buffer[32] = {};
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
            return buffer;
        }
        bool truthy(const Json& object, const char* key) {
            if (!object.is_object() || !object.contains(key))
                return false;
            const auto& value = object[key];
            if (value.is_boolean())
                return value.get<bool>();
            if (value.is_object() || value.is_array())
                return !value.empty();
            if (value.is_number())
                return value.get<double>() != 0;
            return false;
        }
        // Providers spell the same capability several ways; take the first positive number.
        int firstPositive(const Json& object, std::initializer_list<const char*> keys) {
            if (!object.is_object())
                return 0;
            for (auto key : keys) {
                if (!object.contains(key))
                    continue;
                const auto& value = object[key];
                int64_t number = 0;
                if (value.is_number_integer() || value.is_number_unsigned())
                    number = value.get<int64_t>();
                else if (value.is_number_float())
                    number = static_cast<int64_t>(value.get<double>());
                else if (value.is_string()) {
                    try {
                        number = std::stoll(value.get<std::string>());
                    } catch (const std::exception&) {
                        continue;
                    }
                } else
                    continue;
                if (number > 0)
                    return static_cast<int>(std::min<int64_t>(number, 0x7FFFFFFF));
            }
            return 0;
        }
        std::vector<std::string> normalizedList(std::vector<std::string> items) {
            std::vector<std::string> result;
            for (auto& item : items) {
                auto value = lowered(trimmed(item));
                if (value.empty() ||
                    std::find(result.begin(), result.end(), value) != result.end())
                    continue;
                result.push_back(std::move(value));
            }
            return result;
        }
        std::vector<std::string> stringList(const Json& value) {
            std::vector<std::string> items;
            if (value.is_string())
                items.push_back(value.get<std::string>());
            else if (value.is_array()) {
                for (auto& item : value)
                    if (item.is_string())
                        items.push_back(item.get<std::string>());
            }
            return normalizedList(std::move(items));
        }
        // "text", "text+image" and "text->text" all appear in OpenAI-compatible model lists.
        std::vector<std::string> splitModalities(const std::string& text) {
            std::vector<std::string> items;
            std::string current;
            for (char c : text) {
                if (c == '+' || c == ',' || c == '/' || c == '|') {
                    items.push_back(current);
                    current.clear();
                } else
                    current += c;
            }
            items.push_back(current);
            return normalizedList(std::move(items));
        }
        // The sibling /models route of the configured chat endpoint: same scheme, host and port,
        // so discovery never contacts a second service.
        std::string modelsEndpoint(std::string url) {
            if (auto cut = url.find('#'); cut != std::string::npos)
                url.resize(cut);
            if (auto cut = url.find('?'); cut != std::string::npos)
                url.resize(cut);
            while (!url.empty() && url.back() == '/')
                url.pop_back();
            for (auto suffix : {std::string("/chat/completions"), std::string("/completions")}) {
                if (url.size() >= suffix.size() &&
                    url.compare(url.size() - suffix.size(), suffix.size(), suffix) == 0) {
                    url.resize(url.size() - suffix.size());
                    break;
                }
            }
            return url + "/models";
        }
        const char* roleLabel(const std::string& role) {
            if (role == "user")
                return "用户";
            if (role == "assistant")
                return "助手";
            if (role == "system")
                return "摘要";
            return "工具";
        }
        // A summary that never needs the network: the oldest turns survive in a bounded, clearly
        // labelled form, so a failed compression still preserves the conversation.
        std::string deterministicSummary(const std::vector<AgentMessage>& older, const std::string& reason,
                                         size_t limit) {
            std::string body;
            for (auto& message : older) {
                body += std::string(roleLabel(message.role)) + ": " + message.content + "\n";
                if (body.size() > limit * 2)
                    break;
            }
            return summarize(reason + "\n" + body, limit);
        }
        // estimateMessageTokens is a frozen, string-typed helper: a provider tool turn may carry
        // null or structured content, so the accounting only runs when every string field is one.
        bool tokenEstimateSafe(const Json& messages) {
            for (auto& message : messages) {
                if (!message.is_object())
                    return false;
                for (auto key : {"role", "content", "name", "tool_call_id"})
                    if (message.contains(key) && !message[key].is_string())
                        return false;
            }
            return true;
        }
        const char* kCompressionInstruction =
            "你是 MORA 音乐终端的对话压缩器。把用户给出的早期对话压缩成一段不超过 1200 字符的"
            "中文摘要。必须保留：用户偏好（常听的歌手、风格、口味）、已经确认的事实（歌名、"
            "歌手、歌曲或歌单 id、播放与喜欢的结果）、未完成的请求与待办、以及用户明确否定的"
            "选项。不要编造新事实，不要添加寒暄、标题或解释，只输出摘要正文。";
        std::wstring systemDefault() {
            return L"你是 MORA 音乐终端的音乐助手。用中文简短回答（不超过 120 字）。"
                   L"需要事实、候选或播放状态时先调用工具，不要凭记忆编造歌名、歌手或播放结果。"
                   L"工具返回 needs_player 时告诉用户先运行 mora 终端；写操作只在用户明确给出"
                   L"目标时调用。\n"
                   L"操作常识：\n"
                   L"1) 模糊也要行动：能从上下文或常见口味推断就直接搜索/推荐并播放；只有真正"
                   L"无法判断才问，且最多问一次；播放前不要反复确认。\n"
                   L"2) 意图→操作：单曲→music_search 后 music_play(kind=song,id)；专辑或歌单→"
                   L"music_play(kind=album|playlist,id)，替换队列并播放整个集合（"
                   L"music_open_collection 只列曲目、不播放）；歌手/风格/场景/心情（无明确专辑）→"
                   L"music_recommend_queue(intent=取向,count=首数) 一次建队列并开始播放；"
                   L"“从我的歌单/收藏挑 N 首”→music_recommend_queue(source=playlists,"
                   L"count=N)（intent 可留空）；“我喜欢的歌/红心”→music_recommend_queue("
                   L"source=liked,count=N)，只想看看有哪些就 music_list_likes；也可以先用 "
                   L"music_open_collection 或 music_list_likes 读出 id，再一次 "
                   L"music_queue_edit(op=replace, ids=[…]) 入队，禁止逐首播放；不确定或随机→"
                   L"music_recommend 后 music_play_random；换音质→music_control(action=quality, "
                   L"level=standard|higher|exhigh|lossless|hires)。\n"
                   L"3) 替换 vs 追加：“换成/只听/播放这张专辑”=替换；“再加上/再来点/接着放/排在"
                   L"后面”=append 或 add_next（插到当前曲之后），不得替换当前队列。\n"
                   L"4) 系列请求取回 5..20 首（有界不超过 50）；工具返回 accepted 后必须用 "
                   L"music_now_playing 确认真实状态，不得编造播放中。";
        }
    } // namespace

    // Stable per-install conversation id (not a secret); kept next to the non-secret config.
    std::string agentSessionId() {
        auto stored = readJson(agentFile("agent.json"));
        if (stored.is_object() && stored.contains("sessionId") &&
            stored["sessionId"].is_string())
            return stored["sessionId"].get<std::string>();
        auto random = randomBytes(12);
        auto id = "mora-" + hex(random);
        if (!stored.is_object())
            stored = Json::object();
        stored["sessionId"] = id;
        std::ofstream file(agentFile("agent.json"), std::ios::binary | std::ios::trunc);
        file << stored.dump(2) << '\n';
        return id;
    }

    AgentConfig loadAgentConfig() {
        AgentConfig config;
        auto stored = readJson(agentFile("agent.json"));
        if (stored.is_object()) {
            config.baseUrl = stored.value("baseUrl", config.baseUrl);
            config.model = stored.value("model", config.model);
            if (auto value = stored.value("reasoningEffort", std::string());
                validReasoningEffort(value))
                config.reasoningEffort = value;
            config.maxOutputTokens = stored.value("maxOutputTokens", config.maxOutputTokens);
            config.maxRounds = stored.value("maxRounds", config.maxRounds);
            config.contextTokens = stored.value("contextTokens", config.contextTokens);
            config.contextFallbackTokens =
                stored.value("contextFallbackTokens", config.contextFallbackTokens);
        }
        if (auto value = env("MORA_AGENT_BASE_URL"); !value.empty())
            config.baseUrl = value;
        if (auto value = env("MORA_AGENT_MODEL"); !value.empty())
            config.model = value;
        if (auto value = env("MORA_AGENT_REASONING_EFFORT"); !value.empty()) {
            if (!validReasoningEffort(value))
                throw AgentError("invalid_reasoning_effort", "unsupported reasoning effort");
            config.reasoningEffort = value;
        }
        // The discovered catalog is the source of truth for the window and modalities of the
        // selected model: the user never enters either by hand, and a model without a cached entry
        // keeps the conservative fallback budget.
        config.modelModalities.clear();
        if (stored.is_object() && stored.contains("modelCatalog") &&
            stored["modelCatalog"].is_object()) {
            Json models = stored["modelCatalog"].value("models", Json::array());
            for (auto& item : models) {
                if (!item.is_object() || item.value("id", std::string()) != config.model)
                    continue;
                if (auto window = firstPositive(item, {"contextTokens"}); window > 0)
                    config.contextTokens = window;
                config.modelModalities = stringList(item.value("inputModalities", Json()));
                for (auto& modality : stringList(item.value("outputModalities", Json())))
                    if (std::find(config.modelModalities.begin(), config.modelModalities.end(),
                                  modality) == config.modelModalities.end())
                        config.modelModalities.push_back(modality);
                break;
            }
        }
        if (auto value = env("MORA_AGENT_CONTEXT_TOKENS"); !value.empty()) {
            int parsed = 0;
            try {
                parsed = std::stoi(value);
            } catch (const std::exception&) {
                parsed = 0;
            }
            if (parsed <= 0)
                throw AgentError("invalid_context_tokens",
                                 "MORA_AGENT_CONTEXT_TOKENS must be a positive integer");
            config.contextTokens = parsed;
        }
        return config;
    }

    void saveAgentConfig(const AgentConfig& config) {
        if (!validReasoningEffort(config.reasoningEffort))
            throw AgentError("invalid_reasoning_effort", "unsupported reasoning effort");
        auto sessionId = agentSessionId();
        // Merge instead of replacing: the discovered catalog and the session id must survive a
        // settings change, and the model key was never part of this file.
        Json stored = readJson(agentFile("agent.json"));
        if (!stored.is_object())
            stored = Json::object();
        stored["baseUrl"] = config.baseUrl;
        stored["sessionId"] = sessionId;
        stored["model"] = config.model;
        stored["reasoningEffort"] = config.reasoningEffort;
        stored["maxOutputTokens"] = config.maxOutputTokens;
        stored["maxRounds"] = config.maxRounds;
        stored["contextTokens"] = config.contextTokens;
        stored["contextFallbackTokens"] = config.contextFallbackTokens;
        writeJson(agentFile("agent.json"), stored);
    }

    bool agentKeyPresent() {
        return !readFile(agentFile("agent.bin")).empty();
    }

    void saveAgentKey(const std::string& key) {
        if (key.empty())
            throw AgentError("invalid_key", "refusing to store an empty model key");
        auto encrypted = protect(bytes(key));
        std::ofstream file(agentFile("agent.bin"), std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(encrypted.data()),
                   static_cast<std::streamsize>(encrypted.size()));
    }

    void clearAgentKey() {
        std::filesystem::remove(agentFile("agent.bin"));
    }

    std::string loadAgentKey() {
        auto encrypted = readFile(agentFile("agent.bin"));
        if (encrypted.empty())
            return {};
        auto clear = unprotect(bytes(encrypted));
        return std::string(clear.begin(), clear.end());
    }

    ModelCatalog fetchModelCatalog(const AgentConfig& config) {
        ModelCatalog catalog;
        catalog.endpoint = modelsEndpoint(config.baseUrl);
        if (config.baseUrl.empty()) {
            catalog.code = "bad_response";
            catalog.error = "no model endpoint configured";
            return catalog;
        }
        auto key = loadAgentKey();
        if (key.empty()) {
            catalog.code = "missing_key";
            catalog.error = "no model key stored; run mora --set-agent-key";
            return catalog;
        }
        Response response;
        try {
            Http http;
            response = http.request(unutf8(catalog.endpoint), {},
                                    L"User-Agent: mora-player/0.7.0\r\nAuthorization: Bearer " +
                                        unutf8(key) + L"\r\n",
                                    256 * 1024, false);
        } catch (const std::exception& error) {
            catalog.code = "network";
            catalog.error = error.what();
            return catalog;
        }
        if (response.status == 401 || response.status == 403) {
            catalog.code = "unauthorized";
            catalog.error = "provider refused the key (HTTP " + std::to_string(response.status) +
                            "); the /models route needs the same credential as chat";
            return catalog;
        }
        if (response.status == 404 || response.status == 405 || response.status == 501) {
            catalog.code = "not_supported";
            catalog.error = "endpoint does not expose a model list (HTTP " +
                            std::to_string(response.status) + ")";
            return catalog;
        }
        if (response.status == 429) {
            catalog.code = "rate_limited";
            catalog.error = "provider rate limit reached";
            return catalog;
        }
        if (response.status >= 500) {
            catalog.code = "server_error";
            catalog.error = "provider error HTTP " + std::to_string(response.status);
            return catalog;
        }
        if (response.status != 200) {
            catalog.code = "bad_response";
            catalog.error = "provider returned HTTP " + std::to_string(response.status);
            return catalog;
        }
        auto parsed = Json::parse(response.body, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("data") ||
            !parsed["data"].is_array()) {
            catalog.code = "bad_response";
            catalog.error = "endpoint did not return an OpenAI-compatible data[] model list";
            return catalog;
        }
        for (auto& item : parsed["data"]) {
            if (!item.is_object())
                continue;
            ModelCapability model;
            model.id = trimmed(item.value("id", std::string()));
            if (model.id.empty())
                continue;
            Json provider = item.value("top_provider", Json::object());
            model.contextTokens = firstPositive(
                item, {"context_length", "context_window", "max_context_length", "max_model_len"});
            if (model.contextTokens == 0)
                model.contextTokens =
                    firstPositive(provider, {"context_length", "context_window"});
            model.maxOutputTokens =
                firstPositive(item, {"max_output_tokens", "max_completion_tokens"});
            if (model.maxOutputTokens == 0)
                model.maxOutputTokens =
                    firstPositive(provider, {"max_completion_tokens", "max_output_tokens"});
            Json architecture = item.value("architecture", Json::object());
            model.inputModalities = stringList(architecture.value("input_modalities", Json()));
            if (model.inputModalities.empty())
                model.inputModalities = stringList(item.value("input_modalities", Json()));
            model.outputModalities = stringList(architecture.value("output_modalities", Json()));
            if (model.outputModalities.empty())
                model.outputModalities = stringList(item.value("output_modalities", Json()));
            auto modality = architecture.value("modality", item.value("modality", std::string()));
            if (!modality.empty()) {
                auto arrow = modality.find("->");
                if (model.inputModalities.empty())
                    model.inputModalities = splitModalities(
                        arrow == std::string::npos ? modality : modality.substr(0, arrow));
                if (model.outputModalities.empty())
                    model.outputModalities = splitModalities(
                        arrow == std::string::npos ? std::string() : modality.substr(arrow + 2));
            }
            model.reasoning = truthy(item, "reasoning") || truthy(architecture, "reasoning") ||
                              truthy(item.value("capabilities", Json::object()), "reasoning");
            for (auto& parameter : stringList(item.value("supported_parameters", Json())))
                if (parameter == "reasoning" || parameter == "include_reasoning" ||
                    parameter == "reasoning_effort" || parameter == "thinking")
                    model.reasoning = true;
            catalog.models.push_back(std::move(model));
        }
        // A valid but empty list is still a successful discovery: MORA never invents a model.
        catalog.ok = true;
        return catalog;
    }

    void cacheModelCatalog(const ModelCatalog& catalog) {
        // A failed discovery must not poison the offline view; the last good catalog stays.
        if (!catalog.ok)
            return;
        Json stored = readJson(agentFile("agent.json"));
        if (!stored.is_object())
            stored = Json::object();
        Json models = Json::array();
        for (auto& model : catalog.models)
            models.push_back(Json{{"id", model.id},
                                  {"inputModalities", model.inputModalities},
                                  {"outputModalities", model.outputModalities},
                                  {"contextTokens", model.contextTokens},
                                  {"maxOutputTokens", model.maxOutputTokens},
                                  {"reasoning", model.reasoning}});
        stored["modelCatalog"] = Json{{"ok", catalog.ok},
                                      {"code", catalog.code},
                                      {"endpoint", catalog.endpoint},
                                      {"fetchedAt", nowIso()},
                                      {"models", models}};
        writeJson(agentFile("agent.json"), stored);
    }

    ModelCatalog cachedModelCatalog() {
        ModelCatalog catalog;
        auto stored = readJson(agentFile("agent.json"));
        if (!stored.is_object() || !stored.contains("modelCatalog") ||
            !stored["modelCatalog"].is_object()) {
            catalog.code = "not_cached";
            catalog.error = "no model catalog cached yet; run mora --models";
            return catalog;
        }
        auto cached = stored["modelCatalog"];
        catalog.ok = cached.value("ok", false);
        catalog.code = cached.value("code", std::string());
        catalog.error = cached.value("error", std::string());
        catalog.endpoint = cached.value("endpoint", std::string());
        Json models = cached.value("models", Json::array());
        for (auto& item : models) {
            if (!item.is_object())
                continue;
            ModelCapability model;
            model.id = item.value("id", std::string());
            if (model.id.empty())
                continue;
            model.inputModalities = stringList(item.value("inputModalities", Json()));
            model.outputModalities = stringList(item.value("outputModalities", Json()));
            model.contextTokens = firstPositive(item, {"contextTokens"});
            model.maxOutputTokens = firstPositive(item, {"maxOutputTokens"});
            model.reasoning = truthy(item, "reasoning");
            catalog.models.push_back(std::move(model));
        }
        return catalog;
    }

    Json agentToolsFrom(const Json& musicTools) {
        Json tools = Json::array();
        for (auto& tool : musicTools) {
            if (!tool.is_object() || !tool.contains("name"))
                continue;
            Json parameters = tool.value("inputSchema", Json{{"type", "object"}});
            tools.push_back(Json{{"type", "function"},
                                 {"function",
                                  Json{{"name", tool.value("name", std::string())},
                                       {"description", tool.value("description", std::string())},
                                       {"parameters", parameters}}}});
        }
        return tools;
    }

    AgentReply parseAssistant(const Json& response) {
        AgentReply reply;
        if (!response.is_object())
            throw AgentError("bad_response", "model response is not an object");
        if (response.contains("error")) {
            auto error = response["error"];
            throw AgentError("provider_error",
                             error.is_object() ? error.value("message", error.dump())
                                               : error.dump());
        }
        auto choices = response.value("choices", Json::array());
        if (choices.empty() || !choices[0].is_object())
            throw AgentError("bad_response", "model response has no choices");
        auto message = choices[0].value("message", Json::object());
        if (message.contains("content") && message["content"].is_string())
            reply.text = unutf8(message["content"].get<std::string>());
        for (auto& call : message.value("tool_calls", Json::array())) {
            if (!call.is_object())
                continue;
            auto function = call.value("function", Json::object());
            ToolCall parsed;
            parsed.id = call.value("id", std::string());
            parsed.name = function.value("name", std::string());
            auto raw = function.value("arguments", std::string("{}"));
            parsed.arguments = Json::parse(raw, nullptr, false);
            if (parsed.arguments.is_discarded() || !parsed.arguments.is_object())
                parsed.arguments = Json::object();
            reply.calls.push_back(std::move(parsed));
        }
        return reply;
    }

    Json transportChat(const AgentConfig& config, const Json& messages, const Json& tools) {
        if (!validReasoningEffort(config.reasoningEffort))
            throw AgentError("invalid_reasoning_effort", "unsupported reasoning effort");
        auto key = loadAgentKey();
        if (key.empty())
            throw AgentError("missing_key", "no model key stored; run mora --set-agent-key");
        // The Go documentation asks each client to identify itself with its own user agent and to
        // send a stable session id so routing and prompt caching can be optimised.
        Json body = {{"model", config.model},
                     {"messages", messages},
                     {"max_tokens", config.maxOutputTokens}};
        if (!config.reasoningEffort.empty() && config.reasoningEffort != "off")
            body["reasoning_effort"] = config.reasoningEffort;
        if (tools.is_array() && !tools.empty()) {
            body["tools"] = tools;
            body["tool_choice"] = "auto";
        }
        auto sessionId = agentSessionId();
        Http http;
        auto response = http.request(
            unutf8(config.baseUrl), body.dump(),
            L"Content-Type: application/json\r\nUser-Agent: mora-player/0.7.0\r\n"
            L"x-opencode-session: " +
                unutf8(sessionId) + L"\r\nAuthorization: Bearer " + unutf8(key) + L"\r\n",
            4 * 1024 * 1024, true);
        if (response.status == 402)
            throw AgentError("payment_required",
                             "provider reported no usable quota: " + response.body.substr(0, 200),
                             response.status);
        if (response.status == 401 || response.status == 403)
            throw AgentError("unauthorized",
                             "provider refused the key (HTTP " + std::to_string(response.status) +
                                 "). Paid or Go-subscription models are required outside the "
                                 "provider's own client; free-tier chat models return 403.",
                             response.status);
        if (response.status == 429)
            throw AgentError("rate_limited", "provider rate limit reached", response.status);
        if (response.status >= 500)
            throw AgentError("server_error", "provider error HTTP " + std::to_string(response.status),
                             response.status);
        if (response.status != 200)
            throw AgentError("http_error",
                             "provider returned HTTP " + std::to_string(response.status) + ": " +
                                 response.body.substr(0, 200),
                             response.status);
        auto parsed = Json::parse(response.body, nullptr, false);
        if (parsed.is_discarded())
            throw AgentError("bad_response", "model response is not valid JSON");
        return parsed;
    }

    size_t estimateTokens(std::string_view text) {
        size_t tokens = 0, ascii = 0;
        for (unsigned char c : text) {
            if (c < 0x80) {
                ++ascii;
                continue;
            }
            // One non-ASCII lead byte stands for one code point; CJK costs about one token each,
            // which over-counts Latin scripts and keeps the budget conservative.
            if ((c & 0xC0) != 0x80)
                ++tokens;
        }
        return tokens + (ascii + 3) / 4;
    }
    size_t estimateMessageTokens(const Json& messages) {
        // A provider tool turn legitimately carries "content": null, and a null name or
        // tool_call_id is normal too, so read every field defensively instead of letting value<>
        // throw a type error inside the budget arithmetic.
        auto textOf = [](const Json& object, const char* key) -> std::string {
            if (!object.contains(key))
                return {};
            const Json& value = object[key];
            return value.is_string() ? value.get<std::string>() : std::string();
        };
        size_t total = 0;
        for (auto& message : messages) {
            if (!message.is_object())
                continue;
            total += 4; // per-message envelope overhead
            total += estimateTokens(textOf(message, "role"));
            total += estimateTokens(textOf(message, "content"));
            total += estimateTokens(textOf(message, "name"));
            if (message.contains("tool_calls") && !message["tool_calls"].is_null())
                total += estimateTokens(message["tool_calls"].dump());
        }
        return total;
    }

    CompressionPlan planCompression(const AgentConfig& config, const std::wstring& systemPrompt,
                                    const std::vector<AgentMessage>& history,
                                    const std::string& userText) {
        CompressionPlan plan;
        plan.limitTokens = config.contextTokens > 0
                               ? static_cast<size_t>(config.contextTokens)
                               : static_cast<size_t>(config.contextFallbackTokens > 0
                                                         ? config.contextFallbackTokens
                                                         : 32768);
        auto ratio = config.compressAt > 0 && config.compressAt <= 1 ? config.compressAt : .72;
        auto threshold = static_cast<size_t>(static_cast<double>(plan.limitTokens) * ratio);
        Json system = Json{{"role", "system"},
                           {"content", utf8(systemPrompt.empty() ? systemDefault() : systemPrompt)}};
        auto build = [&](const std::vector<AgentMessage>& kept, const std::string& summary) {
            Json messages = Json::array();
            messages.push_back(system);
            if (!summary.empty())
                messages.push_back(Json{{"role", "system"}, {"content", summary}});
            for (auto& turn : kept)
                messages.push_back(Json{{"role", turn.role},
                                        {"content", turn.content},
                                        {"tool_call_id", turn.toolCallId},
                                        {"name", turn.name}});
            messages.push_back(Json{{"role", "user"}, {"content", userText}});
            return messages;
        };
        plan.messages = build(history, {});
        plan.promptTokens = estimateMessageTokens(plan.messages);
        plan.kept = history;
        if (!config.autoCompress || plan.promptTokens <= threshold)
            return plan;
        // A leading system message is the previous checkpoint, not a turn: it is folded too, so a
        // second compression merges into one summary instead of stacking checkpoints.
        size_t firstTurn = 0;
        while (firstTurn < history.size() && history[firstTurn].role == "system")
            ++firstTurn;
        std::vector<AgentMessage> older(history.begin(), history.begin() + firstTurn);
        std::vector<AgentMessage> turns(history.begin() + firstTurn, history.end());
        if (turns.size() < 2)
            return plan; // nothing worth folding; the caller sees an honest prompt size
        auto retain = std::min(config.keepRecentMessages ? config.keepRecentMessages : size_t(1),
                               turns.size());
        retain = std::max<size_t>(retain, 1);
        auto keepFrom = turns.size() - retain;
        older.insert(older.end(), turns.begin(), turns.begin() + keepFrom);
        std::vector<AgentMessage> kept(turns.begin() + keepFrom, turns.end());
        // Never open the kept block with a reply whose question was folded away.
        while (kept.size() > 1 && kept.front().role == "assistant") {
            older.push_back(kept.front());
            kept.erase(kept.begin());
        }
        auto placeholder = [&]() {
            return std::string("[早前对话摘要]\n") +
                   deterministicSummary(older, "[确定性截断摘要：未经模型摘要]", 1200);
        };
        Json messages = build(kept, placeholder());
        // keepRecentMessages is the retention target. Only when the retained turns alone still
        // exceed the window does the plan fold more of them (oldest first) so a request is never
        // knowingly oversized; the summary and the reported folded count stay honest either way.
        while (estimateMessageTokens(messages) > threshold && kept.size() > 1) {
            older.push_back(kept.front());
            kept.erase(kept.begin());
            if (kept.size() > 1 && kept.front().role == "assistant") {
                older.push_back(kept.front());
                kept.erase(kept.begin());
            }
            messages = build(kept, placeholder());
        }
        plan.messages = messages;
        plan.promptTokens = estimateMessageTokens(messages);
        plan.needed = true;
        plan.foldedTurns = older.size();
        plan.older = older;
        plan.kept = kept;
        return plan;
    }

    namespace {
        struct SummaryResult {
            std::string text, code; // empty code means the model produced the summary
        };
        // One bounded, tool-free summary call. A failure degrades to the labelled deterministic
        // truncation, so the folded turns survive and the caller can report the degradation.
        SummaryResult summarizeBounded(const AgentConfig& config, const ModelTransport& transport,
                                       const std::vector<AgentMessage>& older) {
            SummaryResult result;
            if (older.empty())
                return result;
            std::string transcript;
            for (auto& message : older) {
                transcript += std::string(roleLabel(message.role)) + ": " + message.content + "\n";
                if (transcript.size() >= 8000)
                    break;
            }
            transcript = summarize(transcript, 8000);
            result.code = "empty_summary";
            try {
                const auto& send = transport ? transport
                                             : ModelTransport([](const AgentConfig& settings,
                                                                 const Json& messages,
                                                                 const Json& tools) {
                                                   return transportChat(settings, messages, tools);
                                               });
                Json messages = Json::array();
                messages.push_back(Json{{"role", "system"}, {"content", kCompressionInstruction}});
                messages.push_back(
                    Json{{"role", "user"}, {"content", "早期对话记录：\n" + transcript}});
                // One bounded call with no tools: a summary can never fan out into a tool loop.
                auto reply = parseAssistant(send(config, messages, Json::array()));
                auto text = trimmed(utf8(reply.text));
                if (!text.empty()) {
                    result.text = summarize(text, 1200);
                    result.code.clear();
                    return result;
                }
            } catch (const AgentError& error) {
                result.code = error.code.empty() ? std::string("provider_error") : error.code;
            } catch (const std::exception&) {
                result.code = "network";
            }
            result.text = deterministicSummary(
                older, "[压缩降级: " + result.code + "] 早期对话确定性截断（可能缺失细节）：", 1200);
            return result;
        }
    } // namespace

    std::string summarizeHistory(const AgentConfig& config, const ModelTransport& transport,
                                 const std::vector<AgentMessage>& older) {
        return summarizeBounded(config, transport, older).text;
    }

    ChatOutcome runChatLoop(AgentConfig config, const std::string& userText,
                            const MusicToolExecutor& execute, const ModelTransport& transport,
                            const std::wstring& systemPrompt,
                            const std::vector<AgentMessage>& history) {
        ChatOutcome outcome;
        auto toolMarkup = [](const std::wstring& value) {
            return value.find(L"DSML") != std::wstring::npos &&
                   (value.find(L"invoke") != std::wstring::npos ||
                    value.find(L"calls") != std::wstring::npos);
        };
        std::wstring completedAction;
        std::wstring requiredCountInAnswer;
        if (userText.empty()) {
            outcome.code = "invalid_arguments";
            outcome.error = "empty chat request";
            return outcome;
        }
        if (!agentKeyPresent()) {
            outcome.code = "missing_key";
            outcome.error = "no model key stored; run mora --set-agent-key";
            return outcome;
        }
        if (!execute) {
            outcome.code = "invalid_arguments";
            outcome.error = "no tool executor";
            return outcome;
        }
        auto request = unutf8(userText);
        auto containsAny = [&](std::initializer_list<std::wstring_view> words) {
            for (auto word : words)
                if (request.find(word) != std::wstring::npos)
                    return true;
            return false;
        };
        bool currentSongReference = containsAny({L"正在播放", L"现在播放", L"当前播放",
                                                 L"播放的歌", L"播放的是", L"播放什么"});
        bool explicitCommand = containsAny({L"请播放", L"帮我播放", L"给我播放",
                                            L"随机播放", L"换成", L"换一首", L"换歌"});
        // Asking what is playing is a question about the player, not an instruction to change it.
        bool readOnlyQuestion = currentSongReference && !explicitCommand;
        // A listening, recommendation or queue verb means the user wants music. "推荐三十首歌我听听"
        // carries no 播放/听歌 wording, and treating it as small talk is what left the model without
        // any queue tool at all.
        bool musicVerb = containsAny({L"播放", L"放歌", L"放一", L"放几", L"听听", L"试听",
                                      L"听歌", L"听一", L"听一下", L"推荐", L"来点", L"来首",
                                      L"来几首", L"整点", L"换", L"随机"});
        // An opinion about the track that is already playing stays small talk: the prompt itself
        // carries the live track and the current line, so the model needs no tool to comment. A
        // greeting only stays small talk while nothing in it asks for music either, so a mixed
        // request ("推荐三十首并锐评" or "谢谢，顺便放首歌") is still an operation.
        bool opinion = containsAny({L"聊聊", L"看法", L"觉得", L"评价", L"感想", L"怎么想"});
        bool aboutCurrent = currentSongReference || containsAny({L"这首歌", L"这首", L"这歌"});
        bool discussion = !explicitCommand &&
                          ((opinion && aboutCurrent) ||
                           (containsAny({L"你好", L"谢谢"}) && !musicVerb));
        bool hasCount = request.find(L"首") != std::wstring::npos &&
            request.find_first_of(L"0123456789一二三四五六七八九十百千万两几") !=
                std::wstring::npos;
        bool multiSongPlayback = !discussion && !readOnlyQuestion && musicVerb && hasCount &&
            !containsAny({L"一首", L"1首"}) &&
            !containsAny({L"追加", L"加到", L"排在后面", L"接着放"});
        bool writeRequest = !discussion && !readOnlyQuestion &&
            (musicVerb || containsAny({L"暂停", L"继续播放", L"下一首", L"上一首",
                                       L"跳过", L"音量", L"收藏", L"加入", L"追加",
                                       L"添加", L"创建人设", L"设定人设", L"删除"}));
        Json definitions = musicToolDefinitions();
        Json permitted = Json::array();
        std::set<std::string> readOnlyTools;
        // Single-shot read-only tools stay available next to the queue tool, so a mixed request can
        // still look at the live player, but search/list are held back: one "verify every song"
        // loop would exhaust the tool budget that the one-call queue tool exists to protect.
        const std::set<std::string> multiSongReadOnly{
            "music_get_status", "music_now_playing", "music_queue_list", "music_get_lyrics",
            "music_recommend", "music_list_likes"};
        for (const auto& definition : definitions) {
            auto name = definition.value("name", std::string());
            bool readOnly = definition.value("annotations", Json::object())
                                .value("readOnlyHint", false);
            if (readOnly)
                readOnlyTools.insert(name);
            if (discussion)
                continue;
            if (multiSongPlayback) {
                // One batch queue tool, or one queue edit built from ids the model already read -
                // that is how "从我的歌单挑 30 首" works after listing a playlist. Only the one-song
                // tools stay blocked, so a series request can never become a per-song tool loop.
                if (name != "music_recommend_queue" && name != "music_queue_edit" &&
                    !(readOnly && multiSongReadOnly.contains(name)))
                    continue;
            } else if (!writeRequest && !readOnly)
                continue;
            permitted.push_back(definition);
        }
        Json tools = agentToolsFrom(permitted);
        // Persona creation is internal to the terminal player, so it is offered to the built-in
        // model only and never advertised through the external MCP tool list.
        Json fields;
        fields["id"] = Json{{"type", "string"},
                            {"description", "1-32 letters, digits, - or _; stable key"}};
        fields["name"] = Json{{"type", "string"}, {"description", "display name, max 24 chars"}};
        fields["tagline"] = Json{{"type", "string"}, {"description", "one short line, max 60 chars"}};
        fields["voice"] = Json{{"type", "string"},
                               {"description", "how this persona speaks; max 220 chars"}};
        fields["taste"] = Json{{"type", "string"},
                               {"description", "what music it proposes; max 220 chars"}};
        fields["greeting"] = Json{{"type", "string"}, {"description", "one short greeting line"}};
        Json parameters;
        parameters["type"] = "object";
        parameters["properties"] = fields;
        parameters["required"] = Json::array({"id", "name", "voice", "taste"});
        Json function;
        function["name"] = "mora_create_persona";
        function["description"] =
            "Create a custom MORA music-agent persona the user asked for. Only call this after the "
            "user has told you the name, how it should sound and what music it likes. The new "
            "persona becomes selectable and is adopted immediately.";
        function["parameters"] = parameters;
        Json personaTool;
        personaTool["type"] = "function";
        personaTool["function"] = function;
        if (writeRequest && !multiSongPlayback)
            tools.push_back(personaTool);
        std::set<std::string> allowedTools;
        for (const auto& tool : tools)
            allowedTools.insert(tool["function"]["name"].get<std::string>());
        const auto& send = transport ? transport
                                     : ModelTransport([](const AgentConfig& cfg, const Json& m,
                                                         const Json& t) {
                                           return transportChat(cfg, m, t);
                                       });
        // Conversation memory. The oldest turns are dropped first so the newest context wins when
        // the caller hands over more history than the message budget allows; a leading compaction
        // checkpoint stays, because it is the only trace of the turns it replaced.
        std::vector<AgentMessage> memory = history;
        memory.erase(std::remove_if(memory.begin(), memory.end(), [&](const AgentMessage& item) {
                         return item.role == "assistant" && toolMarkup(unutf8(item.content));
                     }), memory.end());
        size_t checkpoint = 0;
        while (checkpoint < memory.size() && memory[checkpoint].role == "system")
            ++checkpoint;
        auto messageBudget = static_cast<size_t>(std::max(2, config.maxContextMessages));
        auto turnBudget = messageBudget > 2 ? messageBudget - 2 : 0;
        while (memory.size() > checkpoint + turnBudget && memory.size() > checkpoint)
            memory.erase(memory.begin() + static_cast<std::ptrdiff_t>(checkpoint));
        auto plan = planCompression(config, systemPrompt, memory, userText);
        outcome.contextLimitTokens = plan.limitTokens;
        outcome.promptTokens = plan.promptTokens;
        Json messages = plan.messages;
        const std::string routingHint =
            "\nConversation and opinions about the current song are not playback commands. Never "
            "start music unless the user explicitly requests an operation. For a request to replace "
            "the queue with N songs, use music_recommend_queue in one call: an intent based on the "
            "active persona's taste for a style or mood request, source=playlists for 'from my "
            "playlists', source=liked for 'my liked songs' (music_list_likes only lists them). "
            "Counts of 10, 30, 100 or 1000 are supported and count defaults to 30. If the user "
            "already has ids (for example after music_open_collection or music_list_likes), one "
            "music_queue_edit(op=replace, ids=[...]) is the correct batch write - never queue songs "
            "one at a time with music_play. Report queuedCount versus requestedCount honestly. For "
            "requests to add songs without replacing the queue, use append instead.";
        if (messages.size() > 0 && messages[0].is_object() &&
            messages[0].value("role", std::string()) == "system" &&
            messages[0].contains("content") && messages[0]["content"].is_string())
            messages[0]["content"] = messages[0]["content"].get<std::string>() + routingHint;
        // Over the window: fold the oldest turns into one summary before the request goes out. A
        // failed summary is labelled in band and the folded turns are still there in bounded form,
        // so history is never silently dropped while the turn still reports success.
        if (plan.needed) {
            auto summary = summarizeBounded(config, send, plan.older);
            if (!summary.text.empty() && messages.size() > 1 && messages[1].is_object() &&
                messages[1].value("role", std::string()) == "system")
                messages[1]["content"] = "[早前对话摘要]\n" + summary.text;
            outcome.compressedMessages = static_cast<int>(plan.foldedTurns);
            outcome.compressionDegraded = !summary.code.empty();
            outcome.compressionCode = summary.code;
            // A degraded compression is visible evidence, not a silent success: the trace keeps
            // the code and the folded count next to the tool calls.
            if (!summary.code.empty())
                outcome.trace.push_back(
                    Json{{"round", -1},
                         {"compression", "degraded"},
                         {"code", summary.code},
                         {"foldedTurns", static_cast<int64_t>(plan.foldedTurns)},
                         {"resultBytes", static_cast<int64_t>(summary.text.size())},
                         {"resultSummary", summarize(summary.text, 200)}});
        }
        auto finishWithoutTools = [&]() {
            try {
                Json finalMessages = messages;
                finalMessages[0]["content"] =
                    finalMessages[0]["content"].get<std::string>() +
                    "\nTool use has ended. Answer the user's request briefly using only completed tool results. State clearly what was completed and what was not; do not claim accepted playback is already audible.";
                auto reply = parseAssistant(send(config, finalMessages, Json::array()));
                if (reply.calls.empty() && !reply.text.empty() && !toolMarkup(reply.text)) {
                    outcome.text = reply.text;
                    outcome.ok = true;
                    return true;
                }
            } catch (const std::exception&) {
            }
            return false;
        };
        for (int round = 0; round < config.maxRounds; ++round) {
            // The accounting describes the request this round actually sends.
            if (tokenEstimateSafe(messages))
                outcome.promptTokens = estimateMessageTokens(messages);
            Json response;
            try {
                response = send(config, messages, tools);
            } catch (const AgentError& error) {
                outcome.code = error.code;
                outcome.error = error.what();
                return outcome;
            } catch (const std::exception& error) {
                outcome.code = "network";
                outcome.error = error.what();
                return outcome;
            }
            auto reply = parseAssistant(response);
            if (reply.calls.empty()) {
                if (toolMarkup(reply.text)) {
                    if (finishWithoutTools())
                        return outcome;
                    if (!completedAction.empty()) {
                        outcome.text = completedAction;
                        outcome.ok = true;
                        return outcome;
                    }
                    outcome.code = "bad_response";
                    outcome.error = "model returned tool markup instead of a user-facing answer";
                    return outcome;
                }
                outcome.text = reply.text;
                outcome.ok = true;
                return outcome;
            }
            bool unadvertised = std::any_of(reply.calls.begin(), reply.calls.end(),
                                            [&](const ToolCall& call) {
                                                return !allowedTools.contains(call.name);
                                            });
            if (unadvertised) {
                if (finishWithoutTools())
                    return outcome;
                if (!completedAction.empty()) {
                    outcome.text = completedAction;
                    outcome.ok = true;
                    return outcome;
                }
                outcome.code = "bad_response";
                outcome.error = "model requested a tool not allowed for this request";
                return outcome;
            }
            // Keep the assistant tool turn paired with every result. If it cannot fit, ask for a
            // final answer using only results already present in the conversation.
            if (outcome.toolCalls + static_cast<int>(reply.calls.size()) > config.maxToolCalls) {
                if (finishWithoutTools())
                    return outcome;
                outcome.code = "tool_budget";
                outcome.error = "tool call budget exhausted";
                return outcome;
            }
            if (messages.size() + 1 + reply.calls.size() >
                static_cast<size_t>(config.maxContextMessages)) {
                if (finishWithoutTools())
                    return outcome;
                outcome.code = "context_budget";
                outcome.error = "context message budget exhausted";
                return outcome;
            }
            auto choices = response.value("choices", Json::array());
            auto assistant = choices[0].value("message", Json::object());
            if (!assistant.is_object())
                assistant = Json::object();
            // A tool-calling assistant turn normally carries content: null. Normalising it to an
            // empty string keeps the wire shape valid and the token estimator type-safe.
            if (!assistant.contains("content") || assistant["content"].is_null())
                assistant["content"] = "";
            messages.push_back(assistant);
            bool completedWrite = false;
            for (auto& call : reply.calls) {
                ++outcome.toolCalls;
                Json result;
                if (completedWrite) {
                    result = Json{{"error", "skipped_after_action"},
                                  {"message", "one successful write per turn"}};
                } else {
                    try {
                        result = execute(call.name, call.arguments);
                    } catch (const ToolError& error) {
                        result = Json{{"error", error.code}, {"message", error.what()}};
                    } catch (const std::exception& error) {
                        result = Json{{"error", "tool_failed"}, {"message", error.what()}};
                    }
                }
                if (result.is_object() && !result.contains("error") &&
                    result.value("changed", true)) {
                    if (!readOnlyTools.contains(call.name))
                        completedWrite = true;
                    if (call.name == "music_recommend_queue") {
                        auto queued = result.value("queuedCount", size_t(0));
                        auto requested = result.value("requestedCount", 0);
                        completedAction = L"已替换播放队列，共 " +
                                          std::to_wstring(queued) + L" 首歌";
                        if (queued < static_cast<size_t>(requested))
                            completedAction += L"（请求 " + std::to_wstring(requested) + L" 首）";
                        if (queued < static_cast<size_t>(requested))
                            requiredCountInAnswer = std::to_wstring(queued);
                        completedAction += L"。";
                    } else if (call.name == "music_queue_edit" && call.arguments.is_object() &&
                        call.arguments.value("op", std::string()) == "replace") {
                        auto ids = call.arguments.value("ids", Json::array());
                        if (ids.is_array() && !ids.empty())
                            completedAction = L"已替换播放队列，共 " +
                                              std::to_wstring(ids.size()) + L" 首歌。";
                    } else if (call.name == "music_play" ||
                               call.name == "music_play_random") {
                        completedAction = L"已提交播放请求。";
                    } else if (completedWrite) {
                        completedAction = L"已执行请求的操作。";
                    }
                }
                auto text = result.dump();
                // The trace is evidence for a human, not a second copy of the data.
                outcome.trace.push_back(Json{
                    {"round", round},
                    {"tool", call.name},
                    {"arguments", call.arguments},
                    {"resultBytes", static_cast<int64_t>(text.size())},
                    {"resultSummary", summarize(text, 400)}});
                messages.push_back(Json{{"role", "tool"},
                                        {"tool_call_id", call.id},
                                        {"content", text}});
                if (multiSongPlayback && call.name == "music_recommend_queue" &&
                    result.is_object() && result.contains("error")) {
                    auto code = result.value("error", std::string("tool_failed"));
                    auto message = result.value("message", std::string());
                    // The deterministic short-circuit must not claim a cause it cannot know: the
                    // tool distinguishes "nothing playable" from a player or account problem.
                    outcome.text =
                        code == "no_candidates"
                            ? L"没找到可播放的曲目，播放队列没有改变。可以说「从我的歌单挑 N 首」"
                              L"或「我喜欢的歌」，也可以换个关键词。"
                        : code == "no_likes"
                            ? L"这个账号没有红心歌曲（或未登录），播放队列没有改变。"
                        : code == "needs_player"
                            ? L"播放器当前不可用，请先启动 MORA。"
                        : code == "needs_login" || code == "unauthorized"
                            ? L"账号登录已失效，请先在终端重新登录，播放队列没有改变。"
                            : L"这次未能确认播放队列已替换（" + unutf8(code) +
                                  (message.empty() ? L"" : L"：" + unutf8(message)) + L"）。";
                    outcome.ok = true;
                    return outcome;
                }
            }
            if (completedWrite) {
                if (finishWithoutTools()) {
                    if (!requiredCountInAnswer.empty() &&
                        outcome.text.find(requiredCountInAnswer) == std::wstring::npos)
                        outcome.text = completedAction;
                    return outcome;
                }
                outcome.text = completedAction;
                outcome.ok = true;
                return outcome;
            }
        }
        // The requested action may already have succeeded. Give the model one final chance to
        // describe the observed tool results without allowing another tool round.
        if (finishWithoutTools())
            return outcome;
        outcome.code = "round_budget";
        outcome.error = "model kept requesting tools without answering";
        return outcome;
    }
} // namespace mora
