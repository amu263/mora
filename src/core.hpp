#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include "json.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mora {
    using Json = nlohmann::ordered_json;
    using Bytes = std::vector<uint8_t>;
    using Clock = std::chrono::steady_clock;
    std::string utf8(std::wstring_view text);
    std::wstring unutf8(std::string_view text);
    std::wstring sanitize(std::wstring text);
    int displayWidth(std::wstring_view text);
    std::wstring truncate(std::wstring_view text, int width);
    std::wstring pad(std::wstring_view text, int width);
    std::wstring timeText(double seconds);
    size_t editDistance(std::wstring_view a, std::wstring_view b);
    enum class Kind { Song = 1, Album = 10, Playlist = 1000 };
    struct Track {
        int64_t id = 0;
        std::wstring name, artist, album;
        std::string cover;
        int duration = 0;
        Kind kind = Kind::Song;
        int64_t owner = 0;
        bool liked = false;
    };
    Track parseTrack(const Json& item);
    Json trackJson(const Track& track);
    // One timed lyric line. `translation` is the provider's translated line when it ships one.
    struct LyricLine {
        double seconds = 0;
        std::wstring text, translation;
    };
    struct Lyrics {
        int64_t songId = 0;
        bool instrumental = false, unavailable = false;
        std::vector<LyricLine> lines;
    };
    // Parses LRC text (and the provider's translated LRC) into timed lines, skipping metadata tags.
    std::vector<LyricLine> parseLrcTimed(const std::string& plain, const std::string& translated);
    // The single lyrics projection shared by the terminal player and the adapters, so both report
    // the same bounded payload: timestamps, the line matching `position`, and a truncated flag.
    Json lyricsJson(const Lyrics& lyrics, int64_t songId, const Track* track, double position,
                    bool includeTranslation, size_t maxLines);
    // Index of the line that is playing at `position`, or -1 before the first line.
    int lyricIndexAt(const Lyrics& lyrics, double position);
    std::vector<Track> parseTracks(const Json& items);
    double matchScore(const Track& track, std::wstring_view query);
    bool playable(const Json& item);
    bool isRiskCode(int code);
    struct Queue {
        std::vector<Track> tracks;
        size_t index = 0;
        bool repeat = false;
        void assign(std::vector<Track> items, size_t selected = 0);
        Track* current();
        bool next();
        bool previous();
    };
    struct RGB {
        int r = 0, g = 0, b = 0;
    };
    struct Cover {
        int width = 0, height = 0;
        Bytes pixels;
    };
    struct Palette {
        RGB background{21, 25, 32}, surface{29, 35, 43}, accent{165, 191, 194}, text{225, 228, 222},
            muted{142, 155, 163};
    };
    double contrast(RGB a, RGB b);
    Palette palette(const Cover& cover);
    void writeTestTone(const std::filesystem::path& path, int seconds, int rate = 44100,
                       int bits = 16);
} // namespace mora
