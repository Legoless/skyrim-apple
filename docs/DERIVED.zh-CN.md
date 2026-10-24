# 仓库里与游戏有关的东西

English: [DERIVED.md](DERIVED.md)

当前源码树不含 Skyrim 程序、RomFS、密钥、存档、捕获的游戏着色器或翻译后的游戏着色器。这些留在持有卡带转储的那台机器上。

## 许可范围

本项目自行编写的代码和文档均采用仓库的 [MIT 许可证](../LICENSE)。
第三方代码保留下表列出的许可证和版权声明。
游戏衍生内容，包括本地翻译后的着色器和游戏截图，不在本项目 MIT 许可证的许可范围内。

## 共享的着色器

渲染器的清屏和呈现辅助着色器，以及 `skyhost/tests/fixtures/shaders/` 中的合成测试着色器，均由本项目自行编写，采用 MIT 许可证。这些测试独立验证渲染和资源绑定，不使用 Skyrim 着色器捕获。

Ryujinx 翻译器自带的 GLSL 辅助代码保留上游 MIT 许可证。

## 私有游戏着色器缓存

`skyrim-work/build/shaders/` 已被 Git 忽略。捕获的 Skyrim 程序及其翻译后的 `.metal`/`.json` 文件仍是游戏衍生内容；翻译并不使其成为项目自行编写的着色器。请仅在本地保存。当前流程及其限制见 [着色器捕获说明](../skyhost/README.md#local-game-shaders)。

不得将游戏衍生的着色器缓存、从游戏捕获生成的测试样本或游戏截图提交到仓库或包含在任何公开历史记录中。本发布版本不包含这些内容。

## 内置的第三方代码

| 位置 | 内容 | 许可证 |
|---|---|---|
| `skyhost/third_party/ryujinx` | Ryujinx 1.3.2 着色器翻译库（`Ryujinx.Graphics.Shader`、`Spv.Generator`）和 `Ryujinx.Common`，由 `skyhost/tools/dmsh_xlate` 使用 | MIT，见 `LICENSE.txt` |
| `skyhost/audio_renderer/vendor` | Ryujinx 音频渲染 DSP 子集 | MIT，见该目录中的许可证 |
| `skyhost/third_party/bcdec` | bcdec 0.985 BC1–BC7 纹理解码器；固定版本及 C 兼容性修正记录在 `UPSTREAM.json` 中 | 选择 MIT 许可证，见该目录中的 `LICENSE` |
| `skyhost/nvn/nvn_tex.c` | Tegra X1 块线性贴图布局，移植自 deko3d 的 `dk_image.cpp` | [zlib](../skyhost/nvn/LICENSE.deko3d)，Copyright (C) 2018-2020 fincs |

NVN 格式表和块线性解排布与这份贴图布局放在一起，都在 `skyhost/nvn/`。
