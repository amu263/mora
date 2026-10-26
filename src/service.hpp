#pragma once
#include "api.hpp"
#include "bridge.hpp"
#include "recommend.hpp"
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>

namespace mora {
    // A tool-level failure with a machine-readable code. mcp.cpp turns it into result.isError so
    // the host can distinguish needs_player / invalid_arguments / no_permission from protocol
    // errors instead of parsing prose.
    struct ToolError : std::runtime_error {
        std::string code;
        ToolError(std::string errorCode, std::string message)
            : std::runtime_error(std::move(message)), code(std::move(errorCode)) {}
    };

    // One boundary for deterministic music operations. The terminal worker, the MCP adapter and
    // (M3) the model tool loop all call this service; none of them talks to Api directly, and none
    // of them keeps a private queue or playback state.
    class MusicService {
        // Lazily created, logically const: the stored session already knows the account id and
        // nickname, so reading them must never report "not logged in" merely because no network
        // call has happened yet. (Likes are network state, not session state: only account()
        // fills them, exactly as before.)
        mutable std::unique_ptr<Api> api;
        Api::Transport transport;
        std::function<BridgeReply(const std::string&, const Json&)> link;
        // Bounded memory of the last few recommendation sets, so play_random can refer back to one.
        std::map<std::string, std::vector<Candidate>> candidateSets;
        std::deque<std::string> candidateOrder;
        uint64_t candidateSerial = 0;
        // Account state outlives releaseBackend(). A thin adapter rebuilds its Api between tool
        // calls, and re-reading uid/nickname/likes meant two extra account requests per call - the
        // amplifier behind the risk-control trip when a host paged the liked list. account() and
        // our own like() refresh it, logout() clears it, and like reads expire so an outside
        // change is still noticed.
        mutable std::mutex cacheMutex;
        mutable int64_t cachedUid = 0;
        mutable std::wstring cachedNickname;
        mutable std::set<int64_t> cachedLikes;
        mutable Clock::time_point cachedLikesAt{};
        mutable bool cachedLikesValid = false;
        void cacheAccount(const Api& source) const;
        void clearAccountCache() const;
        void invalidateLikes() const;
        Api& backend() const;
        BridgeReply session(const std::string& method, const Json& params);
        Json searchTool(std::wstring query, Kind kind, int offset, int limit);
        Json collectionTool(const std::string& kind, int64_t id, int offset, int limit);
        Json libraryTool(bool ownedOnly);
        // Queue reads/writes ride the same bridge as music_control: the running player owns the
        // one queue, so the adapter only projects and never keeps a private copy.
        Json queueState(const Json& data) const;
        // Resolve song ids into queue rows before an insert. Metadata is a nicety: an id without
        // detail still becomes a row (the player fills it in later); nothing is ever fabricated.
        std::vector<Track> queueTracks(const std::vector<int64_t>& ids);
        // Lyrics are cached per song on the calling side, not here: the adapter must stay thin.
        Json lyricsTool(int64_t songId, bool includeTranslation, int maxLines);
        std::string rememberCandidates(std::vector<Candidate> picked);

      public:
        // Attach the authoritative player session. The MCP adapter points this at the local named
        // pipe; a future in-process agent points it at the session directly.
        void attach(std::function<BridgeReply(const std::string&, const Json&)> handler);
        bool attached() const;
        // Test seam: build the network backend over an injected transport instead of the real
        // WinHTTP one. The tool boundary itself is unchanged, so tests exercise the real
        // projection, ordering and permission logic offline.
        void useTransport(Api::Transport adapter);
        // Thin adapters (MCP) drop the network backend between calls instead of holding TLS
        // buffers for the whole host session.
        void releaseBackend();

        // Typed operations used by the terminal UI worker.
        SearchPage search(std::wstring query, Kind kind, int offset = 0);
        std::vector<Track> library(bool ownedOnly = false);
        std::vector<Track> album(int64_t id);
        std::vector<Track> playlist(int64_t id);
        std::vector<Track> details(const std::vector<int64_t>& ids);
        Playback playback(const Track& track, const std::string& quality);
        Lyrics lyrics(int64_t id);
        std::string qrKey();
        int qrCheck(const std::string& key);
        void sendSms(std::string phone);
        void phoneLogin(std::string phone, std::string captcha);
        void account();
        void like(int64_t id, bool value);
        void save(int64_t song, int64_t playlist);
        void logout();
        void resume();
        std::wstring nickname() const;
        int64_t uid() const;
        std::set<int64_t> likes() const;

        // Tool/agent entry: validated arguments in, JSON result out. tools/list comes from
        // musicToolDefinitions(); mcp.cpp only routes, it never re-implements music logic.
        Json invoke(const std::string& name, const Json& arguments);
    };
    Json musicToolDefinitions();
} // namespace mora
