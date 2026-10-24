#include "session.hpp"
#include "stats.hpp"

namespace mora {
    bool PlaybackSession::mergeInto(Track& base, const Track& update) {
        bool changed = false;
        if (!update.name.empty() && update.name != base.name) {
            base.name = update.name;
            changed = true;
        }
        if (!update.artist.empty() && update.artist != base.artist) {
            base.artist = update.artist;
            changed = true;
        }
        if (!update.album.empty() && update.album != base.album) {
            base.album = update.album;
            changed = true;
        }
        if (!update.cover.empty() && update.cover != base.cover) {
            base.cover = update.cover;
            changed = true;
        }
        if (update.duration > 0 && update.duration != base.duration) {
            base.duration = update.duration;
            changed = true;
        }
        if (update.owner && update.owner != base.owner) {
            base.owner = update.owner;
            changed = true;
        }
        return changed;
    }
    void PlaybackSession::assign(std::vector<Track> items, size_t selected) {
        queue.assign(std::move(items), selected);
        touch();
    }
    void PlaybackSession::assignId(int64_t id) {
        queue.assign({Track{.id = id}});
        touch();
    }
    bool PlaybackSession::move(int amount) {
        bool changed = amount < 0 ? queue.previous() : queue.next();
        if (changed)
            touch();
        return changed;
    }
    void PlaybackSession::setRepeat(bool value) {
        setMode(value ? QueueMode::RepeatAll : QueueMode::Sequential);
    }
    void PlaybackSession::setMode(QueueMode value, uint64_t seed) {
        // An explicit seed must rebuild the order even when the mode is already shuffle: otherwise
        // "set shuffle with this seed" silently keeps the previous permutation and the same request
        // is no longer reproducible.
        bool rebuild = value == QueueMode::Shuffle && seed != 0;
        if (queue.mode == value && !rebuild)
            return;
        queue.setMode(value, seed);
        touch();
    }
    bool PlaybackSession::playIndex(size_t value) {
        if (!queue.setIndex(value))
            return false;
        touch();
        return true;
    }
    bool PlaybackSession::removeIndex(size_t value) {
        if (!queue.removeAt(value))
            return false;
        touch();
        return true;
    }
    bool PlaybackSession::moveTrack(size_t from, size_t to) {
        if (!queue.moveTrack(from, to))
            return false;
        touch();
        return true;
    }
    void PlaybackSession::clearQueue() {
        queue.clear();
        touch();
    }
    bool PlaybackSession::enqueue(std::vector<Track> items) {
        if (!queue.append(std::move(items)))
            return false;
        touch();
        return true;
    }
    bool PlaybackSession::playNext(std::vector<Track> items) {
        bool inserted = queue.insertNext(std::move(items));
        if (inserted)
            touch();
        return inserted;
    }
    void PlaybackSession::refresh(const Track& track) {
        if (!track.id)
            return;
        bool changed = false;
        for (auto& row : queue.tracks)
            if (row.id == track.id)
                changed = mergeInto(row, track) || changed;
        if (current.id == track.id)
            changed = mergeInto(current, track) || changed;
        if (changed)
            touch();
    }
    void PlaybackSession::setCurrent(const Track& track) {
        current = track;
        for (auto& row : queue.tracks)
            if (row.id == track.id)
                mergeInto(row, track);
        touch();
    }
    void PlaybackSession::reportEnd(bool completed) {
        if (listenSink && playingReported && current.id) {
            auto state = audioState();
            listenSink->onTrackEnd(current, state.position, completed);
        }
        playingReported = false;
    }
    void PlaybackSession::startTrack(const Track& track, std::string actualQuality,
                                     std::wstring url) {
        // A replaced track counts as a listen when it actually played; the log decides how much of
        // it to keep. Starting a new row always closes the previous one first.
        reportEnd(false);
        setCurrent(track);
        actual = std::move(actualQuality);
        if (device)
            device->play(std::move(url));
        if (listenSink && current.id) {
            listenSink->onTrackStart(current, sourceHint);
            playingReported = true;
        }
    }
    void PlaybackSession::setQuality(std::string value) {
        if (requested == value)
            return;
        requested = std::move(value);
        touch();
    }
    void PlaybackSession::setVolume(double value) {
        desiredVolume = std::clamp(value, 0.0, 1.0);
        if (device)
            device->volume(desiredVolume);
        touch();
    }
    void PlaybackSession::pause() {
        if (device)
            device->pause();
    }
    void PlaybackSession::setPaused(bool value) {
        if (device)
            device->setPaused(value);
        touch();
    }
    void PlaybackSession::seek(double seconds) {
        if (device)
            device->seek(seconds);
    }
    void PlaybackSession::stop() {
        reportEnd(false);
        if (device)
            device->stop();
    }
    AudioState PlaybackSession::audioState() const {
        return device ? device->state() : AudioState{};
    }
    SessionSnapshot PlaybackSession::snapshot() const {
        SessionSnapshot s;
        s.ids.reserve(queue.tracks.size());
        for (auto& track : queue.tracks)
            s.ids.push_back(track.id);
        s.index = queue.index;
        s.currentId = current.id;
        s.repeat = queue.repeat();
        s.mode = queue.mode;
        s.volume = desiredVolume;
        s.requestedQuality = requested;
        s.actualQuality = actual;
        s.revision = revision;
        s.request = request;
        return s;
    }
} // namespace mora
