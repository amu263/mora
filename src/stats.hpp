#pragma once
#include "core.hpp"

namespace mora {
    // One completed listen. Written when a track is replaced, stopped or reaches its end, so the
    // log never contains a half-finished row. No cookies, no stream URLs, no local file paths.
    struct ListenEntry {
        int64_t songId = 0;
        std::string name, artist, album, source; // search|playlist|library|queue|agent|random|local
        int64_t startedAt = 0;                   // unix seconds
        int listened = 0;                        // seconds actually played
        int duration = 0;
        bool completed = false;
        std::wstring title() const { return unutf8(name); }
    };
    enum class ListenPeriod { Day, Month, Year };
    const char* listenPeriodName(ListenPeriod period);
    bool parseListenPeriod(std::string_view value, ListenPeriod& out);
    // Aggregated report for one day, month or year. Everything is bounded: at most `topLimit`
    // rows per ranking, so the payload stays small enough to hand to the model.
    struct ListenStats {
        ListenPeriod period = ListenPeriod::Day;
        std::string key; // 2026-10-25 | 2026-10 | 2026
        int64_t plays = 0, listenedSeconds = 0, uniqueSongs = 0, uniqueArtists = 0;
        int64_t completedPlays = 0, skippedPlays = 0;
        struct Row {
            std::string label, artist;
            int64_t plays = 0, seconds = 0;
            bool operator==(const Row&) const = default;
        };
        std::vector<Row> topSongs, topArtists;
        // Always 24 buckets (also for month/year reports): late-night listening is exactly the kind
        // of habit the review is meant to notice.
        std::vector<Row> byHour; // 24 buckets, index = local hour
        int64_t firstStart = 0, lastStart = 0; // unix seconds covered by this report
        std::vector<std::string> availableKeys;
    };
    // The player reports playback here; the implementation decides how a listen is recorded.
    class ListenSink {
      public:
        virtual ~ListenSink() = default;
        virtual void onTrackStart(const Track& track, std::string_view source) = 0;
        virtual void onTrackEnd(const Track& track, double positionSeconds, bool completed) = 0;
    };
    // Append-only JSONL history in the user's data directory, plus the day/month/year reports.
    class ListenHistory : public ListenSink {
      public:
        explicit ListenHistory(std::filesystem::path directory);
        ~ListenHistory() override;
        ListenHistory(const ListenHistory&) = delete;
        ListenHistory& operator=(const ListenHistory&) = delete;

        void onTrackStart(const Track& track, std::string_view source) override;
        void onTrackEnd(const Track& track, double positionSeconds, bool completed) override;
        void setSource(std::string_view source) { pendingSource = std::string(source); }
        // Flushes whatever is in flight; safe to call repeatedly and on shutdown.
        void flush();

        std::vector<ListenEntry> entries(size_t limit = 0) const;
        ListenStats aggregate(ListenPeriod period, std::string_view key = {}) const;
        std::vector<std::string> availableKeys(ListenPeriod period) const;

      private:
        std::filesystem::path file;
        ListenEntry pending;
        bool pendingActive = false;
        std::string pendingSource = "unknown";
        void write(const ListenEntry& entry) const;
    };
    // One bounded projection shared by the terminal report and the model tool.
    Json listenStatsJson(const ListenStats& stats);
} // namespace mora
