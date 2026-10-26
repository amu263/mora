# 音乐工具契约

机器可读清单由 `src/service.cpp::musicToolDefinitions()` 产生，通过 `tools/list` 返回；本文记录同一份契约与执行语义。**工具名和参数以运行中的 `tools/list` 为准。**

## 当前工具（17 个，随版本演进）

| 工具 | 状态 | 输入 | 输出/约束 |
| --- | --- | --- | --- |
| `music_search` | 已接通 | query, kind, limit, offset | 真实元数据，最多 30 条；无 Cookie、播放 URL；不自动播放。歌手/风格/场景/心情类系列请求优先用 `music_recommend_queue`（一次建队并入队），或取回一组 id 后用 `music_queue_edit(op="replace", ids=[…])` |
| `music_open_collection` | 已接通 | kind(album/playlist), id, offset, limit | 按需曲目页（最多 100 条），`offset` 是**真分页**；本页歌名为空的曲目会额外补一次详情（每次 ≤50 个 id），不返回播放地址，不自动播放 |
| `music_play` | 已接通（需 TUI） | kind(song/album/playlist), id, startIndex | 提交到**同一个** TUI 播放会话；song 用该曲替换队列，album/playlist 用**整个集合替换队列**并从 startIndex 开始（整张专辑/歌单走这里）；返回 accepted，真实播放状态用 `music_now_playing` 查询；无播放器时 `needs_player` |
| `music_control` | 已接通（需 TUI） | action(pause/resume/next/previous/seek/volume/quality), value, level | 修改同一会话；seek 单位为秒，volume 为 0..100；`quality` 需要 `level`（`standard/higher/exhigh/lossless/hires`），从下一首生效，实际档位仍取决于账号权限；不创建第二个播放器；无播放器时 `needs_player` |
| `music_now_playing` | 已接通（需 TUI） | 空对象 | 真实状态：曲目、队列位置、暂停、进度、音量、循环、账号实际获得的音质与采样率、正在唱的歌词行 |
| `music_get_lyrics` | 已接通 | songId, includeTranslation, maxLines, fromCurrent | 有界时间轴歌词（≤200 行）；不含流地址；没有播放器时按 songId 只读读取 |
| `music_get_status` | 已接通 | 空对象 | 能力清单 + `playerSessionAttached` + 当前会话快照 + 只读 `account:{signedIn,nickname}`（不含任何凭据）；未连接时 `playerSessionAttached` 必须为 false，本地会话文件损坏时仍返回可用结果（`signedIn:false`） |
| `music_list_playlists` | 已接通（真实账号） | ownedOnly（默认 false，含收藏） , limit ≤ 50, offset | 真实用户歌单；`canWrite` 由播放器按归属判断，适配器不得自行猜测 |
| `music_list_likes` | 已接通（真实账号） | limit ≤ 50, offset ≤ 5000 | 只读分页红心歌曲；冷启动先刷新一次账号红心集合；未登录 `needs_login`，真空红心 `no_likes` |
| `music_get_like` | 已接通（真实账号） | songId | 单曲是否已红心：`{songId, liked, likedTotal}`；只读，避免为判断一首歌而翻完整红心列表 |
| `music_set_like` | 已接通（真实账号） | songId, liked | 幂等的目标状态，不使用 toggle 语义；可重放 |
| `music_add_to_playlist` | 已接通（真实账号） | songId, playlistId, operationId | 只允许自己拥有的歌单（先校验再写，否则 `no_permission`）；重复映射 `already_present`；不自动重试 |
| `music_recommend_queue` | 已接通（需 TUI） | intent, count(默认 30，1..1000), source(auto/search/playlists/liked), seed | **一次调用**选出真实曲目、替换队列并从第一首开始播放；见下方专节 |
| `music_recommend` | 已接通（M5） | intent, source(auto/search/playlists), limit, seed | 有真实 ID 的候选与理由；**绝不播放、不收藏**；返回 `candidateSetId` |
| `music_play_random` | 已接通（M5） | candidateSetId 或 intent, seed | 从实际候选随机挑一首（排除正在播放的），提交给播放器；不会自动换曲重试 |
| `music_queue_list` | 已接通（M8；需 TUI） | limit ≤ 100, offset | 只读投影当前队列：`total`、0 基 `index`、`mode`、`repeat`、有界行窗口（`id/name/artist/album/durationSeconds/current`）；不返回流地址或 Cookie |
| `music_queue_edit` | 已接通（M8；需 TUI） | op, mode, seed, index, from, to, id, ids | 单次一个写操作：`set_mode/play_index/remove/move/clear/add_next/append/replace`；索引 **0 基**，与 `music_play.startIndex` 相同；越界与未知字段 `invalid_arguments`；无播放器 `needs_player`；不自动重试 |

