#include "persona.hpp"
#include "mascot.hpp"
#include "platform.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <stdexcept>

// Persona avatars are generated assets under assets/personas; the build adds -Iassets so these
// lookups do not depend on __has_include resolving a relative path.
#if __has_include(<personas/fox/fox_sprite.hpp>)
#include <personas/fox/fox_sprite.hpp>
#define MORA_HAS_FOX 1
#endif
#if __has_include(<personas/devil/devil_sprite.hpp>)
#include <personas/devil/devil_sprite.hpp>
#define MORA_HAS_DEVIL 1
#endif
#if __has_include(<personas/mushroom/mushroom_sprite.hpp>)
#include <personas/mushroom/mushroom_sprite.hpp>
#define MORA_HAS_MUSHROOM 1
#endif
#if __has_include(<personas/daidai/daidai_sprite.hpp>)
#include <personas/daidai/daidai_sprite.hpp>
#define MORA_HAS_DAIDAI 1
#endif

namespace mora {
    namespace {
        constexpr size_t kMaxCustomPersonas = 16;
        constexpr size_t kMaxName = 24, kMaxTagline = 60, kMaxVoice = 220, kMaxTaste = 220;

        std::filesystem::path personasFile() {
            auto dir = stateDirectory();
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            return dir / "personas.json";
        }

        // A custom persona may not impersonate the built-ins or reuse a stored id.
        bool validId(const std::string& id) {
            if (id.empty() || id.size() > 32)
                return false;
            for (char c : id)
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'))
                    return false;
            return !findBuiltinPersona(id);
        }

        std::wstring clean(const std::wstring& value, size_t limit) {
            auto first = value.find_first_not_of(L" \t\r\n");
            if (first == std::wstring::npos)
                return {};
            auto text = value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
            if (text.size() > limit)
                text.resize(limit);
            for (auto& c : text)
                if (c == L'\r' || c == L'\n' || c == L'\t')
                    c = L' ';
            return text;
        }

        // Custom personas pick their palette from a fixed, readable set; the id chooses which, so
        // the same persona always looks the same on every machine.
        const std::array<std::array<RGB, 4>, 5> customPalettes{{
            {{{112, 183, 245}, {30, 38, 58}, {150, 170, 200}, {18, 22, 34}}},
            {{{235, 170, 110}, {44, 44, 50}, {168, 164, 158}, {26, 26, 30}}},
            {{{244, 150, 190}, {48, 34, 50}, {186, 160, 186}, {30, 22, 32}}},
            {{{120, 190, 180}, {34, 44, 42}, {162, 176, 172}, {20, 28, 27}}},
            {{{232, 205, 140}, {46, 44, 52}, {178, 172, 186}, {28, 26, 32}}},
        }};

        uint64_t stableHash(const std::string& text) {
            uint64_t hash = 1469598103934665603ull;
            for (unsigned char c : text) {
                hash ^= c;
                hash *= 1099511628211ull;
            }
            return hash;
        }

        std::optional<RGB> sampleSprite(const uint8_t* pixels,
                                        const std::array<uint8_t, 3>* palette, int width,
                                        int height, int x, int y) {
            if (x < 0 || y < 0 || x >= width || y >= height)
                return {};
            auto index = pixels[static_cast<size_t>(y) * width + x];
            if (index == 255)
                return {};
            const auto& color = palette[index];
            return RGB{color[0], color[1], color[2]};
        }

        Persona makePersona(std::string id, std::wstring name, std::wstring tagline,
                            std::wstring voice, std::wstring taste, std::wstring greeting,
                            RGB accent, RGB surface, RGB muted, RGB background) {
            Persona persona;
            persona.id = std::move(id);
            persona.name = std::move(name);
            persona.tagline = std::move(tagline);
            persona.voice = std::move(voice);
            persona.taste = std::move(taste);
            persona.greeting = std::move(greeting);
            persona.accent = accent;
            persona.surface = surface;
            persona.muted = muted;
            persona.background = background;
            return persona;
        }
    } // namespace

