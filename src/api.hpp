#pragma once
#include "platform.hpp"
#include <functional>
#include <map>
#include <set>

namespace mora {
    struct ApiError : std::runtime_error {
        int code;
        ApiError(int c, std::string message) : std::runtime_error(std::move(message)), code(c) {}
    };
    struct SearchPage {
        std::vector<Track> items;
        int total = 0;
    };
    struct Playback {
        Track track;
        std::wstring url;
        std::string quality;
        int bitrate = 0;
        Cover cover;
    };
    class Api {
      public:
        using Transport =
            std::function<Response(std::wstring, std::string, std::wstring, size_t, bool)>;

      private:
        Http http;
        Transport transport;
        Json cookies = Json::object(), publicKey;
        std::string device;
        std::filesystem::path directory;
        Clock::time_point last{}, lastWrite{}, lastSms{}, blockedUntil{};
        void load();
        void save();
        void mergeCookies(const Response& r);
        std::string cookieHeader(bool android = false) const;
        void throttle(bool write = false);
        Json call(const std::string& path, Json data, bool web = false, bool write = false,
                  bool modern = false);
        void registerKey();
        Response request(std::wstring url, std::string body = {}, std::wstring headers = {},
                         size_t cap = 2 * 1024 * 1024, bool post = true);

      public:
        int64_t uid = 0;
        std::wstring nickname = L"未登录";
        std::set<int64_t> likes;
        explicit Api(std::filesystem::path dataDir = stateDirectory(), Transport adapter = {});
        Api(const Api&) = delete;
        Api& operator=(const Api&) = delete;
        bool loggedIn() const;
        void account();
        SearchPage search(std::wstring query, Kind kind, int offset = 0);
        std::vector<Track> album(int64_t id);
        std::vector<Track> playlist(int64_t id);
        std::vector<Track> details(const std::vector<int64_t>& ids);
        Playback playback(Track track, std::string quality);
        Lyrics lyrics(int64_t id);
        std::string qrKey();
        int qrCheck(const std::string& key);
        void sendSms(std::string phone, std::string country = "86");
        void phoneLogin(std::string phone, std::string captcha, std::string country = "86");
        std::vector<Track> myPlaylists(bool ownedOnly = false);
        void like(int64_t id, bool value);
        void add(int64_t song, int64_t playlist);
        void logout();
        void resume();
    };
    Bytes decompressResponse(const Bytes& data);
    std::string anonymousUsername(std::string_view device);
} // namespace mora
