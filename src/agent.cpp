#include "agent.hpp"
#include "platform.hpp"
#include "service.hpp"
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
        std::wstring systemDefault() {
            return L"你是 MORA 音乐终端的音乐助手。用中文简短回答（不超过 120 字）。"
                   L"需要事实、候选或播放状态时先调用工具，不要凭记忆编造歌名、歌手或播放结果。"
                   L"工具返回 needs_player 时告诉用户先运行 mora 终端；写操作只在用户明确给出目标时调用。";
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
        return config;
    }

    void saveAgentConfig(const AgentConfig& config) {
        if (!validReasoningEffort(config.reasoningEffort))
            throw AgentError("invalid_reasoning_effort", "unsupported reasoning effort");
        auto sessionId = agentSessionId();
        Json stored = {{"baseUrl", config.baseUrl},
                       {"sessionId", sessionId},
                       {"model", config.model},
                       {"reasoningEffort", config.reasoningEffort},
                       {"maxOutputTokens", config.maxOutputTokens},
                       {"maxRounds", config.maxRounds}};
        std::ofstream file(agentFile("agent.json"), std::ios::binary | std::ios::trunc);
        file << stored.dump(2) << '\n';
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

    std::string loadAgentKey() {
        auto encrypted = readFile(agentFile("agent.bin"));
        if (encrypted.empty())
            return {};
        auto clear = unprotect(bytes(encrypted));
        return std::string(clear.begin(), clear.end());
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
            L"Content-Type: application/json\r\nUser-Agent: mora-player/0.6.0\r\n"
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

    ChatOutcome runChatLoop(AgentConfig config, const std::string& userText,
                            const MusicToolExecutor& execute, const ModelTransport& transport,
                            const std::wstring& systemPrompt) {
        ChatOutcome outcome;
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
        Json tools = agentToolsFrom(musicToolDefinitions());
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
        tools.push_back(personaTool);
        Json messages = Json::array();
        messages.push_back(Json{{"role", "system"},
                                {"content", utf8(systemPrompt.empty() ? systemDefault()
                                                                      : systemPrompt)}});
        messages.push_back(Json{{"role", "user"}, {"content", userText}});
        const auto& send = transport ? transport
                                     : ModelTransport([](const AgentConfig& cfg, const Json& m,
                                                         const Json& t) {
                                           return transportChat(cfg, m, t);
                                       });
        for (int round = 0; round < config.maxRounds; ++round) {
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
                outcome.text = reply.text;
                outcome.ok = true;
                return outcome;
            }
            auto choices = response.value("choices", Json::array());
            messages.push_back(choices[0].value("message", Json::object()));
            for (auto& call : reply.calls) {
                if (++outcome.toolCalls > config.maxToolCalls) {
                    outcome.code = "tool_budget";
                    outcome.error = "tool call budget exhausted";
                    return outcome;
                }
                Json result;
                try {
                    result = execute(call.name, call.arguments);
                } catch (const ToolError& error) {
                    result = Json{{"error", error.code}, {"message", error.what()}};
                } catch (const std::exception& error) {
                    result = Json{{"error", "tool_failed"}, {"message", error.what()}};
                }
                auto text = result.dump();
                // The trace is evidence for a human, not a second copy of the data.
                outcome.trace.push_back(Json{
                    {"round", round},
                    {"tool", call.name},
                    {"arguments", call.arguments},
                    {"resultBytes", static_cast<int64_t>(text.size())},
                    {"resultSummary", summarize(text, 400)}});
                if (messages.size() >= static_cast<size_t>(config.maxContextMessages)) {
                    outcome.code = "context_budget";
                    outcome.error = "context message budget exhausted";
                    return outcome;
                }
                messages.push_back(Json{{"role", "tool"},
                                        {"tool_call_id", call.id},
                                        {"content", text}});
            }
        }
        outcome.code = "round_budget";
        outcome.error = "model kept requesting tools without answering";
        return outcome;
    }
} // namespace mora
