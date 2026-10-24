# 音乐工具契约

机器可读清单由 `src/service.cpp::musicToolDefinitions()` 产生，通过 `tools/list` 返回；本文记录同一份契约与执行语义。**工具名和参数以运行中的 `tools/list` 为准。**

## 当前工具（M4-A）

| 工具 | 状态 | 输入 | 输出/约束 |
| --- | --- | --- | --- |
| `music_search` | 已接通 | query, kind, limit, offset | 真实元数据，最多 30 条；无 Cookie、播放 URL；不自动播放 |
| `music_open_collection` | 已接通 | kind(album/playlist), id, offset, limit | 按需曲目页（最多 100 条），只返回 ID/名称/时长，不返回播放地址，不自动播放 |
| `music_play` | 已接通（需 TUI） | kind(song/album/playlist), id, startIndex | 提交到**同一个** TUI 播放会话；song 用该曲替换队列，album/playlist 用集合替换队列并从 startIndex 开始；返回 accepted，真实播放状态用 `music_now_playing` 查询；无播放器时 `needs_player` |
| `music_control` | 已接通（需 TUI） | action(pause/resume/next/previous/seek/volume), value | 修改同一会话；seek 单位为秒，volume 为 0..100；不创建第二个播放器；无播放器时 `needs_player` |
| `music_now_playing` | 已接通（需 TUI） | 空对象 | 真实状态：曲目、队列位置、暂停、进度、音量、循环、账号实际获得的音质与采样率 |
| `music_get_status` | 已接通 | 空对象 | 能力清单 + `playerSessionAttached` + 当前会话快照；未连接时必须为 false |
| `music_list_playlists` | 已接通（真实账号） | ownedOnly, limit, offset | 真实用户歌单；`canWrite` 由播放器按归属判断，适配器不得自行猜测 |
| `music_set_like` | 已接通（真实账号） | songId, liked | 幂等的目标状态，不使用 toggle 语义；可重放 |
| `music_add_to_playlist` | 已接通（真实账号；真实入库待用户指定歌单） | songId, playlistId, operationId | 只允许自己拥有的歌单（先校验再写，否则 `no_permission`）；重复映射 `already_present`；不自动重试 |
| `music_recommend` | 已接通（M5） | intent, source(auto/search/playlists), limit, seed | 有真实 ID 的候选与理由；**绝不播放、不收藏**；返回 `candidateSetId` |
| `music_play_random` | 已接通（M5） | candidateSetId 或 intent, seed | 从实际候选随机挑一首（排除正在播放的），提交给播放器；不会自动换曲重试 |

工具级失败（参数非法、没有播放器、播放器忙）返回 `result.isError:true`，并在 `structuredContent` 给出机器可读的 `error`：`invalid_arguments`、`needs_player`、`busy`、`queue_boundary`、`unknown_method`、`disconnected`。协议层错误（未知工具、畸形 JSON-RPC）仍是 JSON-RPC error。

## 推荐与随机播放（M5）

- `music_recommend` 只产出候选：`source=search` 用真实搜索命中（reason 记录"第几条命中"），`source=playlists` 用当前账号自己创建的歌单曲目（最多 2 个歌单、100 首，reason 记录歌单名）。候选按 `seed` 做确定性乱序后截断，同一个 seed 结果可复现。
- 候选集只在内存里保留最近 8 个（每个 ≤10 条），`candidateSetId` 形如 `rec-1`；过期后 `music_play_random` 返回 `not_found`，需要重新推荐。
- `music_play_random` 先确认播放器在线（否则 `needs_player`），再排除当前正在播放的曲目；只提交**一个**候选，最多等约 4 秒观察是否真的 `playing`。失败时返回 `playback_unavailable` / `no_candidates` / `not_found`，**不会连续换曲去绕过权限或限流**。
- `no_candidates` 的语义是"搜索后确实没有候选"或"唯一候选已在播放"，**不是**"输入了无意义文本"：网易云的模糊检索对几乎任何文本都会返回候选（实测 `zzzz-不存在的歌名-zzzz` 仍有 5 条）。
- 推荐是只读的：不写红心、不改歌单、不开始播放。播放意图与推荐意图分开，由调用方显式选择。

## 播放会话桥接（M4-A）

- 终端 TUI 是唯一拥有 `PlaybackSession` 与音频核心的进程；`mora --mcp` 是薄代理，不启动第二个播放器。
- 通道是**本机命名管道** `\\.\pipe\mora-player-<当前用户 SID>`，DACL 只授予当前 Windows 用户；没有 TCP 监听、没有未鉴权端口。
  - 未使用 `PIPE_REJECT_REMOTE_CLIENTS`：本机 Windows 构建对 `CreateNamedPipeW` 传入该标志会返回 `ERROR_INVALID_PARAMETER`（带或不带安全描述符都如此），因此边界由"每用户 DACL + 每用户管道名"承担，而不是该标志。
