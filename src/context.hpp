#pragma once
#include "agent.hpp"

namespace mora {
    // Conversation memory of the terminal chat. It survives a restart, keeps the recent turns
    // verbatim and stores one compaction checkpoint for everything older. Plain text in the user's
    // own data directory: never a model key, cookie or stream URL.
    struct Conversation {
        std::vector<AgentMessage> turns; // role "user"/"assistant", oldest first
        std::string summary;             // compacted checkpoint, empty until the first compression
        int compressedTurns = 0;
        void append(const std::string& role, const std::string& content, size_t limit = 40);
        void clear();
        bool empty() const { return turns.empty() && summary.empty(); }
    };
    Conversation loadConversation(const std::filesystem::path& directory);
    bool saveConversation(const std::filesystem::path& directory, const Conversation& conversation);
    // Bounded projection handed to the model: the checkpoint first, then the newest turns.
    std::vector<AgentMessage> conversationHistory(const Conversation& conversation, size_t limit = 12);
    // Replaces the oldest turns with `summary`, keeping the newest `keep` turns verbatim.
    void compactConversation(Conversation& conversation, const std::string& summary, size_t keep = 6);
} // namespace mora
