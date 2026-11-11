# Warband-Native-Rebuild

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

窗口 ESC 退出。`vk_mesh.exe` 的用法见 `vk_mesh.cpp` 头部注释。

## 格式破译要点（自研经验，欢迎对照公开资料复核）

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
- [x] 角色与相机操控（WASD+QE 飞行/走地，方向键转头，地形高度钳制，不穿地）
- [ ] 碰撞与寻路（SCO ai_mesh）
- [ ] 完整原版场景体验

## 许可证

[MIT](LICENSE)
