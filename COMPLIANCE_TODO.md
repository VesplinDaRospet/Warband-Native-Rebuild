# 合规整改操作清单（Warband-Native-Rebuild）

> 目标：明确 MIT 只覆盖原创代码；不随仓库/发布包分发任何官方游戏文件；中性化“逆向”表述；隔离第三方许可证风险。
> 说明：以下为工程合规整理，不构成法律意见。

---

## 一、今天必做（低成本、高收益）

### 1. README 顶部加“非官方声明”

- [x] 在 README 最顶部加：

```markdown
> **非官方粉丝项目**：本项目与 TaleWorlds Entertainment 无关联，未获其背书或授权。
> 本项目不提供《骑马与砍杀：战团》的任何官方游戏文件、资产、数据或可执行文件。
> 运行本项目需用户自行拥有正版游戏。
```

验收：README 首屏可见。 ✅（已做，2026-11-12；英文版声明同时加在英文区顶部）

### 2. README 写清 MIT 覆盖范围

- [x] 在 README 的 Legal / License 段落加入：

```markdown
## License scope

本仓库的 MIT 许可证仅适用于由本项目作者原创的源代码。

MIT 许可证不适用于：
- 《骑马与砍杀：战团》及其任何官方代码、资产、数据、文本、音频、字体、商标；
- 任何第三方依赖或环境依赖（其许可证由各自项目决定）；
- 用户本地拥有的官方游戏文件。

本仓库不分发任何官方游戏文件。示例路径仅作说明，运行需用户自行提供合法拥有的游戏环境。
```

验收：LICENSE 文件保持 MIT；README 明确“仅原创代码”。 ✅

### 3. 加 EULA 责任声明（中性表述）

- [x] 在 README 加入：

```markdown
## EULA notice

This project is an independent interoperability effort. It does not distribute any official game files.
Users are solely responsible for ensuring their use complies with the Mount & Blade: Warband EULA
and all applicable laws. The author claims no endorsement by or affiliation with TaleWorlds Entertainment.
```

验收：不承认“反向工程”，只声明独立互操作与用户责任。 ✅

### 4. 中性化“Reverse-engineered”措辞

- [x] 全文搜索并替换：

| 原表述 | 建议替换 |
|---|---|
| Reverse-engineered format notes | Independent format interoperability notes |
| 格式破译要点 | 独立格式互操作性笔记 |
| reverse engineering | interoperability research / independent implementation |
| 逆向 | 独立实现 / 互操作分析 |

命令示例：

```sh
grep -RniE "reverse[- ]?engineer|逆向|破译" . --exclude-dir=.git
```

验收：README、docs、注释中不再主动使用“逆向工程/破译”作为项目自我描述。 ✅（README 两处已替换；仓库其余文件无命中。本清单第 4 节对照表中的旧词仅作整改记录，非项目自我描述。）

### 5. 检查 git 历史是否提交过官方文件

- [x] 检查敏感路径和扩展名：

```sh
git log --all --name-only --pretty=format: \
| sort -u \
| grep -Ei '(^|/)(Modules/Native|SceneObj|Resource|Textures|Sounds|Music|Data|Languages)(/|$)|\.(dll|exe|brf|sco|dds|ogg|wav|ttf|bik)$'
```

- [x] 检查大文件：

```sh
git rev-list --objects --all \
| git cat-file --batch-check='%(objecttype) %(objectname) %(objectsize) %(rest)' \
| awk '$1=="blob" && $3>5*1024*1024 {print $3, $4}'
```

处理原则：

- 如果从未提交：只补 .gitignore。
- 如果历史提交过：先备份仓库，再评估用 git filter-repo 清理历史；不要只 git rm 当前文件。
- 不确定时先不要跑改写历史的命令。

.gitignore 建议加入：

```gitignore
# 官方游戏文件与本地环境
Modules/
SceneObj/
Resource/
Textures/
Sounds/
Music/
Data/
Languages/
*.dll
*.exe
*.brf
*.sco
*.dds
*.ogg
*.wav
*.ttf
*.bik
```

验收：git log 敏感路径检查无输出，或已记录清理计划。 ✅（2026-11-12 跑过：敏感路径 0 命中、>5MB blob 0 个；.gitignore 已加入。）

### 6. 确认没有复制 GPL 项目代码

