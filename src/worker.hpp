#pragma once
#include "service.hpp"
#include "ui_tool_calls.hpp"
#include <functional>
#include <future>

namespace mora {
    struct Reply {
        std::string type, origin, text, key;
        int code = 0, total = 0;
        int64_t id = 0, uid = 0;
        uint64_t request = 0, ticket = 0;
        std::wstring nickname;
        Json data = Json(nullptr);
        std::vector<Track> items;
        std::set<int64_t> likes;
        Playback playback;
        Lyrics lyrics;
    };
    // Serial and bounded: one request in flight, at most a few queued, and no per-keystroke
    // traffic. Tasks call MusicService only; playback and UI state stay on the owner thread, which
    // collects replies through poll().
    class Worker {
        struct Job {
            uint64_t request = 0, ticket = 0;
            std::string name;
            std::function<Reply(MusicService&)> task;
            std::shared_ptr<std::promise<Reply>> completion;
        };
        std::mutex mutex;
        std::condition_variable signal;
        std::deque<Job> tasks;
        std::deque<Reply> results;
        bool quit = false;
        MainThreadToolCalls* mainCalls = nullptr;
        std::atomic<uint64_t>* activeChat = nullptr;
        uint64_t activeToolRequest = 0; // worker thread only
        std::thread thread;

      public:
        explicit Worker(MainThreadToolCalls* calls = nullptr,
                        std::atomic<uint64_t>* active = nullptr)
            : mainCalls(calls), activeChat(active), thread([this] { run(); }) {}
        ~Worker() {
            cancel();
            {
                std::lock_guard lock(mutex);
                quit = true;
            }
            signal.notify_all();
            thread.join();
        }
        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;
        bool post(uint64_t request, std::string name, std::function<Reply(MusicService&)> task) {
            {
                std::lock_guard lock(mutex);
                if (tasks.size() >= 4)
                    return false;
                tasks.push_back({request, 0, std::move(name), std::move(task)});
            }
            signal.notify_one();
            return true;
        }
        // Adapter work (ticket != 0) is not part of the UI request identity: it is answered by its
        // own ticket and survives UI cancellation.
        bool postAdapter(uint64_t ticket, std::string name,
                         std::function<Reply(MusicService&)> task) {
            {
                std::lock_guard lock(mutex);
                if (tasks.size() >= 4)
                    return false;
                tasks.push_back({0, ticket, std::move(name), std::move(task)});
            }
            signal.notify_one();
            return true;
        }
        Json callTool(uint64_t request, const std::string& name, const Json& arguments) {
            auto completion = std::make_shared<std::promise<Reply>>();
            auto result = completion->get_future();
            {
                std::lock_guard lock(mutex);
                if (quit || tasks.size() >= 4)
                    throw ToolError("busy", "the music worker is busy");
                Job job;
                job.request = request;
                job.name = "chat-tool";
                job.task = [name, arguments](MusicService& service) {
                    Reply reply;
                    reply.type = "chat-tool";
                    reply.data = service.invoke(name, arguments);
                    return reply;
                };
                job.completion = completion;
                tasks.push_back(std::move(job));
            }
            signal.notify_one();
            auto reply = result.get();
            if (reply.type == "error")
                throw ToolError(reply.key.empty() ? "tool_failed" : reply.key, reply.text);
            return std::move(reply.data);
        }
        // Cancellation drops queued UI work. Adapter jobs are correlated by their own ticket, so
        // they are kept. A request already inside WinHTTP finishes, but its reply is
        // rejected by the session's request identity and cannot change newer state.
        size_t cancel() {
            std::lock_guard lock(mutex);
            size_t dropped = 0;
            for (auto it = tasks.begin(); it != tasks.end();) {
                if (it->ticket != 0 || it->completion) {
                    ++it;
                } else {
                    it = tasks.erase(it);
                    ++dropped;
                }
            }
            return dropped;
        }
        size_t pending() {
            std::lock_guard lock(mutex);
            return tasks.size();
        }
        std::vector<Reply> poll() {
            std::lock_guard lock(mutex);
            std::vector<Reply> out;
            while (!results.empty()) {
                out.push_back(std::move(results.front()));
                results.pop_front();
            }
            return out;
        }

