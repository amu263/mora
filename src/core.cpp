#include "core.hpp"
#include <cwctype>
#include <fstream>
#include <numeric>
#include <random>
#include <sstream>

namespace mora {
    std::string utf8(std::wstring_view text) {
        if (text.empty())
            return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr,
                                    nullptr);
        std::string result(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), result.data(), n, nullptr,
                            nullptr);
        return result;
    }
    std::wstring unutf8(std::string_view text) {
        if (text.empty())
            return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
        std::wstring result(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), result.data(), n);
        return result;
    }
    std::wstring sanitize(std::wstring text) {
        for (auto& c : text) {
            if (c == L'\x1b')
                c = 0;
            else if (c < 32 || (c >= 127 && c < 160))
                c = L' ';
        }
        text.erase(std::remove(text.begin(), text.end(), 0), text.end());
        return text;
    }
    static int cellWidth(wchar_t c) {
        if (c >= 0x300 && c <= 0x36f)
            return 0;
        return (c >= 0x1100 &&
                ((c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf) || (c >= 0xac00 && c <= 0xd7a3) ||
                 (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe10 && c <= 0xfe6f) ||
                 (c >= 0xff01 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6)))
                   ? 2
                   : 1;
    }
    int displayWidth(std::wstring_view text) {
        int n = 0;
        for (auto c : text)
            n += cellWidth(c);
        return n;
    }
    std::wstring truncate(std::wstring_view text, int width) {
        if (width <= 0)
            return {};
        if (displayWidth(text) <= width)
            return std::wstring(text);
        std::wstring out;
        int n = 0;
        for (auto c : text) {
            int k = cellWidth(c);
            if (n + k > width - 1)
                break;
            out += c;
            n += k;
        }
        return out + L"…";
    }
    std::wstring pad(std::wstring_view text, int width) {
        auto s = truncate(text, width);
        return s + std::wstring(std::max(0, width - displayWidth(s)), L' ');
    }
    std::wstring timeText(double seconds) {
        int s = std::max(0, (int)seconds);
        wchar_t b[32];
        swprintf(b, 32, L"%02d:%02d", s / 60, s % 60);
        return b;
    }
    size_t editDistance(std::wstring_view a, std::wstring_view b) {
        a = a.substr(0, 256);
        b = b.substr(0, 256);
        std::vector<size_t> row(b.size() + 1);
        for (size_t i = 0; i < row.size(); ++i)
            row[i] = i;
        for (size_t i = 0; i < a.size(); ++i) {
            size_t prev = row[0];
            row[0] = i + 1;
            for (size_t j = 0; j < b.size(); ++j) {
                size_t old = row[j + 1];
                row[j + 1] = std::min({row[j + 1] + 1, row[j] + 1, prev + (a[i] != b[j])});
                prev = old;
            }
        }
        return row.back();
    }
    static std::wstring jtext(const Json& j, const char* key) {
        auto it = j.find(key);
        return it != j.end() && it->is_string() ? sanitize(unutf8(it->get<std::string>())) : L"";
    }
    Track parseTrack(const Json& item) {
        Track t;
        t.id = item.value("id", int64_t(0));
        t.name = jtext(item, "name");
        auto ar = item.find("ar");
        if (ar == item.end())
            ar = item.find("artists");
        if (ar != item.end() && ar->is_array())
            for (auto& a : *ar) {
                if (!t.artist.empty())
                    t.artist += L" / ";
                t.artist += jtext(a, "name");
            }
        auto al = item.find("al");
        if (al == item.end())
            al = item.find("album");
        if (al != item.end() && al->is_object()) {
            t.album = jtext(*al, "name");
            if (al->contains("picUrl") && (*al)["picUrl"].is_string())
                t.cover = (*al)["picUrl"].get<std::string>();
        }
        auto dt = item.find("dt");
        if (dt == item.end() || !dt->is_number())
            dt = item.find("duration");
        if (dt != item.end() && dt->is_number())
            t.duration = (int)(dt->get<int64_t>() / 1000);
        return t;
    }
    Json trackJson(const Track& track) {
        return Json{{"id", track.id},
                    {"name", utf8(track.name)},
                    {"artist", utf8(track.artist)},
                    {"album", utf8(track.album)},
                    {"durationSeconds", track.duration}};
    }
    namespace {
        // LRC lines look like "[00:12.34]text" and may carry several timestamps for one lyric, which
        // is how repeated choruses are written. Tags such as [ti:...] are metadata, not lyrics.
        void collectLrc(const std::string& text, std::map<int, std::wstring>& out) {
            for (size_t start = 0; start < text.size();) {
                auto end = text.find('\n', start);
                if (end == std::string::npos)
                    end = text.size();
                std::string line = text.substr(start, end - start);
                start = end + 1;
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                size_t cursor = 0;
                std::vector<double> stamps;
                while (cursor < line.size() && line[cursor] == '[') {
                    auto close = line.find(']', cursor);
                    if (close == std::string::npos)
                        break;
                    auto body = line.substr(cursor + 1, close - cursor - 1);
                    auto colon = body.find(':');
                    bool numeric = colon != std::string::npos && colon > 0;
                    for (size_t i = 0; numeric && i < body.size(); ++i)
                        if (i != colon && !isdigit(static_cast<unsigned char>(body[i])) &&
                            body[i] != '.' && body[i] != '-')
                            numeric = false;
                    if (!numeric)
                        break; // a metadata tag such as [ti:...]: the rest of the line is not text
                    double seconds = 0;
                    try {
                        seconds = std::stod(body.substr(0, colon)) * 60 +
                                  std::stod(body.substr(colon + 1));
                    } catch (...) {
                        break;
                    }
                    stamps.push_back(seconds);
                    cursor = close + 1;
                }
                if (stamps.empty())
                    continue;
                auto content = sanitize(unutf8(line.substr(cursor)));
                if (content.empty())
                    continue;
                if (content.size() > 200)
                    content.resize(200);
                for (double seconds : stamps)
                    out[static_cast<int>(seconds * 100.0 + .5)] = content;
            }
        }
    } // namespace
    std::vector<LyricLine> parseLrcTimed(const std::string& plain,
                                        const std::string& translated) {
        std::map<int, std::wstring> primary, secondary;
        collectLrc(plain, primary);
        collectLrc(translated, secondary);
        std::vector<LyricLine> lines;
        lines.reserve(primary.size());
        for (auto& [key, text] : primary) {
            LyricLine line;
            line.seconds = key / 100.0;
            line.text = text;
            if (auto found = secondary.find(key); found != secondary.end())
                line.translation = found->second;
            lines.push_back(std::move(line));
            if (lines.size() >= 400)
                break;
        }
        return lines;
    }
    int lyricIndexAt(const Lyrics& lyrics, double position) {
        int index = -1;
        for (size_t i = 0; i < lyrics.lines.size(); ++i) {
            if (lyrics.lines[i].seconds > position + .05)
                break;
            index = static_cast<int>(i);
        }
        return index;
    }
    Json lyricsJson(const Lyrics& lyrics, int64_t songId, const Track* track, double position,
                    bool includeTranslation, size_t maxLines) {
        int current = lyricIndexAt(lyrics, position);
        // The caller usually wants the neighbourhood of the live line rather than the whole song,
        // so the window starts one line before it and never overruns the end.
        size_t begin = 0;
        if (current >= 0 && lyrics.lines.size() > maxLines) {
            int lead = std::max(1, static_cast<int>(maxLines) / 4);
            begin = static_cast<size_t>(std::max(0, current - lead));
            begin = std::min(begin, lyrics.lines.size() - maxLines);
        }
        Json lines = Json::array();
        bool hasTranslation = false;
        for (size_t i = begin; i < lyrics.lines.size() && lines.size() < maxLines; ++i) {
            const auto& line = lyrics.lines[i];
            Json item = {{"seconds", line.seconds}, {"text", utf8(line.text)}};
            if (includeTranslation && !line.translation.empty()) {
                item["translation"] = utf8(line.translation);
                hasTranslation = true;
            }
            lines.push_back(std::move(item));
        }
        Json result = {{"songId", lyrics.songId ? lyrics.songId : songId},
                       {"instrumental", lyrics.instrumental},
                       {"unavailable", lyrics.unavailable},
                       {"hasTranslation", hasTranslation},
                       {"lineCount", static_cast<int64_t>(lyrics.lines.size())},
                       {"fromLine", static_cast<int64_t>(begin)},
                       {"truncated", begin > 0 || begin + lines.size() < lyrics.lines.size()},
                       {"positionSeconds", position},
                       {"currentIndex", current},
                       {"lines", std::move(lines)}};
        if (current >= 0 && static_cast<size_t>(current) < lyrics.lines.size()) {
            result["currentLine"] = utf8(lyrics.lines[(size_t)current].text);
            if (current + 1 < static_cast<int>(lyrics.lines.size()))
                result["nextLine"] = utf8(lyrics.lines[(size_t)current + 1].text);
        }
        if (track) {
            result["title"] = utf8(track->name);
            result["artist"] = utf8(track->artist);
            result["album"] = utf8(track->album);
        }
        return result;
    }
    std::vector<Track> parseTracks(const Json& items) {
        std::vector<Track> out;
        if (items.is_array())
            for (auto& j : items) {
                auto t = parseTrack(j);
                if (t.id)
                    out.push_back(std::move(t));
            }
        return out;
    }
    double matchScore(const Track& t, std::wstring_view query) {
        auto lower = [](std::wstring s) {
            for (auto& c : s)
                c = (wchar_t)towlower(c);
            return s;
        };
        std::array fields{lower(t.name), lower(t.artist), lower(t.album)};
        std::wistringstream input(lower(std::wstring(query)));
        std::wstring word;
        double result = 0;
        while (input >> word) {
            double best = 0;
            for (auto& f : fields) {
                if (f.empty())
                    continue;
                if (f.find(word) != std::wstring::npos)
                    best = std::max(best, 2.0);
                else
                    best = std::max(best, 1.0 - double(editDistance(f, word)) /
                                                    std::max(f.size(), word.size()));
            }
            result += best;
        }
        return result;
    }
    bool playable(const Json& j) {
        return j.contains("url") && j["url"].is_string() && !j["url"].get<std::string>().empty() &&
               (!j.contains("freeTrialInfo") || j["freeTrialInfo"].is_null() ||
                j["freeTrialInfo"] == "null");
    }
    bool isRiskCode(int c) {
        return c == 405 || c == 429 || c == 460 || c == 461 || c == 462 || c == -460 || c == -461 ||
               c == -462 || c == 509;
    }
    const char* queueModeName(QueueMode mode) {
        switch (mode) {
        case QueueMode::RepeatAll:
            return "repeat_all";
        case QueueMode::RepeatOne:
            return "repeat_one";
        case QueueMode::Shuffle:
            return "shuffle";
        default:
            return "sequential";
        }
    }
    bool parseQueueMode(std::string_view value, QueueMode& out) {
        for (auto mode : queueModes()) {
            if (value == queueModeName(mode)) {
                out = mode;
                return true;
            }
        }
        // Accept the words a listener or a model is most likely to use.
        if (value == "sequence" || value == "order" || value == "normal") {
            out = QueueMode::Sequential;
            return true;
        }
        if (value == "repeat" || value == "loop" || value == "loop_all") {
            out = QueueMode::RepeatAll;
            return true;
        }
        if (value == "single" || value == "loop_one" || value == "one") {
            out = QueueMode::RepeatOne;
            return true;
        }
        if (value == "random" || value == "shuffle_all") {
            out = QueueMode::Shuffle;
            return true;
        }
        return false;
    }
    const std::vector<QueueMode>& queueModes() {
        static const std::vector<QueueMode> modes{QueueMode::Sequential, QueueMode::RepeatAll,
                                                  QueueMode::RepeatOne, QueueMode::Shuffle};
        return modes;
    }
    void Queue::assign(std::vector<Track> items, size_t selected) {
        tracks = std::move(items);
        index = tracks.empty() ? 0 : std::min(selected, tracks.size() - 1);
        if (mode == QueueMode::Shuffle)
            rebuildShuffle(1);
    }
    Track* Queue::current() {
        return index < tracks.size() ? &tracks[index] : nullptr;
    }
    void Queue::rebuildShuffle(uint64_t seed) {
        shuffleOrder.resize(tracks.size());
        for (size_t i = 0; i < shuffleOrder.size(); ++i)
            shuffleOrder[i] = i;
        std::mt19937 rng(static_cast<uint32_t>(seed ? seed : 1));
        std::shuffle(shuffleOrder.begin(), shuffleOrder.end(), rng);
        shuffleCursor = 0;
        // Enabling shuffle mid-track must not restart the song the listener is hearing.
        if (index < shuffleOrder.size()) {
            auto found = std::find(shuffleOrder.begin(), shuffleOrder.end(), index);
            if (found != shuffleOrder.end())
                std::iter_swap(shuffleOrder.begin(), found);
        }
    }
    void Queue::setMode(QueueMode value, uint64_t seed) {
        mode = value;
        if (mode == QueueMode::Shuffle)
            rebuildShuffle(seed);
        else
            shuffleOrder.clear(), shuffleCursor = 0;
    }
    void Queue::setRepeat(bool value) {
        setMode(value ? QueueMode::RepeatAll : QueueMode::Sequential);
    }
    bool Queue::next() {
        if (tracks.empty())
            return false;
        switch (mode) {
        case QueueMode::RepeatOne:
            return true; // same row: the caller replays the track it already owns
        case QueueMode::Shuffle: {
            if (shuffleOrder.size() != tracks.size())
                rebuildShuffle(1);
            if (shuffleCursor + 1 < shuffleOrder.size()) {
                index = shuffleOrder[++shuffleCursor];
                return true;
            }
            // The pass is over. Wrap over the *same* permutation instead of reshuffling: a shuffle
            // queue still keeps playing, and next()/previous() stay invertible so a failed playback
            // can roll back one step without silently changing the order the caller asked for.
            shuffleCursor = 0;
            index = shuffleOrder[0];
            return true;
        }
        default:
            break;
        }
        if (index + 1 < tracks.size()) {
            ++index;
            return true;
        }
        if (mode == QueueMode::RepeatAll) {
            index = 0;
            return true;
        }
        return false;
    }
    bool Queue::previous() {
        if (tracks.empty())
            return false;
        if (mode == QueueMode::RepeatOne)
            return true;
        if (mode == QueueMode::Shuffle) {
            if (shuffleOrder.size() != tracks.size())
                rebuildShuffle(1);
            if (shuffleCursor > 0) {
                index = shuffleOrder[--shuffleCursor];
                return true;
            }
            return false;
        }
        if (index > 0) {
            --index;
            return true;
        }
        if (mode == QueueMode::RepeatAll) {
            index = tracks.size() - 1;
            return true;
        }
        return false;
    }
    bool Queue::setIndex(size_t value) {
        if (value >= tracks.size() || value == index)
            return false;
        index = value;
        if (mode == QueueMode::Shuffle) {
            auto found = std::find(shuffleOrder.begin(), shuffleOrder.end(), index);
            if (found != shuffleOrder.end())
                shuffleCursor = static_cast<size_t>(found - shuffleOrder.begin());
        }
        return true;
    }
    bool Queue::removeAt(size_t value) {
        if (value >= tracks.size())
            return false;
        tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(value));
        if (index > value)
            --index;
        else if (index >= tracks.size())
            index = tracks.empty() ? 0 : tracks.size() - 1;
        if (mode == QueueMode::Shuffle)
            rebuildShuffle(1);
        return true;
    }
    bool Queue::moveTrack(size_t from, size_t to) {
        if (from >= tracks.size() || to >= tracks.size() || from == to)
            return false;
        Track moved = std::move(tracks[from]);
        tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(from));
        tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
        // Follow the row the listener is hearing instead of its old slot number.
        if (index == from)
            index = to;
        else if (from < index && to >= index)
            --index;
        else if (from > index && to <= index)
            ++index;
        if (mode == QueueMode::Shuffle)
            rebuildShuffle(1);
        return true;
    }
    void Queue::clear() {
        tracks.clear();
        shuffleOrder.clear();
        shuffleCursor = 0;
        index = 0;
    }
    bool Queue::append(std::vector<Track> items) {
        bool added = false;
        for (auto& item : items) {
            if (!item.id)
                continue;
            tracks.push_back(std::move(item));
            added = true;
        }
        if (added && mode == QueueMode::Shuffle)
            rebuildShuffle(1);
        return added;
    }
    bool Queue::insertNext(std::vector<Track> items) {
        if (tracks.empty()) {
            assign(std::move(items));
            return !tracks.empty();
        }
        size_t at = std::min(index + 1, tracks.size());
        bool added = false;
        for (auto& item : items) {
            if (!item.id)
                continue;
            tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(at), std::move(item));
            ++at;
            added = true;
        }
        if (added && mode == QueueMode::Shuffle)
            rebuildShuffle(1);
        return added;
    }
    static double luminance(RGB c) {
        auto f = [](int x) {
            double v = x / 255.0;
            return v <= .04045 ? v / 12.92 : pow((v + .055) / 1.055, 2.4);
        };
        return .2126 * f(c.r) + .7152 * f(c.g) + .0722 * f(c.b);
    }
    double contrast(RGB a, RGB b) {
        double x = luminance(a), y = luminance(b);
        return (std::max(x, y) + .05) / (std::min(x, y) + .05);
    }
    Palette palette(const Cover& c) {
        Palette p;
        if (c.pixels.empty())
            return p;
        struct Bin {
            int n = 0;
            double r = 0, g = 0, b = 0;
        };
        std::array<Bin, 512> bins{};
        for (size_t i = 0; i + 2 < c.pixels.size(); i += 3) {
            int r = c.pixels[i], g = c.pixels[i + 1], b = c.pixels[i + 2];
            int hi = std::max({r, g, b}), lo = std::min({r, g, b});
            if (hi < 35 || lo > 230)
                continue;
            auto& v = bins[(r / 32) * 64 + (g / 32) * 8 + b / 32];
            ++v.n;
            v.r += r;
            v.g += g;
            v.b += b;
        }
        auto it =
            std::max_element(bins.begin(), bins.end(), [](auto& a, auto& b) { return a.n < b.n; });
        if (!it->n)
            return p;
        RGB seed{int(it->r / it->n), int(it->g / it->n), int(it->b / it->n)};
        auto blend = [](RGB a, RGB b, double x) {
            return RGB{int(a.r * (1 - x) + b.r * x), int(a.g * (1 - x) + b.g * x),
                       int(a.b * (1 - x) + b.b * x)};
        };
        p.background = blend(seed, {15, 18, 24}, .91);
        p.surface = blend(seed, {27, 30, 37}, .85);
        p.accent = blend(seed, {225, 229, 220}, .58);
        p.muted = blend(seed, {168, 178, 183}, .82);
        return p;
    }
    void writeTestTone(const std::filesystem::path& path, int seconds, int rate, int bits) {
        std::ofstream f(path, std::ios::binary);
        uint32_t size = rate * (bits / 8) * seconds;
        auto w16 = [&](uint16_t n) { f.write((char*)&n, 2); };
        auto w32 = [&](uint32_t n) { f.write((char*)&n, 4); };
        f.write("RIFF", 4);
        w32(size + 36);
        f.write("WAVEfmt ", 8);
        w32(16);
        w16(1);
        w16(1);
        w32(rate);
        w32(rate * (bits / 8));
        w16(bits / 8);
        w16(bits);
        f.write("data", 4);
        w32(size);
        for (uint32_t i = 0; i < (uint32_t)(rate * seconds); ++i) {
            int32_t value =
                (int32_t)(sin(i * 440.0 * 6.283185307 / rate) * 500 * (bits == 24 ? 256 : 1));
            f.write((char*)&value, bits / 8);
        }
    }
} // namespace mora
