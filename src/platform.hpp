#pragma once
#include "core.hpp"
#include <memory>
#include <objbase.h>
#include <winhttp.h>

namespace mora {
    template <class T> class Com {
        T* p = nullptr;

      public:
        Com() = default;
        ~Com() {
            if (p)
                p->Release();
        }
        Com(const Com&) = delete;
        Com& operator=(const Com&) = delete;
        T* operator->() const {
            return p;
        }
        T* get() const {
            return p;
        }
        T** put() {
            if (p) {
                p->Release();
                p = nullptr;
            }
            return &p;
        }
        explicit operator bool() const {
            return p != nullptr;
        }
    };
    void hrcheck(HRESULT result, const char* operation);
    Bytes bytes(std::string_view text);
    std::string str(const Bytes& data);
    std::string hex(const Bytes& data, bool upper = false);
    Bytes unhex(std::string_view text);
    Bytes randomBytes(size_t size);
    Bytes hash(std::string_view data, LPCWSTR algorithm, const Bytes& key = {});
    Bytes aes(const Bytes& data, const Bytes& key, bool encrypt, const Bytes& iv = {});
    Bytes aesGcm(const Bytes& data, const Bytes& key, const Bytes& iv);
    std::string base64(const Bytes& data, bool url = false);
    Bytes unbase64(std::string text);
    std::string urlEncode(std::string_view data);
    std::string form(const Json& data);
    std::string eapi(std::string_view path, std::string_view json);
    std::string webRsa(const Bytes& secret);
    Json weapi(const Json& data);
    std::string xeapiSign(std::string_view timestamp, std::string_view nonce);
    Bytes x25519Public(const std::array<uint8_t, 32>& secret);
    Json xeapi(std::string_view path, const Json& data, const Json& publicKey);
    Bytes protect(const Bytes& data);
    Bytes unprotect(const Bytes& data);
    struct Response {
        std::string body;
        std::vector<std::string> cookies;
        int status = 0;
    };
    class Http {
        HINTERNET session = nullptr;

      public:
        Http();
        ~Http();
        Http(const Http&) = delete;
        Http& operator=(const Http&) = delete;
        Response request(std::wstring url, std::string body = {}, std::wstring headers = {},
                         size_t cap = 2 * 1024 * 1024, bool post = true);
    };
    Cover decodeCover(const Bytes& data);
    double memoryMB();
    double privateMB();
    double peakMemoryMB();
    double peakPrivateMB();
    std::filesystem::path stateDirectory();
} // namespace mora