    const std::vector<Persona>& builtinPersonas() {
        static const std::vector<Persona> personas{
            makePersona("amu", L"小阿木",
                        L"口味最多的小阿木",
                        L"温柔、好奇、爱分享，语气轻松像朋友，偶尔冒出像素小吐槽",
                        L"口味很杂，什么都愿意试；按用户当下心情在流行、钢琴、电子与老歌之间挑",
                        L"我是小阿木，今天想听点什么？各风格我都能陪你翻翻。",
                        RGB{112, 183, 245}, RGB{30, 38, 58}, RGB{150, 170, 200},
                        RGB{18, 22, 34}),
            makePersona("fox", L"小灰狐娘",
                        L"爱吃干噎酸奶的华语歌者",
                        L"爽快利落、有点江湖气，聊到华语歌会兴奋，爱用比喻，说话不绕弯",
                        L"华语流行、民谣、老歌与摇滚，偏爱有故事感的嗓音和现场感",
                        L"叫我小灰就行。华语这块我熟，报个心情，我给你挑。",
                        RGB{235, 170, 110}, RGB{44, 44, 50}, RGB{168, 164, 158},
                        RGB{26, 26, 30}),
            makePersona("devil", L"小恶魔少女",
                        L"浅粉白双马尾的日语歌势顶级歌星",
                        L"元气自信的顶级歌星口吻，偶尔夹一点日语词，爱调侃但从不说满话",
                        L"日语流行、动画歌、J-ROCK 与电子，偏爱高能量节奏和强旋律副歌",
                        L"来啦？我是台上的那位。日语歌这一块，交给我。",
                        RGB{244, 150, 190}, RGB{48, 34, 50}, RGB{186, 160, 186},
                        RGB{30, 22, 32}),
            makePersona("mushroom", L"小蘑菇菌娘",
                        L"温柔甜嗓的芭乐与 R&B 爱好者",
                        L"轻声细语、温柔体贴，说话慢而稳，会先确认用户的感受",
                        L"芭乐情歌、R&B、City Pop 与爵士，偏爱温柔人声、和声与松弛律动",
                        L"慢慢来就好。想听芭乐还是 R&B？我陪你安静听一会儿。",
                        RGB{120, 190, 180}, RGB{34, 44, 42}, RGB{162, 176, 172},
                        RGB{20, 28, 27}),
            makePersona("daidai", L"小宅女呆呆",
                        L"笨笨少女音的小甜歌爱好者",
                        L"软软的、有点呆的小宅女语气，会突然发呆，但特别贴心",
                        L"小甜歌、轻音乐、动画 OST 与治愈系，偏爱安静不吵、听着能放松的歌",
                        L"啊、你来啦……我刚刚在发呆。要不要听点甜甜的？",
                        RGB{232, 205, 140}, RGB{46, 44, 52}, RGB{178, 172, 186},
                        RGB{28, 26, 32}),
        };
        return personas;
    }

    const Persona* findBuiltinPersona(const std::string& id) {
        for (const auto& persona : builtinPersonas())
            if (persona.id == id)
                return &persona;
        return nullptr;
    }