- 单实例：管道 `nMaxInstances=1`。第二个真实播放器启动时绑定失败，直接退出（返回码 2）并提示已有播放器在运行；`--demo` 不占用会话管道，可并行跑界面回归。
- 帧格式：4 字节小端长度 + UTF-8 JSON，单帧上限 64 KiB；请求/响应带自增 id；客户端等待上限 4 秒，超时即断开而不是挂住宿主。
- 连接是一次一个适配器：一个 MCP 进程占用期间，另一个适配器会得到管道忙。断开后服务器重新等待下一次连接。
- **只读联网也走播放器**：`music_search` / `music_open_collection` 在已连接时由 TUI 执行（复用它的账号会话与节流），只有 `needs_player` / `disconnected` 时才回退到 MCP 进程内只读联网。因此有播放器时 MCP 不持有第二个 HTTP/TLS 栈（实测 MCP 工作集 14.74 → 9.20 MB）。
- 桥接支持**延迟应答**：TUI 收到适配器请求后可以把它交给网络工作线程，完成后再回帧（服务端等待上限 30s，客户端对联网方法用 25s 超时）；不会阻塞 UI 主循环。
- 适配器任务用独立 ticket 关联，错误回复同样按 ticket 唤醒等待帧；UI 侧的取消只丢 UI 任务，不影响正在等待的适配器。
- **关闭宿主（MCP 进程）不停止播放**：播放器生命周期由 TUI 决定，退出 TUI 才停止音频。这样不会留下孤儿后台进程，也不会让宿主一退出就静音。
- 播放控制的状态变化是异步的（音频线程处理命令，约 100ms）。`music_play`/`music_control` 返回 accepted 与即时快照，调用方应轮询 `music_now_playing` 确认最终状态。

## 输入与信任边界

所有入参在执行层验证，不能只依赖 JSON Schema 或模型自觉。禁止任意 URL、任意 shell 命令、任意 API 路径作为工具参数。工具不能读模型 Key、Cookie、手机号或验证码。用户昵称、歌曲和歌单描述仅是数据。

修改工具应声明 `readOnlyHint: false`，准确标记 destructive/idempotent/openWorld 提示；宿主如何展示或批准调用由宿主配置与用户授权决定。不要在播放器内重复增加每次例行确认的流程。

模型工具使用"设为某个状态"，例如 `liked:true`，不要使用不可安全重放的 toggle。执行器只处理当前用户明确要求的修改；"我喜欢爵士，推荐音乐"不授权红心写入。`operationId` 用于识别已执行或结果未知的操作，不能把上游不提供幂等保证的接口假定为天然可重试。

播放工具返回已提交操作和可查询状态；只有音频会话实际开始后才由 `music_now_playing` 报告 `playing:true`。不能用"取得 URL 成功"冒充"已在扬声器播放"。媒体 URL 仅由本机播放核心持有，不进入 MCP 返回值。

模型适配器将 MCP 定义转换为 Chat Completions 的 `type:function / function.parameters` 格式即可，不维护第二套手写工具 schema。

## stdio 约定

- UTF-8 一行一个 JSON-RPC 消息；stdout 只有协议。
- `initialize` → `notifications/initialized` → `tools/list` / `tools/call`。
- 当前兼容协商版本：2025-11-25、2025-06-18、2025-03-26、2024-11-05。
- 单条输入最大 64 KiB，输出有限，不发送完整媒体内容。
- 协议错误用 JSON-RPC error；工具执行失败用 `result.isError:true`。
- 没有 TUI 时，只读工具（search/open_collection/get_status/list_playlists）照常可用，播放类工具与**全部写工具**返回 `needs_player`；`playerSessionAttached` 必须为 false。写工具先探活播放器，绝不回退到适配器本地网络；未知结果不重试，`operationId` 只用于宿主自身去重。

## 外部 Agent 配置示例

以下是通用 MCP 客户端格式示例，**尚未修改本机 DST 的配置**。DST 的实际配置入口应在接入时根据已安装版本核实。

```json
{
  "mcpServers": {
    "mora-music": {
      "command": "C:/Users/YOU/AppData/Local/Programs/MORA/mora.exe",
      "args": ["--mcp"]
    }
  }
}
```

直接使用 EXE 路径，避免宿主对 `.cmd` 或 shell 命令解析产生差异。外部 Agent 模式无需内置模型 API Key。播放类工具要求用户先在自己的终端里运行 `mora`（单播放器实例），宿主不会、也不应该另起一个播放器。
