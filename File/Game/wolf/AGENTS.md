# 狼人杀 (Werewolf)

联网控制台版狼人杀 C++ 项目。源码：`Start.cpp`（房间管理器）/ `Server.cpp`（单局服务器）/ `Client.cpp`（客户端）/ `common.h`（共享协议与职业表），测试脚本在 `tests/`。狼人杀复用了 `reference/demon/`（恶魔轮盘，已完成的参考项目）已验证的架构、协议思路和所有已修复的坑。

> 当前状态：**第十一、十二轮验收完成**。第十一轮（round11_test.ps1 37 项全 PASS）：在线 NPC 游戏内决策走 HTTP（WOLF_NPC_API_URL/KEY/TIMEOUT 环境变量、retries=1、key 落盘 npc_key.bin、超时回退离线）+ 房内 @/聊天；第十二轮（round12_test.ps1 17 项全 PASS）：离线 NPC 房内聊天（@ 必答恰一条、相关性接话 名字 85%/话题词 30%/纯闲聊 6%、2s 限频、多话题词嵌入、极端输入不崩）+ 在线 NPC 房内对话（fake_chat HTTP 响应「AI房内回话」广播、无 key 回退离线模板、超时快速兜底）。**本轮修复**：NpcExtractText 对 8 字符 `"reply"` 键的 k+9 偏移错位导致 AI 文本解析成整段兜底（改 `find(':', k)`）；NpcOnlineRoomChat 重试默认 0→1（本机 WinHTTP↔PowerShell TcpListener 环回偶发 EOF/12030，重试兜底）；fake server/chat 本机冷启动 ~15s 才就绪（脚本加 25s 就绪探针）；stdout 重定向块缓冲致 REQ 不落盘（改用 AppendAllText 日志 fake_server_log.txt/fake_chat_log.txt）；Get-NetTCPConnection 对 Loopback 监听不可靠（按 PID 文件 fake_chat.pid/npc_fake_server.pid 清理残留）；round11 S3 段女巫毒杀+狼杀首夜屠神导致无白天（关毒杀保证白天 1 必到）；R2-2 概率断言 5 试→10 试（0.7^5≈16.8% 偶发假 FAIL）。Start.cpp 探针日志（NPC-ONLINE-REQ/RES）与 NpcRoomMaybeChat 的 @ 必答不再重复触发，保留。第十轮及其之前均已实现并通过 tests/ 脚本。状态更新与代码修复完成后，记得 git add + git commit 提交（AGENTS.md 更新也要一起提交）。

## 工作目录铁律（最重要）

- 所有工作文件**只允许放在项目根目录（`AGENTS.md` 所在目录，即 D:\wolf）下**（包括探测、临时、测试文件）。禁止写 C 盘（如 `%TEMP%`、用户目录）。
- 注意：**不同电脑工作目录可能不同**（如曾为 `D:\Demon\wolf`），禁止在脚本/文档中写死绝对路径，一律用相对路径（`%~dp0` 等）或自动探测。
- 确定 `cl` 可用后先删除探测用临文件，不把临时文件遗留在仓库。
- 编译验证一律在项目根目录下进行，产物（.exe/.obj）也放这里；不再需要即删除。

## 构建环境（已验证 2026-08-03；路径通用化 2026-08-05）

- `cl.exe` 不在 PATH。必须先初始化 MSVC 开发环境：
  `cmd /c "call \"<vcvars64.bat 绝对路径>\" >nul 2>&1 && cl ..."`
- vcvars64.bat 路径**按机器自动探测**（`build.bat` 已内置候选列表，依次尝试）：
  - `D:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat`
  - `D:\Soft\Visual Studio\VC\Auxiliary\Build\vcvars64.bat`
  - `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat`
  - `C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat`
- 已实测：MSVC 19.51（工具集 14.51），SDK 10.0，可用 vcvars64.bat 正常编译。
- 参考构建命令格式（每文件分别编译，参考 build.bat）：
  `cl /nologo /utf-8 /MT /EHsc /D "_CRT_SECURE_NO_WARNINGS" <File>.cpp /link ws2_32.lib [user32.lib|shell32.lib] /SUBSYSTEM:CONSOLE /OUT:<File>.exe`
- 必须 `/utf-8`：源码 UTF-8 无 BOM，缺了 cl 会按 GBK 误读，C4819/C2001 连环报错。
- 无 CMake/Makefile。运行时序：Start.exe（房间管理器）先启动，再连客户端。
- 产物：`Start.exe`、`Server.exe`、`Client.exe`（中文版）、`Client_en.exe`（英文版，`-D WOLF_EN`）。

## 参考实现（必须先读）

- `reference/demon/` — 恶魔轮盘。架构：**Start.exe（房间管理器）/ Server.exe（单局服务器）/ Client.exe（客户端）三进程 + 共享 common.h**。其 `AGENTS.md` 包含大量已经踩过的坑（输入门、暂停、断线重连、CJK 名、抓安全细节），是狼人杀房间/联机底座的教科书。**动手前先读 `reference/demon/Start.cpp`、`Server.cpp`、`Client.cpp` 三者文件头的设计说明。**

