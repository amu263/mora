#pragma once
#include "core.hpp"
#include <functional>
namespace mora {
    // M3 internal model adapter. Defaults follow the provider documentation
    // (opencode console inference, OpenAI-compatible chat completions).
    struct AgentConfig {
        std::string baseUrl = "https://opencode.ai/inference/go/openai/v1/chat/completions";
        std::string model = "deepseek-v4.1-flash";
        // Empty keeps the provider default. Chat Completions accepts a requested
        // reasoning_effort for models that expose adjustable reasoning.
        std::string reasoningEffort;
        std::string credentialRef = "agent.bin";
        int maxToolCalls = 8, maxContextMessages = 24, maxOutputTokens = 800;
        int maxRounds = 4;
    };
    struct AgentMessage {
        std::string role, content, toolCallId, name;
    };
    struct ToolCall {
        std::string id, name;
        Json arguments;
    };
    struct AgentReply {
        std::wstring text;
        std::vector<ToolCall> calls;
    };
    // Model/transport failures carry a machine-readable code; messages never contain the key.
    class AgentError : public std::runtime_error {
      public:
        AgentError(std::string code, std::string message, int status = 0)
            : std::runtime_error(message), code(std::move(code)), status(status) {
        }
        std::string code;
        int status = 0;
    };
    using ModelTransport = std::function<Json(const AgentConfig&, const Json& messages,
                                              const Json& tools)>;
    using MusicToolExecutor = std::function<Json(const std::string&, const Json&)>;

    // config (non-secret) lives in <data dir>/agent.json, the key only in DPAPI agent.bin
    AgentConfig loadAgentConfig();
    void saveAgentConfig(const AgentConfig& config);
    bool agentKeyPresent();
    void saveAgentKey(const std::string& key);
    std::string loadAgentKey();

    // MCP tool definitions -> OpenAI function tools
    Json agentToolsFrom(const Json& musicTools);
    Json transportChat(const AgentConfig& config, const Json& messages, const Json& tools);
    AgentReply parseAssistant(const Json& response);

    struct ChatOutcome {
        std::wstring text;
        Json trace = Json::array();
        bool ok = false;
        std::string code, error;
        int toolCalls = 0;
    };
    // Bounded tool loop. The transport is injectable so the loop can be tested offline.
    ChatOutcome runChatLoop(AgentConfig config, const std::string& userText,
                            const MusicToolExecutor& execute, const ModelTransport& transport = {},
                            const std::wstring& systemPrompt = {});
} // namespace mora
