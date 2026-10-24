#pragma once
#include "core.hpp"
#include <functional>

namespace mora {
    // Per-user, single-instance session bridge between the terminal player (which owns the one
    // PlaybackSession) and thin adapters such as the MCP stdio process. It is a local named pipe
    // whose DACL only admits the current Windows user: no network listener, no unauthenticated
    // LAN port, and no second audio core.
    std::wstring bridgePipeName();

    struct BridgeReply {
        bool ok = false;
        std::string code, message;
        Json data = Json::object();
        BridgeReply() = default;
        BridgeReply(bool success, std::string errorCode, std::string text, Json payload)
            : ok(success), code(std::move(errorCode)), message(std::move(text)),
              data(std::move(payload)) {}
    };

    using BridgeCompletion = std::function<void(BridgeReply)>;

    // Owner side. A background thread accepts one adapter at a time and turns frames into
    // requests; pump() runs on the thread that owns the session and hands the reply back.
    class BridgeServer {
        class Impl;
        std::unique_ptr<Impl> impl;

      public:
        BridgeServer();
        ~BridgeServer();
        BridgeServer(const BridgeServer&) = delete;
        BridgeServer& operator=(const BridgeServer&) = delete;
        // Fails when another MORA player already owns the session pipe (single player instance).
        bool start(std::string& error);
        bool start(const std::wstring& name, std::string& error);
        void stop();
        bool running() const;
        bool attached() const;
        // handler must call completion exactly once, immediately or later (e.g. when the shared
        // network worker answers). The adapter's frame waits up to 30 s for that call.
        void pump(const std::function<void(const std::string&, const Json&,
                                           BridgeCompletion)>& handler);
    };

    // Adapter side. Requests are bounded and time out instead of hanging the caller.
    class BridgeClient {
        void* pipe = nullptr;
        std::wstring name;

      public:
        explicit BridgeClient(std::wstring pipeName = {});
        ~BridgeClient();
        BridgeClient(const BridgeClient&) = delete;
        BridgeClient& operator=(const BridgeClient&) = delete;
        bool connect(int timeoutMs = 500);
        bool connected() const;
        void disconnect();
        BridgeReply call(const std::string& method, const Json& params, int timeoutMs = 4000);
    };
} // namespace mora