## 已实现命令（全中英双语：英文命令、英文短别名与中文别名等效）

通用：
- `HELP [ALL|职业名]`：命令帮助（含短别名列）；HELP ALL 输出全部职业列表；带职业名输出职业详细介绍（中/英职业名均可）。
- `NAME <新名>`：改名。全服唯一，重名拒绝；限 10 码点；**净化后码点数 < 2 拒绝**（单字符/单数字/单汉字，提示「名字至少需要 2 个字符」；空名回退 Player）；**白名单字符集：仅中英文/数字/下划线**（含空格、连字符、点号、全角、emoji 一律拒绝）；禁止 IP 格式（含前导零/点分数字形似）；客户端与服务端双重校验；BAN 参数不受长度限制。

大厅：
- `LIST`：房间列表（含人数、游戏中标记；**房间内也可用**）；`CREATE <端口>`（短别名 `CR`）：建房；`JOIN <端口>`：按端口入房。

房间（非房主命令全员可用）：
- `READY`：准备/取消准备；`STATUS`（短别名 `ST`）：查看本房成员与准备状态（**竖排等宽表**：ID 右对齐、NAME 左对齐全角 2 宽、ST 右对齐，多行经单条 `ROOM_STATUS|` 下发）；房间内普通打字=聊天广播。

房间（【房主】专属）：
- `TRANSFER <槽号或名字>`（短别名 `TF`）：转移房主（目标收 ADMIN 提示，原房主失去配置权限）。
- `PICK <槽号或名字>`：点名踢出（禁入 10 秒）。
- `BAN <名字/IP 或 .ban 文件>`（短别名同前，空格分隔批量）：拉黑玩家或 IP。名字按 NAME 同规则规范化入库、大小写不敏感；IP 拉黑立即踢出房内同 IP（房主除外）；游戏中不能拉黑、不能拉黑自己；参数以 `.ban` 结尾视为黑名单文件导入（不存在/路径穿越 clean 报错不崩溃）；批量完成输出汇总（成功 N 项（名字 X、IP Y），拒绝 M 项）；黑名单随房间销毁一并清除。
- `UNBAN <名字/IP 或 .ban 文件>`：取消拉黑（名字比对大小写不敏感，批量/文件同 BAN）。
- `IP <名字>`：查询指定玩家当前连接 IP（未知玩家 clean 提示）；`LG`：房内成员进出日志（[in]/[out] 与 IP，最多 3 条/人，房间销毁即清）。二者均【房主】专属。
- `LEVEL <0|1|2>`：职业档位（0 基础 / 1 经典 / 2 豪华）；`VILLAGER <0|1>`（短别名 `VG`）：村民开关。
- `RATIO <狼> <中立> <神>`：三阵营人数（真实人数；村民关时三方之和须等于当前人数，非法直接失败不设置）。
- `CONFIRM <1|0>`（短别名 `CF`）：对全员准备时比例不符触发的自动配置结果确认（1=同意并按建议开局 / 0=拒绝、全员取消准备）。
- `START`：全员准备后手动开局；`AUTO`：切换「全员准备自动开局」开关。

游戏：`VOTE <编号>`（短别名 `V`）：白天投票放逐（0=弃权）；`BOMB <编号>`（短别名 `B`）：白狼王白天自爆。

**Start.exe 启动**：`Start.exe [端口]`；有参数用参数（非法直接中文报错退出）；**无参数时交互输入监听端口**（提示「请输入监听端口（1024-65535）：」，非法输出具体原因后重新输入，EOF 退出）；传给 Server.exe 的 startIp/startPort 用 Start 实际监听端口（非硬编码 8888，否则 Server 的 GAME_ENDED/RELEASE 回连丢失）。

**游戏链路（真实协议）**：Start 开局时给每个已连接槽位分配 **gamePid=压缩名单序号 1..N**（槽位升序跳过未连接槽，Slot.gamePid 记录）并存入槽位；命令行按同序传 Server.exe，`GAME_PREPARE|port|roomId|ip|gamePid` 下发；客户端**直连游戏端口**并发 `PLAYER_ID|<gamePid>` 认领槽位（Server 的 playerId=名单位置 1..N，与 gamePid 一致——**槽位空洞时 GAME_PREPARE pid 必须用 gamePid 而非槽号+1**，否则张冠李戴）；开局广播 `PLAYER_LIST|<总数>|<名1>|...|<名N>`（名字=槽位序、含玩家总数头字段，Client 解析必须跳过头字段）。游戏内其他协议：`PING`（心跳保活行，Server 收到只刷新 lastSeen 并回一行 PING）、`GAME_ENDED|<roomId>`（Server 通知本局结束）、`RELEASE|<roomId>`（全部玩家失联时 Server 通知销毁房间、清空黑名单）、`__GAME_OVER__`（Server 收尾关连接前发的终态裸行，客户端据此判定「本局正常结束」直接回房、不进入重连流程）、`ROLE|<职业enName>`（身份私信）、`__DAY_OPEN__`/`__INPUT__`/`__CLS__`/`__PAUSE__` 等控制消息。`REJOIN|<roomId>|<playerId>` 的 playerId=开局分配的 gamePid：Start 按 `slots[i].gamePid == pid` 匹配回**原槽位**（含空洞局），匹配失败才回落空槽（不含槽 0）；房主保护 hostPid=1（槽 0 恒为名单首位）。兜底回滚（`WOLF_GAME_WAIT_SECONDS` 注入）只对「Server.exe 进程已死」生效——进程活着时房间保持 [游戏中]，善后交给 Server 自身 25s 开局超时 RELEASE。

