#pragma once
#include "bridge.hpp"
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

namespace mora {
    // A worker may wait for one playback operation while the terminal's main thread owns the
    // PlaybackSession. No network work is posted back to that worker from this queue.
    class MainThreadToolCalls {
        struct Request {
            uint64_t owner = 0;
            std::string method;
            Json params;
            BridgeReply reply;
            bool done = false;
        };
        std::mutex mutex;
        std::condition_variable signal;
        std::deque<std::shared_ptr<Request>> pending;
        bool closed = false;

      public:
        BridgeReply call(std::string method, Json params, uint64_t owner = 0) {
            auto request = std::make_shared<Request>();
            request->owner = owner;
            request->method = std::move(method);
            request->params = std::move(params);
            std::unique_lock lock(mutex);
            if (closed)
                return BridgeReply(false, "cancelled", "the chat session has ended", Json::object());
            pending.push_back(request);
            signal.wait(lock, [&] { return closed || request->done; });
            return closed ? BridgeReply(false, "cancelled", "the chat session has ended",
                                        Json::object())
                          : std::move(request->reply);
        }

        size_t pump(const std::function<BridgeReply(uint64_t, const std::string&,
                                                    const Json&)>& handle) {
            std::shared_ptr<Request> request;
            {
                std::lock_guard lock(mutex);
                if (closed || pending.empty())
                    return 0;
                request = std::move(pending.front());
                pending.pop_front();
            }
            auto reply = handle(request->owner, request->method, request->params);
            {
                std::lock_guard lock(mutex);
                request->reply = std::move(reply);
                request->done = true;
            }
            signal.notify_all();
            return 1;
        }

        void close() {
            {
                std::lock_guard lock(mutex);
                closed = true;
                pending.clear();
            }
            signal.notify_all();
        }
    };
} // namespace mora
