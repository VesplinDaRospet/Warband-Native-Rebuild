# Warband-Native-Rebuild

> **Unofficial fan project**: This project is not affiliated with, endorsed by, or authorized by
> TaleWorlds Entertainment. This project does not provide any official Mount & Blade: Warband game
> files, assets, data, or executables. Running this project requires a legally owned copy of the game.

> **非官方粉丝项目**：本项目与 TaleWorlds Entertainment 无关联，未获其背书或授权。
> 本项目不提供《骑马与砍杀：战团》的任何官方游戏文件、资产、数据或可执行文件。
> 运行本项目需用户自行拥有正版游戏。

## English

A from-scratch C++/Vulkan renderer that reads **user-provided** Mount&Blade: Warband assets and **does not depend on WSE2**.

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

> **MIT coverage**: the MIT license applies only to original code authored by the project author.
> Full compliance policy, prohibited contributions, and enforcement rules: see **[COMPLIANCE.md](COMPLIANCE.md)**.

### World map / 大地图

`vk_map` parses a module's `map.txt` (vertex + triangle mesh, each triangle tagged with a terrain code) and renders the campaign map, grouping triangles by terrain type and applying the matching ground texture. Party icons and positions are read from the module's own data files at runtime. The player party is a cyan badge, AI parties are red badges, and settlement icons use the module author's own icons.

Controls: **WASD** moves the player party in real time (**Shift** speeds up, impassable water/river slides), **click** paths to a point (yellow destination marker), clicking a settlement marches there and auto-enters its scene (`M` returns to the map, keeping the party position), **Space** pauses game time, **QE** zooms with a proportional camera height, **arrow keys** rotate, **ESC** exits. AI parties patrol around their spawn points; touching one triggers an encounter notification (battle comes later).

```
vk_map "<module>\map.txt" "<game root>\Textures"
```

Game files such as `map.txt` and `.dds` textures are **not** distributed with this repository; point the tool at your own game installation.

### Battle scene / 战斗场景