## 测试（tests/ 十五套自动化脚本，全部 PASS）

- `tests/proto_test.ps1` — **60 项**协议级验收：房主流程 / 转移房主 / PICK+禁入 / 职业配置 / 比例 / START / AUTO / BAN-UNBAN / IP 黑名单 / 名字规则 / GAME_ENDED+REJOIN 回房等。
- `tests/server_test.ps1` — **5 项** 4 人直连局（1 狼 + 0 中立 + 2 神 + 村民开，完整昼夜循环走通）。
- `tests/server_test8.ps1` — **5 项** 8 人全流程直连局（2 狼 + 0 中立 + 2 神 + 村民开）。
- `tests/pen_test.ps1` — **71 项**渗透专项（注入清洗 / 重名 / 长相截断 / IP 名 / 空名回退 / 端口参数 / 非房主越权 / 黑名单绕过 / 职业配置非法值 / 比例非法 / 断线 / 大厅命令隔离）。
- `tests/speech_test.ps1` — **20 项**白天交互专项（全角冒号聊天 / 注入不触发指令 / 超长聊天 / VOTE 非法值 / 白天断线重连 / 平票 / 弃权 / 遗言广播 / 遗言超时续局 / 女巫救人 / 进程不崩）。
- `tests/round3_test.ps1` — **37 项**第三轮验收（§12.8：房间内 LIST 可用+房内 CREATE/JOIN 拒绝 / CR·VG·ST·TF·CF·V·B 短别名 / 心跳失联判定+发 PING 不误判 / 投票超时自动弃权（环境变量 WOLF_VOTE_TIMEOUT_SECONDS 注入缩短）/ 白天断线重连后可续投 / LANG|en 服务器英文输出）。
- `tests/round4_test.ps1` — **52 项**第四轮验收（§13.4：进游戏后房间保留且 LIST 人数定格含 [游戏中] / 开局失败路径回滚可重开（WOLF_GAME_WAIT_SECONDS 注入）/ 游戏进行中 REJOIN 拒绝+客户端自动重试 / 游戏期外人 JOIN 拒绝 / 全员回房前 JOIN 拒绝、回房后允许 / 房主保护（原房主最后回房仍为房主）/ 重复 REJOIN 不超员 / 夜晚四阶段「守卫/狼人/预言家/女巫请睁眼」广播全员收到+专属提示只发对应职业 / 攻击用例（垃圾字节、PING-only、伪造 pid、撞槽回落、拉黑回房拒绝、未知房间、格式错误））。
- `tests/round5_test.ps1` — **56 项**第五轮验收（§14.7：死亡玩家白天静默+死亡提示单播 / 夜晚只发存活玩家 / __GAME_OVER__ 终态行客户端直接回房不再重连 / 正常结束后回房一次成功 / IP 命令与未知玩家提示 / LG 进出日志与三人上限 / 批量 BAN 混自己名字拒绝 / 黑名单名字/IP JOIN 拒绝 / .ban 文件导入（相对/绝对/不存在/路径穿越/非 .ban 参数按名字）/ UNBAN 文件解除 / NAME 白名单（空格/连字符/点号/emoji/全角/IP 形似拒绝，合法名与截断成功））。
- `tests/round6_test.ps1` — **40 项**第六轮验收（§16：NAME 长度≥2 码点（单字符/单数字/单汉字拒、2 码点成、空名回退不回归、白名单先于长度）/ STATUS 竖排表（表头/行数/中文名对齐/ST 随 READY 变）/ IP·LG 服务端行为（大厅拒、房主查、非房主拒）/ 短别名与全称混用（ST·CR·TF·VG 全通）/ 端口释放三路径（E：兜底回滚杀孤儿 Server→REJOIN 重开同端口成功、F：Server 25s 超时 RELEASE→销毁→同端口重建、G：BAN 踢空回收→同端口重建）/ 攻击（伪造 RELEASE/GAME_ENDED 未知房间不崩、单字符名可 BAN、拉黑大小写变体 JOIN 拒、聊天注入 | 原样广播不入指令））。新增 E5：E3 兜底回滚后同端口重新开局成功（含新 Server 欢迎语断言）。
- `tests/round7_test.ps1` — **10 项**第七轮验收（§17 实际落地内容：A 段直连 4 人局全员收到 `PLAYER_LIST|4|...` 广播且行内容一致（顺序=Server 传参顺序、含总数头字段、无职业信息）、B 段 Server 对 PING 回 PING 应答（半开死连检测基础）、C 段兜底不误杀存活 Server（WOLF_GAME_WAIT_SECONDS=2 注入 → 全员断大厅不连游戏服 4s 后 Server.exe 仍在+房间仍 [游戏中] → 4 人连游戏服触发真开局，中文名全链路进 PLAYER_LIST+PING 应答））。
- `tests/round8_test.ps1` — **16 项**第八轮验收（§18：A 段 Start 无参数交互输入监听端口（stdin 喂 8890 生效；先喂非法 80 再喂 8891 的重输流程）与 Server 回连用实际端口；B 段紧凑 4 人局 PLIST 对齐（GAME_PREPARE pid=1,2,3,4 / 欢迎语槽位一致 / PLAYER_LIST 行 / 白天「玩家Alice 投票给了玩家Bob（槽2）」名字对齐 / VOTE·BOMB 非法目标原因+请重新输入）；C 段槽位空洞局（5 人房去 1 人）压缩名单序对齐（pid=1,2,3,4、PLAYER_LIST=AliceC|BobC|DaveC|EveC、DaveC=3 号位、白天 DaveC 投票广播）；D 段 PICK/TRANSFER 目标不存在「目标玩家不存在：<参数>（…），请重新输入」；E 段杀 Server 后兜底回滚、空洞局全员按 gamePid REJOIN 回原槽、STATUS 槽位对齐）。
- `tests/round9_test.ps1` — **47 项**第九轮验收（§19：A 段本地用户 JOIN 分支空指针修复（A11 根因：room->localUsers 解引用）、B 段命令封装与输出（SHOW/LOOK 族、NPC 列表 'NPCs'、ADD USER/NPC、BAN 模式、LOOK 非法用法）；C 段槽位空洞局（GAME_PREPARE 压缩 pid、PICK/TRANSFER 非法目标、NPC 局全流程：在线 NPC 发言广播+自动投票+完整昼夜走到 __GAME_OVER__）；D 段本地用户窗口（ADD USER 拉起窗口进程、-u 指定控制者、重名拒绝、窗口自动入房 STATUS、SHOW ADD、Player 3/4 自动连游戏端口、PLAYER_LIST 含 LuUser 名）；E 段断线判活（3s 静默清连接、PING 保活 6s 不断）；F 段在线 NPC 发言与 API 调用（4 人局白天到达、假 HTTP 服务器收到 REQ）——**47 项全 PASS**）。

