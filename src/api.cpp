#include "api.hpp"
#include "miniz_tinfl.h"
#include <fstream>
#include <thread>

namespace mora {
    static std::string timestamp() {
        return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count());
    }
    // LRC parsing lives in core.cpp so the pure text rules can be unit-tested without a network.
    std::string anonymousUsername(std::string_view device) {
        std::string xored(device), key = "3go8&$8*3*3h0k(2)2";
        for (size_t i = 0; i < xored.size(); ++i)
            xored[i] ^= key[i % key.size()];
        return base64(bytes(std::string(device) + " " + base64(hash(xored, BCRYPT_MD5_ALGORITHM))));
    }
    Bytes decompressResponse(const Bytes& data) {
        if (data.size() < 2 || data[0] != 0x1f || data[1] != 0x8b)
            return data;
        if (data.size() < 18 || data[2] != 8)
            throw std::runtime_error("Invalid compressed response");
        size_t start = 10;
        int flags = data[3];
        if (flags & 4) {
            if (start + 2 > data.size())
                throw std::runtime_error("Invalid gzip");
            size_t n = data[start] + 256 * data[start + 1];
            start += 2 + n;
        }
        for (int flag : {8, 16})
            if (flags & flag) {
                while (start < data.size() && data[start])
                    ++start;
                ++start;
            }
        if (flags & 2)
            start += 2;
        if (start >= data.size() - 8)
            throw std::runtime_error("Invalid gzip payload");
        uint32_t expected = 0;
        for (int i = 0; i < 4; ++i)
            expected |= uint32_t(data[data.size() - 4 + i]) << (8 * i);
        if (expected > 2 * 1024 * 1024)
            throw std::runtime_error("Compressed response exceeds memory budget");
        Bytes output(expected);
        size_t n = tinfl_decompress_mem_to_mem(output.data(), output.size(), data.data() + start,
                                               data.size() - start - 8, 0);
        if (n == (size_t)-1 || n != expected)
            throw std::runtime_error("Response decompression failed");
        return output;
    }
    Api::Api(std::filesystem::path dir, Transport adapter)
        : transport(std::move(adapter)), directory(std::move(dir)) {
        load();
    }
    Response Api::request(std::wstring url, std::string body, std::wstring headers, size_t cap,
                          bool post) {
        if (transport)
            return transport(std::move(url), std::move(body), std::move(headers), cap, post);
        return http.request(std::move(url), std::move(body), std::move(headers), cap, post);
    }
    void Api::load() {
        auto file = directory / L"session.bin";
        if (std::filesystem::exists(file)) {
            if (std::filesystem::file_size(file) > 65536)
                throw std::runtime_error("Session file too large");
            std::ifstream f(file, std::ios::binary);
            Bytes encrypted((std::istreambuf_iterator<char>(f)), {});
            auto clear = unprotect(encrypted);
            auto j = Json::parse(str(clear));
            device = j.at("device");
            cookies = j.value("cookies", Json::object());
            uid = j.value("uid", int64_t(0));
            nickname = unutf8(j.value("nickname", std::string("未登录")));
            SecureZeroMemory(clear.data(), clear.size());
        } else
            device = hex(randomBytes(16));
    }
    void Api::save() {
        std::filesystem::create_directories(directory);
        Json data = {
            {"device", device}, {"cookies", cookies}, {"uid", uid}, {"nickname", utf8(nickname)}};
        auto clear = bytes(data.dump());
        auto encrypted = protect(clear);
        SecureZeroMemory(clear.data(), clear.size());
        auto tmp = directory / L"session.tmp", file = directory / L"session.bin";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write((char*)encrypted.data(), encrypted.size());
            if (!f)
                throw std::runtime_error("Cannot save session");
        }
        if (!MoveFileExW(tmp.c_str(), file.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace session file");
    }
    void Api::mergeCookies(const Response& r) {
        bool changed = false;
        for (auto c : r.cookies) {
            auto end = c.find(';');
            c = c.substr(0, end);
            auto equal = c.find('=');
            if (equal == std::string::npos)
                continue;
            auto k = c.substr(0, equal), v = c.substr(equal + 1);
            if (k.empty() || k.size() > 100 || v.size() > 16000)
                continue;
            cookies[k] = v;
            changed = true;
        }
        if (changed)
            save();
    }
    std::string Api::cookieHeader(bool android) const {
        Json all = cookies;
        all["os"] = android ? "android" : "pc";
        all["appver"] = android ? "9.5.61" : "3.1.17.204416";
        all["deviceId"] = device;
        all["sDeviceId"] = device;
        all["osver"] = android ? "12" : "Microsoft-Windows-10-Professional-build-19045-64bit";
        all["channel"] = "netease";
        all["__remember_me"] = "true";
        std::string out;
        for (auto it = all.begin(); it != all.end(); ++it) {
            if (!it->is_string())
                continue;
            if (!out.empty())
                out += "; ";
            out += urlEncode(it.key()) + "=" + urlEncode(it->get<std::string>());
        }
        return out;
    }
    void Api::throttle(bool write) {
        auto now = Clock::now();
        if (now < blockedUntil)
            throw ApiError(429, "风控冷却中，五分钟后按 R 手动恢复；请勿连续登录");
        auto due = std::max(last + std::chrono::milliseconds(850),
                            write ? lastWrite + std::chrono::seconds(3) : last);
        if (now < due)
            std::this_thread::sleep_until(due);
        last = Clock::now();
        if (write)
            lastWrite = last;
    }
    bool Api::loggedIn() const {
        return cookies.contains("MUSIC_U") && cookies["MUSIC_U"].is_string() &&
               !cookies["MUSIC_U"].get<std::string>().empty();
    }
    Json Api::call(const std::string& path, Json data, bool web, bool write, bool modern) {
        if (modern && publicKey.empty())
            registerKey();
        throttle(write);
        bool android = modern;
        std::wstring headers = L"Referer: https://music.163.com/\r\nCookie: " +
                               unutf8(cookieHeader(android)) + L"\r\n";
        std::wstring url;
        std::string body;
        data["e_r"] = false;
        if (modern) {
            headers += L"User-Agent: NeteaseMusic/9.5.61.260802021928(9005061);Dalvik/2.1.0 "
                       L"(Linux; U; Android 12; HBN-AL00 Build/cd737a2.0)\r\nX-Client-Enc-State: "
                       L"ENCRYPTED\r\nx-aeapi: true\r\nx-deviceid: " +
                       unutf8(device) +
                       L"\r\nx-os: android\r\nx-osver: 12\r\nx-appver: 9.5.61\r\nx-sdeviceid: " +
                       unutf8(device) + L"\r\nx-buildver: " + unutf8(timestamp().substr(0, 10)) +
                       L"\r\n";
            if (loggedIn())
                headers += L"x-music-u: " + unutf8(cookies["MUSIC_U"].get<std::string>()) + L"\r\n";
            url = L"https://interface3.music.163.com/xeapi/" + unutf8(path.substr(5));
            body = form(xeapi(path, data, publicKey));
        } else if (web) {
            headers += L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                       L"Chrome/124.0.0.0 Safari/537.36\r\n";
            data["csrf_token"] = cookies.value("__csrf", std::string());
            url = L"https://music.163.com/weapi/" + unutf8(path.substr(5));
            body = form(weapi(data));
        } else {
            Json h = {{"os", "pc"},
                      {"appver", "3.1.17.204416"},
                      {"osver", "Microsoft-Windows-10-Professional-build-19045-64bit"},
                      {"deviceId", device},
                      {"channel", "netease"},
                      {"requestId", timestamp() + "_" + std::to_string(GetTickCount64() % 1000)},
                      {"__csrf", cookies.value("__csrf", std::string())},
                      {"versioncode", "140"},
                      {"resolution", "1920x1080"}};
            for (auto k : {"MUSIC_U", "MUSIC_A", "NMTID"})
                if (cookies.contains(k))
                    h[k] = cookies[k];
            data["header"] = h;
            headers += L"User-Agent: NeteaseMusicDesktop/3.1.17.204416 (Windows; MORA)\r\n";
            url = L"https://interfacepc.music.163.com/eapi/" + unutf8(path.substr(5));
            body = "params=" + eapi(path, data.dump());
        }
        auto response = request(url, body, headers);
        mergeCookies(response);
        if (isRiskCode(response.status)) {
            blockedUntil = Clock::now() + std::chrono::minutes(5);
            throw ApiError(response.status,
                           "服务端要求验证或限流，已暂停请求五分钟；可使用官方客户端完成验证");
        }
        if (response.status < 200 || response.status >= 300)
            throw ApiError(response.status, "网易云服务暂时不可用，请稍后手动重试");
        std::string text = response.body;
        if (modern && (!text.empty() && text.front() != '{'))
            text = str(decompressResponse(aes(bytes(text), bytes("e82ckenh8dichen8"), false)));
        auto result = Json::parse(text, nullptr, false);
        if (result.is_discarded() || !result.is_object())
            throw ApiError(0, "服务端响应格式已变化，请更新播放器");
        int code = result.value("code", 200);
        if (isRiskCode(code)) {
            blockedUntil = Clock::now() + std::chrono::minutes(5);
            throw ApiError(code,
                           "账号触发验证或限流，已暂停请求；请到官方客户端完成验证，五分钟后按 R");
        }
        if (code != 200 && code != 800 && code != 801 && code != 802 && code != 803) {
            if (code == 301 || code == 302)
                throw ApiError(code, "登录已失效，请重新扫码登录");
            if (code == 512)
                throw ApiError(code, "歌曲已在歌单中，或歌单不允许修改");
            throw ApiError(code,
                           "请求未成功（" + std::to_string(code) + "），请检查账号状态后重试");
        }
        return result;
    }
    void Api::registerKey() {
        throttle();
        auto t = timestamp(), nonce = std::to_string(GetTickCount64()) + hex(randomBytes(4));
        Json data = {{"appVersion", "9.5.61"},
                     {"currentKeyVersion", ""},
                     {"deviceId", device},
                     {"nonce", nonce},
                     {"os", "android"},
                     {"requestType", "active"},
                     {"signature", xeapiSign(t, nonce)},
                     {"t1", ""},
                     {"t2", ""},
                     {"timestamp", t},
                     {"uid", ""}};
        auto r = request(L"https://interface.music.163.com/api/bsr/sk/get", form(data),
                         L"User-Agent: NeteaseMusic/9.5.61.260802021928(9005061);Dalvik/2.1.0 "
                         L"(Linux; U; Android 12; HBN-AL00 Build/cd737a2.0)\r\nCookie: deviceId=" +
                             unutf8(device) + L"\r\n");
        if (isRiskCode(r.status)) {
            blockedUntil = Clock::now() + std::chrono::minutes(5);
            throw ApiError(r.status, "播放服务限流，已进入五分钟冷却");
        }
        auto j = Json::parse(r.body);
        if (isRiskCode(j.value("code", 0)))
            blockedUntil = Clock::now() + std::chrono::minutes(5);
        if (j.value("code", 0) != 200 || !j.contains("data"))
            throw ApiError(j.value("code", 0), "无法取得播放协议密钥，请稍后重试");
        auto d = j["data"];
        auto stamp =
            d["timestamp"].is_string() ? d["timestamp"].get<std::string>() : d["timestamp"].dump();
        if (xeapiSign(stamp, nonce) != d.value("signature", std::string()))
            throw std::runtime_error("播放密钥签名不匹配，已停止请求");
        publicKey = Json::parse(str(
            aes(unbase64(d.at("encryptedData")),
                unhex("ab1d5a430f6bb04a3f01e81ddd72bd916d5ce591248ac128714806d7f8fb1b84"), false)));
        if (!publicKey.contains("sk") || !publicKey.contains("publicKey"))
            throw std::runtime_error("播放密钥缺少字段");
    }
    void Api::account() {
        if (!loggedIn())
            return;
        auto j = call("/api/w/nuser/account/get", Json::object(), true);
        if (!j.contains("profile") || j["profile"].is_null()) {
            uid = 0;
            nickname = L"登录已失效";
            throw ApiError(301, "登录已失效，请重新扫码");
        }
        uid = j["profile"].value("userId", int64_t(0));
        nickname = sanitize(unutf8(j["profile"].value("nickname", std::string("网易云用户"))));
        save();
        auto liked = call("/api/song/like/get", Json{{"uid", uid}});
        likes.clear();
        if (liked.contains("ids"))
            for (auto& id : liked["ids"])
                likes.insert(id.get<int64_t>());
    }
    SearchPage Api::search(std::wstring query, Kind kind, int offset) {
        auto j = call("/api/cloudsearch/pc", Json{{"s", utf8(query)},
                                                  {"type", (int)kind},
                                                  {"limit", 30},
                                                  {"offset", offset},
                                                  {"total", true}});
        SearchPage page;
        auto result = j.value("result", Json::object());
        if (kind == Kind::Song) {
            page.items = parseTracks(result.value("songs", Json::array()));
            page.total = result.value("songCount", 0);
        } else {
            const char* key = kind == Kind::Album ? "albums" : "playlists";
            page.total = result.value(kind == Kind::Album ? "albumCount" : "playlistCount", 0);
            for (auto& row : result.value(key, Json::array())) {
                Track t;
                t.id = row.value("id", int64_t(0));
                t.name = sanitize(unutf8(row.value("name", std::string())));
                t.kind = kind;
                if (kind == Kind::Album) {
                    auto a = row.value("artist", Json::object());
                    t.artist = sanitize(unutf8(a.value("name", std::string())));
                    t.cover = row.value("picUrl", std::string());
                } else {
                    auto a = row.value("creator", Json::object());
                    t.artist = sanitize(unutf8(a.value("nickname", std::string())));
                    t.owner = a.value("userId", int64_t(0));
                    t.cover = row.value("coverImgUrl", std::string());
                }
                page.items.push_back(std::move(t));
            }
        }
        std::stable_sort(page.items.begin(), page.items.end(), [&](auto& a, auto& b) {
            return matchScore(a, query) > matchScore(b, query);
        });
        for (auto& t : page.items)
            t.liked = likes.contains(t.id);
        return page;
    }
    std::vector<Track> Api::details(const std::vector<int64_t>& ids) {
        Json rows = Json::array();
        for (auto id : ids) {
            if (rows.size() >= 50)
                break;
            rows.push_back(Json{{"id", id}});
        }
        auto j = call("/api/v3/song/detail", Json{{"c", rows.dump()}});
        auto result = parseTracks(j.value("songs", Json::array()));
        for (auto& t : result)
            t.liked = likes.contains(t.id);
        return result;
    }
    Lyrics Api::lyrics(int64_t id) {
        Lyrics out;
        out.songId = id;
        auto j = call("/api/song/lyric",
                      Json{{"id", id}, {"lv", -1}, {"kv", -1}, {"tv", -1}});
        out.instrumental = j.value("nolyric", false);
        out.unavailable = j.value("uncollected", false);
        if (out.instrumental || out.unavailable)
            return out;
        auto plain = j.contains("lrc") && j["lrc"].is_object()
                         ? j["lrc"].value("lyric", std::string())
                         : std::string();
        auto translated = j.contains("tlyric") && j["tlyric"].is_object()
                              ? j["tlyric"].value("lyric", std::string())
                              : std::string();
        out.lines = parseLrcTimed(plain, translated);
        // A track with no timed lines is either instrumental or simply has no lyrics on file.
        if (out.lines.empty())
            out.instrumental = plain.empty();
        return out;
    }
    std::vector<Track> Api::album(int64_t id) {
        auto j = call("/api/v1/album/" + std::to_string(id), Json::object(), true);
        auto r = parseTracks(j.value("songs", Json::array()));
        if (r.size() > 10000)
            r.resize(10000);
        return r;
    }
    std::vector<Track> Api::playlist(int64_t id) {
        auto j = call("/api/v6/playlist/detail", Json{{"id", id}, {"n", 0}, {"s", 0}});
        std::vector<Track> out;
        if (!j.contains("playlist") || j["playlist"].is_null())
            return out;
        for (auto& row : j["playlist"].value("trackIds", Json::array())) {
            if (out.size() >= 10000)
                break;
            Track t;
            t.id = row.at("id");
            out.push_back(t);
        }
        if (!out.empty()) {
            std::vector<int64_t> ids;
            for (size_t i = 0; i < std::min(size_t(50), out.size()); ++i)
                ids.push_back(out[i].id);
            auto loaded = details(ids);
            std::map<int64_t, Track> index;
            for (auto& t : loaded)
                index[t.id] = std::move(t);
            for (auto& t : out)
                if (index.contains(t.id))
                    t = index[t.id];
        }
        return out;
    }
    Playback Api::playback(Track track, std::string quality) {
        if (!loggedIn() && (!cookies.contains("MUSIC_A") || cookies["MUSIC_A"] == ""))
            call("/api/register/anonimous", Json{{"username", anonymousUsername(device)}}, false,
                 false, true);
        if (track.name.empty()) {
            auto d = details({track.id});
            if (d.empty())
                throw ApiError(404, "歌曲元数据不可用");
            track = d.front();
        }
        auto j = call("/api/song/enhance/player/url/v1",
                      Json{{"ids", "[" + std::to_string(track.id) + "]"},
                           {"level", quality},
                           {"encodeType", "flac"}},
                      false, false, true);
        if (!j.contains("data") || j["data"].empty() || !playable(j["data"][0]))
            throw ApiError(403, "当前账号没有完整播放权限，或歌曲不可用；请登录有权限的账号");
        auto row = j["data"][0];
        Playback p;
        p.track = track;
        p.url = unutf8(row.at("url").get<std::string>());
        if (p.url.starts_with(L"http://"))
            p.url = L"https://" + p.url.substr(7);
        p.quality = row.value("level", quality);
        p.bitrate = row.value("br", 0);
        if (!track.cover.empty())
            try {
                auto cover = track.cover;
                if (cover.starts_with("http://"))
                    cover = "https://" + cover.substr(7);
                cover += (cover.find('?') == std::string::npos ? "?" : "&");
                cover += "param=96y96";
                auto r = request(unutf8(cover), {}, {}, 128 * 1024, false);
                if (r.status == 200)
                    p.cover = decodeCover(bytes(r.body));
            } catch (...) {
            }
        return p;
    }
    std::string Api::qrKey() {
        auto j = call("/api/login/qrcode/unikey", Json{{"type", 3}});
        return j.at("unikey").get<std::string>();
    }
    int Api::qrCheck(const std::string& key) {
        auto j = call("/api/login/qrcode/client/login", Json{{"key", key}, {"type", 3}});
        int code = j.value("code", 0);
        if (code == 803) {
            if (!loggedIn())
                throw ApiError(301, "扫码成功但会话未建立，请重新扫码");
            account();
        }
        return code;
    }
    void Api::sendSms(std::string phone, std::string country) {
        if (Clock::now() < lastSms + std::chrono::seconds(60))
            throw ApiError(429, "验证码发送间隔至少 60 秒");
        lastSms = Clock::now();
        call("/api/sms/captcha/sent",
             Json{{"ctcode", country},
                  {"secrete", "music_middleuser_pclogin"},
                  {"cellphone", phone}},
             true, true);
    }
    void Api::phoneLogin(std::string phone, std::string captcha, std::string country) {
        call("/api/w/login/cellphone",
             Json{{"type", "1"},
                  {"https", "true"},
                  {"phone", phone},
                  {"countrycode", country},
                  {"captcha", captcha},
                  {"remember", "true"},
                  {"secureCaptcha", ""}},
             true, true);
        if (!loggedIn())
            throw ApiError(301, "手机登录未取得会话，请尝试扫码登录");
        account();
    }
    std::vector<Track> Api::myPlaylists(bool owned) {
        if (!loggedIn())
            throw ApiError(301, "请先按 L 扫码登录");
        if (!uid)
            account();
        auto j =
            call("/api/user/playlist",
                 Json{{"uid", uid}, {"limit", 100}, {"offset", 0}, {"includeVideo", false}}, true);
        std::vector<Track> out;
        for (auto& row : j.value("playlist", Json::array())) {
            auto creator = row.value("creator", Json::object());
            int64_t owner = creator.value("userId", int64_t(0));
            if (owned && owner != uid)
                continue;
            Track t;
            t.id = row.at("id");
            t.name = sanitize(unutf8(row.value("name", std::string())));
            t.artist = sanitize(unutf8(creator.value("nickname", std::string())));
            t.cover = row.value("coverImgUrl", std::string());
            t.kind = Kind::Playlist;
            t.owner = owner;
            out.push_back(std::move(t));
        }
        return out;
    }
    void Api::like(int64_t id, bool value) {
        if (!loggedIn())
            throw ApiError(301, "请先按 L 登录");
        call("/api/radio/like",
             Json{{"alg", "itembased"}, {"trackId", id}, {"like", value}, {"time", "3"}}, true,
             true);
        if (value)
            likes.insert(id);
        else
            likes.erase(id);
    }
    void Api::add(int64_t song, int64_t playlist) {
        if (!loggedIn())
            throw ApiError(301, "请先按 L 登录");
        call("/api/playlist/manipulate/tracks",
             Json{{"op", "add"},
                  {"pid", playlist},
                  {"trackIds", "[" + std::to_string(song) + "]"},
                  {"imme", "true"}},
             false, true);
    }
    void Api::logout() {
        cookies = Json::object();
        likes.clear();
        uid = 0;
        nickname = L"未登录";
        save();
    }
    void Api::resume() {
        if (Clock::now() < blockedUntil)
            throw ApiError(429, "冷却尚未结束，请稍后重试");
        blockedUntil = {};
    }
} // namespace mora
