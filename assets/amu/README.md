# 阿木首页像素素材

形象依据用户本轮最新提供的蓝发侧脸参考，明确排除旧表情包和原先银白发占位角色。蓝短发、白眉、蓝眼、带 Amu 标记的白蓝色头戴耳机和白卫衣为当前约束。

流程：内置 image_gen 按最新附件生成像素风源图 → 用户指定的 [perfectPixel](https://github.com/theamusing/perfectPixel) NumPy 后端规整 → 同一精炼网格采样透明度 → 32 色预算无抖动量化 → 底部补透明行 → 生成 C++ 索引色数组。

- `amu-source.png`：本轮生成的源图，不是旧表情包。
- `amu-pixel.png`：最终 48×50 RGBA 素材。
- `amu-pixel-preview.png`：最近邻放大预览。
- `amu_sprite.hpp`：运行时编译进 EXE 的色板与索引，约 2.5 KB。
- `prompt.txt`：本次实际生成规范。
- `provenance.json`：工具提交、参数、尺寸、哈希与参考来源。

perfectPixel 的精炼输出是 48×49。仅在底部补 1 透明行，方便两个像素行合成一个终端 `▀`；不裁掉衣领或拉伸该精炼结果。首页首版为静态头像，保留参考表情，不用临时改眼睛像素伪造眨眼。

程序使用已生成的数组，运行时不加载 Python、perfectPixel、源 PNG 或额外图像服务。普通源码构建不必重新生图或安装素材工具。

需要重新规整时，先将固定提交的 perfectPixel 放到项目的 `tools/perfectPixel`，再执行：

```powershell
python scripts/build_amu_asset.py --size 48 --colors 32
```

重新生图请使用原始用户参考与 `prompt.txt`，不要拿首页整张截图或旧表情包代替角色参考。