- `tests/round10_test.ps1` — **23 项**第十轮验收（A 女巫救/毒流程与屠边、B 丘比特/盗贼情报保密、D 房内 MUTE/UNMUTE/SHOW/通配化简、E 游戏内禁言传递）。
- `tests/round11_test.ps1` — **37 项**第十一轮验收（S1 ADD/UNADD NPC 与重名/越权/批量、S2 6 人局 NPC 全流程昼夜、S3 API：在线 NPC 白天决策「AI 分析中」等待提示 + fake server 收到 REQ + key 落盘 npc_key.bin + 无 env key 从文件恢复 + 超时回退离线、S4 @NPC 房内对话在线路径）。round11 修复：S3 段女巫毒杀关停保证白天必到、fake server 25s 就绪探针、fake_server_log.txt AppendAllText 落盘 + npc_fake_server.pid 按 PID 清理、R2-2 话题断言 5→10 试。
- `tests/round12_test.ps1` — **17 项**第十二轮验收（R1 房内 @离线 NPC 必答：单次恰一条/多样性/词嵌入；R2 普通聊天相关性：名字 85% 6试/话题词 30% 10试/纯闲聊 6% 上限 5 次；R3 两人名接话；R4 极端输入：超长/纯标点/管道注入/@不存在/进程存活；R5 在线 NPC 房内 @：AI 文本广播 + fake_chat 收到 REQ（重试兜底）+ NpcExtractText 干净解析；R6 无 key 回退离线；R7 短超时快速兜底；R8 单次 @ 恰一条回复无重复发声）。

运行方式（串行执行，输出文件仅作参考，以脚本 **exit code** 为准）：
`powershell -NoProfile -ExecutionPolicy Bypass -File tests\xxx.ps1 *> tests\xxx_out.txt`

脚本自身约束：`.ps1` 必须 UTF-8 带 BOM 保存（汉字字符串不借 BOM 会在代码页下被误读拼接出错）；裸 socket 模拟客户端必须像真实 Client.exe 一样每 2-3 秒发 `PING` 保活（服务端 10 秒无字节判定失联，静默窗口会被误杀）。

