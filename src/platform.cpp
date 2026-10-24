#include "platform.hpp"
#include "monocypher.h"
#include <iomanip>
#include <psapi.h>
#include <shlobj.h>
#include <sstream>
#include <wincodec.h>
#include <wincrypt.h>

namespace mora {
    void hrcheck(HRESULT r, const char* op) {
        if (FAILED(r)) {
            char b[160];
            snprintf(b, sizeof(b), "%s (0x%08lX)", op, (unsigned long)r);
            throw std::runtime_error(b);
        }
    }
    Bytes bytes(std::string_view s) {
        return Bytes(s.begin(), s.end());
    }
    std::string str(const Bytes& b) {
        return std::string(b.begin(), b.end());
    }
    std::string hex(const Bytes& b, bool upper) {
        const char* h = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        std::string s;
        s.reserve(b.size() * 2);
        for (auto x : b) {
            s += h[x >> 4];
            s += h[x & 15];
        }
        return s;
    }
    Bytes unhex(std::string_view s) {
        if (s.size() % 2)
            throw std::runtime_error("Invalid hex");
        Bytes b;
        for (size_t i = 0; i < s.size(); i += 2)
            b.push_back((uint8_t)std::stoul(std::string(s.substr(i, 2)), nullptr, 16));
        return b;
    }
    Bytes randomBytes(size_t n) {
        Bytes b(n);
        hrcheck(BCryptGenRandom(nullptr, b.data(), (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG),
                "Secure random");
        return b;
    }
    struct Alg {
        BCRYPT_ALG_HANDLE h = nullptr;
        ~Alg() {
            if (h)
                BCryptCloseAlgorithmProvider(h, 0);
        }
    };
    struct Key {
        BCRYPT_KEY_HANDLE h = nullptr;
        ~Key() {
            if (h)
                BCryptDestroyKey(h);
        }
    };
    Bytes hash(std::string_view s, LPCWSTR algorithm, const Bytes& key) {
        Alg a;
        hrcheck(BCryptOpenAlgorithmProvider(&a.h, algorithm, nullptr,
                                            key.empty() ? 0 : BCRYPT_ALG_HANDLE_HMAC_FLAG),
                "Hash algorithm");
        ULONG n = 0, k = 0;
        hrcheck(BCryptGetProperty(a.h, BCRYPT_HASH_LENGTH, (PUCHAR)&n, sizeof(n), &k, 0),
                "Hash size");
        Bytes out(n);
        hrcheck(BCryptHash(a.h, (PUCHAR)key.data(), (ULONG)key.size(), (PUCHAR)s.data(),
                           (ULONG)s.size(), out.data(), n),
                "Hash");
        return out;
    }
    Bytes aes(const Bytes& data, const Bytes& key, bool encrypt, const Bytes& iv) {
        Alg a;
        Key k;
        hrcheck(BCryptOpenAlgorithmProvider(&a.h, BCRYPT_AES_ALGORITHM, nullptr, 0),
                "AES algorithm");
        auto mode = iv.empty() ? BCRYPT_CHAIN_MODE_ECB : BCRYPT_CHAIN_MODE_CBC;
        hrcheck(BCryptSetProperty(a.h, BCRYPT_CHAINING_MODE, (PUCHAR)mode,
                                  (ULONG)((wcslen(mode) + 1) * sizeof(wchar_t)), 0),
                "AES mode");
        hrcheck(BCryptGenerateSymmetricKey(a.h, &k.h, nullptr, 0, (PUCHAR)key.data(),
                                           (ULONG)key.size(), 0),
                "AES key");
        Bytes out(data.size() + 32), initial = iv;
        ULONG n = 0;
        auto func = encrypt ? BCryptEncrypt : BCryptDecrypt;
        hrcheck(func(k.h, (PUCHAR)data.data(), (ULONG)data.size(), nullptr,
                     initial.empty() ? nullptr : initial.data(), (ULONG)initial.size(), out.data(),
                     (ULONG)out.size(), &n, BCRYPT_BLOCK_PADDING),
                "AES transform");
        out.resize(n);
        return out;
    }
    Bytes aesGcm(const Bytes& data, const Bytes& key, const Bytes& iv) {
        Alg a;
        Key k;
        hrcheck(BCryptOpenAlgorithmProvider(&a.h, BCRYPT_AES_ALGORITHM, nullptr, 0),
                "GCM algorithm");
        hrcheck(BCryptSetProperty(a.h, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                                  sizeof(BCRYPT_CHAIN_MODE_GCM), 0),
                "GCM mode");
        hrcheck(BCryptGenerateSymmetricKey(a.h, &k.h, nullptr, 0, (PUCHAR)key.data(),
                                           (ULONG)key.size(), 0),
                "GCM key");
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        Bytes tag(16), out(data.size());
        ULONG n = 0;
        info.pbNonce = (PUCHAR)iv.data();
        info.cbNonce = (ULONG)iv.size();
        info.pbTag = tag.data();
        info.cbTag = 16;
        hrcheck(BCryptEncrypt(k.h, (PUCHAR)data.data(), (ULONG)data.size(), &info, nullptr, 0,
                              out.data(), (ULONG)out.size(), &n, 0),
                "GCM encrypt");
        out.insert(out.end(), tag.begin(), tag.end());
        return out;
    }
    std::string base64(const Bytes& b, bool url) {
        DWORD n = 0;
        CryptBinaryToStringA(b.data(), (DWORD)b.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                             nullptr, &n);
        std::string s(n, 0);
        if (!CryptBinaryToStringA(b.data(), (DWORD)b.size(),
                                  CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, s.data(), &n))
            throw std::runtime_error("Base64 encode");
        s.resize(n);
        if (!s.empty() && s.back() == 0)
            s.pop_back();
        if (url) {
            for (auto& c : s) {
                if (c == '+')
                    c = '-';
                if (c == '/')
                    c = '_';
            }
            while (!s.empty() && s.back() == '=')
                s.pop_back();
        }
        return s;
    }
    Bytes unbase64(std::string s) {
        for (auto& c : s) {
            if (c == '-')
                c = '+';
            if (c == '_')
                c = '/';
        }
        while (s.size() % 4)
            s += '=';
        DWORD n = 0;
        if (!CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, nullptr, &n,
                                  nullptr, nullptr))
            throw std::runtime_error("Base64 input");
        Bytes b(n);
        if (!CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, b.data(), &n,
                                  nullptr, nullptr))
            throw std::runtime_error("Base64 decode");
        b.resize(n);
        return b;
    }
    std::string urlEncode(std::string_view s) {
        static const char* h = "0123456789ABCDEF";
        std::string r;
        for (unsigned char c : s) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~')
                r += c;
            else {
                r += '%';
                r += h[c >> 4];
                r += h[c & 15];
            }
        }
        return r;
    }
    std::string form(const Json& d) {
        std::string s;
        for (auto it = d.begin(); it != d.end(); ++it) {
            if (!s.empty())
                s += '&';
            s += urlEncode(it.key()) + '=' +
                 urlEncode(it.value().is_string() ? it.value().get<std::string>()
                                                  : it.value().dump());
        }
        return s;
    }
    std::string eapi(std::string_view path, std::string_view text) {
        std::string p(path), s(text);
        auto digest = hex(hash("nobody" + p + "use" + s + "md5forencrypt", BCRYPT_MD5_ALGORITHM));
        return hex(aes(bytes(p + "-36cd479b6b5-" + s + "-36cd479b6b5-" + digest),
                       bytes("e82ckenh8dichen8"), true),
                   true);
    }
    std::string webRsa(const Bytes& secret) {
        const std::string der =
            "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDgtQn2JZ34ZC28NWYpAUd98iZ37BUrX/"
            "aKzmFbt7clFSs6sXqHauqKWqdtLkF2KexO40H1YTX8z2lSgBBOAxLsvaklV8k4cBFK9snQXE9/"
            "DDaFt6Rr7iVZMldczhC0JNgTz+SHXT6CBHuX3e9SdB1Ua44oncaTWz7OBGLbCiK45wIDAQAB";
        auto binary = unbase64(der);
        CERT_PUBLIC_KEY_INFO* info = nullptr;
        DWORD n = 0;
        Key key;
        if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, binary.data(),
                                 (DWORD)binary.size(), CRYPT_DECODE_ALLOC_FLAG, nullptr, &info, &n))
            throw std::runtime_error("RSA public key");
        BOOL ok = CryptImportPublicKeyInfoEx2(X509_ASN_ENCODING, info, 0, nullptr, &key.h);
        LocalFree(info);
        if (!ok)
            throw std::runtime_error("RSA import");
        Bytes input(128, 0);
        std::copy(secret.rbegin(), secret.rend(), input.end() - secret.size());
        Bytes output(128);
        ULONG count = 0;
        hrcheck(BCryptEncrypt(key.h, input.data(), 128, nullptr, nullptr, 0, output.data(), 128,
                              &count, BCRYPT_PAD_NONE),
                "RSA transform");
        output.resize(count);
        return hex(output);
    }
    Json weapi(const Json& d) {
        auto secret = bytes(hex(randomBytes(8)));
        auto iv = bytes("0102030405060708");
        auto first = base64(aes(bytes(d.dump()), bytes("0CoJUm6Qyw8W8jud"), true, iv));
        return Json{{"params", base64(aes(bytes(first), secret, true, iv))},
                    {"encSecKey", webRsa(secret)}};
    }
    std::string xeapiSign(std::string_view t, std::string_view n) {
        return base64(hash(std::string(t) + std::string(n), BCRYPT_SHA256_ALGORITHM,
                           bytes("mUHCwVNWJbunMqAHf5MImuirT6plvs6VSFW62MGHstFQxhBGdEoIhLItH3djc4+"
                                 "FB/OKty3+lL2rGeoFBpVe5g==")));
    }
    Bytes x25519Public(const std::array<uint8_t, 32>& secret) {
        Bytes b(32);
        crypto_x25519_public_key(b.data(), secret.data());
        return b;
    }
    Json xeapi(std::string_view path, const Json& data, const Json& pub) {
        auto staticKey = unhex("ab1d5a430f6bb04a3f01e81ddd72bd916d5ce591248ac128714806d7f8fb1b84");
        auto dynamic = randomBytes(16);
        Json body = data;
        body.erase("e_r");
        Json fields = {{"content", form(body)}, {"queryString", "e_r=true"}};
        auto inner = aes(bytes(fields.dump()), staticKey, true), salt = randomBytes(16);
        for (size_t i = 0; i < inner.size(); ++i)
            inner[i] ^= salt[i & 15];
        size_t rotation = (salt[0] & 15) % inner.size();
        std::rotate(inner.begin(), inner.begin() + rotation, inner.end());
        Bytes mixed = salt;
        mixed.insert(mixed.end(), inner.begin(), inner.end());
        auto c = aes(mixed, dynamic, true);
        auto raw = randomBytes(32);
        std::array<uint8_t, 32> secret;
        std::copy(raw.begin(), raw.end(), secret.begin());
        auto ephemeral = x25519Public(secret), peer = unbase64(pub.at("publicKey"));
        if (peer.size() != 32)
            throw std::runtime_error("Invalid server key");
        Bytes shared(32);
        crypto_x25519(shared.data(), secret.data(), peer.data());
        crypto_wipe(secret.data(), 32);
        if (std::all_of(shared.begin(), shared.end(), [](uint8_t b) { return b == 0; }))
            throw std::runtime_error("Invalid key exchange");
        auto prk = hash(str(shared), BCRYPT_SHA256_ALGORITHM, Bytes(32, 0));
        Bytes context = ephemeral;
        context.push_back(1);
        auto derived = hash(str(context), BCRYPT_SHA256_ALGORITHM, prk);
        derived.resize(16);
        auto iv = randomBytes(12);
        auto encrypted = aesGcm(
            bytes(base64(dynamic) + "|android|" + pub.at("sk").get<std::string>()), derived, iv);
        Bytes s = ephemeral;
        s.insert(s.end(), iv.begin(), iv.end());
        s.insert(s.end(), encrypted.begin(), encrypted.end());
        auto version = pub.at("version");
        auto v = version.is_string() ? version.get<std::string>() : version.dump();
        auto r = aes(bytes(v + "|"), staticKey, true);
        crypto_wipe(shared.data(), shared.size());
        crypto_wipe(dynamic.data(), dynamic.size());
        (void)path;
        return Json{{"C", base64(c, true)}, {"S", base64(s)}, {"R", base64(r)}};
    }
    Bytes protect(const Bytes& b) {
        DATA_BLOB in{(DWORD)b.size(), (BYTE*)b.data()}, out{};
        if (!CryptProtectData(&in, L"MORA session", nullptr, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &out))
            throw std::runtime_error("Protect session");
        Bytes r(out.pbData, out.pbData + out.cbData);
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        return r;
    }
    Bytes unprotect(const Bytes& b) {
        DATA_BLOB in{(DWORD)b.size(), (BYTE*)b.data()}, out{};
        if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                                &out))
            throw std::runtime_error(
                "Session belongs to another Windows user; remove session.bin and log in again");
        Bytes r(out.pbData, out.pbData + out.cbData);
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        return r;
    }
    struct Internet {
        HINTERNET h = nullptr;
        ~Internet() {
            if (h)
                WinHttpCloseHandle(h);
        }
    };
    Http::Http() {
        session = WinHttpOpen(L"MORA/0.1 (Windows)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            throw std::runtime_error("Windows HTTP unavailable");
        WinHttpSetTimeouts(session, 5000, 5000, 10000, 12000);
    }
    Http::~Http() {
        if (session)
            WinHttpCloseHandle(session);
    }
    Response Http::request(std::wstring url, std::string body, std::wstring headers, size_t cap,
                           bool post) {
        URL_COMPONENTS parts{sizeof(parts)};
        parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = (DWORD)-1;
        if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &parts) ||
            (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP))
            throw std::runtime_error("HTTPS URL required");
        // Plain HTTP is only tolerated for a loopback test provider; every remote call stays TLS.
        if (parts.nScheme == INTERNET_SCHEME_HTTP) {
            std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
            if (host != L"127.0.0.1" && host != L"localhost" && host != L"::1")
                throw std::runtime_error("HTTPS URL required");
        }
        std::wstring host(parts.lpszHostName, parts.dwHostNameLength),
            path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength)
            path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        Internet connection{WinHttpConnect(session, host.c_str(), parts.nPort, 0)};
        if (!connection.h)
            throw std::runtime_error("Network connection failed");
        Internet req{WinHttpOpenRequest(connection.h, post ? L"POST" : L"GET", path.c_str(),
                                        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        parts.nScheme == INTERNET_SCHEME_HTTPS
                                            ? WINHTTP_FLAG_SECURE
                                            : 0)};
        if (!req.h)
            throw std::runtime_error("HTTP request failed");
        DWORD disable = WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
        if (post) {
            // Never glue two header lines together: a missing terminator used to corrupt the last
            // caller header (for example the Authorization token).
            if (!headers.empty() && headers.back() != L'\n')
                headers += L"\r\n";
            if (headers.find(L"Content-Type") == std::wstring::npos)
                headers += L"Content-Type: application/x-www-form-urlencoded;charset=utf-8\r\n";
        }
        if (!WinHttpSendRequest(req.h, headers.c_str(), (DWORD)headers.size(),
                                body.empty() ? WINHTTP_NO_REQUEST_DATA : body.data(),
                                (DWORD)body.size(), (DWORD)body.size(), 0) ||
            !WinHttpReceiveResponse(req.h, nullptr))
            throw std::runtime_error(
                "Network unavailable or request timed out; try again manually");
        Response result;
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                            &status, &size, nullptr);
        result.status = (int)status;
        DWORD index = 0;
        for (;;) {
            size = 0;
            WinHttpQueryHeaders(req.h, WINHTTP_QUERY_SET_COOKIE, nullptr, nullptr, &size, &index);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
                break;
            std::wstring cookie(size / 2, 0);
            if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_SET_COOKIE, nullptr, cookie.data(), &size,
                                     &index))
                break;
            cookie.resize(size / 2);
            while (!cookie.empty() && cookie.back() == 0)
                cookie.pop_back();
            result.cookies.push_back(utf8(cookie));
        }
        std::array<char, 16384> buffer;
        DWORD count = 0;
        do {
            if (!WinHttpReadData(req.h, buffer.data(), (DWORD)buffer.size(), &count))
                throw std::runtime_error("Response interrupted");
            if (result.body.size() + count > cap)
                throw std::runtime_error("Response exceeds memory budget");
            result.body.append(buffer.data(), count);
        } while (count);
        return result;
    }
    Cover decodeCover(const Bytes& data) {
        if (data.empty())
            return {};
        Com<IWICImagingFactory> factory;
        hrcheck(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(factory.put())),
                "Image decoder");
        Com<IWICStream> stream;
        hrcheck(factory->CreateStream(stream.put()), "Image stream");
        hrcheck(stream->InitializeFromMemory((BYTE*)data.data(), (DWORD)data.size()),
                "Image bytes");
        Com<IWICBitmapDecoder> decoder;
        hrcheck(factory->CreateDecoderFromStream(stream.get(), nullptr,
                                                 WICDecodeMetadataCacheOnDemand, decoder.put()),
                "Decode image");
        Com<IWICBitmapFrameDecode> frame;
        hrcheck(decoder->GetFrame(0, frame.put()), "Image frame");
        UINT w = 0, h = 0;
        frame->GetSize(&w, &h);
        if (!w || !h || w > 2048 || h > 2048)
            throw std::runtime_error("Image dimensions exceed limit");
        Com<IWICBitmapScaler> scaler;
        factory->CreateBitmapScaler(scaler.put());
        hrcheck(scaler->Initialize(frame.get(), 48, 48, WICBitmapInterpolationModeFant),
                "Scale cover");
        Com<IWICFormatConverter> converter;
        factory->CreateFormatConverter(converter.put());
        hrcheck(converter->Initialize(scaler.get(), GUID_WICPixelFormat24bppRGB,
                                      WICBitmapDitherTypeNone, nullptr, 0,
                                      WICBitmapPaletteTypeCustom),
                "Cover colors");
        Cover c;
        c.width = c.height = 48;
        c.pixels.resize(48 * 48 * 3);
        hrcheck(converter->CopyPixels(nullptr, 48 * 3, (UINT)c.pixels.size(), c.pixels.data()),
                "Copy cover");
        return c;
    }
    double memoryMB() {
        PROCESS_MEMORY_COUNTERS_EX p{};
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&p, sizeof(p));
        return p.WorkingSetSize / 1048576.0;
    }
    double privateMB() {
        PROCESS_MEMORY_COUNTERS_EX p{};
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&p, sizeof(p));
        return p.PrivateUsage / 1048576.0;
    }
    double peakMemoryMB() {
        PROCESS_MEMORY_COUNTERS_EX p{};
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&p, sizeof(p));
        return p.PeakWorkingSetSize / 1048576.0;
    }
    double peakPrivateMB() {
        PROCESS_MEMORY_COUNTERS_EX p{};
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&p, sizeof(p));
        return p.PeakPagefileUsage / 1048576.0;
    }
    std::filesystem::path stateDirectory() {
        wchar_t buf[32768];
        DWORD n = GetEnvironmentVariableW(L"MORA_DATA_DIR", buf, 32768);
        if (n > 0 && n < 32768)
            return std::filesystem::path(buf);
        n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, 32768);
        if (n > 0 && n < 32768)
            return std::filesystem::path(buf) / L"MORA";
        throw std::runtime_error("LOCALAPPDATA unavailable");
    }
} // namespace mora
