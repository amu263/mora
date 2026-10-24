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
        int maxToolCalls = 8, maxContextMessages = 64, maxOutputTokens = 800;
        int maxRounds = 4;
        // Context window reported by the endpoint for the selected model. 0 means "not discovered
        // yet"; the loop then uses contextFallbackTokens so memory still has a hard bound.
        int contextTokens = 0;
        int contextFallbackTokens = 32768;
        // Compress the older conversation once estimated usage crosses this ratio of the window.
        double compressAt = .72;
        bool autoCompress = true;
        size_t keepRecentMessages = 6;
        // Filled by fetchModelCatalog: the modalities the endpoint reports for the chosen model.
        // The user never picks these by hand; they are reported, not requested.
        std::vector<std::string> modelModalities;
    };
    // One model the configured endpoint advertises, with the capability range it reports.
    struct ModelCapability {
        std::string id;
        std::vector<std::string> inputModalities, outputModalities;
        int contextTokens = 0, maxOutputTokens = 0;
        bool reasoning = false;
    };
    struct ModelCatalog {
        bool ok = false;
        std::string code, error, endpoint;
        std::vector<ModelCapability> models;
    };
    // Read-only discovery: ask the configured chat endpoint which models it offers. The endpoint is
    // derived from baseUrl (/models), so no extra host is contacted.
    ModelCatalog fetchModelCatalog(const AgentConfig& config);
    // The catalog is cached in agent.json so the capability range survives a restart.
    void cacheModelCatalog(const ModelCatalog& catalog);
    ModelCatalog cachedModelCatalog();
    // Cheap, dependency-free prompt-size estimate used for the context budget. It deliberately
    // over-counts CJK (one token per character) so the budget stays conservative.
    size_t estimateTokens(std::string_view text);
    size_t estimateMessageTokens(const Json& messages);
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
    void clearAgentKey();
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
        // Context accounting of the request that produced this answer, for the UI and the tests.
        size_t promptTokens = 0;
        size_t contextLimitTokens = 0;
        int compressedMessages = 0;
        // True when the automatic compression had to fall back to deterministic truncation. The UI
        // must be able to say so out loud instead of presenting the turn as an unqualified success.
        bool compressionDegraded = false;
        std::string compressionCode;
        bool operator==(const ChatOutcome&) const = delete;
    };
    // Bounded tool loop with conversation memory. `history` is the prior conversation (user and
    // assistant turns) without the system prompt; older turns are folded into one summary message
    // when the estimated prompt would cross the model's context limit. The transport is injectable
    // so the loop and the compression path can be tested offline.
    ChatOutcome runChatLoop(AgentConfig config, const std::string& userText,
                            const MusicToolExecutor& execute, const ModelTransport& transport = {},
                            const std::wstring& systemPrompt = {},
                            const std::vector<AgentMessage>& history = {});

    // Fitting a conversation into the model's context window. `planCompression` is a pure function
    // (no network) so the budget behaviour is testable on its own; `summarizeHistory` performs the
    // one model call that turns the folded turns into a bounded summary.
    struct CompressionPlan {
        Json messages = Json::array(); // system + summary + kept turns, ready to send
        bool needed = false;           // the window was crossed, so a summary is required
        size_t foldedTurns = 0;        // how many older turns the summary replaces
        size_t promptTokens = 0, limitTokens = 0;
        std::vector<AgentMessage> older, kept;
    };
    CompressionPlan planCompression(const AgentConfig& config, const std::wstring& systemPrompt,
                                    const std::vector<AgentMessage>& history,
                                    const std::string& userText);
    std::string summarizeHistory(const AgentConfig& config, const ModelTransport& transport,
                                 const std::vector<AgentMessage>& older);
} // namespace mora