      private:
        void run() {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            std::unique_ptr<MusicService> service;
            for (;;) {
                Job job;
                {
                    std::unique_lock lock(mutex);
                    signal.wait(lock, [this] { return quit || !tasks.empty(); });
                    if (quit)
                        break;
                    job = std::move(tasks.front());
                    tasks.pop_front();
                }
                Reply reply;
                try {
                    if (job.completion && activeChat && job.request != activeChat->load())
                        throw ToolError("cancelled", "the chat request was cancelled");
                    if (!service)
                        service = std::make_unique<MusicService>();
                    if (mainCalls) {
                        service->attach([this, &service](const std::string& method,
                                                         const Json& params) {
                            // All read-only requests stay on this worker and reuse its one Api.
                            if (method == "search" || method == "collection" ||
                                method == "playlists")
                                return BridgeReply(false, "needs_player", "use local music service",
                                                   Json::object());
                            if (method == "like") {
                                service->like(params.value("id", (int64_t)0),
                                              params.value("liked", false));
                                return BridgeReply(true, "", "", Json::object());
                            }
                            if (method == "add") {
                                service->save(params.value("songId", (int64_t)0),
                                              params.value("playlistId", (int64_t)0));
                                return BridgeReply(true, "", "", Json::object());
                            }
                            if (method == "lyrics") {
                                // The live position and the playing track live on the main thread;
                                // the fetch itself stays here so it reuses this worker's one Api.
                                auto view = mainCalls->call("status", Json::object(),
                                                            activeToolRequest);
                                if (!view.ok)
                                    return view;
                                Track track;
                                auto current = view.data.value("track", Json::object());
                                track.id = current.value("id", (int64_t)0);
                                track.name = unutf8(current.value("name", std::string()));
                                track.artist = unutf8(current.value("artist", std::string()));
                                track.album = unutf8(current.value("album", std::string()));
                                auto id = params.value("songId", (int64_t)0);
                                if (id <= 0)
                                    id = track.id;
                                if (id <= 0)
                                    return BridgeReply(false, "not_playing",
                                                       "no track is playing", Json::object());
                                double position =
                                    id == track.id
                                        ? view.data.value("positionSeconds", 0.0)
                                        : 0.0;
                                auto loaded = service->lyrics(id);
                                auto maxLines = params.value("maxLines", 60);
                                auto payload = lyricsJson(
                                    loaded, id, id == track.id ? &track : nullptr, position,
                                    params.value("includeTranslation", true),
                                    static_cast<size_t>(std::clamp(maxLines, 1, 200)));
                                return BridgeReply(true, "", "", std::move(payload));
                            }
                            return mainCalls->call(method, params, activeToolRequest);
                        });
                    }
                    activeToolRequest = job.completion ? job.request : 0;
                    reply = job.task(*service);
                } catch (const ToolError& e) {
                    reply.type = "error";
                    reply.key = e.code;
                    reply.text = e.what();
                } catch (const ApiError& e) {
                    reply.type = "error";
                    reply.code = e.code;
                    reply.text = e.what();
                } catch (const std::exception& e) {
                    reply.type = "error";
                    reply.text = e.what();
                }
                reply.origin = job.name;
                reply.request = job.request;
                reply.ticket = job.ticket;
                activeToolRequest = 0;
                if (job.completion)
                    job.completion->set_value(std::move(reply));
                else {
                    std::lock_guard lock(mutex);
                    results.push_back(std::move(reply));
                }
            }
            service.reset();
            CoUninitialize();
        }
    };
} // namespace mora