## 踩坑记录（已排的坑，改哪里先对照这里）

1. **超长 IP 名截断后漏判 IP**：11 位 IP 名会被长度限截成 10 位的非 IP 串后不再命中 IP 判定 → IP 判定必须在截断前进行（`LooksLikeIpName` 对原始输入判）。
2. **前导零 IP 异写**（`199.09.1.1`）不是合法 IPv4，`IsIpAddress` 拦不下 → 用**'点分数字形似'**规则拦（2-4 段、每段 1-3 位、总长≤15 判形似）。
3. **黑名单绕过三变体**：大小写（Grace/grace）、尾随空格（NAME 解析裁剪）、超长截断（15 字符入单被截成 10）都可能绕过精确串匹配 → 黑名单入库必须与 NAME 同规则规范化（净化+截断）+ 比对大小写不敏感（`ContainsName`/`NameEquals`）。
4. **`$pid` 是 PowerShell 保留变量**：不能用作脚本参数名，否则脚本拿到的进程 ID 恒为当前 PowerShell 自带进程的 PID、拉入拉出全部错位。
5. **exe 被锁**：编译/链接前必须先杀掉残留的 Start.exe / Server.exe / Client.exe 进程，否则出现无法删除/链接失败的 `LNK1168` 类错误。
6. **并行 PowerShell 调用串扰**：同时跑多套脚本会 OOM / 控制台乱码（多套共享端口 8888 等资源）→ 构建与测试一律**串行执行**，并重定向日志 `*>` 单独文件再逐个判读。
7. **心跳（10s 无字节判失联）误杀静默裸 socket**：第三轮引入 PING 心跳后，测试脚本里一切长静默窗口（禁入 10s 等待、遗言超时、断线观察）都会触发失联判定被清连接 → 脚本必须给模拟客户端加后台 PING 保活（每 2-3 秒），长睡眠前后都要发。
8. **无限阻塞轮询让超时永不触发**：`PollAllForMessage` 阻塞等消息且 PING 不入队，投票超时/窗口重发在全体静默时永远不触发、白天挂死 → 有超时需求的轮询必须用带超时版本（`PollAllForMessageTimed`，100ms 轮询）。
9. **编译期常量不可测**：90 秒投票超时无法自动化 → 运行时读环境变量 `WOLF_VOTE_TIMEOUT_SECONDS` 覆盖（非法/缺省回退默认值），测试注入 6 秒验证。
10. **build.bat 用 GBK+CRLF 保存**：cmd 批处理按当前代码页预读缓冲，UTF-8 无 BOM + LF 会解析错乱；中文提示的 .bat 用 GBK+CRLF 才稳（与 .cpp/.ps1 的 UTF-8 约定互不冲突）。
11. **StreamWriter 并发写会抛异常**：PowerShell 后台 runspace 与主线程共用 StreamWriter 发 PING 需加锁或专用写通道。
12. **进游戏断开≠离开（第四轮）**：玩家收 GAME_PREPARE 后关闭大厅连接是预期行为 → `RemovePlayerFromRoom` 在 `gameStarted || gameEnded` 期间只清 sock、保留槽位（name/ip）、不减 playerCount、不顶替房主、不销毁房间；LIST 也要跳过 `playerCount==0` 的房间（游戏中人数定格显示）。房主保护靠 `hostPid`（开局时槽 0 玩家 pid）：非房主 REJOIN 不能占槽 0。全员回房（占用数==gamePlayerCount）才清 gameEnded（外人才能 JOIN）。
13. **手动直连 Server.exe 必须带全参数**：契约是 `port 名字... startIp startPort roomId W N G level villager 语言码...`（总数 9+2N，尾部 N 个语言码）；漏语言码会让 Server 按 `(argc-10)/2` 算错人数（6 人局传 15 参数被当成 3 人局，bot 4-6 全被拒）→ 测试脚本直连局必须按 Start 组装参数的同一格式传参。
14. **单元素 Hashtable 的 `.Count` 是键数**：PowerShell 中 `($arr | Where-Object {...}).Count` 当结果为单个 Hashtable 对象时返回**键数**（如 12）而非 1 → 断言元素个数必须用 `@(...)` 强制数组：`@($arr | Where-Object {...}).Count -eq 1`。
15. **`HasExited` 在杀进程后恒 True**：`Stop-Process -Force` 之后再读 `$proc.HasExited` 无法判定"测试期间是否自己崩过" → 必须在杀进程前记录（round4 的 crashed 判定）。
16. **TcpListener(Loopback) 探测不到 0.0.0.0 监听（第五轮 H7 三连败的根因）**：测试脚本 Get-FreePort 用 `TcpListener([IPAddress]::Loopback, $p).Start()` 探测空闲端口，而 Server.exe 绑定 `INADDR_ANY`（0.0.0.0）——Windows 上 127.0.0.1 的 bind 能与 0.0.0.0 的监听**共存**（.NET TcpListener 默认 `ExclusiveAddressUse=false` 会开 SO_REUSEADDR），探测误报「空闲」→ 两个测试段拿到同一端口 → 后一局 Server bind 10048 失败、玩家 CREATE 被 Start 按「端口已被使用」拒绝（症状：建房收不到 CREATED、BAN 回「只有房主可以执行该操作」）。**修复：Get-FreePort 必须用 `TcpListener([Net.IPAddress]::Any, $p)` 探测**（实测 Any 与 0.0.0.0 监听冲突检测正确），或改用 `TcpClient.Connect` 主动连接探测（连接成功=被占）。第 5 轮 round5_test.ps1 三连败（H7-H9）即此坑。
17. **PowerShell 5.1 的 `IO.StreamWriter($s)` 无编码参数按 GBK 写**：给模拟客户端发中文（如 NAME|中文）会被写坏导致服务端拒绝/静默断连；必须用 `New-Object IO.StreamWriter($s, [Text.Encoding]::UTF8)` 或裸字节写（`[Text.Encoding]::UTF8.GetBytes` + `NetworkStream.Write`）。
18. **write 工具生成的 .ps1 无 BOM**：编辑器工具写出的 UTF-8 无 BOM 文件被 PS 5.1 按 GBK 预读，中文注释/字符串乱码会破坏引号与管道语法（ParserError: ExpressionsMustBeFirstInPipeline）→ 新建含中文的 .ps1 后先补 `EF BB BF` 再运行。
19. **双重 BOM 会破坏脚本首行**：`[IO.File]::WriteAllLines(UTF8)` 会自带 BOM，若再手动拼 `EF BB BF` 就变成双 BOM，PS 把第二个 BOM 当字符拼在 `#` 前、首行注释变成命令报 `?# 无法识别`——补 BOM 前先检查文件头三字节。
20. **`*> 重定向` 产出 UTF-16LE（头 FF FE）**：读回测试输出必须 `[Text.Encoding]::Unicode`；GBK 控制台读 UTF-8 文件须显式 `[Text.Encoding]::UTF8`。Start/Server 的 Log 是 cout 缓冲，被 `Stop-Process -Force` 强杀时**缓冲丢失**，start.log 可能缺最后若干行，调试对照时不能据此否定服务端行为（要配合探针逐行收包）。
21. **`IO.StreamWriter($s, [Text.Encoding]::UTF8)` 首次写入会输出 UTF-8 BOM（第六轮 A 段 8 连败的根因）**：.NET Framework 的 `Encoding.UTF8` 属性 `GetPreamble()` 返回 `EF BB BF`，StreamWriter 构造时带上编码 preamble、首写时输出 → Start 收到的握手行变成 `EF BB BF HELLO|3` 不匹配直接 `closesocket`（症状：HELLO 后收不到 WELCOME、连接立即被关、A1-A8 全 FAIL）。round5 用无编码参数的 StreamWriter（踩坑 17 按 GBK 写）只测 ASCII 名所以没暴露。**修复：必须 `New-Object IO.StreamWriter($s, [System.Text.UTF8Encoding]::new($false))`（显式无 BOM UTF-8）**——这也是测中文名的唯一正确姿势（踩坑 17 的无 BOM 替代方案）。
22. **STATUS 改竖排后 RecvUntilStream 只读到头行**：`ROOM_STATUS|ID | NAME | ST\n1 | ...` 多行下发，`RecvUntilStream` 消费了头行（含换行），后续数据行还在流里 → 必须像 round5 Recv-LG 一样补 `ReadChunk` 读剩余并拼回（`Recv-Status`）。round3 的旧 `Recv-Status` 是 `|` 单行解析，round6 B 段 4 行断言失败即此因（源码 6 处 ROOM_STATUS 消费点已统一改）。
23. **Get-FreePort 不感知房间占用（第六轮 C 段 6 连败的根因）**：TcpListener(Any) 探测只能发现**正在监听**的端口；Start 房间只记录端口（不监听），B 段房间占 8420 后 C 段 Get-FreePort 又返回 8420 → `CR 8420` 被 Start 按「端口已被使用」拒绝。**修复：脚本级 `usedPorts` 登记已分配端口**（round6 C3-C8 全 FAIL 即此坑）。注意：F/G 段「同端口重建」是**刻意复用**房间销毁后的同一端口，不走 Get-FreePort。
24. **回滚后同名 JOIN 被「名字已被占用」拦截（round6 E3/E4 失败的根因）**：开局兜底回滚只清 ready、不清槽位（name 保留给原玩家），原玩家重连大厅后发 JOIN 会撞名字占用检查被拒 → **重连必须走 REJOIN|<roomId>|<playerId>**（与真实客户端一致；GAME_PREPARE 行格式 `GAME_PREPARE|<port>|<roomId>|<ip>|<playerId>`，`-split '\|'` 取 [2]/[4]）。房主（pid=1=hostPid）REJOIN 回槽 0 重得 isAdmin。
25. **Server 收到 PLAYER_ID|k 不回显编号**：分配成功回复的是按玩家语言的欢迎语（中文「你被分配到 k 号位。」/英文 "You are assigned to slot k."），断言必须匹配欢迎语而非 `PLAYER_ID|k`（round6 E4 假失败）。round5 的 bot 连上后不等回复，故未暴露。
26. **房间聊天广播发送者不回显**：Start 的聊天广播（`ROOM_MSG|名字：内容`，全角冒号 §10.1）只发给房内**其他人**（pen_test 11b 断言用"他人收到"，round6 H4 复踩）——单人间测聊天注入必然超时假失败，接收断言必须用房间内另一名玩家。
27. **兜底回滚只对「Server.exe 进程已死」生效（round7 修复，round8 E 段复踩）**：`CheckGameWaitTimeouts` 检查到「全员进游戏且超时未收到通知」时，若 `GameServerProcessAlive(r)` 为真（进程活着在等玩家/对局中）就重置计时继续观察，**不回滚**——测「回滚后 REJOIN」的脚本必须先 `Stop-Process Server` 模拟启动即死，再等注入窗口；否则房间一直 [游戏中]，REJOIN 全被拒「游戏仍在进行中」。
28. **PowerShell 变量名大小写不敏感**：`$R` 与 `$r` 是**同一变量**（round6 E3 改造新增 `$r = RecvUntil ...` 后，E 段数组 `$R` 被覆盖成 String → foreach 遍历出 Char 元素 → `$cl.wlock` 为 null、Monitor.Enter 抛异常、E 段全毁）——脚本内避免大小写仅不同的变量名；数组收集一律用 `[System.Collections.ArrayList]` + `.Add()`（`@() + $hash` 会塌缩成单元素且 hashtable+hashtable 是**合并**不是追加）。
29. **断言用 `-match` 收集行时正则别写太宽（round8 B 段假 PASS 前身）**：`$hits += $line` 前用 `-match '投票目标不合法|自爆目标不合法|投票给了玩家|弃权'` 收集白天证据，结果「白天发言阶段…0 弃权」提示行匹配「弃权」→ `Count -ge 3` 提前 break、VOTE/BOMB 响应还没收到就退出（同一晚跑 PASS 另一晚 FAIL 的不稳定源）→ 收集正则必须精确到目标行本身，退出条件用 `$hits -match '目标串'` 显式判断而非计数。
30. **AskChoice 只问一次不重问（round9 C 段 120s 卡死根因之一）**：夜晚 `AskChoice/AskWolfTarget` 发提示+`__INPUT__` 后**阻塞等待**，不循环重发（循环重问只在收到非法输入后发生）——狼 bot 目标「未定时不发」会永久等死。**修复：狼 bot 收到 `__INPUT__` 必须立即应答任意合法目标**（不能自刀）；目标池只取 NPC 槽位（4 人局 NPC 恒占 3/4），真人 bot 互轮换会在目标死后恒指向死人被拒、无限重问。
31. **白天投票必须按「白天发言阶段」重置（round9 C11 白天 2 卡死根因）**：投票标志白天 1 投过就永久 true，白天 2 起真人 bot 不再投票 → `GatherDayVotes` 等剩余票直到超时 → 无 `__GAME_OVER__`。**修复：每次收到「白天发言阶段」广播把已投标志置 false**（该广播每白天恰一次，是天然阶段同步点）；白天投票不经 `__INPUT__`（白天只开 `__DAY_OPEN__`），按横幅触发集中发 `PLAYER_k|VOTE|0`。
32. **Start-Process 子进程输出是 UTF-8 无 BOM，不是主脚本 `*> 重定向` 的 UTF-16LE（round9 F3 FAIL 根因）**：npc_fake_server.ps1 由 Start-Process -RedirectStandardOutput 拉起，stdout 走子进程管道编码（UTF-8 无 BOM），主脚本按 `[Text.Encoding]::Unicode` 读全乱码 → `Contains('REQ:')` 恒假 → **读子进程输出必须先看字节头：FF FE → Unicode，否则按 UTF-8**（与踩坑 20 主进程重定向场景相反）。
33. **Server 必须全槽位连上才开局（round9 D8 FAIL 根因）**：`WaitForGameStart` 等**所有**压缩名单槽位连好才广播 `PLAYER_LIST`。D 段真人 socket 收 GAME_PREPARE 即关（不连游戏端口），本地用户窗口自动连 3/4 位，1/2 空槽无人 → 永远等 → 25s RELEASE。**验证「本地用户自动连游戏端口」时，脚本必须自己补连空槽（`PLAYER_ID|k` 填上 1/2）+ 每 1s PING 保活**，否则 PLAYER_LIST 永不广播。

