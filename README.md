# MORA 云间

MORA 是一款 Windows 终端音乐播放器，支持网易云音乐搜索与播放、扫码登录、个人歌单、歌词和音乐聊天。界面由键盘操作，也支持鼠标点击。运行时不需要安装 Python、Node.js 或浏览器。

![MORA 首页](docs/home-preview.png)

> 当前为 Windows x64 预览版。MORA 是独立开源项目，与网易云音乐没有隶属关系。能否播放某首歌曲取决于你的账号权限和服务端返回结果。

## 下载与启动

> 公开发行下载因隐私审查暂时关闭。审查完成后，Windows 成品会重新发布到 Releases；当前可按文末步骤从源码构建。

发行恢复后，下载名称带 **`win-x64`** 的 ZIP：

1. 解压整个 ZIP。解压后的目录顶层应能看到 **`mora.exe`**、`mora.cmd` 和 `install.cmd`。
2. 在该目录双击 `mora.cmd`；从终端启动时，先进入解压目录，再运行 `.\mora.cmd`（CMD 与 PowerShell 均可）。

希望在任意目录输入 `mora` 时，先在解压目录运行 `install.cmd`，再**打开一个新的** CMD 或 PowerShell 窗口输入 `mora`。安装到当前用户目录，不需要管理员权限。直接运行 `mora.exe` 也可以。

**请下载 Releases 中名称带 `win-x64` 的 ZIP。** GitHub 的 **Code → Download ZIP** 和 `Source code (zip)` 是源码，不含 EXE；它们用于自行构建，不能直接运行 `mora.cmd`。

建议终端窗口至少 116×36，最低 78×24。首次启动会询问称呼，并用三页简短引导介绍听歌、登录和可选的聊天功能；按 Enter 继续，Esc 可跳过。

## 开始听歌

在底部输入命令并按 Enter。输入 `/` 可查看命令提示，Tab 可补全。

| 想做什么 | 输入 |
| --- | --- |
| 搜索歌曲 | `/search 歌名 歌手` |
| 播放搜索结果 | `/play 1`，或选中结果按 Enter |
| 暂停、下一首、上一首 | `/pause`、`/next`、`/prev` |
| 调整音量 | `/volume 65` |
| 查看当前歌词 | `/lyrics` |
| 登录网易云音乐 | `/login qr`，用 App 扫码确认 |
| 查看个人歌单 | `/library` |
| 红心或加入自己的歌单 | `/like`、`/add` |
| 选择音频设备 | `/audio-device` |
| 查看设置或重看引导 | `/settings`、`/guide` |
| 查看全部命令 | `/help` |

可以先搜索再决定是否登录。个人歌单、红心及部分曲目的完整播放需要账号权限；扫码登录是推荐方式。请求高音质时，播放器会显示实际取得的音质，不保证每首歌都有无损或 Hi-Res。

## 音乐聊天（可选）

输入 `/ai`，按界面提示设置模型密钥和模型 ID，然后用 `/chat 你想听什么` 开始对话。聊天可以结合当前歌曲与歌词，也可以按你的明确要求使用音乐操作。内置默认配置使用 OpenCode Go 的 `deepseek-v4.1-flash`；使用该服务需要你自己的可用密钥和相应服务权限。

用 `/persona` 切换助手角色，用 `/effort` 调整模型思考强度。未配置密钥不影响普通搜索和播放。

## 数据与许可

账号会话和模型密钥使用 Windows DPAPI 加密，保存在当前 Windows 用户的 `%LOCALAPPDATA%\MORA`，**不包含在下载包或源码中**。同一台电脑上运行新版时会检测已有登录，但会先询问是否继续使用；按 `D` 可清除本机登录并改用游客模式，按 Esc 可退出。模型密钥与账号会话分开保存；在 `/ai` 中按 `D` 可清除本机模型密钥。也可用 `--data-dir` 指定独立的数据目录。

播放器只按账号实际拥有的音乐权限播放，不提供解锁或绕过服务限制的功能。

源码按 [LICENSE](LICENSE) 开源；第三方组件及协议来源见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

## 从源码构建

源码包和 GitHub 自动生成的源码快照不包含预编译 EXE。需要 Windows x64、Python（仅用于下载构建工具）和 PowerShell：

```powershell
python scripts/bootstrap.py
./build.ps1
./build.ps1 -Test
```

构建完成后运行 `./mora.cmd`。发行版 ZIP 不包含开发用原始肖像。
