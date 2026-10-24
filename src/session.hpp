#pragma once
#include "audio.hpp"
#include "core.hpp"

namespace mora {
    class ListenSink;
    // Copyable view of the authoritative playback state. Reports and tests compare snapshots
    // instead of reaching into the session, so every entry point reads the same numbers.
    struct SessionSnapshot {
        std::vector<int64_t> ids;
        size_t index = 0;
        int64_t currentId = 0;
        bool repeat = false;
        QueueMode mode = QueueMode::Sequential;
        double volume = .65;
        std::string requestedQuality = "lossless", actualQuality;
        uint64_t revision = 0, request = 0;
        bool operator==(const SessionSnapshot& other) const = default;
    };

    // The single owner of "what is playing": queue, current track, quality and volume.
    // The terminal UI owns one instance; MCP and the model loop are meant to share the same one
    // instead of keeping a private queue. Network work stays on the Worker thread, and this class
    // is touched only from the thread that owns it.
    class PlaybackSession {
        Audio* device = nullptr;
        Queue queue;
        Track current;
        std::string requested = "lossless", actual;
        double desiredVolume = .65;
        uint64_t revision = 0, request = 0;
        ListenSink* listenSink = nullptr;
        std::string sourceHint = "unknown";
        bool playingReported = false;

        void touch() { ++revision; }
        void reportEnd(bool completed);
        static bool mergeInto(Track& base, const Track& update);

      public:
        explicit PlaybackSession(Audio* audio = nullptr) : device(audio) {}
        PlaybackSession(const PlaybackSession&) = delete;

        // Request identity. beginRequest() hands out a fresh id and invalidates every id issued
        // earlier, so a reply from a superseded request can never overwrite newer state.
        uint64_t beginRequest() { return ++request; }
        uint64_t requestId() const { return request; }
        bool owns(uint64_t id) const { return id != 0 && id == request; }
        void cancelRequests() { ++request; }

        void assign(std::vector<Track> items, size_t selected = 0);
        void assignId(int64_t id);
        const std::vector<Track>& tracks() const { return queue.tracks; }
        size_t index() const { return queue.index; }
        Track* currentTrack() { return queue.current(); }
        const Track* currentTrack() const {
            return const_cast<PlaybackSession*>(this)->queue.current();
        }
        const Track& playing() const { return current; }
        bool move(int amount);
        bool repeat() const { return queue.repeat(); }
        void setRepeat(bool value);
        // Queue control the terminal, MCP adapters and the model loop all share. Every mutator
        // touches the revision so a stale reply can never overwrite newer queue state.
        QueueMode mode() const { return queue.mode; }
        void setMode(QueueMode value, uint64_t seed = 1);
        bool playIndex(size_t value);
        bool removeIndex(size_t value);
        bool moveTrack(size_t from, size_t to);
        void clearQueue();
        bool enqueue(std::vector<Track> items);
        bool playNext(std::vector<Track> items);
        bool queueEmpty() const { return queue.tracks.empty(); }
        size_t queueSize() const { return queue.tracks.size(); }
        size_t queueIndex() const { return queue.index; }
        // Merge richer metadata for one id into the queue row and the current track. Non-empty
        // fields win, so a stale or partial response can never blank out loaded metadata.
        void refresh(const Track& track);
        // Record a track as current without touching the device (demo, local preview, selection).
        void setCurrent(const Track& track);
        void startTrack(const Track& track, std::string actualQuality, std::wstring url);

        const std::string& quality() const { return requested; }
        void setQuality(std::string value);
        const std::string& actualQuality() const { return actual; }

        double volume() const { return desiredVolume; }
        void setVolume(double value);
        void adjustVolume(double delta) { setVolume(desiredVolume + delta); }

        void pause();
        void setPaused(bool value);
        void seek(double seconds);
        void stop();
        AudioState audioState() const;
        // Playback history hook. The session is the one place that knows a track really started,
        // so the listen log is fed from here instead of from every entry point.
        void attachListenSink(ListenSink* sink) { listenSink = sink; }
        void setSource(std::string value) { sourceHint = std::move(value); }
        const std::string& source() const { return sourceHint; }

        SessionSnapshot snapshot() const;
    };
} // namespace mora