## 需求规格（狼人杀版，作为验收依据）

> 现状：第一、二轮需求已实现并以 `REQUIREMENTS.md` 为唯一依据（其 §2.4/§2.5 与 §8 验收已全部落地，见上文「测试」小节）。第三轮（§11：双语言/心跳/投票超时/命令简化/提示精简）、第四轮（§13：夜晚阶段广播/房间生命周期/回房可靠性）、第五轮（§14：白天自由发言修复/游戏结束回房可靠性/IP·LG/BAN·UNBAN 批量与文件/NAME 白名单）与第六轮（§16：NAME 长度/STATUS 竖排/IP·LG 行为/短别名混用/端口释放三路径/攻击）均已实现并通过 tests/ 脚本，见 `REQUIREMENTS.md` §12/§13/§14/§16 实现契约。职业管理采用「档位 LEVEL + 村民开关 VILLAGER」方案（需求条款允许更好的做法，且已人工可验证），未采用下方"逐职业 0/1"的默认做法。

- 控制台程序，Windows、MSVC C++，支持联机（参考恶魔轮盘的房间模式）。
- 房间管理系统要**比恶魔轮盘更完善**：
  - 支持**转移房主**（transfer host）。
  - 支持房主 **PICK 指定对象**（点名指定对象，显然要有指定的人）。
  - **用户名不允许重复**（客户端与服务端都要校验，重名直接拒绝/改名）。
  - 房间内更完善详见需求。
