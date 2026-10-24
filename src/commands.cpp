#include "commands.hpp"
#include <cwctype>
namespace mora {
    const std::vector<CommandSpec>& commandRegistry() {
        static const std::vector<CommandSpec> specs = {
            {L"search", L"<歌名 / 歌手 / 专辑>", L"搜索单曲元数据"},
            {L"albums", L"<关键词>", L"搜索专辑"},
            {L"playlists", L"<关键词>", L"搜索歌单"},
            {L"login", L"[qr | phone]", L"登录网易云账号"},
            {L"library", L"", L"我的歌单"},
            {L"queue", L"", L"查看播放队列"},
            {L"play", L"[结果序号]", L"播放选择的歌曲 / 打开专辑"},
            {L"pause", L"", L"暂停 / 继续"},
            {L"next", L"", L"下一首"},
            {L"prev", L"", L"上一首"},
            {L"like", L"", L"红心 / 取消红心"},
            {L"add", L"", L"收藏到自己创建的歌单"},
            {L"quality", L"[standard | exhigh | lossless | hires]", L"选择音质"},
            {L"volume", L"<0–100>", L"设置音量"},
            {L"seek", L"<秒 / +秒 / -秒>", L"跳转播放位置"},
            {L"repeat", L"[on | off]", L"队列循环（兼容旧开关）"},
            {L"mode", L"[sequential | repeat_all | repeat_one | shuffle]", L"队列播放模式"},
            {L"detail", L"", L"歌曲详情与歌词"},
            {L"stats", L"[day | month | year]", L"听歌统计报表"},
            {L"review", L"[day | month | year]", L"让 Agent 写一份听歌锐评"},
            {L"models", L"", L"已缓存的模型目录"},
            {L"motion", L"[on | off]", L"音乐频谱动效"},
            {L"home", L"", L"返回阿木首页"},
            {L"chat", L"<音乐需求>", L"与音乐 Agent 对话并执行明确的音乐工具"},
            {L"name", L"[昵称]", L"设置聊天中显示的名字"},
            {L"persona", L"[人设ID]", L"切换或查看音乐 Agent 人设"},
            {L"effort", L"[off|minimal|low|medium|high|xhigh|max]", L"模型思考强度"},
            {L"audio-device", L"[序号 | default]", L"选择 Windows 音频输出设备"},
            {L"lyrics", L"", L"显示当前正在唱的那句歌词"},
            {L"react", L"[on | off | now]", L"换歌时让 Agent 自动回应一句"},
            {L"settings", L"", L"打开个人设置"},
            {L"guide", L"", L"重看新手引导"},
            {L"ai", L"", L"模型适配器与密钥配置说明"},
            {L"agent", L"", L"查看外部 Agent / MCP 接入"},
            {L"resume", L"", L"风控冷却后手动恢复"},
            {L"help", L"", L"命令与键盘说明"},
            {L"quit", L"", L"退出播放器"}};
        return specs;
    }
    static std::wstring trim(std::wstring s) {
        auto a = s.find_first_not_of(L" \t\r\n");
        if (a == std::wstring::npos)
            return {};
        auto b = s.find_last_not_of(L" \t\r\n");
        return s.substr(a, b - a + 1);
    }
    Invocation parseCommand(std::wstring_view text) {
        auto s = trim(std::wstring(text));
        if (s.empty())
            return {};
        if (s[0] != L'/')
            return {L"chat", s};
        s.erase(s.begin());
        auto split = s.find_first_of(L" \t");
        auto verb = s.substr(0, split);
        for (auto& c : verb)
            c = (wchar_t)towlower(c);
        return {verb, split == std::wstring::npos ? L"" : trim(s.substr(split + 1))};
    }
    std::vector<CommandSpec> commandSuggestions(std::wstring_view input) {
        std::vector<CommandSpec> out;
        if (input.empty() || input[0] != L'/' || input.find_first_of(L" \t") != std::wstring::npos)
            return out;
        auto prefix = std::wstring(input.substr(1));
        for (auto& c : prefix)
            c = (wchar_t)towlower(c);
        for (auto& s : commandRegistry())
            if (s.name.starts_with(prefix))
                out.push_back(s);
        return out;
    }
} // namespace mora
