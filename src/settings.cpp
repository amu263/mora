#include "settings.hpp"
#include <fstream>

namespace mora {
    namespace {
        constexpr std::array<const char*, 5> qualities{"standard", "higher", "exhigh", "lossless",
                                                       "hires"};
        constexpr size_t kMaxHistory = 40;
        bool validQuality(const std::string& quality) {
            for (auto known : qualities)
                if (quality == known)
                    return true;
            return false;
        }
    } // namespace
    Settings Settings::load(const std::filesystem::path& directory) {
        Settings settings;
        std::ifstream file(directory / L"settings.json", std::ios::binary);
        if (!file)
            return settings;
        auto data = Json::parse(file, nullptr, false);
        if (data.is_discarded() || !data.is_object())
            return settings;
        if (data.contains("volume") && data["volume"].is_number())
            settings.volume = std::clamp(data["volume"].get<double>(), 0.0, 1.0);
        if (data.contains("quality") && data["quality"].is_string()) {
            auto quality = data["quality"].get<std::string>();
            if (validQuality(quality))
                settings.quality = quality;
        }
        if (data.contains("repeat") && data["repeat"].is_boolean())
            settings.repeat = data["repeat"];
        // `repeat` predates explicit queue modes; it still decides the mode when no newer value
        // was stored, and every mode round-trips through the single `queueMode` field.
        settings.queueMode = settings.repeat ? QueueMode::RepeatAll : QueueMode::Sequential;
        if (data.contains("queueMode") && data["queueMode"].is_string()) {
            QueueMode parsed;
            if (parseQueueMode(data["queueMode"].get<std::string>(), parsed))
                settings.queueMode = parsed;
        }
        settings.repeat = settings.queueMode == QueueMode::RepeatAll ||
                          settings.queueMode == QueueMode::RepeatOne;
        if (data.contains("reactEnabled") && data["reactEnabled"].is_boolean())
            settings.reactEnabled = data["reactEnabled"];
        if (data.contains("reactMinSeconds") && data["reactMinSeconds"].is_number_integer())
            settings.reactMinSeconds = std::clamp(data["reactMinSeconds"].get<int>(), 10, 600);
        if (data.contains("reactMaxChars") && data["reactMaxChars"].is_number_integer())
            settings.reactMaxChars = std::clamp(data["reactMaxChars"].get<int>(), 10, 200);
        if (data.contains("contextLimitTokens") && data["contextLimitTokens"].is_number_integer())
            settings.contextLimitTokens =
                std::clamp(data["contextLimitTokens"].get<int>(), 0, 4000000);
        if (data.contains("autoCompress") && data["autoCompress"].is_boolean())
            settings.autoCompress = data["autoCompress"];
        if (data.contains("reviewChars") && data["reviewChars"].is_number_integer())
            settings.reviewChars = std::clamp(data["reviewChars"].get<int>(), 200, 4000);
        if (data.contains("motion") && data["motion"].is_boolean())
            settings.motion = data["motion"];
        if (data.contains("userName") && data["userName"].is_string()) {
            auto name = unutf8(data["userName"].get<std::string>());
            if (!name.empty() && name.size() <= 24 &&
                name.find_first_of(L"\r\n\t") == std::wstring::npos)
                settings.userName = std::move(name);
        }
        // Existing installations predate the guide; don't interrupt returning listeners.
        settings.guideDone = !settings.userName.empty();
        if (data.contains("guideDone") && data["guideDone"].is_boolean())
            settings.guideDone = data["guideDone"];
        if (data.contains("personaId") && data["personaId"].is_string()) {
            auto id = data["personaId"].get<std::string>();
            if (!id.empty() && id.size() <= 32)
                settings.personaId = id;
        }
        if (data.contains("outputDeviceId") && data["outputDeviceId"].is_number_integer())
            settings.outputDeviceId = std::clamp(data["outputDeviceId"].get<int>(), -1, 64);
        if (data.contains("history") && data["history"].is_array()) {
            for (auto& item : data["history"]) {
                if (!item.is_string() || settings.history.size() >= kMaxHistory)
                    break;
                auto entry = unutf8(item.get<std::string>());
                if (!entry.empty() && entry.size() <= 256)
                    settings.history.push_back(std::move(entry));
            }
        }
        return settings;
    }
    bool Settings::save(const std::filesystem::path& directory) const {
        Json data{{"version", 1},
                  {"volume", std::clamp(volume, 0.0, 1.0)},
                  {"quality", quality},
                  {"repeat", repeat},
                  {"queueMode", queueModeName(queueMode)},
                  {"reactEnabled", reactEnabled},
                  {"reactMinSeconds", std::clamp(reactMinSeconds, 10, 600)},
                  {"reactMaxChars", std::clamp(reactMaxChars, 10, 200)},
                  {"contextLimitTokens", std::clamp(contextLimitTokens, 0, 4000000)},
                  {"autoCompress", autoCompress},
                  {"reviewChars", std::clamp(reviewChars, 200, 4000)},
                  {"motion", motion},
                  {"userName", userName.empty() || userName.size() > 24 ? "" : utf8(userName)},
                  {"guideDone", guideDone},
                  {"personaId", personaId.size() > 32 ? "amu" : personaId},
                  {"outputDeviceId", std::clamp(outputDeviceId, -1, 64)},
                  {"history", Json::array()}};
        if (!validQuality(quality))
            data["quality"] = "lossless";
        size_t count = 0;
        for (auto& entry : history) {
            if (count++ >= kMaxHistory)
                break;
            data["history"].push_back(utf8(entry));
        }
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        auto path = directory / L"settings.json";
        auto temporary = directory / L"settings.tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file)
                return false;
            file << data.dump(2) << '\n';
            if (!file)
                return false;
        }
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
        return !error;
    }
} // namespace mora