- **职业管理**：房主可配置职业启用。默认做法要求：依次输出每个职业名 + `:`，房主输入 `0`（禁用）/`1`（启用）；输入不合法就重新输出该职业那一行继续设置；输入 `EXIT` 取消本次设置；允许有更好的做法，但必须人工可验证。
- **狼:中立:神 比例配置**：输入修改指令后直接让用户输入比例数字（**这里的比例是真实人数，不是传统意义上「若干狼 … 分配」**）。数字不合法则直接失败、不设置。
  - 全部玩家准备完毕时，若实际人数与比例不对应，**自动设置**成符合上限、甚至合理的组合（需考虑狼人杀人数/职业数量），同时**考虑合法性：狼个数等三方合计应等于实际人数**。
  - 自动设置后必须输出结果并询问房主是否同意（`1`同意 /`0`不同意；不同意则保持准备前的分配或回到配置流程）。
- 全程中文提示；所有端 UTF-8 编码（调用 `SetConsoleUtf8()` 设置 CP_UTF8，同时关闭快速编辑/文本选择 `DisableConsoleQuickEdit()`）。
- 需求确认后再动手，明确不接受的方案：比如不能用脚本语言替代、不能用拼音代替中文。

## 规范和约束（用户强制）

- **码风良好，不压行**；`if`/`for` 等 compound body 不写在一行；逻辑块/函数之间空行。
- **所有注释用中文**，且只写「为什么」不写「做了什么」；修改行为时必须同步更新注释。
- 安全铁律：所有用户输入被视为不可信；对 `|`、换行、引号必须清理/转义（参考 `common.h` 的 `SanitizeName`/`SanitizeChat`）；不硬编码密钥/Python；防御性编程（空值、边界、超时、日志）。
- 复用优先：能复用 `reference/demon/common.h` 的工具和套路就复用，不重复造轮子。
- 你可以在任何环节自行：校验端口（1024–65535），限长、深重理由用 STL。

