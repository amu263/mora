#include "bridge.hpp"
#include <aclapi.h>
#include <sddl.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace mora {
    namespace {
        constexpr size_t kMaxFrame = 64 * 1024;
        constexpr int kOwnerWaitMs = 30000;

        std::wstring currentUserSid() {
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
                return {};
            DWORD size = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &size);
            std::vector<uint8_t> buffer(size);
            std::wstring text;
            if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
                auto user = reinterpret_cast<TOKEN_USER*>(buffer.data());
                LPWSTR sid = nullptr;
                if (ConvertSidToStringSidW(user->User.Sid, &sid)) {
                    text = sid;
                    LocalFree(sid);
                }
            }
            CloseHandle(token);
            return text;
        }

        // DACL that only admits the current Windows user; remote clients are refused by the pipe.
        struct Security {
            SECURITY_DESCRIPTOR descriptor{};
            PACL acl = nullptr;
            SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
            Security() {
                auto sid = currentUserSid();
                if (sid.empty())
                    return;
                PSID user = nullptr;
                if (!ConvertStringSidToSidW(sid.c_str(), &user))
                    return;
                EXPLICIT_ACCESSW access{};
                access.grfAccessPermissions = GENERIC_ALL;
                access.grfAccessMode = GRANT_ACCESS;
                access.grfInheritance = NO_INHERITANCE;
                access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
                access.Trustee.TrusteeType = TRUSTEE_IS_USER;
                access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(user);
                if (SetEntriesInAclW(1, &access, nullptr, &acl) == ERROR_SUCCESS &&
                    InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) &&
                    SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE)) {
                } else if (acl) {
                    LocalFree(acl);
                    acl = nullptr;
                }
                LocalFree(user);
            }
            ~Security() {
                if (acl)
                    LocalFree(acl);
            }
            bool ok() const { return acl != nullptr; }
        };

        bool readExact(HANDLE pipe, void* buffer, DWORD size) {
            auto* bytes = static_cast<uint8_t*>(buffer);
            DWORD total = 0;
            while (total < size) {
                DWORD got = 0;
                if (!ReadFile(pipe, bytes + total, size - total, &got, nullptr) || got == 0)
                    return false;
                total += got;
            }
            return true;
        }

        bool writeExact(HANDLE pipe, const void* buffer, DWORD size) {
            const auto* bytes = static_cast<const uint8_t*>(buffer);
            DWORD total = 0;
            while (total < size) {
                DWORD written = 0;
                if (!WriteFile(pipe, bytes + total, size - total, &written, nullptr) || written == 0)
                    return false;
                total += written;
            }
            return true;
        }

        bool writeFrame(HANDLE pipe, const Json& message) {
            auto text = message.dump();
            if (text.empty() || text.size() > kMaxFrame)
                return false;
            uint32_t length = static_cast<uint32_t>(text.size());
            return writeExact(pipe, &length, 4) && writeExact(pipe, text.data(), length);
        }

        bool readFrame(HANDLE pipe, Json& message) {
            uint32_t length = 0;
            if (!readExact(pipe, &length, 4) || length == 0 || length > kMaxFrame)
                return false;
            std::string text(length, '\0');
            if (!readExact(pipe, text.data(), length))
                return false;
            message = Json::parse(text, nullptr, false);
            return !message.is_discarded();
        }
    } // namespace

    std::wstring bridgePipeName() {
        static const std::wstring name = [] {
            auto sid = currentUserSid();
            for (auto& c : sid)
                if (c == L'\\' || c == L'/')
                    c = L'-';
            std::wstring base = std::wstring(L"\\\\.\\pipe\\mora-player-") +
                                (sid.empty() ? L"default" : sid);
            // An explicit --data-dir gets its own pipe scope. Test profiles and parallel
            // installations stop fighting over one pipe (and stop touching the player the user is
            // actually listening to); the default profile keeps the historical name so existing
            // MCP adapters and a running player stay attached.
            wchar_t buffer[32768];
            DWORD length = GetEnvironmentVariableW(L"MORA_DATA_DIR", buffer, 32768);
            if (length == 0 || length >= 32768)
                return base;
            auto path = std::filesystem::absolute(std::wstring(buffer, length)).wstring();
            // FNV-1a over the widened path: deterministic, dependency-free and short.
            uint64_t hash = 1469598103934665603ull;
            for (wchar_t c : path) {
                hash ^= static_cast<uint64_t>(c);
                hash *= 1099511628211ull;
            }
            const wchar_t* digits = L"0123456789abcdef";
            std::wstring suffix;
            for (int shift = 28; shift >= 0; shift -= 4)
                suffix += digits[(hash >> shift) & 0xf];
            return base + L"-" + suffix;
        }();
        return name;
    }

    class BridgeServer::Impl {
      public:
        struct Request {
            std::string method;
            Json params = Json::object();
            BridgeReply reply;
            bool done = false;
        };
        std::wstring name;
        HANDLE pipe = INVALID_HANDLE_VALUE;
        std::thread thread;
        std::atomic<bool> stopping{false}, started{false}, attached{false};
        std::mutex mutex;
        std::condition_variable signal;
        std::deque<std::shared_ptr<Request>> pending;

        bool start(std::string& error) {
            Security security;
            if (!security.ok()) {
                error = "cannot restrict the session pipe to the current user";
                return false;
            }
            // PIPE_REJECT_REMOTE_CLIENTS is rejected by CreateNamedPipeW on this Windows build
            // (ERROR_INVALID_PARAMETER, with or without a security descriptor), so the boundary is
            // the user-only DACL plus per-user pipe naming: there is no TCP listener at all.
            pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
                                    static_cast<DWORD>(kMaxFrame), static_cast<DWORD>(kMaxFrame), 0,
                                    &security.attributes);
            if (pipe == INVALID_HANDLE_VALUE) {
                DWORD code = GetLastError();
                error = (code == ERROR_ACCESS_DENIED || code == ERROR_PIPE_BUSY)
                            ? "another MORA player already owns the session"
                            : "session pipe creation failed (" + std::to_string(code) + ")";
                return false;
            }
            started = true;
            thread = std::thread([this] { run(); });
            return true;
        }

        void stop() {
            if (!started)
                return;
            stopping = true;
            signal.notify_all();
            if (pipe != INVALID_HANDLE_VALUE)
                CancelIoEx(pipe, nullptr);
            if (thread.joinable())
                thread.join();
            if (pipe != INVALID_HANDLE_VALUE) {
                CloseHandle(pipe);
                pipe = INVALID_HANDLE_VALUE;
            }
            started = false;
            attached = false;
        }

        void run() {
            while (!stopping) {
                BOOL connected = ConnectNamedPipe(pipe, nullptr);
                if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
                    if (stopping)
                        break;
                    continue;
                }
                attached = true;
                for (;;) {
                    Json frame;
                    if (stopping || !readFrame(pipe, frame))
                        break;
                    if (!frame.is_object() || !frame.contains("method") ||
                        !frame["method"].is_string()) {
                        writeFrame(pipe, Json{{"id", frame.value("id", Json(nullptr))},
                                              {"ok", false},
                                              {"error", Json{{"code", "invalid_request"},
                                                             {"message", "method must be a string"}}}});
                        continue;
                    }
                    auto request = std::make_shared<Request>();
                    request->method = frame["method"].get<std::string>();
                    request->params = frame.contains("params") && frame["params"].is_object()
                                          ? frame["params"]
                                          : Json::object();
                    Json id = frame.value("id", Json(nullptr));
                    {
                        std::lock_guard lock(mutex);
                        pending.push_back(request);
                    }
                    signal.notify_all();
                    {
                        std::unique_lock lock(mutex);
                        if (!signal.wait_for(lock, std::chrono::milliseconds(kOwnerWaitMs),
                                             [&] { return request->done || stopping.load(); }))
                            request->reply = BridgeReply(false, "timeout",
                                                         "player did not answer the request",
                                                         Json::object());
                    }
                    Json response = {{"id", id}, {"ok", request->reply.ok}};
                    if (request->reply.ok)
                        response["result"] = request->reply.data;
                    else
                        response["error"] = Json{{"code", request->reply.code},
                                                 {"message", request->reply.message}};
                    if (!writeFrame(pipe, response))
                        break;
                }
                attached = false;
                DisconnectNamedPipe(pipe);
                if (stopping)
                    break;
            }
        }

        void pump(const std::function<void(const std::string&, const Json&,
                                           BridgeCompletion)>& handler) {
            std::deque<std::shared_ptr<Request>> batch;
            {
                std::lock_guard lock(mutex);
                batch.swap(pending);
            }
            if (batch.empty())
                return;
            for (auto& request : batch) {
                auto complete = [this, request](BridgeReply reply) {
                    {
                        std::lock_guard lock(mutex);
                        if (request->done)
                            return;
                        request->reply = std::move(reply);
                        request->done = true;
                    }
                    signal.notify_all();
                };
                try {
                    handler(request->method, request->params, complete);
                } catch (const std::exception& e) {
                    complete(BridgeReply(false, "internal_error", e.what(), Json::object()));
                }
            }
        }
    };

    BridgeServer::BridgeServer() : impl(std::make_unique<Impl>()) {}
    BridgeServer::~BridgeServer() { stop(); }
    bool BridgeServer::start(std::string& error) { return start(bridgePipeName(), error); }
    bool BridgeServer::start(const std::wstring& name, std::string& error) {
        if (impl->started) {
            error = "session pipe already started";
            return false;
        }
        impl->name = name;
        return impl->start(error);
    }
    void BridgeServer::stop() { impl->stop(); }
    bool BridgeServer::running() const { return impl->started && !impl->stopping; }
    bool BridgeServer::attached() const { return impl->attached; }
    void BridgeServer::pump(
        const std::function<void(const std::string&, const Json&, BridgeCompletion)>& handler) {
        impl->pump(handler);
    }

    BridgeClient::BridgeClient(std::wstring pipeName)
        : name(pipeName.empty() ? bridgePipeName() : std::move(pipeName)) {}
    BridgeClient::~BridgeClient() { disconnect(); }
    bool BridgeClient::connect(int timeoutMs) {
        if (pipe)
            return true;
        auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            HANDLE handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                        OPEN_EXISTING, 0, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                pipe = handle;
                return true;
            }
            if (GetLastError() != ERROR_PIPE_BUSY || Clock::now() >= deadline)
                return false;
            WaitNamedPipeW(name.c_str(), 100);
        }
    }
    bool BridgeClient::connected() const { return pipe != nullptr; }
    void BridgeClient::disconnect() {
        if (pipe) {
            CloseHandle(static_cast<HANDLE>(pipe));
            pipe = nullptr;
        }
    }
    BridgeReply BridgeClient::call(const std::string& method, const Json& params, int timeoutMs) {
        if (!pipe)
            return BridgeReply(false, "needs_player", "no running MORA player is attached",
                               Json::object());
        static std::atomic<uint64_t> sequence{0};
        uint64_t id = ++sequence;
        if (!writeFrame(static_cast<HANDLE>(pipe), Json{{"id", id},
                                                        {"method", method},
                                                        {"params", params}})) {
            disconnect();
            return BridgeReply(false, "disconnected", "MORA player disconnected", Json::object());
        }
        auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(static_cast<HANDLE>(pipe), nullptr, 0, nullptr, &available,
                               nullptr)) {
                disconnect();
                return BridgeReply(false, "disconnected", "MORA player disconnected",
                                   Json::object());
            }
            if (available >= 4)
                break;
            if (Clock::now() >= deadline) {
                disconnect();
                return BridgeReply(false, "timeout", "MORA player did not answer in time",
                                   Json::object());
            }
            Sleep(10);
        }
        for (int attempt = 0; attempt < 4; ++attempt) {
            Json response;
            if (!readFrame(static_cast<HANDLE>(pipe), response)) {
                disconnect();
                return BridgeReply(false, "disconnected", "MORA player disconnected",
                                   Json::object());
            }
            if (!response.is_object())
                continue;
            if (response.contains("id") && response["id"] != Json(id))
                continue;
            BridgeReply reply;
            reply.ok = response.value("ok", false);
            if (reply.ok) {
                reply.data = response.value("result", Json::object());
            } else if (response.contains("error") && response["error"].is_object()) {
                reply.code = response["error"].value("code", std::string("error"));
                reply.message = response["error"].value("message", std::string());
            } else {
                reply.code = "error";
            }
            return reply;
        }
        return BridgeReply(false, "invalid_response", "MORA player sent an unexpected reply",
                           Json::object());
    }
} // namespace mora
