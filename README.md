# Warband-Native-Rebuild

> **Unofficial fan project**: This project is not affiliated with, endorsed by, or authorized by
> TaleWorlds Entertainment. This project does not provide any official Mount & Blade: Warband game
> files, assets, data, or executables. Running this project requires a legally owned copy of the game.

> **非官方粉丝项目**：本项目与 TaleWorlds Entertainment 无关联，未获其背书或授权。
> 本项目不提供《骑马与砍杀：战团》的任何官方游戏文件、资产、数据或可执行文件。
> 运行本项目需用户自行拥有正版游戏。

## English

A from-scratch C++/Vulkan renderer that reads original Mount&Blade: Warband assets and **does not depend on WSE2**.

### v0.1-static-render — minimal static-rendering loop ✅

Verified pipeline:

```
original .sco scene    ──► in-house SCO parser (sco.h)
original .brf assets   ──► in-house BRF parser (brf.h)
original .dds textures ──► in-house DXT1/3/5 software decoder (dds.h)
                              │
                              ▼
        in-house Vulkan renderer (vk_scene.cpp; no SDK, loads vulkan-1.dll at runtime)
                              │
                              ▼
       Win32 window: 145 scene props + 107×108 terrain mesh (layered textures)
       + original skybox_clearday dome + distance fog + directional light
```

> Legal scope: this repository contains **only original code and documentation**.
> It includes no Mount&Blade copyrighted assets (models/textures/scene files), no translated
> Native module TXT content, and no local paths or personal information. At runtime, point it
> at a game directory that you legally own.

### Layout

- `src/`
  - `brf.h` — BRF container parser (mesh/material/texture/shader/skeleton etc., independent implementation)
  - `dds.h` — DDS DXT1/3/5 software decoder to RGBA8
  - `sco.h` — SceneObj parser (objects + ai_mesh skip + all terrain layers)
  - `vk_scene.cpp` — main scene renderer
  - `vk_mesh.cpp` — minimal single-mesh + texture validation program
  - `brf_dump / obj_export / dds_dump / sco_dump` — format validation tools
  - `vk_probe.cpp` — Vulkan toolchain smoke test (dynamically loads vulkan-1.dll)
  - `tmp_terrain.cpp` / `tmp_test.cpp` — terrain-layer / BRF-parse diagnostics
  - `shot.ps1` — captures the vkSceneWnd window (`-OutDir` chooses the output folder, default `shots/`)
- `shader/` — GLSL sources (`lit.*` = directional light version; `mesh.*`/`tex.*` = unlit validation versions)
- `tools/shader-tool/` — GLSL → SPIR-V compiler tool (npm `@webgpu/glslang`)

### Build

Dependencies (not in this repo; obtain them yourself):

1. `Vulkan-Headers` (headers only, no Vulkan SDK required): `git clone https://github.com/KhronosGroup/Vulkan-Headers`
2. Compiler: w64devkit (GCC, works out of the box on Windows) or another MinGW/MSVC
3. SPIR-V: `cd tools/shader-tool && npm install`, then compile `shader/*.vert|frag` into `*.spv` with `@webgpu/glslang`

```sh
g++ -std=c++20 -O2 src/vk_scene.cpp -o vk_scene.exe \
    -I <vulkan-headers>/include -luser32 -lgdi32
```

### Run

```sh
vk_scene.exe \
  "$GAMEDIR/Modules/Native/SceneObj/scn_town_1_center.sco" \
  "$GAMEDIR/Modules/Native/scene_props.txt" \
  "$GAMEDIR/CommonRes" \
  "$GAMEDIR/Textures"
```

Press ESC in the window to exit. See the header comment of `vk_mesh.cpp` for its usage.
Example paths are illustrative only; no official game files are bundled with this repository.

### Independent format interoperability notes (our own findings; cross-checking against public docs is welcome)

- BRF: token stream `rfver `(i32) → texture/shader/material/mesh/skeleton/skeleton_anim/body → `end`;
  every object type is preceded by a u32 count; strings = u32 length + bytes.
