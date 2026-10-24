#include "session.hpp"

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
        if (queue.repeat == value)
            return;
        queue.repeat = value;
        touch();
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
    void PlaybackSession::startTrack(const Track& track, std::string actualQuality,
                                     std::wstring url) {
        setCurrent(track);
        actual = std::move(actualQuality);
        if (device)
            device->play(std::move(url));
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
        s.repeat = queue.repeat;
        s.volume = desiredVolume;
        s.requestedQuality = requested;
        s.actualQuality = actual;
        s.revision = revision;
        s.request = request;
        return s;
    }
} // namespace mora