- [x] 检查是否 vendored 第三方代码：

```sh
find . -maxdepth 3 \( -iname "LICENSE*" -o -iname "COPYING*" -o -iname "NOTICE*" \)
```

- [x] 确认没有一行代码实质性取自 OpenBRF 或其他 GPL 项目。
- [ ] 如有第三方库，新增 THIRD_PARTY_NOTICES.md 保留其许可证。

验收：MIT 代码与 GPL 代码无混用；致谢不等于代码复制。 ✅（仓库内仅根 LICENSE；OpenBRF 等仅作格式事实来源，解析器为独立实现；tools/shader-tool 只有 npm 元数据，未 vendored 第三方代码，故暂无需 THIRD_PARTY_NOTICES.md。）

### 7. 检查发布包 / 安装脚本

- [x] Release 附件、Docker 镜像、安装脚本中不包含官方 DLL/EXE/BRF/SCO/DDS/音频/字体/文本数据。
- [x] 脚本可以从官方链接下载到用户本地，但不得缓存进仓库或发布包。
- [x] README 示例路径旁补一句：“示例路径仅作说明，不附带官方文件。”

验收：Release 页面和脚本无官方文件打包。 ✅（仓库无 Release 附件、无 Docker/安装脚本；README 运行段已补说明。）

## 二、本周建议做（不急但值得）

### 8. 加 Issue / PR 模板约束

- [x] `.github/ISSUE_TEMPLATE/config.yml` 或模板中写明：
  - 不接受提交官方资产、反编译代码、破解内容；
  - 不接受请求打包官方游戏文件。

✅（已做：`config.yml` 关闭空白 issue + `report.md` issue 模板 + `PULL_REQUEST_TEMPLATE.md`，三条规则原文写入，违规直接关闭。）

### 9. 项目名 / 图标去官方混淆

- [x] README 首屏有“非官方”声明。
- [x] 图标、Logo 不使用 TaleWorlds 官方商标或易混淆素材。（仓库无任何 Logo/图标文件。）
- [x] 项目描述避免“官方”“正统重构”等措辞。（README 仅用“官方游戏文件/官方资产”作否定性边界声明，无自称官方。）

### 10. 加 NOTICE 文件

- [x] 新增 NOTICE：

```text
Warband-Native-Rebuild
Copyright (c) 2026 VesplinDaRospet

This repository's MIT license applies only to original source code authored by the project author.
It does not apply to Mount & Blade: Warband, its official assets, data, trademarks, or any third-party dependencies.
No official game files are distributed by this repository.
```

验收：NOTICE 与 README 口径一致。 ✅

## 三、现在不需要做

- [ ] 不需要现在找律师。
- [ ] 不需要现在主动联系 TaleWorlds。
- [ ] 不需要立刻把 MIT 换成 GPLv3。
- [ ] 不需要立刻把核心层拆成闭源二进制。

原因：项目当前影响力低，先补齐边界声明和资产隔离即可；等触发条件出现再升级。

## 四、触发升级条件

出现以下任一情况，把“本周建议”升级为优先事项，并考虑 WSE2 式隔离或 GPLv3 评估：

- [ ] star / fork 快速增长，或被社区大 V 转发；
- [ ] 出现闭源商业下游使用你的代码；
- [ ] 有人要求打包官方资产，或 Release 混入官方文件；
- [ ] 收到 TaleWorlds 或权利人邮件；
- [ ] 你开始发布预编译完整包，而不只是源码。

## 五、最终验收清单

- [x] README 首屏有非官方声明。
- [x] README 明确 MIT 仅覆盖原创代码。
- [x] README 有 EULA notice 和用户责任声明。
- [x] 全文无“reverse-engineered / 逆向 / 破译”自我描述。
- [x] git 历史敏感文件检查通过，或已制定清理计划。
- [x] .gitignore 覆盖官方文件类型和目录。
- [x] 无 GPL 代码混入 MIT 代码。
- [x] Release / 脚本不打包官方文件。
- [x] Issue / PR 模板拒绝官方资产和反编译代码。
- [x] NOTICE 与 README 口径一致。

---

优先级：今天已完成 1–10 全部；仅第 6 项的 THIRD_PARTY_NOTICES.md 保持待命（当前无 vendored 第三方代码，暂不需要）。