    std::vector<Persona> loadCustomPersonas() {
        std::vector<Persona> out;
        std::ifstream file(personasFile(), std::ios::binary);
        if (!file)
            return out;
        std::string text((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
        auto parsed = Json::parse(text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_array())
            return out;
        for (auto& item : parsed) {
            if (out.size() >= kMaxCustomPersonas || !item.is_object())
                break;
            auto id = item.value("id", std::string());
            auto name = clean(unutf8(item.value("name", std::string())), kMaxName);
            if (!validId(id) || name.empty())
                continue;
            auto palette = customPalettes[stableHash(id) % customPalettes.size()];
            Persona persona = makePersona(id, name,
                                          clean(unutf8(item.value("tagline", std::string())),
                                                kMaxTagline),
                                          clean(unutf8(item.value("voice", std::string())),
                                                kMaxVoice),
                                          clean(unutf8(item.value("taste", std::string())),
                                                kMaxTaste),
                                          clean(unutf8(item.value("greeting", std::string())),
                                                kMaxVoice),
                                          palette[0], palette[1], palette[2], palette[3]);
            persona.builtin = false;
            out.push_back(std::move(persona));
        }
        return out;
    }

    bool saveCustomPersonas(const std::vector<Persona>& personas) {
        Json array = Json::array();
        for (const auto& persona : personas) {
            if (persona.builtin || !validId(persona.id))
                continue;
            array.push_back(Json{{"id", persona.id},
                                 {"name", utf8(persona.name)},
                                 {"tagline", utf8(persona.tagline)},
                                 {"voice", utf8(persona.voice)},
                                 {"taste", utf8(persona.taste)},
                                 {"greeting", utf8(persona.greeting)}});
        }
        std::ofstream file(personasFile(), std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        file << array.dump(2) << '\n';
        return true;
    }

    Persona createCustomPersona(const std::string& id, const std::wstring& name,
                                const std::wstring& tagline, const std::wstring& voice,
                                const std::wstring& taste, const std::wstring& greeting) {
        if (!validId(id))
            throw std::invalid_argument("persona id must be 1-32 letters, digits, - or _ and not a "
                                        "built-in id");
        auto cleanName = clean(name, kMaxName);
        if (cleanName.empty())
            throw std::invalid_argument("persona name is required");
        auto cleanVoice = clean(voice, kMaxVoice);
        auto cleanTaste = clean(taste, kMaxTaste);
        if (cleanVoice.empty() || cleanTaste.empty())
            throw std::invalid_argument("persona voice and taste are required");
        auto stored = loadCustomPersonas();
        for (const auto& existing : stored)
            if (existing.id == id)
                throw std::invalid_argument("a custom persona with that id already exists");
        if (stored.size() >= kMaxCustomPersonas)
            throw std::invalid_argument("too many custom personas; delete one first");
        auto palette = customPalettes[stableHash(id) % customPalettes.size()];
        Persona persona = makePersona(id, cleanName, clean(tagline, kMaxTagline), cleanVoice,
                                      cleanTaste, clean(greeting, kMaxVoice), palette[0],
                                      palette[1], palette[2], palette[3]);
        persona.builtin = false;
        stored.push_back(persona);
        if (!saveCustomPersonas(stored))
            throw std::runtime_error("could not write personas.json");
        return persona;
    }

    std::vector<Persona> allPersonas() {
        auto personas = builtinPersonas();
        for (auto& custom : loadCustomPersonas())
            personas.push_back(std::move(custom));
        return personas;
    }

    const Persona& resolvePersona(const std::string& id) {
        for (const auto& persona : builtinPersonas())
            if (persona.id == id)
                return persona;
        static thread_local std::vector<Persona> custom;
        custom = loadCustomPersonas();
        for (const auto& persona : custom)
            if (persona.id == id)
                return persona;
        return builtinPersonas().front();
    }

    std::wstring personaSystemPrompt(const Persona& persona, const std::wstring& userName) {
        std::wstring prompt = L"你是 MORA 音乐终端的音乐助手，当前人设是「" + persona.name + L"」。";
        prompt += L"人设性格：" + persona.voice + L"。";
        prompt += L"推歌取向：" + persona.taste + L"。";
        if (!userName.empty())
            prompt += L"请称呼用户为「" + userName + L"」。";
        prompt += L"用中文回答，保持人设语气但不要过长（不超过 120 字）。";
        prompt += L"需要事实、候选或播放状态时先调用工具，不要凭记忆编造歌名、歌手或播放结果。";
        prompt += L"工具返回 needs_player 时告诉用户先运行 mora 终端；写操作只在用户明确给出目标时调用。";
        prompt += L"用户想创建自定义人设时调用 mora_create_persona，并先问清名字、语气和推歌取向。";
        prompt += L"不得声称已经发生的播放或账号操作之外的任何副作用。";
        prompt += personaIntentGuidance(persona);
        return prompt;
    }

    std::wstring personaIntentGuidance(const Persona&) {
        return L"【意图边界】闲聊、问候、评价歌曲、询问看法时只自然回答，不搜索、不推荐、不播放、不修改队列。提到‘正在播放的歌’只是谈话上下文，不是播放命令。只有用户明确要求播放、换歌、追加、暂停、收藏等操作时才调用对应工具。"
               L"多首并播放：按取向→music_recommend_queue(intent=人设推歌取向,count=首数)；"
               L"从用户歌单→music_recommend_queue(source=playlists,count=首数)；"
               L"用户喜欢的歌/红心→music_recommend_queue(source=liked,count=首数)（只看清单用 music_list_likes）；"
               L"已有 id 时用一次 music_queue_edit(op=replace, ids=[…]) 批量入队，禁止逐首播放；"
               L"换音质用 music_control(action=quality, level=standard|higher|exhigh|lossless|hires)；"
               L"以实际 queuedCount 为准，不足要说明；只推荐不播放时不得修改队列。操作成功后简短答复，不重复调用写工具。";
    }
    std::wstring personaWaitLine(const Persona& persona, int index) {
        // One short corpus per built-in, in the persona's own voice. It stays small on purpose: the
        // contract only needs a few rotating lines, not a second personality description.
        static const std::map<std::string, std::vector<std::wstring>> lines{
            {"amu",
             {L"正在翻唱片架，稍等一下…", L"在曲库里挑一挑，马上回来。",
              L"正在想哪首更适合你，别急。"}},
            {"fox", {L"别急，我这就去扒拉两首。", L"给我一会儿，好歌不怕等。", L"正在挑，保准有味道。"}},
            {"devil",
             {L"稍等，台上的我正在选曲。", L"让我想一下，这首歌单怎么排。", L"马上就好，别走开哦。"}},
            {"mushroom",
             {L"慢慢来，我先翻一翻歌单。", L"正在找温柔的旋律，请稍候。",
              L"稍等一下，我想给你挑首舒服的。"}},
            {"daidai",
             {L"唔……我找找看，等一下下。", L"让我想想哦，马上就好。", L"在翻歌单啦，别催我呀。"}},
        };
        auto found = lines.find(persona.id);
        if (found != lines.end() && !found->second.empty()) {
            int size = static_cast<int>(found->second.size());
            int pick = ((index % size) + size) % size;
            return found->second[static_cast<size_t>(pick)];
        }
        // A custom persona reuses one of three neutral templates, chosen by its id hash, and is
        // addressed by its own name so it never borrows a built-in's voice.
        static const std::array<const wchar_t*, 3> fallback{
            L"正在为「%s」整理歌单，稍等一下…", L"让我想想要放哪几首，马上回来。",
            L"歌单还在挑，请稍候。"};
        std::wstring text = fallback[stableHash(persona.id) % fallback.size()];
        auto at = text.find(L"%s");
        if (at != std::wstring::npos)
            text.replace(at, 2, persona.name);
        return text;
    }

    std::wstring personaSearchSeed(const Persona& persona) {
        if (persona.id == "amu") return L"流行音乐 钢琴 电子 老歌";
        if (persona.id == "fox") return L"华语流行 民谣 摇滚 老歌";
        if (persona.id == "devil") return L"日语流行 动画 J-ROCK 电子";
        if (persona.id == "mushroom") return L"轻音乐 治愈 钢琴";
        if (persona.id == "daidai") return L"钢琴 轻音乐 治愈";
        auto end = persona.taste.find_first_of(L"，、。；,;/");
        auto seed = persona.taste.substr(0, std::min(end, size_t(30)));
        return seed.empty() ? L"流行音乐" : seed;
    }

    bool usePersonaSearchSeed(const std::wstring& request) {
        bool genericTaste = request.find(L"品味") != std::wstring::npos ||
                            request.find(L"口味") != std::wstring::npos ||
                            request.find(L"随便") != std::wstring::npos;
        bool shortRandom = request.find(L"随机") != std::wstring::npos &&
                           request.size() <= 12;
        return (genericTaste || shortRandom) &&
               request.find(L"的歌") == std::wstring::npos;
    }
    bool personaHasDrawnAvatar(const std::string& id) {
        if (id == "amu")
            return true;
#ifdef MORA_HAS_FOX
        if (id == "fox")
            return true;
#endif
#ifdef MORA_HAS_DEVIL
        if (id == "devil")
            return true;
#endif
#ifdef MORA_HAS_MUSHROOM
        if (id == "mushroom")
            return true;
#endif
#ifdef MORA_HAS_DAIDAI
        if (id == "daidai")
            return true;
#endif
        return false;
    }

    std::optional<RGB> personaAvatarPixel(const std::string& id, int x, int y) {
        if (id == "amu")
            return mascotPixel(x, y, false);
#ifdef MORA_HAS_FOX
        if (id == "fox")
            return sampleSprite(fox_asset::pixels.data(), fox_asset::palette.data(),
                                fox_asset::width, fox_asset::height, x, y);
#endif
#ifdef MORA_HAS_DEVIL
        if (id == "devil")
            return sampleSprite(devil_asset::pixels.data(), devil_asset::palette.data(),
                                devil_asset::width, devil_asset::height, x, y);
#endif
#ifdef MORA_HAS_MUSHROOM
        if (id == "mushroom")
            return sampleSprite(mushroom_asset::pixels.data(), mushroom_asset::palette.data(),
                                mushroom_asset::width, mushroom_asset::height, x, y);
#endif
#ifdef MORA_HAS_DAIDAI
        if (id == "daidai")
            return sampleSprite(daidai_asset::pixels.data(), daidai_asset::palette.data(),
                                daidai_asset::width, daidai_asset::height, x, y);
#endif
        return {};
    }

    std::optional<RGB> personaGeneratedPixel(const Persona& persona, int x, int y) {
        if (x < 0 || x >= personaAvatarWidth || y < 0 || y >= personaAvatarHeight)
            return {};
        // A small deterministic face, so a persona the user typed still has its own homepage art.
        // The palette decides every colour, which keeps it recognisable next to the drawn avatars.
        const double centreX = 24.0, headY = 27.0;
        const double dx = (x - centreX) / 15.5, dy = (y - headY) / 18.5;
        const double head = dx * dx + dy * dy;
        uint64_t hash = stableHash(persona.id);
        bool longHair = (hash & 1) != 0;
        RGB hair{static_cast<uint8_t>(persona.accent.r * 0.72),
                 static_cast<uint8_t>(persona.accent.g * 0.72),
                 static_cast<uint8_t>(persona.accent.b * 0.72)};
        RGB skin{static_cast<uint8_t>(std::min(255, persona.muted.r + 34)),
                 static_cast<uint8_t>(std::min(255, persona.muted.g + 26)),
                 static_cast<uint8_t>(std::min(255, persona.muted.b + 22))};
        RGB top{static_cast<uint8_t>(persona.surface.r + 14),
                static_cast<uint8_t>(persona.surface.g + 14),
                static_cast<uint8_t>(persona.surface.b + 18)};
        if (y >= 42) {
            int half = 8 + (y - 42);
            return std::abs(x - 24) <= half ? std::optional<RGB>(top) : std::nullopt;
        }
        if (head <= 1.0) {
            const double faceDx = (x - centreX) / 10.5, faceDy = (y - (headY + 3.0)) / 13.5;
            bool face = faceDx * faceDx + faceDy * faceDy <= 1.0;
            if (!face && longHair && y < headY + 14 && head > 0.62)
                face = false;
            return face ? std::optional<RGB>(skin) : std::optional<RGB>(hair);
        }
        if (longHair && head <= 1.55 && y > headY - 2)
            return hair;
        return std::nullopt;
    }
} // namespace mora
