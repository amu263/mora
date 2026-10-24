#include "context.hpp"
#include "platform.hpp"

#include <fstream>

namespace mora {
    namespace {
        constexpr size_t kMaxTurns = 40;
        constexpr size_t kMaxTurnChars = 4000;
        constexpr size_t kMaxSummaryChars = 4000;
        constexpr uintmax_t kMaxFileBytes = 512 * 1024;

        std::string trimTo(std::string value, size_t limit) {
            if (value.size() > limit)
                value.resize(limit);
            return value;
        }
        std::filesystem::path memoryFile(const std::filesystem::path& directory) {
            return directory / L"chat-memory.json";
        }
    } // namespace
    void Conversation::append(const std::string& role, const std::string& content, size_t limit) {
        if (content.empty())
            return;
        auto size = std::min(limit ? limit : kMaxTurns, kMaxTurns);
        turns.push_back(AgentMessage{.role = role, .content = trimTo(content, kMaxTurnChars)});
        while (turns.size() > size)
            turns.erase(turns.begin());
    }
    void Conversation::clear() {
        turns.clear();
        summary.clear();
        compressedTurns = 0;
    }
    Conversation loadConversation(const std::filesystem::path& directory) {
        Conversation conversation;
        auto path = memoryFile(directory);
        std::error_code error;
        if (!std::filesystem::exists(path, error))
            return conversation;
        if (std::filesystem::file_size(path, error) > kMaxFileBytes || error)
            return conversation;
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return conversation;
        auto data = Json::parse(file, nullptr, false);
        if (data.is_discarded() || !data.is_object())
            return conversation;
        if (data.contains("summary") && data["summary"].is_string())
            conversation.summary = trimTo(data["summary"].get<std::string>(), kMaxSummaryChars);
        if (data.contains("compressedTurns") && data["compressedTurns"].is_number_integer())
            conversation.compressedTurns = std::max(0, data["compressedTurns"].get<int>());
        if (data.contains("turns") && data["turns"].is_array()) {
            for (auto& item : data["turns"]) {
                if (!item.is_object() || conversation.turns.size() >= kMaxTurns)
                    break;
                auto role = item.value("role", std::string());
                auto content = item.value("content", std::string());
                if ((role != "user" && role != "assistant") || content.empty())
                    continue;
                conversation.turns.push_back(
                    AgentMessage{.role = role, .content = trimTo(content, kMaxTurnChars)});
            }
        }
        return conversation;
    }
    bool saveConversation(const std::filesystem::path& directory, const Conversation& conversation) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        auto path = memoryFile(directory);
        auto temporary = directory / L"chat-memory.tmp";
        Json data = {{"version", 1},
                     {"summary", trimTo(conversation.summary, kMaxSummaryChars)},
                     {"compressedTurns", conversation.compressedTurns},
                     {"turns", Json::array()}};
        size_t count = 0;
        for (auto& turn : conversation.turns) {
            if (count++ >= kMaxTurns)
                break;
            data["turns"].push_back(
                Json{{"role", turn.role}, {"content", trimTo(turn.content, kMaxTurnChars)}});
        }
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file)
                return false;
            file << data.dump(2) << '\n';
            if (!file)
                return false;
        }
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
        return !error;
    }
    std::vector<AgentMessage> conversationHistory(const Conversation& conversation, size_t limit) {
        std::vector<AgentMessage> history;
        // The checkpoint travels as its own system message: OpenAI-compatible endpoints accept a
        // system message anywhere in the list, and it keeps the summary clearly separated from the
        // turns the listener actually wrote.
        if (!conversation.summary.empty())
            history.push_back(AgentMessage{.role = "system",
                                           .content = "[早前对话摘要]\n" + conversation.summary});
        auto keep = limit ? limit : 12;
        size_t begin = conversation.turns.size() > keep ? conversation.turns.size() - keep : 0;
        for (size_t i = begin; i < conversation.turns.size(); ++i)
            history.push_back(conversation.turns[i]);
        return history;
    }
    void compactConversation(Conversation& conversation, const std::string& summary, size_t keep) {
        if (summary.empty())
            return;
        auto retained = std::min(keep, conversation.turns.size());
        int folded = static_cast<int>(conversation.turns.size() - retained);
        if (folded <= 0)
            return;
        conversation.turns.erase(conversation.turns.begin(),
                                 conversation.turns.end() - static_cast<std::ptrdiff_t>(retained));
        conversation.compressedTurns += folded;
        // A second compression folds the earlier checkpoint into the new one instead of losing it.
        conversation.summary = conversation.summary.empty()
                                   ? summary
                                   : conversation.summary + "\n" + summary;
        conversation.summary = trimTo(conversation.summary, kMaxSummaryChars);
    }
} // namespace mora
