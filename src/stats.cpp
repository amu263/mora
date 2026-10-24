// Listening history and day/month/year statistics.
//
// Storage is an append-only JSONL file in the user's data directory. One listen is one complete
// line: a row is only written after the player reports the track ended (or a replaced track is
// closed), so a torn tail can be dropped without losing the rest. Day/month/year buckets are
// derived with the Windows local-time conversion, never UTC: a listen at 00:30 belongs to the
// local day the user actually experienced.
#include "stats.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mora {
    namespace {
        // Every bound the engine enforces: one log line, the log file, one ranking, the projection.
        constexpr size_t kMaxNameBytes = 160;
        constexpr size_t kMaxArtistBytes = 120;
        constexpr size_t kMaxAlbumBytes = 120;
        constexpr size_t kMaxSourceBytes = 16;
        constexpr size_t kMaxLineBytes = 64 * 1024;
        constexpr uintmax_t kMaxLogBytes = 16ull * 1024 * 1024;
        constexpr uintmax_t kKeepLogBytes = 8ull * 1024 * 1024;
        constexpr size_t kKeepEntries = 4000;
        constexpr uintmax_t kMaxReadBytes = 32ull * 1024 * 1024;
        constexpr size_t kRankLimit = 20;
        constexpr size_t kKeyLimit = 36;
        constexpr size_t kJsonKeyLimit = 12;
        constexpr size_t kProjectionBytes = 8 * 1024;
        constexpr size_t kJsonNameBytes = 56;
        constexpr size_t kJsonArtistBytes = 36;
        constexpr int64_t kUncleSeconds = 11644473600LL; // 1601-01-01 .. 1970-01-01

        std::string lowered(std::string_view text) {
            std::string out(text);
            for (auto& c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        // Truncates at a UTF-8 code point boundary. A clipped name must stay valid UTF-8 or the
        // JSON projection cannot be serialized at all.
        std::string cutUtf8(std::string text, size_t limit) {
            if (text.size() <= limit)
                return text;
            size_t at = limit;
            while (at > 0 && (static_cast<unsigned char>(text[at - 1]) & 0xC0) == 0x80)
                --at;
            size_t lead = at > 0 ? at - 1 : 0;
            auto first = static_cast<unsigned char>(text[lead]);
            size_t need = first < 0x80 ? 1
                        : (first & 0xE0) == 0xC0 ? 2
                        : (first & 0xF0) == 0xE0 ? 3
                        : (first & 0xF8) == 0xF0 ? 4 : 1;
            text.resize(lead + need > limit ? lead : limit);
            return text;
        }

        // Keeps display text only. Control characters and backslashes go away, and anything that
        // looks like a stream URL, a local path or a cookie is dropped entirely, so the log can
        // never carry a sensitive value even if a provider sends one in a title.
        std::string cleanText(std::string_view raw, size_t limit) {
            std::string text;
            text.reserve(std::min(raw.size(), limit + 8));
            bool space = false;
            for (unsigned char c : raw) {
                if (c == '\t' || c == '\r' || c == '\n')
                    c = ' ';
                if (c < 0x20 || c == 0x7F)
                    continue;
                if (c == ' ') {
                    if (space || text.empty())
                        continue;
                    space = true;
                } else {
                    space = false;
                }
                text += static_cast<char>(c);
            }
            while (!text.empty() && text.back() == ' ')
                text.pop_back();
            text = cutUtf8(std::move(text), limit);
            if (text.empty())
                return text;
            auto lower = lowered(text);
            if (lower.find("://") != std::string::npos || lower.find("cookie") != std::string::npos ||
                lower.find("music_u") != std::string::npos || text.find('\\') != std::string::npos)
                return {};
            return text;
        }

        // The source is a closed vocabulary, so a caller can never smuggle a URL or a path in.
        std::string allowedSource(std::string_view raw) {
            static constexpr std::array<const char*, 11> known{
                "search", "playlist", "library", "queue",   "agent", "random",
                "local",  "album",    "artist",  "startup", "unknown"};
            auto value = lowered(cleanText(raw, kMaxSourceBytes));
            for (auto item : known)
                if (value == item)
                    return value;
            return "unknown";
        }

        int64_t nowSeconds() {
            FILETIME file{};
            GetSystemTimeAsFileTime(&file);
            ULARGE_INTEGER value{};
            value.LowPart = file.dwLowDateTime;
            value.HighPart = file.dwHighDateTime;
            return static_cast<int64_t>((value.QuadPart - 116444736000000000ull) / 10000000ull);
        }

        // Local wall-clock fields for a unix timestamp. Windows applies the time zone rules that
        // were in effect for that date, so daylight saving never shifts a bucket.
        bool localStamp(int64_t seconds, SYSTEMTIME& out) {
            if (seconds < -kUncleSeconds || seconds > 32503680000LL) // 1601 .. 3000
                return false;
            int64_t ticks = (seconds + kUncleSeconds) * 10000000LL;
            FILETIME utc{};
            utc.dwLowDateTime = static_cast<DWORD>(static_cast<uint64_t>(ticks) & 0xFFFFFFFFull);
            utc.dwHighDateTime = static_cast<DWORD>(static_cast<uint64_t>(ticks) >> 32);
            FILETIME local{};
            if (!FileTimeToLocalFileTime(&utc, &local))
                return false;
            return FileTimeToSystemTime(&local, &out) != 0;
        }

        std::string bucketKey(int64_t seconds, ListenPeriod period) {
            SYSTEMTIME local{};
            if (!localStamp(seconds, local))
                return {};
            char buffer[16]{};
            if (period == ListenPeriod::Year)
                std::snprintf(buffer, sizeof buffer, "%04d", local.wYear);
            else if (period == ListenPeriod::Month)
                std::snprintf(buffer, sizeof buffer, "%04d-%02d", local.wYear, local.wMonth);
            else
                std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02d", local.wYear, local.wMonth,
                              local.wDay);
            return buffer;
        }

        std::string localIso(int64_t seconds) {
            SYSTEMTIME local{};
            if (!localStamp(seconds, local))
                return {};
            char buffer[24]{};
            std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT%02d:%02d:%02d", local.wYear,
                          local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
            return buffer;
        }

        std::string clockText(int64_t seconds) {
            if (seconds < 0)
                seconds = 0;
            char buffer[32]{};
            auto hours = seconds / 3600;
            auto minutes = (seconds % 3600) / 60;
            auto rest = seconds % 60;
            if (hours > 0)
                std::snprintf(buffer, sizeof buffer, "%lld:%02lld:%02lld", static_cast<long long>(hours),
                              static_cast<long long>(minutes), static_cast<long long>(rest));
            else
                std::snprintf(buffer, sizeof buffer, "%lld:%02lld", static_cast<long long>(minutes),
                              static_cast<long long>(rest));
            return buffer;
        }

        double ratio(int64_t part, int64_t whole) {
            if (whole <= 0)
                return 0.0;
            return std::round(static_cast<double>(part) / static_cast<double>(whole) * 1000.0) / 1000.0;
        }

        std::string clip(std::string text, size_t limit) {
            return cutUtf8(std::move(text), limit);
        }

        int64_t number(const Json& data, const char* key, int64_t fallback) {
            auto item = data.find(key);
            if (item == data.end())
                return fallback;
            if (item->is_number_integer())
                return item->get<int64_t>();
            if (item->is_number_unsigned())
                return static_cast<int64_t>(item->get<uint64_t>());
            if (item->is_number_float())
                return static_cast<int64_t>(item->get<double>());
            return fallback;
        }

        std::string string(const Json& data, const char* key) {
            auto item = data.find(key);
            if (item == data.end() || !item->is_string())
                return {};
            return item->get<std::string>();
        }

        bool boolean(const Json& data, const char* key, bool fallback) {
            auto item = data.find(key);
            if (item == data.end() || !item->is_boolean())
                return fallback;
            return item->get<bool>();
        }

        // A row is only accepted when it can be placed on the calendar and identified; anything
        // else (torn tail, foreign line, hand-edited junk) is quietly skipped.
        std::optional<ListenEntry> entryFrom(const Json& data) {
            ListenEntry entry;
            entry.songId = number(data, "song", 0);
            entry.name = cleanText(string(data, "name"), kMaxNameBytes);
            entry.artist = cleanText(string(data, "artist"), kMaxArtistBytes);
            entry.album = cleanText(string(data, "album"), kMaxAlbumBytes);
            entry.source = allowedSource(string(data, "source"));
            entry.startedAt = number(data, "start", 0);
            entry.listened = static_cast<int>(std::clamp<int64_t>(number(data, "listened", 0), 0, 86400));
            entry.duration = static_cast<int>(std::clamp<int64_t>(number(data, "duration", 0), 0, 86400));
            entry.completed = boolean(data, "completed", false);
            // A completion must stay consistent with the playhead even when the row was written by
            // another version or hand-edited: "completed" means it really reached the end.
            if (entry.completed && entry.duration > 0 && entry.listened + 2 < entry.duration)
                entry.completed = false;
            if (entry.startedAt <= 0)
                return std::nullopt;
            if (entry.songId == 0 && entry.name.empty())
                return std::nullopt;
            return entry;
        }

        std::string lineFor(const ListenEntry& entry) {
            Json row = Json::object();
            row["v"] = 1;
            row["song"] = entry.songId;
            row["name"] = entry.name;
            row["artist"] = entry.artist;
            row["album"] = entry.album;
            row["source"] = entry.source;
            row["start"] = entry.startedAt;
            row["listened"] = entry.listened;
            row["duration"] = entry.duration;
            row["completed"] = entry.completed;
            return row.dump();
        }

        // Reads the log newest-last, tolerating every shape of damage: a truncated final line, a
        // line that is not JSON, an object with the wrong types, or a line larger than the cap.
        std::vector<ListenEntry> readAll(const std::filesystem::path& file) {
            std::vector<ListenEntry> entries;
            std::error_code error;
            auto size = static_cast<int64_t>(std::filesystem::file_size(file, error));
            if (error || size <= 0)
                return entries;
            std::ifstream input(file, std::ios::binary);
            if (!input)
                return entries;
            // An externally grown file is not allowed to cost unbounded memory: keep the tail.
            if (static_cast<uintmax_t>(size) > kMaxReadBytes) {
                input.seekg(size - static_cast<int64_t>(kMaxReadBytes));
                std::string partial;
                std::getline(input, partial);
            }
            std::string line;
            while (std::getline(input, line)) {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty() || line.size() > kMaxLineBytes)
                    continue;
                auto data = Json::parse(line, nullptr, false);
                if (data.is_discarded() || !data.is_object())
                    continue;
                try {
                    if (auto entry = entryFrom(data))
                        entries.push_back(std::move(*entry));
                } catch (const std::exception&) {
                    continue;
                }
            }
            return entries;
        }

        // Keeps the newest rows under both a row count and a byte budget.
        void compactLog(const std::filesystem::path& file) {
            auto entries = readAll(file);
            if (entries.empty())
                return;
            std::vector<std::string> lines;
            uintmax_t bytes = 0;
            for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
                auto line = lineFor(*it);
                if (lines.size() >= kKeepEntries)
                    break;
                if (!lines.empty() && bytes + line.size() + 1 > kKeepLogBytes)
                    break;
                bytes += line.size() + 1;
                lines.push_back(std::move(line));
            }
            std::string payload;
            payload.reserve(static_cast<size_t>(bytes));
            for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
                payload += *it;
                payload += '\n';
            }
            auto temporary = file;
            temporary += L".tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                if (!output)
                    return;
                output << payload;
                if (!output)
                    return;
            }
            // Replace in place so a concurrent reader either sees the old file or the new one.
            if (!MoveFileExW(temporary.c_str(), file.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                std::error_code error;
                std::filesystem::remove(file, error);
                error.clear();
                std::filesystem::rename(temporary, file, error);
            }
        }

        std::string songKey(const ListenEntry& entry) {
            if (entry.songId != 0)
                return "i" + std::to_string(entry.songId);
            return "n" + entry.name + "|" + entry.artist;
        }

        struct SongGroup {
            std::string name, artist;
            int64_t plays = 0, seconds = 0;
        };
        struct ArtistGroup {
            std::string artist;
            int64_t plays = 0, seconds = 0;
        };

        const char* hourLabel(size_t hour) {
            static constexpr std::array<const char*, 24> labels{
                "00", "01", "02", "03", "04", "05", "06", "07", "08", "09", "10", "11",
                "12", "13", "14", "15", "16", "17", "18", "19", "20", "21", "22", "23"};
            return labels[hour < labels.size() ? hour : 0];
        }

        bool shrink(Json& array) {
            if (!array.is_array() || array.empty())
                return false;
            size_t keep = array.size() > 4 ? array.size() / 2 : array.size() - 1;
            while (array.size() > keep)
                array.erase(array.end() - 1);
            return true;
        }

        // Last resort for an unusually verbose log: the projection must stay under the budget even
        // when every name is at its cap.
        bool trimOnce(Json& data) {
            if (data.contains("availableKeys") && data["availableKeys"].size() > kJsonKeyLimit)
                return shrink(data["availableKeys"]);
            if (data.contains("hourHistogram") && data["hourHistogram"].size() > 24)
                return shrink(data["hourHistogram"]);
            if (data.contains("topArtists") && data["topArtists"].size() > 4)
                return shrink(data["topArtists"]);
            if (data.contains("topSongs") && data["topSongs"].size() > 4)
                return shrink(data["topSongs"]);
            if (data.contains("availableKeys") && !data["availableKeys"].empty()) {
                data["availableKeys"].erase(data["availableKeys"].end() - 1);
                return true;
            }
            if (data.contains("hourHistogram") && !data["hourHistogram"].empty()) {
                data["hourHistogram"].erase(data["hourHistogram"].end() - 1);
                return true;
            }
            if (data.contains("topArtists") && !data["topArtists"].empty()) {
                data["topArtists"].erase(data["topArtists"].end() - 1);
                return true;
            }
            if (data.contains("topSongs") && !data["topSongs"].empty()) {
                data["topSongs"].erase(data["topSongs"].end() - 1);
                return true;
            }
            for (auto* key : {"availableKeys", "hourHistogram", "topArtists", "topSongs",
                              "mostRepeated", "lateNightPlays", "peakHour"}) {
                if (data.contains(key)) {
                    data.erase(key);
                    return true;
                }
            }
            return false;
        }

        Json minimalProjection(const Json& data) {
            Json slim = Json::object();
            for (auto* key : {"period", "key", "label", "plays", "listenedSeconds", "uniqueSongs",
                              "uniqueArtists", "completedPlays", "skippedPlays", "completionRate"})
                if (data.contains(key))
                    slim[key] = data[key];
            return slim;
        }
    } // namespace

    const char* listenPeriodName(ListenPeriod period) {
        if (period == ListenPeriod::Month)
            return "month";
        if (period == ListenPeriod::Year)
            return "year";
        return "day";
    }

    bool parseListenPeriod(std::string_view value, ListenPeriod& out) {
        auto text = lowered(value);
        if (text == "day" || text == "daily" || text == "d") {
            out = ListenPeriod::Day;
            return true;
        }
        if (text == "month" || text == "monthly" || text == "m") {
            out = ListenPeriod::Month;
            return true;
        }
        if (text == "year" || text == "yearly" || text == "y") {
            out = ListenPeriod::Year;
            return true;
        }
        return false;
    }

    ListenHistory::ListenHistory(std::filesystem::path directory)
        : file(std::move(directory) / L"listen-log.jsonl") {
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
    }

    ListenHistory::~ListenHistory() { flush(); }

    void ListenHistory::onTrackStart(const Track& track, std::string_view source) {
        // A replaced track is closed before the next one opens, so no listen is ever lost and no
        // row is left half-written.
        if (pendingActive)
            flush();
        pending = ListenEntry{};
        pending.songId = track.id;
        pending.name = cleanText(utf8(track.name), kMaxNameBytes);
        pending.artist = cleanText(utf8(track.artist), kMaxArtistBytes);
        pending.album = cleanText(utf8(track.album), kMaxAlbumBytes);
        pending.duration = track.duration > 0 ? track.duration : 0;
        pending.source = allowedSource(source.empty() ? std::string_view(pendingSource) : source);
        pending.startedAt = nowSeconds();
        pendingActive = true;
    }

    void ListenHistory::onTrackEnd(const Track& track, double positionSeconds, bool completed) {
        // Finalize-once: a listen already settled by flush() (or by a previous end) is never
        // written again, so the log cannot grow a duplicate row for one listen.
        if (!pendingActive)
            return;
        ListenEntry entry = pending;
        pending = ListenEntry{};
        pendingActive = false;
        if (entry.duration <= 0 && track.duration > 0)
            entry.duration = track.duration;
        if (entry.name.empty())
            entry.name = cleanText(utf8(track.name), kMaxNameBytes);
        if (entry.artist.empty())
            entry.artist = cleanText(utf8(track.artist), kMaxArtistBytes);
        double position = positionSeconds > 0 ? positionSeconds : 0;
        int64_t listened = static_cast<int64_t>(position + 0.5);
        if (entry.duration > 0 && listened > entry.duration)
            listened = entry.duration;
        entry.listened = static_cast<int>(std::clamp<int64_t>(listened, 0, 86400));
        // A completion is only trusted when the playhead really reached the end (or the duration
        // is unknown and the player says so).
        entry.completed =
            completed && (entry.duration <= 0 || position + 2.0 >= static_cast<double>(entry.duration));
        if (entry.startedAt <= 0)
            entry.startedAt = nowSeconds();
        if (entry.songId == 0 && entry.name.empty())
            return;
        write(entry);
    }

    void ListenHistory::flush() {
        // Finalize-once: the in-flight listen is bounded by its own duration and closed here; a
        // repeated flush has nothing left to write, and a later onTrackEnd is a no-op.
        if (!pendingActive)
            return;
        ListenEntry entry = pending;
        pending = ListenEntry{};
        pendingActive = false;
        int64_t elapsed = nowSeconds() - entry.startedAt;
        if (elapsed < 0)
            elapsed = 0;
        if (elapsed > 4 * 3600)
            elapsed = 4 * 3600;
        if (entry.duration > 0 && elapsed > entry.duration)
            elapsed = entry.duration;
        entry.listened = static_cast<int>(elapsed);
        entry.completed = false;
        if (entry.startedAt <= 0)
            entry.startedAt = nowSeconds();
        if (entry.songId == 0 && entry.name.empty())
            return;
        write(entry);
    }

    void ListenHistory::write(const ListenEntry& entry) const {
        // Append-only: a listen is never rewritten. The only time the file is replaced is the
        // bounded compaction above the size cap.
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        error.clear();
        auto size = std::filesystem::file_size(file, error);
        if (!error && size > kMaxLogBytes)
            compactLog(file);
        std::ofstream output(file, std::ios::binary | std::ios::app);
        if (!output)
            return;
        output << lineFor(entry) << '\n';
    }

    std::vector<ListenEntry> ListenHistory::entries(size_t limit) const {
        auto all = readAll(file);
        if (limit && all.size() > limit)
            return std::vector<ListenEntry>(all.end() - static_cast<ptrdiff_t>(limit), all.end());
        return all;
    }

    std::vector<std::string> ListenHistory::availableKeys(ListenPeriod period) const {
        std::unordered_set<std::string> seen;
        std::vector<std::string> keys;
        for (auto& entry : readAll(file)) {
            auto key = bucketKey(entry.startedAt, period);
            if (!key.empty() && seen.insert(key).second)
                keys.push_back(std::move(key));
        }
        std::sort(keys.begin(), keys.end(), std::greater<>());
        if (keys.size() > kKeyLimit)
            keys.resize(kKeyLimit);
        return keys;
    }

    ListenStats ListenHistory::aggregate(ListenPeriod period, std::string_view key) const {
        ListenStats stats;
        stats.period = period;
        auto all = readAll(file);
        stats.availableKeys = availableKeys(period);
        stats.key = key.empty() ? bucketKey(nowSeconds(), period) : std::string(key);

        std::unordered_map<std::string, size_t> songIndex, artistIndex;
        std::vector<SongGroup> songs;
        std::vector<ArtistGroup> artists;
        std::unordered_set<std::string> songIds, artistNames;
        std::vector<ListenStats::Row> hours(24);
        for (size_t hour = 0; hour < hours.size(); ++hour)
            hours[hour].label = hourLabel(hour);
        int64_t first = 0, last = 0, completed = 0;

        for (auto& entry : all) {
            if (bucketKey(entry.startedAt, period) != stats.key)
                continue;
            ++stats.plays;
            stats.listenedSeconds += entry.listened;
            if (entry.completed)
                ++completed;
            if (first == 0 || entry.startedAt < first)
                first = entry.startedAt;
            if (entry.startedAt > last)
                last = entry.startedAt;
            songIds.insert(songKey(entry));
            if (!entry.artist.empty())
                artistNames.insert(entry.artist);

            auto songKeyText = songKey(entry);
            auto songSlot = songIndex.find(songKeyText);
            if (songSlot == songIndex.end()) {
                songSlot = songIndex.emplace(songKeyText, songs.size()).first;
                songs.emplace_back();
            }
            auto& song = songs[songSlot->second];
            if (song.name.empty()) {
                song.name = entry.name;
                song.artist = entry.artist;
            }
            ++song.plays;
            song.seconds += entry.listened;

            auto artistName = entry.artist.empty() ? std::string("unknown") : entry.artist;
            auto artistSlot = artistIndex.find(artistName);
            if (artistSlot == artistIndex.end()) {
                artistSlot = artistIndex.emplace(artistName, artists.size()).first;
                artists.emplace_back();
            }
            auto& artist = artists[artistSlot->second];
            artist.artist = artistName;
            ++artist.plays;
            artist.seconds += entry.listened;

            SYSTEMTIME local{};
            if (localStamp(entry.startedAt, local)) {
                auto slot = local.wHour < 24 ? local.wHour : 0;
                ++hours[slot].plays;
                hours[slot].seconds += entry.listened;
            }
        }

        stats.uniqueSongs = static_cast<int64_t>(songIds.size());
        stats.uniqueArtists = static_cast<int64_t>(artistNames.size());
        stats.completedPlays = completed;
        stats.skippedPlays = stats.plays - completed;
        stats.firstStart = first;
        stats.lastStart = last;

        std::sort(songs.begin(), songs.end(), [](const SongGroup& a, const SongGroup& b) {
            if (a.plays != b.plays)
                return a.plays > b.plays;
            if (a.seconds != b.seconds)
                return a.seconds > b.seconds;
            return a.name < b.name;
        });
        for (size_t i = 0; i < songs.size() && stats.topSongs.size() < kRankLimit; ++i) {
            ListenStats::Row row;
            row.label = songs[i].name;
            row.artist = songs[i].artist;
            row.plays = songs[i].plays;
            row.seconds = songs[i].seconds;
            stats.topSongs.push_back(std::move(row));
        }

        std::sort(artists.begin(), artists.end(), [](const ArtistGroup& a, const ArtistGroup& b) {
            if (a.plays != b.plays)
                return a.plays > b.plays;
            if (a.seconds != b.seconds)
                return a.seconds > b.seconds;
            return a.artist < b.artist;
        });
        for (size_t i = 0; i < artists.size() && stats.topArtists.size() < kRankLimit; ++i) {
            ListenStats::Row row;
            row.label = artists[i].artist;
            row.plays = artists[i].plays;
            row.seconds = artists[i].seconds;
            row.artist = artists[i].artist;
            stats.topArtists.push_back(std::move(row));
        }

        // The hour distribution is useful for every period (a month of late-night listening is
        // exactly the material a review wants), so it is always populated with 24 buckets.
        stats.byHour = std::move(hours);
        return stats;
    }

    Json listenStatsJson(const ListenStats& stats) {
        Json data = Json::object();
        data["period"] = listenPeriodName(stats.period);
        data["key"] = stats.key;
        data["label"] = stats.key;
        data["timezone"] = "local";
        data["plays"] = stats.plays;
        data["listenedSeconds"] = stats.listenedSeconds;
        data["listenedText"] = clockText(stats.listenedSeconds);
        data["uniqueSongs"] = stats.uniqueSongs;
        data["uniqueArtists"] = stats.uniqueArtists;
        data["completedPlays"] = stats.completedPlays;
        data["skippedPlays"] = stats.skippedPlays;
        data["completionRate"] = ratio(stats.completedPlays, stats.plays);
        data["averageSecondsPerPlay"] = stats.plays > 0 ? stats.listenedSeconds / stats.plays : 0;
        data["repeatRate"] = stats.plays > 0 ? ratio(stats.plays - stats.uniqueSongs, stats.plays) : 0.0;
        if (stats.firstStart > 0)
            data["firstListenAt"] = localIso(stats.firstStart);
        if (stats.lastStart > 0)
            data["lastListenAt"] = localIso(stats.lastStart);

        Json songs = Json::array();
        for (auto& row : stats.topSongs) {
            Json item = Json::object();
            item["name"] = clip(row.label, kJsonNameBytes);
            item["artist"] = clip(row.artist, kJsonArtistBytes);
            item["plays"] = row.plays;
            item["seconds"] = row.seconds;
            songs.push_back(std::move(item));
            if (songs.size() >= kRankLimit)
                break;
        }
        data["topSongs"] = std::move(songs);
        data["mostRepeated"] = (!data["topSongs"].empty() && data["topSongs"][0]["plays"] >= 2)
                                   ? data["topSongs"][0]
                                   : Json(nullptr);

        Json artists = Json::array();
        for (auto& row : stats.topArtists) {
            Json item = Json::object();
            item["artist"] = clip(row.label.empty() ? row.artist : row.label, kJsonArtistBytes);
            item["plays"] = row.plays;
            item["seconds"] = row.seconds;
            artists.push_back(std::move(item));
            if (artists.size() >= kRankLimit)
                break;
        }
        data["topArtists"] = std::move(artists);

        Json hours = Json::array();
        int64_t lateNight = 0, peakPlays = 0;
        std::string peak;
        for (auto& row : stats.byHour) {
            Json item = Json::object();
            item["hour"] = clip(row.label, 2);
            item["plays"] = row.plays;
            item["seconds"] = row.seconds;
            hours.push_back(std::move(item));
            auto hour = std::atoi(row.label.c_str());
            if (hour >= 23 || hour < 5)
                lateNight += row.plays;
            if (row.plays > peakPlays) {
                peakPlays = row.plays;
                peak = row.label;
            }
        }
        data["hourHistogram"] = std::move(hours);
        data["lateNightPlays"] = lateNight;
        data["peakHour"] = peakPlays > 0 ? peak : std::string();

        Json keys = Json::array();
        for (size_t i = 0; i < stats.availableKeys.size() && i < kJsonKeyLimit; ++i)
            keys.push_back(stats.availableKeys[i]);
        data["availableKeys"] = std::move(keys);

        for (int guard = 0; data.dump().size() > kProjectionBytes && guard < 128; ++guard)
            if (!trimOnce(data))
                break;
        if (data.dump().size() > kProjectionBytes)
            data = minimalProjection(data);
        return data;
    }
} // namespace mora