工具级失败（参数非法、没有播放器、播放器忙、账号问题）返回 `result.isError:true`，并在 `structuredContent` 给出机器可读的 `error`：`invalid_arguments`、`needs_player`、`busy`、`queue_boundary`、`unknown_method`、`disconnected`、`needs_login`、`no_candidates`、`no_likes`、`no_permission`、`already_present`、`not_found`、`playback_unavailable`、`risk_cooldown`、`account_confirmation_required`、`session_error`。协议层错误（未知工具、畸形 JSON-RPC）仍是 JSON-RPC error。宿主不应靠解析文案判断失败原因。

`session_error` 专门表示**本机会话文件不可用**（缺失、被截断、DPAPI 解不开、超过 64 KiB 上限）；它不是网络失败，重试同一调用不会变好，需要用户重新登录。

## 批量建队列（`music_recommend_queue`）

“一次调用把 N 首歌排进队列并开始播放”，是系列请求的默认入口（取代逐首 `music_play` 或反复 `music_play_random`）。

| 参数 | 语义 |
| --- | --- |
| `source=search` | 用 `intent` 搜真实曲库（分页，去重），适合歌手/风格/场景/心情 |
| `source=playlists` | 从**账号自己与收藏的歌单**里取曲目；`intent` 被忽略（歌单没有关键词排序）；按 `seed` 随机起始歌单，最多读 4 个歌单，凑够 `count` 立即停止 |
| `source=liked` | 从红心歌曲里取；`intent` 被忽略 |
| `source=auto`（默认） | 先按 `intent` 搜索；**只有搜索完全为空**才回落到歌单 |

- `count` 默认 **30**，范围 1..1000。整次调用只做**一次** `queue_edit(op=replace)`，不会逐首提交，也不会按歌曲数量放大请求数。
- 返回 `{requestedCount, queuedCount, availableCount, source, searchIntent, changed, playback}`：`source` 是**实际生效**的来源（例如 `search+playlists`），`queuedCount < requestedCount` 时必须如实告知用户，不得圆成“已经放满”。
- 失败码：`no_candidates`（该来源确实取不到可播放曲目）、`no_likes`（该账号没有红心）、`needs_login`、`needs_player`、`risk_cooldown`。
- `auto` **刻意不做“补足”**：搜索只返回 3 首而用户要 30 首时，不会拿歌单曲目悄悄填满——那会改变用户要的东西。不足就报不足。

## 红心歌曲（读与写）

- `music_list_likes`：只读分页（`limit ≤ 50`、`offset ≤ 5000`），返回 `{total, offset, limit, items[]}`；`items` 每行与搜索/队列投影同构（`id/name/artist/album/durationSeconds`）。
- `music_get_like`：只读单曲判定，返回 `{songId, liked, likedTotal}`。红心集合是**网络态**（不随 `session.bin` 持久化），所以冷启动第一次调用会先刷新一次账号状态再判定。
- `music_set_like`：写，目标状态语义（`liked:true/false`），可重放；不做 toggle。
- 未登录时两个读工具都给 `needs_login` 而不是“没有红心歌”，这两种情况对用户是不同的。

## 队列与播放模式（M8）

两个工具和 `music_control` 走**同一条**路径：播放器在跑就把意图交给同一个 `PlaybackSession`；没有播放器就如实返回 `needs_player`。适配器不保存队列副本，也不启动第二个播放核心。

### `music_queue_list`（只读）

- 参数：`limit`（1..100，默认 100）、`offset`（0..10000，默认 0，**0 基**）。
- 返回：`{total, index, mode, repeat, offset, limit, items[]}`。
  - `total` 是队列真实长度；`index` 是**当前播放行**的 0 基下标；`mode` 取 `sequential` / `repeat_all` / `repeat_one` / `shuffle`。
  - `items` 是有界窗口，每行 `{position（0 基绝对下标）, id, name, artist, album, durationSeconds, current}`；`current` 恰好一行是 `true`（队列非空时）。
  - 为控制返回体大小，最多投影队列前 100 行；`offset` 是在这个有界窗口内的偏移。`total` 始终是完整队列长度。
- 只读：不改播放状态、不写账号、不返回流地址或 Cookie。空队列返回 `total:0, items:[]`，仍带 `mode`/`index`。

### `music_queue_edit`（每次调用一个写操作）