## 工作流程纪律

1. 收到大任务先**计划 Plus Todo 列表**（细分小任务，GoTodo 状态跟踪），开始一项置 in_progress、完成置 completed，发现新工作追加条目，不许乱铺垫。
2. **信息优先**：不认识的 API/库先查官方文档或参考 `reference/demon`；需求含糊/多种合理方案先问用户（用问题界面），不许猜测直接输出最终代码。
3. **独立任务可派发子代理（Task）节省上下文**，但每个子代理要返清晰的结果摘要。
4. 编码前用 1-2 句说明理解和实现策略；给出代码后指出安全/边界处理点、依赖用户确认的信息。
5. 交付前必须自行编译通过并做最小复查（有测试能力就做）。
6. 修复或新增功能后必须提交 GIT（git add + commit），提交信息简洁描述改动；未提交不算完成。

## 验收标准（达成后才算完成）

> 现状：本章三项已由 `tests/` 自动化脚本覆盖并通过（见「测试」小节）；第三轮（§11）新增验收项见 `REQUIREMENTS.md` §12.8，实现后一并回归。

- `Start.exe` + N 个 `Client.exe` 在本机联机跑通：建房 → 重名拒绝 → 入房 → 房主转移 → PICK → 职业配置（含 EXIT/非法输入流程）→ 设比例（合法/非法）→ N 人全准备 → 比例不符自动设置并征得房主同意 → 开局 → 结束回归大厅。
- 全部提示为中文、UTF-8 无乱码、快速编辑已关闭（框选不会冻结卡死）。
- 非法输入（重名、非法端口、非法职业输入、非法比例）都不崩溃、有 clean 提示。