Touching an AI party on the map enters a battle scene from the module's own scene files (`vk_scene --battle`). The player and several enemy troops are rendered as separate character groups; enemies close in and strike back, and **left mouse click** attacks the nearest enemy inside the weapon's reach (read from the module's own item data: `weapon_length` + arm reach). HP is tracked for both sides; when all enemies fall you win, when the player's HP reaches zero you lose, and either way the campaign map is relaunched with the result (and a defeated party disappears from the map).

Characters wear gear loaded from the game's own assets: a skinned armor mesh replaces/overlays the body part, and a rigid weapon mesh follows the hand bone. Attacking plays the game's own one-handed swing animation (`attacks_single_righttoleft`), blended in from the idle pose over 0.18 s, with the attack cooldown derived from the animation length and the weapon's `speed_rating`. All animation, model and texture assets are the user's own game files; this project ships only the loader and renderer code.

### Layout

- `src/`
  - `brf.h` — BRF container parser (mesh/material/texture/shader/skeleton etc., independent implementation)
  - `dds.h` — DDS DXT1/3/5 software decoder to RGBA8
  - `sco.h` — SceneObj parser (objects + ai_mesh skip + all terrain layers)
  - `map.h` — campaign `map.txt` parser (vertices, triangles, terrain codes)
  - `vk_scene.cpp` — main scene renderer
  - `vk_map.cpp` — campaign map renderer with click-to-move BFS pathfinding
  - `vk_mesh.cpp` — minimal single-mesh + texture validation program
  - `brf_dump / obj_export / dds_dump / sco_dump` — format validation tools
  - `bone_dump / glob_bone / anim_dump / anim_probe` — skeleton / animation diagnostics (bone names, bind positions, animation frame listing)
  - `vk_probe.cpp` — Vulkan toolchain smoke test (dynamically loads vulkan-1.dll)
  - `tmp_terrain.cpp` / `tmp_test.cpp` — terrain-layer / BRF-parse diagnostics
  - `shot.ps1` — captures the vkSceneWnd window (`-OutDir` chooses the output folder, default `shots/`)
- `shader/` — GLSL sources (`lit.*` = directional light version; `ground.*` = 3-texture terrain blending; `mesh.*`/`tex.*` = unlit validation versions)
- `tools/shader-tool/` — GLSL → SPIR-V compiler tool (npm `@webgpu/glslang`)

### Build

Dependencies (not in this repo; obtain them yourself):

1. `Vulkan-Headers` (headers only, no Vulkan SDK required): `git clone https://github.com/KhronosGroup/Vulkan-Headers`
2. Compiler: w64devkit (GCC, works out of the box on Windows) or another MinGW/MSVC
3. SPIR-V: `cd tools/shader-tool && npm install`, then compile `shader/*.vert|frag` into `*.spv` with `@webgpu/glslang`

```sh
g++ -std=c++20 -O2 src/vk_scene.cpp -o vk_scene.exe \
    -I <vulkan-headers>/include -luser32 -lgdi32
g++ -std=c++20 -O2 src/vk_map.cpp -o vk_map.exe \
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
- BRF meshes face the XZ plane; standalone rendering maps `x'=x, y'=z, z'=-y`. Scene props additionally apply
  the SCO placement matrix as `S·M·S` (swap y/z, multiply, swap back) with height kept positive.
- SCO: magic i32 `-717`, version 4; object positions are stored on disk as (x, z, y).
- SCO terrain layer index `0..10` matches the row order in `Data/ground_specs.txt` (gray_stone…path);
  `-7793` is the elevation float, `-12565` is vertex color. Terrain cell size is 2 m, grid origin (0,0).
- Column 3 of `ground_specs.txt` is a BRF **material name**, which maps to a dds via the material table's diffuse field.

Format-reference credits: OpenBRF (GPLv2, used only as a source of format facts), Swyter/mab-tools 010 templates,
cmpxchg8b forum documentation, cuellius/ScoTools. The parsers in this repo are independent implementations.

### Roadmap

- [x] v0.1 static render: BRF/SCO/DDS parsing + Vulkan on screen + layered terrain texturing + directional light
- [x] Skybox / fog / far clipping (skyboxes.brf dome + linear distance fog + zf=800)
- [x] Character & camera controls (original `man_body` mesh + `skel_human` skeleton skinning + stand/walk/run animations, CPU-skinned)
- [x] v0.2-1 real prop collision: native `bo_*` collision bodies (sphere/capsule/manifold per-face), free player movement, vertical overlap filter
- [x] v0.2-2 true terrain blending: single ground mesh with 3 weighted textures + leveling vertex colors
- [x] v0.3 campaign map: `map.txt` renderer with terrain-coded texturing and click-to-move BFS pathfinding
- [x] v0.4 map↔scene loop: settlement icons from `map_icons.txt`/`parties.txt`, click to enter the matching `.sco` scene, `M` returns to the map
- [x] v0.5 map party system: real-time player party movement, camera follow, game-time/pause HUD, AI patrol parties, encounter trigger, minimum-screen-size party/settlement badges
- [x] v0.6-1 battle scene rendering: player + enemy troops in a module battle scene (`vk_scene --battle`), auto-entered from map encounters
- [x] v0.6-2 basic combat mechanics: left-click attack with hit cone, enemies close in and strike back, HP tracking, deaths, win/lose returns to the campaign map with the result
- [x] v0.7-1 troop gear rendering: skinned armor mesh + rigid weapon mesh from the game's own assets, third-person camera behind the character
- [x] v0.7-2 attack animation: the game's own `attacks_single_righttoleft` swing with a 0.18 s pose blend; combat reach and cooldown read from the module's own item data (`weapon_length`, `speed_rating`)
- [x] v0.7-3 per-party armor: each enemy party wears its own faction armor rendered from module resources
- [x] v0.7-4 data-driven combat values: attack damage and hit points now come from the module's own data instead of placeholders
- [x] v0.7-5 fog fix: per-object model matrix in the push constant, characters shaded in world space (no more washed-out fog on characters)
- [x] v0.7-6 per-party weapons + four attack directions: each party picks its own melee weapon; swing damage drives both sides; attack direction (righttoleft/lefttoright/overswing/thrust) follows the last movement direction; weapon grip corrected (handle across the palm, blade forward); enemy animation decoupled from the player
- [ ] Next: scene interaction (entries/doors), then gameplay-logic rewrite

### License

[MIT](LICENSE)

---

## 中文

从**用户自行提供的** Mount&Blade: Warband 资源出发、**不依赖 WSE2** 的自研 C++/Vulkan 渲染器。

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

> **MIT 覆盖范围**：MIT 许可证仅适用于由本项目作者原创的代码。
> 完整合规政策、禁止行为与执行规则见 **[COMPLIANCE.md](COMPLIANCE.md)**。

## 大地图 / World map

`vk_map` 解析模组的 `map.txt`（顶点 + 三角形网格，每个三角形带地形码），按地形分组贴图渲染战役大地图。队伍图标与位置在运行时从模组自带的数据文件读取。玩家队伍显示为青色圆标，AI 野队为红色圆标，定居点使用模组作者自带的图标。

操作：**WASD** 实时行军（**Shift** 加速，水域/河流滑墙），**点击**寻路（黄色目的地标记），点定居点会行军过去并自动进城（场景按 `M` 返回，队伍位置带回大地图），**空格**暂停游戏时间，**QE** 缩放（镜头高度按比例同步抬升），**方向键** 旋转，**ESC** 退出。AI 野队绕出生点巡逻，撞上会触发遭遇提示（战斗后续版本接上）。

```
vk_map "<模组目录>\map.txt" "<游戏根目录>\Textures"
```

`map.txt`、`.dds` 等游戏文件**不随本仓库分发**，请把工具指向你自己的游戏安装。

## 战斗场景 / Battle scene

地图上撞到 AI 野队会进入模组自带场景文件的战斗场景（`vk_scene --battle`）。玩家与多名敌方 troops 按独立角色组渲染（敌方偏红染色）；敌方会逼近并反击，**鼠标左键**攻击 2.6 米 / 约 100° 扇区内最近的敌人。双方都有血量：敌方全灭 = 胜，玩家血量归零 = 败，结束后自动回到大地图并显示结算（被消灭的野队从地图上消失）。

## 目录

- `src/`
  - `brf.h` —— BRF 容器解析器（mesh/material/texture/shader/skeleton 等，独立实现）
  - `dds.h` —— DDS DXT1/3/5 软件解码为 RGBA8
  - `sco.h` —— SceneObj 解析器（物件 + ai_mesh 跳过 + terrain 全层）
  - `map.h` —— 战役大地图 `map.txt` 解析器（顶点、三角形、地形码）
  - `vk_scene.cpp` —— 场景渲染主程序
  - `vk_map.cpp` —— 大地图渲染 + 点击寻路（BFS）
  - `vk_mesh.cpp` —— 单网格+贴图最小验证程序
  - `brf_dump / obj_export / dds_dump / sco_dump` —— 格式验证工具
  - `vk_probe.cpp` —— Vulkan 链路自检（动态加载 vulkan-1.dll）
  - `tmp_terrain.cpp` / `tmp_test.cpp` —— 地形分层 / BRF 连通性诊断
  - `shot.ps1` —— 截取 vkSceneWnd 窗口（`-OutDir` 指定输出目录，默认 `shots/`）
- `shader/` —— GLSL 源（`lit.*` 为方向光版本；`ground.*` 为三贴图地形混合；`mesh.*`/`tex.*` 为无光验证版本）
- `tools/shader-tool/` —— GLSL → SPIR-V 编译工具（npm `@webgpu/glslang`）

## 构建

依赖（均不在本仓库内，自行获取）：

1. `Vulkan-Headers`（仅头文件，无需 Vulkan SDK）：`git clone https://github.com/KhronosGroup/Vulkan-Headers`
2. 编译器：w64devkit（GCC，Windows 下开箱即用）或其他 MinGW/MSVC
3. SPIR-V：`cd tools/shader-tool && npm install`，用 `@webgpu/glslang` 把 `shader/*.vert|frag` 编成 `*.spv`

```sh
g++ -std=c++20 -O2 src/vk_scene.cpp -o vk_scene.exe \
    -I <vulkan-headers>/include -luser32 -lgdi32
g++ -std=c++20 -O2 src/vk_map.cpp -o vk_map.exe \
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
- BRF 网格正面在 XZ 平面；独立渲染坐标映射 `x'=x, y'=z, z'=-y`。场景道具还会按
  `S·M·S` 约定套用 SCO 摆放矩阵（先 y/z 互换、乘矩阵、再互换回来），高度保持正向。
- SCO：魔数 i32 `-717`，version 4；物件 pos 磁盘序为 (x, z, y)。
- SCO terrain 层 index `0..10` 对应 `Data/ground_specs.txt` 行序（gray_stone…path），
  `-7793` 是高程浮点，`-12565` 是顶点色。terrain cell 边长 2m，网格原点 (0,0)。
- `ground_specs.txt` 第 3 列是 BRF **材质名**，需经材质表的 diffuse 字段映射到 dds。

格式参考致谢：OpenBRF（GPLv2，仅作格式事实来源）、Swyter/mab-tools 010 模板、
cmpxchg8b 论坛文档、cuellius/ScoTools。本仓库解析器为独立实现。

## 路线图

- [x] v0.1 静态渲染：BRF/SCO/DDS 解析 + Vulkan 上屏 + 地形分层贴图 + 方向光
- [x] 天空盒 / 雾 / 远裁剪（skyboxes.brf 穹顶 + 线性距离雾 + zf=800）
- [x] 角色与相机操控（原版 `man_body` 模型 + `skel_human` 骨骼蒙皮 + 站/走/跑三档动画，CPU 蒙皮）
- [x] v0.2-1 真墙体碰撞：原生 `bo_*` 碰撞体（球/胶囊/manifold 逐面），玩家自由行走，垂直重叠过滤
- [x] v0.2-2 地形真混合：单地面网格三贴图加权混合 + leveling 顶点色
- [x] v0.3 战役大地图：`map.txt` 渲染（按地形码贴图）+ 点击寻路（BFS）
- [x] v0.4 地图↔场景闭环：`map_icons.txt`/`parties.txt` 定居点图标，点击进入对应 `.sco` 场景，按 `M` 回地图
- [x] v0.5 地图队伍系统：玩家队伍实时行军、镜头跟随、游戏时间/暂停 HUD、AI 野队巡逻、遭遇触发、队伍/定居点最小屏占比图标
- [x] v0.6-1 战斗场景渲染：玩家 + 敌方 troops 进入模组战斗场景（`vk_scene --battle`），地图遭遇后自动进入
- [x] v0.6-2 基础战斗机制：左键攻击（扇区判定）、敌方逼近反击、血量、倒地、胜负回大地图并结算
- [x] v0.7-1 兵种装备渲染：游戏自带蒙皮护甲网格 + 刚体武器网格挂手骨，第三人称相机在角色背后
- [x] v0.7-2 攻击动画：游戏自带 `attacks_single_righttoleft` 挥砍 + 0.18 秒姿态混合；攻击距离与冷却读模块自身 item 数据（`weapon_length`、`speed_rating`）
- [x] v0.7-3 按兵种衣甲：各野队穿自己兵种的护甲（网格从模块资源解析渲染）
- [x] v0.7-4 判定数值读模块数据：攻击伤害与血量来自模块自身数据，替换占位值
- [x] v0.7-5 雾修复：push constant 增加逐物件 model 矩阵，角色按世界坐标雾距着色（角色不再糊成雾色）
- [x] v0.7-6 按兵种武器 + 四向攻击：各野队拿自己的近战武器；伤害按武器数据；攻击方向（右挥/左挥/上劈/刺）跟随最后移动方向；武器握持修正（斧柄横贯掌心、刃朝前）；敌方动画与玩家解耦
- [ ] 下一步：场景交互（入口/门），然后玩法逻辑重写

## 许可证

[MIT](LICENSE)