- 参数白名单：`op, mode, seed, index, from, to, id, ids`；出现其他字段 → `invalid_arguments`。
- `op` 取值与语义：

| op | 额外参数 | 语义 |
| --- | --- | --- |
| `set_mode` | `mode`（必填）、`seed`（可选 ≥1） | 常用播放器语义：`sequential` 顺序播完即停、`repeat_all` 列表循环、`repeat_one` 单曲循环、`shuffle` 随机。`mode` 先经 `parseQueueMode` 解析，因此 `random`/`loop`/`single`/`normal` 等别名也接受，回传的始终是 canonical 名。带 `seed` 时随机顺序可复现。 |
| `play_index` | `index`（必填，0 基） | 跳转到队列某一行开始播放；与 `music_play.startIndex` 同一基准。 |
| `remove` | `index`（必填，0 基） | 删除一行；当前行在被删行之后会前移一位，删到队尾则回退到新的最后一行。 |
| `move` | `from`、`to`（必填，0 基） | 把一行移动到新位置；正在播放的行跟着自己的**内容**走，而不是留在旧下标。 |
| `clear` | 无 | 清空整个队列并停止。 |
| `replace` | `id` 或 `ids`（二选一，最多 1000） | **换成这一组**：整队被替换成给定歌曲并从**第一首**开始播放。系列请求（歌手/风格/场景/心情）一次取回 5..20 首后用它建立队列，避免“只播一首”。 |
| `add_next` | `id` 或 `ids`（二选一） | “下一首插播”：把歌曲插到当前曲目之后，**保留**原有队列。 |
| `append` | `id` 或 `ids`（二选一） | 追加到队尾，**保留**原有队列。 |

- `ids` 上限：`replace` 为 **1000**（与播放器的整队上限一致），`add_next`/`append` 为 50；schema 的 `maxItems` 与执行层校验一致；`id` 与 `ids` 同时出现 → `invalid_arguments`。
- 返回新的队列投影：`{op, changed, total, index, mode, repeat, items[]}`；`replace`/`add_next`/`append` 额外返回 `requestedIds` 与 `metadataResolved`（`false` 表示只拿到 id、歌名等元数据缺失，播放器会经既有刷新流程补全，**不会编造歌名**）。
- **已知行为**：`music_play(kind=song)` 入队的当前行元数据（歌名/歌手/专辑/时长）**可能延迟到详情刷新**才出现；`replace`/`add_next`/`append` 由适配器随 `tracks:[{id,name,artist,album,duration}]` 一并送入，通常立即带元数据。两种情况下 id 都是真实的，模型不得据空字段编造曲目信息。
- 越界索引（`index >= total`）→ `invalid_arguments`，且**不产生部分写入**；`play_index` 指向当前行是幂等 no-op。未知 `op`、缺少必填参数、类型错误都在接触播放器之前拒绝。
- 单次调用只发一个写操作，服务端不自动重试；`busy`（播放器正在受理别的请求）如实上报，由调用方决定是否再发一次。
- **`changed` 与随机序**：`op=set_mode` 带**显式 `seed`**、mode=shuffle 且队列 >1 首时，即使 mode/index 未变也回 `changed:true`（语义=可听顺序被重建）；其余情况 `changed` 表示 mode/total/index/行顺序投影是否变化。因此 **`changed:false` 不能等价于“走序未变”**：不带 seed 的重复 `set_mode shuffle` 仍可能重排（`PlaybackSession::setMode` 在 `seed != 0` 时重建，未给 seed 时按默认 1），而回包仍是 `changed:false`。同一 seed + 相同起始行可复现；任何一次 `next`（含播放自然结束的推进）都会消耗随机序，因此可复现性只在没有其它推进者时成立。
- **随机序回归检查**：`python scripts/m8_queue_check.py --shuffle-forensics`（exit 0 = 同一 seed 在 5 条不同前序路径下走序一致且 `changed` 语义正确；exit 1 = 回归），报告 `docs/verification/m8-shuffle-forensics.json`。

### 替换 vs 追加（模型必须靠描述做对选择）

| 用户说 | 正确操作 | 结果 |
| --- | --- | --- |
| “换成/只听/播放这张专辑” | `music_play(kind=album\|playlist)` 或 `music_queue_edit(op="replace", ids=[…])` | 队列被整组替换，从第一首播放 |
| “再加上/再来点/接着放/排在后面” | `music_queue_edit(op="append")` 或 `op="add_next"` | 原有队列保留，只增不换 |

### 系列播放常识（写进工具描述、供模型遵循）