- BRF meshes face the XZ plane; render mapping is `x'=x, y'=z, z'=-y`.
- SCO: magic i32 `-717`, version 4; object positions are stored on disk as (x, z, y).
- SCO terrain layer index `0..10` matches the row order in `Data/ground_specs.txt` (gray_stone…path);
  `-7793` is the elevation float, `-12565` is vertex color. Terrain cell size is 2 m, grid origin (0,0).
- Column 3 of `ground_specs.txt` is a BRF **material name**, which maps to a dds via the material table's diffuse field.

Format-reference credits: OpenBRF (GPLv2, used only as a source of format facts), Swyter/mab-tools 010 templates,
cmpxchg8b forum documentation, cuellius/ScoTools. The parsers in this repo are independent implementations.

### Roadmap

- [x] v0.1 static render: BRF/SCO/DDS parsing + Vulkan on screen + layered terrain texturing + directional light
- [x] Skybox / fog / far clipping (skyboxes.brf dome + linear distance fog + zf=800)
- [x] Character & camera controls (third-person placeholder humanoid: WASD moves the character, arrow keys orbit, QE zooms, ground-snapped)
- [x] Collision & pathfinding (SCO ai_mesh navmesh: 247 vertices / 422 edges / 173 faces; character constrained to walkable faces, slides along walls; G triggers A* pathfinding to the farthest walkable face)
- [ ] Full original-scene experience

### License scope

The MIT license in this repository applies only to original source code authored by the project author.

It does not apply to:

- Mount & Blade: Warband and any of its official code, assets, data, text, audio, fonts, or trademarks;
- any third-party or environment dependencies (each governed by its own project's license);
- official game files owned locally by users.

This repository does not distribute any official game files. Example paths are illustrative only;
running requires a legally owned game environment provided by the user.

### EULA notice

This project is an independent interoperability effort. It does not distribute any official game files.
Users are solely responsible for ensuring their use complies with the Mount & Blade: Warband EULA
and all applicable laws. The author claims no endorsement by or affiliation with TaleWorlds Entertainment.

### License

[MIT](LICENSE)

---

## 中文

从原版 Mount&Blade: Warband 资源出发、**不依赖 WSE2** 的自研 C++/Vulkan 渲染器。

## v0.1-static-render —— 静态渲染最小闭环 ✅

当前已验证链路：

```
原版 .sco 场景  ──► 自研 SCO 解析（sco.h）
原版 .brf 资源  ──► 自研 BRF 解析（brf.h）
原版 .dds 贴图  ──► 自研 DXT1/3/5 软件解码（dds.h）
                         │
                         ▼
             自研 Vulkan 渲染器（vk_scene.cpp，无 SDK，动态加载 vulkan-1.dll）
                         │
                         ▼
         Win32 窗口：145 个场景道具 + 107×108 地形地面（分层贴图）
         + 原版 skybox_clearday 天空穹顶 + 距离雾 + 方向光
```

> 合法范围说明：本仓库**只包含自研代码与文档**。不含任何 Mount&Blade 版权资产
> （模型/贴图/场景文件），不含 Native 模块 TXT 翻译内容，不含本地路径或个人信息。
> 运行时请在你自己合法拥有的游戏目录上指定资源路径。

## 目录

- `src/`
  - `brf.h` —— BRF 容器解析器（mesh/material/texture/shader/skeleton 等，独立实现）
  - `dds.h` —— DDS DXT1/3/5 软件解码为 RGBA8
  - `sco.h` —— SceneObj 解析器（物件 + ai_mesh 跳过 + terrain 全层）
  - `vk_scene.cpp` —— 场景渲染主程序
  - `vk_mesh.cpp` —— 单网格+贴图最小验证程序
  - `brf_dump / obj_export / dds_dump / sco_dump` —— 格式验证工具
  - `vk_probe.cpp` —— Vulkan 链路自检（动态加载 vulkan-1.dll）
  - `tmp_terrain.cpp` / `tmp_test.cpp` —— 地形分层 / BRF 连通性诊断
  - `shot.ps1` —— 截取 vkSceneWnd 窗口（`-OutDir` 指定输出目录，默认 `shots/`）
- `shader/` —— GLSL 源（`lit.*` 为方向光版本；`mesh.*`/`tex.*` 为无光验证版本）
- `tools/shader-tool/` —— GLSL → SPIR-V 编译工具（npm `@webgpu/glslang`）

## 构建

依赖（均不在本仓库内，自行获取）：

1. `Vulkan-Headers`（仅头文件，无需 Vulkan SDK）：`git clone https://github.com/KhronosGroup/Vulkan-Headers`
2. 编译器：w64devkit（GCC，Windows 下开箱即用）或其他 MinGW/MSVC
3. SPIR-V：`cd tools/shader-tool && npm install`，用 `@webgpu/glslang` 把 `shader/*.vert|frag` 编成 `*.spv`

```sh
g++ -std=c++20 -O2 src/vk_scene.cpp -o vk_scene.exe \
    -I <vulkan-headers>/include -luser32 -lgdi32
```

## 运行

```sh
vk_scene.exe \
  "$GAMEDIR/Modules/Native/SceneObj/scn_town_1_center.sco" \
  "$GAMEDIR/Modules/Native/scene_props.txt" \
  "$GAMEDIR/CommonRes" \
  "$GAMEDIR/Textures"
```

窗口 ESC 退出。`vk_mesh.exe` 的用法见 `vk_mesh.cpp` 头部注释。示例路径仅作说明，不附带官方文件。

## 独立格式互操作性笔记（自研经验，欢迎对照公开资料复核）

- BRF：token 流 `rfver `(i32) → texture/shader/material/mesh/skeleton/skeleton_anim/body → `end`；
  每类对象先读 u32 数量；字符串 = u32 长度 + 字节。
- BRF 网格正面在 XZ 平面；渲染坐标映射 `x'=x, y'=z, z'=-y`。
- SCO：魔数 i32 `-717`，version 4；物件 pos 磁盘序为 (x, z, y)。
- SCO terrain 层 index `0..10` 对应 `Data/ground_specs.txt` 行序（gray_stone…path），
  `-7793` 是高程浮点，`-12565` 是顶点色。terrain cell 边长 2m，网格原点 (0,0)。
- `ground_specs.txt` 第 3 列是 BRF **材质名**，需经材质表的 diffuse 字段映射到 dds。

格式参考致谢：OpenBRF（GPLv2，仅作格式事实来源）、Swyter/mab-tools 010 模板、
cmpxchg8b 论坛文档、cuellius/ScoTools。本仓库解析器为独立实现。

## 路线图

- [x] v0.1 静态渲染：BRF/SCO/DDS 解析 + Vulkan 上屏 + 地形分层贴图 + 方向光
- [x] 天空盒 / 雾 / 远裁剪（skyboxes.brf 穹顶 + 线性距离雾 + zf=800）
- [x] 角色与相机操控（第三人称占位人形：WASD 走角色，方向键绕角色转头，QE 拉近拉远，贴地行走）
- [x] 碰撞与寻路（SCO ai_mesh 导航网格：247 顶点/422 边/173 面；角色限走可走面、沿墙滑动；G 键 A* 自动寻路到最远可走面）
- [ ] 完整原版场景体验

## 许可证范围

本仓库的 MIT 许可证仅适用于由本项目作者原创的源代码。

MIT 许可证不适用于：
- 《骑马与砍杀：战团》及其任何官方代码、资产、数据、文本、音频、字体、商标；
- 任何第三方依赖或环境依赖（其许可证由各自项目决定）；
- 用户本地拥有的官方游戏文件。

本仓库不分发任何官方游戏文件。示例路径仅作说明，运行需用户自行提供合法拥有的游戏环境。

## EULA 责任声明

本项目是独立的互操作性尝试，不分发任何官方游戏文件。用户自行负责确保其使用符合
《骑马与砍杀：战团》EULA 及所有适用法律。作者声明与 TaleWorlds Entertainment 无背书或关联。

## 许可证

[MIT](LICENSE)