- **整张专辑/整个歌单**：`music_play(kind=album|playlist, id)` 或 `music_open_collection` → 替换队列并播放整个集合。
- **歌手/风格/场景/心情**（没有明确专辑）：先 `music_search(query=关键词, limit 10..30)` 取回一组 id，再 `music_queue_edit(op="replace", ids=[…])` 一次建立 5..20 首的队列并从第一首播放。**只播一首属于缺陷**。
- **模糊请求**（“随便来点歌”）：`music_recommend` → `music_play_random`（或 `op="replace"` 用候选建队列），不要反问。
- 工具返回 `accepted` 只表示已提交；真实播放状态必须用 `music_now_playing` 确认，不得编造。

### 示例

```json
{"name": "music_queue_list", "arguments": {"limit": 20, "offset": 0}}
{"name": "music_queue_edit", "arguments": {"op": "set_mode", "mode": "shuffle", "seed": 42}}
{"name": "music_queue_edit", "arguments": {"op": "play_index", "index": 3}}
{"name": "music_queue_edit", "arguments": {"op": "add_next", "id": 539928216}}
{"name": "music_queue_edit", "arguments": {"op": "replace", "ids": [1, 2, 3, 4, 5]}}
{"name": "music_queue_edit", "arguments": {"op": "append", "ids": [1, 2, 3]}}
{"name": "music_queue_edit", "arguments": {"op": "remove", "index": 0}}
{"name": "music_queue_edit", "arguments": {"op": "move", "from": 4, "to": 1}}
{"name": "music_queue_edit", "arguments": {"op": "clear"}}
```

无播放器时：

```json
{"isError": true, "structuredContent": {"ok": false, "error": "needs_player", "message": "no MORA player is attached; run mora in a terminal first"}}
```

参数非法时（无播放器也一样，校验发生在探活之前）：

```json
{"isError": true, "structuredContent": {"ok": false, "error": "invalid_arguments", "message": "index must be 0..9999"}}
```

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
- **队列方法**（M8）：`queue_list`（只读，`{limit,offset}` → `{total,index,mode,repeat,items[]}`）与 `queue_edit`（`{op,mode?,seed?,index?,from?,to?,ids?,tracks?}`）。`replace`/`add_next`/`append` 由适配器先把 id 解析成 `tracks:[{id,name,artist,album,duration}]` 再一并送出，UI 线程只做队列写入、不发起网络请求；解析不到元数据时仍以真实 id 占位入队。两者都不创建第二个播放器。
- **关闭宿主（MCP 进程）不停止播放**：播放器生命周期由 TUI 决定，退出 TUI 才停止音频。这样不会留下孤儿后台进程，也不会让宿主一退出就静音。
- 播放控制的状态变化是异步的（音频线程处理命令，约 100ms）。`music_play`/`music_control` 返回 accepted 与即时快照，调用方应轮询 `music_now_playing` 确认最终状态。

## 限流、风控与账号缓存

- **节流门在进程里，不在单个网络后端里**：读请求之间至少 850 ms，写请求之间至少 3 s，命中风控码后进入 5 分钟冷却（期间调用直接返回 `risk_cooldown`，不会排队干等）。因为这份状态是进程级的，适配器可以在每次工具调用后丢弃网络后端（保持内存小）而**不会**让下一次调用重新开始“连发”；同一进程内的多个后端与多个调用方共享这道门，并发调用也绕不过它。
- **账号态有缓存**：`uid`、昵称与红心集合在服务层缓存。`account()` 与自己的红心写入刷新它，`logout()` 清空它，红心集合按 5 分钟 TTL 过期（外部改动能被看到）。所以“翻页读红心”只付一次账号往返，之后每页只剩自己的元数据请求。
- 两者合起来的实测：连续 24 次 `music_list_likes`（每次调用后都 `releaseBackend`，模拟外部宿主翻页）**全部成功**，总耗时 21.7 s（≈0.9 s/次）；修复前同样的连发在第 11 次左右开始返回 `risk_cooldown`。
- 给宿主 agent 的建议：按需翻页就行，不必自己再 sleep；同一个播放会话只接一个适配器（会话管道本身也只允许一个连接）。

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
- 没有 TUI 时，只读工具（search/open_collection/get_lyrics/get_status/list_playlists/list_likes/get_like/recommend）照常可用，播放类工具、`music_queue_list` 与**全部写工具**（含 `music_queue_edit`）返回 `needs_player`；`playerSessionAttached` 必须为 false。写工具先探活播放器，绝不回退到适配器本地网络；未知结果不重试，`operationId` 只用于宿主自身去重。

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
