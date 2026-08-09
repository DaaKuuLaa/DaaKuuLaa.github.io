// Server.cpp - 狼人杀：单局游戏服务器（每局游戏一个进程，由 Start.exe 启动）
//
// 命令行参数（尾部固定 8 个 + 变长玩家名 + N 个语言码）：
//   Server.exe <gamePort> "<name1>" ... "<nameN>" <startIp> <startPort> <roomId> <W> <N> <G> <level> <villager> <lang1> ... <langN>
//   倒数第 N+8..N+1 个参数为 startIp/startPort/roomId/W/N/G/level/villager，
//   最后 N 个为与玩家名一一对应的语言码（zh/en，顺序即槽位 1..N，需求 §12.1）。
//
// 线程分工：
//   主线程   ：开黑身份分配 → 昼夜交替主循环 → 胜负结算
//   接受线程 ：轮询监听，处理首次连接、断线重连（PLAYER_ID|k）与放弃（GIVEUP|k）；
//              只读首行命令，同包剩余字节交还接收线程缓冲
//   接收线程 ：select(10ms) 轮询所有在线套接字，ReceiveLines 转行入队；
//               PING 行只刷新 lastSeen 不入队；断线或心跳超时（3s 无字节）推 __DISCONNECT__<k>
//
// 发送约定（参考 reference/demon）：
//   发送一律 SendToAll 或 SendToClient：持锁快照目标、锁外逐个 send（发送可能阻塞），
//   失败经 MarkDisconnected 回锁标记断线（socket 身份校验防误杀重连后的新套接字）。
//   接受的新套接字统一设置 SO_SNDTIMEO=5000，防止对端不读时 send 无限挂起。
//
// 断线重连规则（与 Client.exe 约定）：
//   1. 任一存活玩家断线 → 进入重连等待（RECONNECT_TIMEOUT_SECONDS 秒）；期间存活玩家仍可等待；
//   2. 玩家放弃（GIVEUP）或等待超时 → 本局中止，保留房间（只发 GAME_ENDED）；
//   3. 全部玩家失联 → 追加 RELEASE 让房间管理器销毁房间；
//   4. 开局等待（WaitForGameStart）同样受超时约束，防止无玩家连接时进程永久挂起。
//
// NPC 玩家（需求 §19.7）：语言码 npc/npc-off 标记，占槽位无 socket——
//   不参与失联判定与"全部失联 RELEASE"统计；夜晚/白天/遗言/开枪各决策点
//   调用 npc_bot.h 的决策模块即时生成动作行，走与真人输入相同的处理路径。
#include "common.h"
#include "npc_bot.h"
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

// 断线后等待重连的最长时间（秒）。
// 客户端断线后会自行重试数次再放弃，这里留出足够窗口装下正常闪断，
// 又不至于让其余玩家大量空等（参考恶魔轮盘的 25s 经验值）。
const int RECONNECT_TIMEOUT_SECONDS = 25;

// 被放逐者遗言的最长等待时间（秒）。给足打字余量，又不让其余玩家空等太久。
const int LASTWORD_TIMEOUT_SECONDS = 10;

// 白天投票窗口的最长时间（秒）。到期后尚未投票的存活玩家自动弃权，
// 防止个别玩家不投票让整个白天无限挂起（需求 §11.4/§12.4）。
// 默认 90 秒；main() 里读环境变量 WOLF_VOTE_TIMEOUT_SECONDS 覆盖——
// 真实游戏 90 秒窗口无法自动化验收，测试脚本注入短窗口才能测
// "超时自动弃权不卡死"这条路径（需求 §12.8 第 4 项）。
int DAY_VOTE_TIMEOUT_SECONDS = 90;

void Log(const string& msg)
{
    string s = LogMsg("server.log", msg);
    cout << s << endl;
}

// 游戏过程观测日志：相对开局时间、毫秒精度。普通 Log 只有秒级时间戳，
// 无法分辨"几十秒的卡顿"到底发生在哪个阶段（从恶魔轮盘移植）。
auto g_gameClock = chrono::steady_clock::now();

void GLog(const string& msg)
{
    long long ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - g_gameClock).count();
    Log("[+ " + to_string(ms) + "ms] " + msg);
}

// ============ 全局配置与游戏状态 ============

// 由命令行尾部参数解析而来：三方人数 / 档位 / 村民开关
int g_numPlayers = 0;        // 实际人数 N（2..MAX_PLAYERS）
int g_wolfCount = 0;         // 狼人人数
int g_neutralCount = 0;      // 中立人数
int g_godCount = 0;          // 神职人数
int g_level = 0;             // 职业档位 0/1/2
bool g_villagerSwitch = false; // 村民职业是否启用

// 房间管理器通知参数（由 Start.exe 传入）
string g_startIp;
int g_startPort = 0;
string g_roomId;

// 游戏结束时是否需要释放（销毁）房间：全部玩家失联时为 true
bool g_releaseRoom = false;

// 玩家数据（槽 1..N，槽 0 不使用；playerIndex 均以 1 为基）
struct Player
{
    string name;            // 玩家名（已消毒）
    Lang   lang = Lang::Zh; // 玩家语言（发给他的提示一律按此渲染）
    int    npcType = 0;     // 0=真人（有 socket）；1=在线 NPC；2=离线 NPC（需求 §19.7）
    int    jobId = 9;       // 职业 ID（JOBS 下标；默认村民仅在身份分配被覆写）
    bool   alive = false;   // 当前是否存活
    int    slot = 0;        // 槽号 1..N（显示用）
    int    guardLast = 0;   // 守卫前一夜守护对象（0=前夜未守）
    bool   witchSaveUsed = false;   // 女巫解药已用
    bool   witchPoisonUsed = false; // 女巫毒药已用
    bool   hunterShootUsed = false; // 猎人是否已开过枪（全局限一枪）
    bool   idiotFlipped = false;    // 白痴是否已翻牌（翻牌后失去投票权）
    int    voteTarget = -1;         // 白天投票目标（-1=尚未投票，0=弃权）
};
vector<Player> g_players;           // 下标 1..g_numPlayers，槽 0 不用

// 丘比特选定的情侣（槽号 1..N；-1 表示本局没有情侣）
int g_loverA = -1;
int g_loverB = -1;

// NPC 决策可用的游戏线索（需求 §19.7：历史至少含验人结果与死亡信息）。
// 这些摘要只用于组装 NPC 决策上下文，真人玩家永远看不到；Server 不存
// 聊天历史，故不含过往发言。验人记录仅预言家 NPC 可见，狼刀记录仅狼
// 阵营 NPC 可见，死亡记录公开——避免线索泄漏让 NPC 变成全知玩家。
vector<string> g_seerCheckNotes;  // 验人记录中文摘要（"第N夜查验X号A：狼人。"）
vector<string> g_deathNotes;      // 死亡记录中文摘要（"X号A已死亡（被狼人击杀）。"）
vector<string> g_wolfKillNotes;   // 狼队刀人记录中文摘要（"第N夜刀杀X号A。"）

// 当前夜晚编号（从 1 开始）——女巫只有首夜可自救
int g_night = 0;

// 是否因断线/超时中止本局（正常结束由胜负判定接管）
bool g_gameAborted = false;

// ============ 网络与重连状态 ============

SOCKET g_clients[MAX_PLAYERS] = { INVALID_SOCKET };
bool g_connected[MAX_PLAYERS] = { false };
string g_recvBuffers[MAX_PLAYERS];       // 各槽位半行缓冲（仅接收线程访问）
queue<string> g_msgQueue;
mutex g_mutex;
condition_variable g_cv;
atomic<bool> g_serverRunning(true);

// 每玩家语言（下标 1..N 与槽位一致，由命令行尾部语言码解析而来，需求 §12.1）
Lang g_playerLang[MAX_PLAYERS] = { Lang::Zh };

// 各槽位最后收到字节的时刻（GetTickCount64），心跳超时判定用（需求 §12.2）
ULONGLONG g_lastSeen[MAX_PLAYERS] = { 0 };

// 是否正处于白天投票收集阶段：接受线程据此判断重连后是否补发 __DAY_OPEN__（需求 §12.4）
atomic<bool> g_dayVoting(false);

// 断线重连状态（见文件头注释）
bool g_waitingReconnect[MAX_PLAYERS] = { false };
time_t g_reconnectDeadline[MAX_PLAYERS] = { 0 };
bool g_giveUp[MAX_PLAYERS] = { false };

// ============ 基本发送 ============

// 带 5 秒超时地发完全部字节。
// 对端停止读取（TCP 接收窗口满）时 send 会长期 WSAEWOULDBLOCK，无限重试会让
// 游戏主线程永久挂死；超时视为连接异常返回 false，由调用者走断线流程。
const auto SEND_TIMEOUT = chrono::seconds(5);

bool SendAllData(SOCKET sock, const char* data, int len)
{
    auto t0 = chrono::steady_clock::now();
    int total = 0;
    auto deadline = t0 + SEND_TIMEOUT;

    while (total < len)
    {
        int sent = send(sock, data + total, len - total, 0);

        if (sent <= 0)
        {
            if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
            {
                if (chrono::steady_clock::now() >= deadline)
                {
                    Log("SendAllData timeout: peer not reading, treating as disconnected");
                    return false;
                }

                Sleep(10);
                continue;
            }

            Log("SendAllData FAIL sock=" + to_string(sock) + " err=" + to_string(WSAGetLastError()));
            return false;
        }

        total += sent;
    }

    return true;
}

// 发送失败后的统一处理：标记断线、关闭套接字、通知游戏逻辑。
// sock 身份校验：若标记期间该槽位已被接受线程重连为新套接字，则不误杀。
void MarkDisconnected(int id, SOCKET sock)
{
    string logMsg;

    {
        lock_guard<mutex> lock(g_mutex);

        if (!g_connected[id] || g_clients[id] != sock) return;

        g_connected[id] = false;
        closesocket(g_clients[id]);
        g_clients[id] = INVALID_SOCKET;
        g_msgQueue.push("__DISCONNECT__" + to_string(id));
        g_cv.notify_one();
        logMsg = "Client " + to_string(id + 1) + " disconnected (send)";
    }

    Log(logMsg);
}

void SendToAll(const string& msg)
{
    string out = msg;

    if (out.empty() || out.back() != '\n') out += '\n';

    vector<SOCKET> targets;

    {
        lock_guard<mutex> lock(g_mutex);

        for (int i = 0; i < g_numPlayers; ++i)
        {
            if (g_connected[i]) targets.push_back(g_clients[i]);
        }
    }

    // 锁外发送（锁内 send 可能被阻塞的 TCP 发送拖住整个游戏）
    for (SOCKET s : targets)
    {
        if (!SendAllData(s, out.c_str(), (int)out.length()))
        {
            int failedId = -1;

            {
                lock_guard<mutex> lock(g_mutex);

                for (int i = 0; i < g_numPlayers; ++i)
                {
                    if (g_clients[i] == s && g_connected[i])
                    {
                        failedId = i;
                        break;
                    }
                }
            }

            if (failedId >= 0) MarkDisconnected(failedId, s);
        }
    }
}

// 向指定槽位（0 基）发送消息
void SendToClient(int id, const string& msg)
{
    if (id < 0 || id >= g_numPlayers) return;

    string out = msg;

    if (out.empty() || out.back() != '\n') out += '\n';

    SOCKET target = INVALID_SOCKET;

    {
        lock_guard<mutex> lock(g_mutex);

        if (g_connected[id]) target = g_clients[id];
    }

    if (target == INVALID_SOCKET) return;

    if (!SendAllData(target, out.c_str(), (int)out.length()))
    {
        MarkDisconnected(id, target);
    }
}

// ============ L10n 输出（按接收者语言渲染，需求 §12.3） ============

// 单播双语提示：按槽位 slot（1 基，与 g_playerLang 下标一致）的语言渲染后发送。
// 中英格式串占位符必须一致；%s 传 .c_str()。
void SendToClientL10n(int slot, const char* zh, const char* en, ...)
{
    if (slot < 1 || slot > g_numPlayers) return;

    char buf[4096];
    const char* fmt = (g_playerLang[slot] == Lang::En) ? en : zh;
    va_list args;

    va_start(args, en);
    vsnprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);

    SendToClient(slot - 1, buf);
}

// 广播双语提示：逐个玩家按其语言渲染（同一消息不同玩家语言不同也能正确分送）
void SendToAllL10n(const char* zh, const char* en, ...)
{
    char zhBuf[4096];
    char enBuf[4096];

    {
        va_list args;

        va_start(args, en);
        vsnprintf_s(zhBuf, _TRUNCATE, zh, args);
        va_end(args);
    }

    {
        va_list args;

        va_start(args, en);
        vsnprintf_s(enBuf, _TRUNCATE, en, args);
        va_end(args);
    }

    for (int i = 1; i <= g_numPlayers; ++i)
    {
        SendToClient(i - 1, (g_playerLang[i] == Lang::En) ? enBuf : zhBuf);
    }
}

// 预渲染成对的文本广播：职业名/死因等按语言取词、占位符不共用的文案
// 无法走 FmtLang 变参，先各自生成完整句子再按语言分送
void SendToAllL10nPair(const string& zh, const string& en)
{
    for (int i = 1; i <= g_numPlayers; ++i)
    {
        SendToClient(i - 1, (g_playerLang[i] == Lang::En) ? en : zh);
    }
}

// 预渲染成对的单播文本（用法同上，slot 为 1 基槽位）
void SendToClientL10nPair(int slot, const string& zh, const string& en)
{
    if (slot < 1 || slot > g_numPlayers) return;

    SendToClient(slot - 1, (g_playerLang[slot] == Lang::En) ? en : zh);
}

// ============ 状态查询（全部带锁） ============

bool IsConnected(int idx)
{
    lock_guard<mutex> lock(g_mutex);
    return g_connected[idx];
}

bool AnyConnected()
{
    lock_guard<mutex> lock(g_mutex);

    for (int i = 0; i < g_numPlayers; ++i)
    {
        if (g_connected[i]) return true;
    }

    return false;
}

// ============ NPC 决策（需求 §19.7） ============

// 槽位是否为 NPC（1 基槽号；真人=0）
bool IsNpc(int slot)
{
    if (slot < 1 || slot > g_numPlayers) return false;
    return g_players[slot].npcType != 0;
}

// 收集存活玩家槽号列表（可排除指定槽）；NPC 决策的可行动目标池
vector<int> AliveTargetList(int excludeSlot)
{
    vector<int> v;

    for (int i = 1; i <= g_numPlayers; ++i)
    {
        if (!g_players[i].alive) continue;
        if (i == excludeSlot) continue;
        v.push_back(i);
    }

    return v;
}

// 是否所有玩家都失联（决定是否释放房间）。
// NPC 无 socket 永远不在线，按契约不参与统计：只数真实 socket 玩家（需求 §19.3）
bool AllLost()
{
    lock_guard<mutex> lock(g_mutex);

    for (int i = 0; i < g_numPlayers; ++i)
    {
        if (IsNpc(i + 1)) continue;

        if (g_connected[i]) return false;
    }

    return true;
}

// 解析 NPC 动作行的目标编号：形如 "PREFIX|i"；-2 表示不可解析。
// 决策模块输出按不可信输入处理，畸形内容不得让游戏崩溃或越界。
int ParseNpcTarget(const string& action, const string& prefix)
{
    string head = prefix + "|";

    if (action.compare(0, head.size(), head) != 0) return -2;

    string num = action.substr(head.size());

    if (num.empty()) return -2;

    for (char c : num)
    {
        if (!isdigit((unsigned char)c)) return -2;
    }

    return atoi(num.c_str());
}

// 从 "SPEECH|内容" 提取发言文本；非 SPEECH 动作返回空串
string ParseNpcSpeech(const string& action)
{
    const string head = "SPEECH|";

    if (action.compare(0, head.size(), head) != 0) return "";

    return action.substr(head.size());
}

// ============ 消息队列 ============

// 从共享队列取一条原始消息；100ms Sleep 轮询。
// 全部玩家失联时返回 false（游戏应立即中止）。
bool PollAllForMessage(string& out)
{
    while (true)
    {
        {
            lock_guard<mutex> lock(g_mutex);

            if (!g_msgQueue.empty())
            {
                out = g_msgQueue.front();
                g_msgQueue.pop();
                return true;
            }

            // 持锁期间直接查 g_connected：不能调 AnyConnected()，否则同线程
            // 递归锁定同一 mutex 会抛 resource_deadlock（2026-08-03 实测崩溃）
            bool anyOnline = false;

            for (int i = 0; i < g_numPlayers; ++i)
            {
                if (g_connected[i])
                {
                    anyOnline = true;
                    break;
                }
            }

            if (!anyOnline) return false;
        }

        Sleep(100);
    }
}

// 白天收集投票用的带超时队列等待：PING 心跳被接收线程丢弃、不进队列，
// 若沿用 PollAllForMessage 的无限阻塞，全体静默时投票超时与 10 秒窗口重发
// 检查就永远轮不到执行（2026-08-05 第三轮验收实测：6 秒注入窗口不生效、白天挂死）。
// 返回 1=取到消息；0=等待超时无消息（调用方应回到循环顶复查死线）；-1=已无在线玩家。
int PollAllForMessageTimed(string& out, int timeoutMs)
{
    auto deadlineMs = GetTickCount64() + timeoutMs;

    while (true)
    {
        {
            lock_guard<mutex> lock(g_mutex);

            if (!g_msgQueue.empty())
            {
                out = g_msgQueue.front();
                g_msgQueue.pop();
                return 1;
            }

            // 持锁期间直接查 g_connected：不能调 AnyConnected()，否则同线程
            // 递归锁定同一 mutex 会抛 resource_deadlock（2026-08-03 实测崩溃）
            bool anyOnline = false;

            for (int i = 0; i < g_numPlayers; ++i)
            {
                if (g_connected[i])
                {
                    anyOnline = true;
                    break;
                }
            }

            if (!anyOnline) return -1;
        }

        if (GetTickCount64() >= deadlineMs) return 0;

        Sleep(100);
    }
}

// ============ 接收线程 ============

void ReceiveThreadFunc()
{
    try
    {
        fd_set readSet;

        while (g_serverRunning)
    {
        FD_ZERO(&readSet);
        SOCKET maxSock = 0;

        {
            lock_guard<mutex> lock(g_mutex);

            for (int i = 0; i < g_numPlayers; ++i)
            {
                if (g_connected[i])
                {
                    FD_SET(g_clients[i], &readSet);

                    if (g_clients[i] > maxSock) maxSock = g_clients[i];
                }
            }
        }

        if (maxSock == 0)
        {
            Sleep(10);
            continue;
        }

        // tv 必须每次循环重置：select() 会把剩余时间写回 timeval
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;

        int sel = select(0, &readSet, NULL, NULL, &tv);

        if (sel > 0)
        {
            for (int i = 0; i < g_numPlayers; ++i)
            {
                bool readable;

                {
                    lock_guard<mutex> lock(g_mutex);
                    readable = g_connected[i] && FD_ISSET(g_clients[i], &readSet);
                }

                if (!readable) continue;

                // 整个接收+缓冲+入队过程持锁，避免与接受线程清空 g_recvBuffers 竞争
                {
                    lock_guard<mutex> lock(g_mutex);

                    if (!ReceiveLines(g_clients[i], g_recvBuffers[i], [i](const string& line)
                    {
                        // PING 是心跳保留字：不算聊天/命令，只由外层刷新 lastSeen（需求 §12.2）。
                        // 回一行 PING 作应答：客户端据此区分"服务器存活但静默"与"半开死连"
                        // （对端已死/网络中断时无应答，客户端读超时 15s 判定失联触发重连，
                        // 2026-08-07 稳定性修复）。持锁 send，SO_SNDTIMEO 已设防阻塞
                        if (IsPingLine(line))
                        {
                            const char* pong = "PING\n";
                            send(g_clients[i], pong, 5, 0);
                            return;
                        }

                        g_msgQueue.push(line);
                    }))
                    {
                        GLog("recv-thread: ReceiveLines failed for player " + to_string(i + 1) + " err=" + to_string(WSAGetLastError()));
                        g_connected[i] = false;
                        closesocket(g_clients[i]);
                        g_clients[i] = INVALID_SOCKET;
                        g_msgQueue.push("__DISCONNECT__" + to_string(i));
                        g_cv.notify_one();
                    }
                    else
                    {
                        // 收到任意字节（含 PING）都刷新最后活跃时刻
                        g_lastSeen[i] = GetTickCount64();
                    }
                }
            }
        }
        else if (sel < 0)
        {
            Sleep(10);
        }

        // 心跳失联判定：超过 HEARTBEAT_DEADLINE_SECONDS（3 秒）未收到任何字节的
        // 在线 socket 视为失联。能检出拔线/进程被杀但 TCP 未关的半开连接
        // （不依赖 FIN/RST），走与 recv 失败相同的 __DISCONNECT__ 流程。
        // NPC 无 socket 不参与失联判定（需求 §19.3）
        {
            lock_guard<mutex> lock(g_mutex);
            ULONGLONG now = GetTickCount64();

            for (int i = 0; i < g_numPlayers; ++i)
            {
                if (IsNpc(i + 1)) continue;

                if (!g_connected[i] || g_lastSeen[i] == 0) continue;

                if (now - g_lastSeen[i] > (ULONGLONG)HEARTBEAT_DEADLINE_SECONDS * 1000)
                {
                    GLog("heartbeat timeout: player " + to_string(i + 1) + " silent for " + to_string(HEARTBEAT_DEADLINE_SECONDS) + "s, marking disconnected");
                    g_connected[i] = false;
                    closesocket(g_clients[i]);
                    g_clients[i] = INVALID_SOCKET;
                    g_msgQueue.push("__DISCONNECT__" + to_string(i));
                    g_cv.notify_one();
                }
            }
        }
    }
    }
    catch (const exception& e)
    {
        Log(string("RECV-THREAD EXCEPTION: ") + e.what());
        Sleep(10000);
    }
    catch (...)
    {
        Log("RECV-THREAD UNKNOWN EXCEPTION");
        Sleep(10000);
    }
}

// ============ 接受线程（首次连接 / 重连 / 放弃） ============

void AcceptThreadFunc(SOCKET listenSock)
{
    try
    {
    while (g_serverRunning)
    {
        sockaddr_in clientAddr;
        int addrLen = sizeof(clientAddr);

        SOCKET clientSock = accept(listenSock, (sockaddr*)&clientAddr, &addrLen);

        if (clientSock == INVALID_SOCKET)
        {
            Sleep(100);
            continue;
        }

        // 对端不读时 send 最多阻塞 5 秒即失败，走断线流程
        int sndTimeout = 5000;
        setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));

        string buf;
        string line;
        string leftover;
        bool handled = false;

        // 只处理首行：不能复用 ReceiveLines 一路读下去——它会把同一包中首行
        // 之后的内容也读走且处理完首行后仍在读的数据会被静默丢弃（吞掉玩家
        // 的第一个游戏消息）。这里手动拼首行，剩余字节交还接收线程缓冲。
        while (!handled)
        {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(clientSock, &readSet);

            timeval tv;
            tv.tv_sec = 30;
            tv.tv_usec = 0;

            int sel = select(0, &readSet, NULL, NULL, &tv);

            if (sel <= 0)
            {
                closesocket(clientSock);
                break;
            }

            char data[4096];
            int bytes = recv(clientSock, data, sizeof(data) - 1, 0);

            if (bytes <= 0)
            {
                closesocket(clientSock);
                break;
            }

            data[bytes] = '\0';
            buf += data;

            if (buf.size() > 16 * 1024)
            {
                closesocket(clientSock);
                break;
            }

            size_t newline = buf.find('\n');

            if (newline == string::npos) continue;

            line = buf.substr(0, newline);

            if (!line.empty() && line.back() == '\r') line.pop_back();

            leftover = buf.substr(newline + 1);

            if (line.empty())
            {
                buf.clear();
                continue;
            }

            handled = true;
        }

        if (!handled) continue;

        int pid = 0;

        if (line.find("GIVEUP|") == 0) pid = atoi(line.c_str() + 7);
        else if (line.find("PLAYER_ID|") == 0) pid = atoi(line.c_str() + 10);

        // 非法槽位 → 拒绝
        if (pid < 1 || pid > g_numPlayers)
        {
            string reject = "ERROR:Invalid PLAYER_ID\n";
            send(clientSock, reject.c_str(), reject.length(), 0);
            closesocket(clientSock);
            continue;
        }

        // NPC 槽位无 socket：真人客户端不得认领，否则会在 NPC 即时决策路径
        // 上凭空多出一个等待输入的连接（需求 §19.7）
        if (IsNpc(pid))
        {
            string reject = "ERROR:NPC slot\n";
            send(clientSock, reject.c_str(), reject.length(), 0);
            closesocket(clientSock);
            continue;
        }

        int idx = pid - 1;
        bool isReconnect = false;
        bool resendDayPrompt = false;
        string notifyOtherZh;
        string notifyOtherEn;

        {
            lock_guard<mutex> lock(g_mutex);

            // 该位置已有人在玩 → 拒绝（正常流程不会发生）
            if (g_connected[idx])
            {
                string reject = "ERROR:Player " + to_string(pid) + " already connected\n";
                send(clientSock, reject.c_str(), reject.length(), 0);
                closesocket(clientSock);
                continue;
            }

            // 该玩家已放弃 → 拒绝
            if (g_giveUp[idx])
            {
                closesocket(clientSock);
                continue;
            }

            // 放弃重连：让游戏逻辑立即中止本局
            if (line.find("GIVEUP|") == 0)
            {
                g_giveUp[idx] = true;
                g_waitingReconnect[idx] = false;
                closesocket(clientSock);
                Log("Player " + to_string(pid) + " gave up reconnecting");
                continue;
            }

            // 正常连接：首次或重连
            isReconnect = g_waitingReconnect[idx];

            g_clients[idx] = clientSock;
            g_connected[idx] = true;
            g_waitingReconnect[idx] = false;
            g_reconnectDeadline[idx] = 0;

            // 发送超时 3 秒：PING 应答在接收线程持锁 send，若对端已半开
            // 死连（缓冲满），send 会长期阻塞并连累持锁的其他发送方冻结全
            // 局，超时后失败返回即可，该连接随后被心跳失联判定清理
            DWORD sndTimeout = 3000;
            setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));

            // 连接建立/重连成功时重置心跳计时：失联判定从此刻重新起算（需求 §12.2）
            g_lastSeen[idx] = GetTickCount64();

            // 首行之后的同包剩余数据交还接收线程缓冲继续解析（防御）
            g_recvBuffers[idx].clear();

            if (!leftover.empty())
            {
                g_recvBuffers[idx] = leftover;
            }

            if (isReconnect)
            {
                Log("Player " + to_string(pid) + " reconnected");
                notifyOtherZh = "玩家" + g_players[pid].name + " 重新连接成功。";
                notifyOtherEn = "Player " + g_players[pid].name + " reconnected.";

                // 白天投票期间重连：补开输入窗口并重发提示（客户端断线会复位
                // g_dayTalk，只发一行文本不补 __DAY_OPEN__ 的话重连者永远无法投票——已知根因）
                resendDayPrompt = g_dayVoting;
            }
            else
            {
                Log("Client assigned as Player " + to_string(pid));
            }
        }

        // 欢迎消息（锁外发送，按玩家语言）
        string zhW = "你被分配到 " + to_string(pid) + " 号位。";
        string enW = "You are assigned to slot " + to_string(pid) + ".";

        if (isReconnect)
        {
            zhW += "欢迎回来！";
            enW += " Welcome back!";
        }
        else
        {
            zhW += "游戏服务器欢迎你！";
            enW += " Welcome to the game server!";
        }

        SendToClientL10nPair(pid, zhW, enW);

        if (!notifyOtherZh.empty())
        {
            SendToAllL10nPair(notifyOtherZh, notifyOtherEn);

            // 提示先发、窗口后开：客户端打印提示期间输入门关闭，先键入的字符不丢失
            if (resendDayPrompt)
            {
                SendToClientL10n(pid, "白天仍在进行，请继续发言或投票（VOTE <编号>/投票 <编号>，0 弃权）。", "Day is still in progress. Keep talking or vote (VOTE <n>, 0 to abstain).");
                SendToClient(pid - 1, "__DAY_OPEN__");
            }
        }

        {
            lock_guard<mutex> lock(g_mutex);
            g_cv.notify_all();
        }
    }
    }
    catch (const exception& e)
    {
        Log(string("ACCEPT-THREAD EXCEPTION: ") + e.what());
        Sleep(10000);
    }
    catch (...)
    {
        Log("ACCEPT-THREAD UNKNOWN EXCEPTION");
        Sleep(10000);
    }
}

// ============ 重连等待 ============

bool EnterReconnectWait(int playerIdx)
{
    lock_guard<mutex> lock(g_mutex);

    if (g_waitingReconnect[playerIdx]) return true;

    g_waitingReconnect[playerIdx] = true;
    g_reconnectDeadline[playerIdx] = time(nullptr) + RECONNECT_TIMEOUT_SECONDS;
    return true;
}

// 等待指定玩家重连。
// 返回 true  = 玩家已重连，游戏继续；
// 返回 false = 对方放弃（GIVEUP）、等待超时，游戏应中止。
bool WaitForReconnect(int playerIdx)
{
    if (playerIdx < 0 || playerIdx >= g_numPlayers) return false;

    GLog("WaitForReconnect entry: player " + to_string(playerIdx + 1) + " lost, waiting up to " + to_string(RECONNECT_TIMEOUT_SECONDS) + "s");

    EnterReconnectWait(playerIdx);

    while (g_serverRunning)
    {
        bool reconnected = false;
        bool gaveUp = false;
        bool timedOut = false;

        {
            lock_guard<mutex> lock(g_mutex);

            reconnected = g_connected[playerIdx];
            gaveUp = g_giveUp[playerIdx];
            timedOut = time(nullptr) > g_reconnectDeadline[playerIdx];

            if (reconnected || gaveUp || timedOut)
            {
                g_waitingReconnect[playerIdx] = false;

                if (timedOut) g_giveUp[playerIdx] = true;
            }
        }

        if (reconnected) return true;

        if (gaveUp || timedOut)
        {
            Log("WaitForReconnect exit: player " + to_string(playerIdx + 1) + " gaveUp=" + to_string(gaveUp) + " timedOut=" + to_string(timedOut));
            return false;
        }

        Sleep(200);
    }

    return false;
}

// 确保所有存活玩家都在线；若有人断线则阻塞等待重连。
// 返回 false 表示应中止游戏。
// NPC 无 socket，跳过重连等待（需求 §19.7：阶段流程不等 NPC 输入）
bool EnsureAliveConnected()
{
    for (int i = 1; i <= g_numPlayers; ++i)
    {
        if (IsNpc(i)) continue;

        if (g_players[i].alive && !IsConnected(i - 1))
        {
            if (!WaitForReconnect(i - 1)) return false;
        }
    }

    return true;
}

// 游戏开始前等待所有玩家连接（接受线程负责分配并唤醒）。
// 带超时：某玩家迟迟不连接时不再让进程永久挂起；超时后走正常中止流程。
void WaitForGameStart()
{
    unique_lock<mutex> lock(g_mutex);

    while (true)
    {
        bool allIn = true;

        // NPC 无 socket 永远连不上，开局只等真人全部入座（需求 §19.7）
        for (int i = 0; i < g_numPlayers; ++i)
        {
            if (IsNpc(i + 1)) continue;

            if (!g_connected[i])
            {
                allIn = false;
                break;
            }
        }

        if (allIn) break;

        if (g_cv.wait_for(lock, chrono::seconds(RECONNECT_TIMEOUT_SECONDS))
            == cv_status::timeout)
        {
            Log("Timed out waiting for all players to connect");
            return;
        }
    }

    // 清空开局前积累的过期消息（如开局瞬间的 __DISCONNECT__）
    while (!g_msgQueue.empty()) g_msgQueue.pop();

    GLog("WaitForGameStart done: all connected");
}

// ============ 玩家输入 ============

// 解析客户端消息 "PLAYER_<k>|<内容>"，返回槽号（0 基）与内容
bool ParseClientMsg(const string& raw, int& slot, string& content)
{
    if (raw.compare(0, 7, "PLAYER_") != 0) return false;

    size_t pipe = raw.find('|');

    if (pipe == string::npos) return false;

    string numStr = raw.substr(7, pipe - 7);

    if (numStr.empty()) return false;

    for (char c : numStr)
    {
        if (!isdigit((unsigned char)c)) return false;
    }

    int k = atoi(numStr.c_str());

    if (k < 1 || k > g_numPlayers) return false;

    slot = k - 1;
    content = raw.substr(pipe + 1);
    return true;
}

// 从 "__DISCONNECT__<k>" 解析槽号（0 基）；失败返回 -1
int ParseDisconnectSlot(const string& raw)
{
    if (raw.compare(0, 14, "__DISCONNECT__") != 0) return -1;

    string numStr = raw.substr(14);

    if (numStr.empty()) return -1;

    for (char c : numStr)
    {
        if (!isdigit((unsigned char)c)) return -1;
    }

    int k = atoi(numStr.c_str());

    if (k < 0 || k >= g_numPlayers) return -1;

    return k;
}

// 等待指定玩家（0 基槽位）输入一行。
// 玩家断线时进入重连等待；对方断线同样转去等待对方重连（防止双方互等挂死）。
// 返回 false 表示游戏应中止。
bool WaitForPlayerInput(string& out, int playerIdx)
{
    while (true)
    {
        if (!g_players[playerIdx + 1].alive)
        {
            out = "0";
            return true;
        }

        if (!IsConnected(playerIdx))
        {
            if (!WaitForReconnect(playerIdx)) return false;

            // 重连成功：重新提示输入（断线期间原来的提示可能已丢失）
            SendToClient(playerIdx, "__INPUT__");
            continue;
        }

        string msg;

        if (!PollAllForMessage(msg)) return false;

        int dis = ParseDisconnectSlot(msg);

        if (dis >= 0)
        {
            if (dis == playerIdx)
            {
                if (!WaitForReconnect(playerIdx)) return false;

                SendToClient(playerIdx, "__INPUT__");
            }
            else
            {
                // 对方断线：转去等待对方重连；对方放弃/超时 → 本局中止
                if (!WaitForReconnect(dis)) return false;
            }

            continue;
        }

        int from;
        string content;

        if (ParseClientMsg(msg, from, content))
        {
            if (from == playerIdx)
            {
                out = content;
                return true;
            }
        }
    }
}

// 先发提示文本、最后发 __INPUT__：客户端在提示打印期间输入门是关闭的，
// 提前键入不会混进提示里；只有 __INPUT__ 到达时才打开输入窗口（2026-08-03 约定）。
bool AskChoice(int playerIdx, const string& prompt, string& out)
{
    GLog("AskChoice entry: player " + to_string(playerIdx + 1));
    SendToClient(playerIdx, prompt);
    SendToClient(playerIdx, "__INPUT__");
    bool ok = WaitForPlayerInput(out, playerIdx);
    GLog("AskChoice exit: player " + to_string(playerIdx + 1) + " ok=" + to_string(ok) + " in='" + out + "'");
    return ok;
}

// AskChoice 的双语变体：提示按玩家语言渲染（playerIdx 为 0 基，与 AskChoice 一致）
bool AskChoiceL10n(int playerIdx, const char* zh, const char* en, string& out)
{
    GLog("AskChoice entry: player " + to_string(playerIdx + 1));
    SendToClientL10n(playerIdx + 1, zh, en);
    SendToClient(playerIdx, "__INPUT__");
    bool ok = WaitForPlayerInput(out, playerIdx);
    GLog("AskChoice exit: player " + to_string(playerIdx + 1) + " ok=" + to_string(ok) + " in='" + out + "'");
    return ok;
}

// AskChoice 的预渲染成对变体：提示内嵌职业名等按语言取词的文案用
bool AskChoicePair(int playerIdx, const string& zh, const string& en, string& out)
{
    GLog("AskChoice entry: player " + to_string(playerIdx + 1));
    SendToClientL10nPair(playerIdx + 1, zh, en);
    SendToClient(playerIdx, "__INPUT__");
    bool ok = WaitForPlayerInput(out, playerIdx);
    GLog("AskChoice exit: player " + to_string(playerIdx + 1) + " ok=" + to_string(ok) + " in='" + out + "'");
    return ok;
}

// ============ 阵营与人数统计 ============

bool IsWolfCamp(int jobId)
{
    return JOBS[jobId].camp == CAMP_WOLF;
}

bool IsGoodCamp(int jobId)
{
    return JOBS[jobId].camp == CAMP_GOD || JOBS[jobId].camp == CAMP_VILLAGER;
}

// 预言家验人结果的三分类标签（按语言取词）
string CampLabel(int jobId, Lang l)
{
    if (IsWolfCamp(jobId)) return (l == Lang::En) ? "wolf" : "狼人";

    if (JOBS[jobId].camp == CAMP_NEUTRAL) return (l == Lang::En) ? "neutral" : "中立";

    return (l == Lang::En) ? "good" : "好人";
}

int CountAlive()
{
    int n = 0;

    for (int i = 1; i <= g_numPlayers; ++i)
    {
        if (g_players[i].alive) ++n;
    }

    return n;
}

int CountAliveWolf()
{
    int n = 0;

    for (int i = 1; i <= g_numPlayers; ++i)
    {
        if (g_players[i].alive && IsWolfCamp(g_players[i].jobId)) ++n;
    }

    return n;
}

int CountAliveGood()
{
    int n = 0;

    for (int i = 1; i <= g_numPlayers; ++i)
    {
        if (g_players[i].alive && IsGoodCamp(g_players[i].jobId)) ++n;
    }

    return n;
}

// 查找指定职业的存活槽位；-1 表示没有
int FindAliveJob(int jobId)
{
    for (int i = 1; i <= g_numPlayers; ++i)
    {
        if (g_players[i].alive && g_players[i].jobId == jobId) return i;
    }

    return -1;
}

// ============ NPC 决策组装与调用（需求 §19.7） ============

// 组装 NPC 决策历史文本（中文）：阶段描述 + 存活名单 + 按身份可见的线索
string BuildNpcHistory(int slot, const string& phase, const string& extra)
{
    string h = "你是" + string(JOBS[g_players[slot].jobId].zhName) + "（名字" + g_players[slot].name
        + "，槽" + to_string(slot) + "）。";

    if (phase.rfind("night_", 0) == 0) h += "现在是第" + to_string(g_night) + "夜。";
    else h += "现在是第" + to_string(g_night) + "天的白天阶段。";

    h += "玩家名单：";

    for (int i = 1; i <= g_numPlayers; ++i)
    {
        h += to_string(i) + "号" + g_players[i].name + "；";
    }

    h += "存活玩家：";

    for (int i = 1; i <= g_numPlayers; ++i)
    {
        if (g_players[i].alive) h += to_string(i) + "号" + g_players[i].name + "；";
    }

    // 验人结果只有预言家自己知道，泄漏给其他 NPC 会让狼队变成全知
    if (g_players[slot].jobId == 2 && !g_seerCheckNotes.empty())
    {
        h += "你的验人记录：";

        for (auto& n : g_seerCheckNotes) h += n;
    }

    if (!g_deathNotes.empty())
    {
        h += "死亡记录：";

        for (auto& n : g_deathNotes) h += n;
    }

    // 狼队刀人记录只有狼阵营自己知道
    if (IsWolfCamp(g_players[slot].jobId) && !g_wolfKillNotes.empty())
    {
        h += "狼队刀人记录：";

        for (auto& n : g_wolfKillNotes) h += n;
    }

    if (!extra.empty()) h += "线索：" + extra + "。";

    return h;
}

// 取 NPC 决策动作行：在线 NPC 先调 NpcOnlineDecide（同步阻塞，失败/超时返回
// 空串），空串回退 NpcOfflineDecide；离线 NPC 直接走离线逻辑。决策模块内部
// 异常不得中断游戏主流程，一律 catch 后回退。NPC 语言固定中文（需求 §19.7）。
string NpcGetAction(int slot, const string& phase, const vector<int>& targets, const string& extra)
{
    if (!IsNpc(slot)) return "NONE";

    NpcContext ctx;
    ctx.selfIndex = slot;
    ctx.roleEn = JOBS[g_players[slot].jobId].enName;
    ctx.phase = phase;
    ctx.aliveSlots = AliveTargetList(0);
    ctx.names.reserve(g_numPlayers);

    for (int i = 1; i <= g_numPlayers; ++i)
    {
        ctx.names.push_back(g_players[i].name);
    }

    ctx.targets = targets;
    ctx.history = BuildNpcHistory(slot, phase, extra);
    ctx.lang = Lang::Zh;

    string action;

    try
    {
        if (g_players[slot].npcType == 1) action = NpcOnlineDecide(ctx);

        if (action.empty()) action = NpcOfflineDecide(ctx);
    }
    catch (const exception& e)
    {
        Log(string("NPC decision exception: ") + e.what());
        action.clear();
    }
    catch (...)
    {
        Log("NPC decision unknown exception");
        action.clear();
    }

    if (action.empty()) action = "NONE";

    GLog("NPC decision slot=" + to_string(slot) + " phase=" + phase + " action=[" + action + "]");
    return action;
}

// ============ 死亡处理（含情侣殉情、猎人开枪） ============

// 玩家死亡时的广播文案：职业名与死因都要按语言取词，中英占位不共用，
// 故按语言各生成完整句子（调用方用 SendToAllL10nPair 分送）
string DeathText(int slot, const string& cause, Lang l)
{
    string who;

    if (l == Lang::En)
    {
        who = "Player " + g_players[slot].name + " (slot " + to_string(slot) + ", " + JOBS[g_players[slot].jobId].enName + ")";
    }
    else
    {
        who = "玩家" + g_players[slot].name + "(槽" + to_string(slot) + ", " + JOBS[g_players[slot].jobId].zhName + ")";
    }

    string how;

    if (cause == "wolf") how = (l == Lang::En) ? " was killed by wolves" : "被狼人击杀";
    else if (cause == "poison") how = (l == Lang::En) ? " was poisoned by the witch" : "被女巫毒杀";
    else if (cause == "exile") how = (l == Lang::En) ? " was exiled" : "被放逐";
    else if (cause == "bomb") how = (l == Lang::En) ? " was taken by the White Wolf King's bomb" : "被白狼王自爆带走";
    else if (cause == "bomber") how = (l == Lang::En) ? " died as the White Wolf King" : "白狼王自爆身亡";
    else if (cause == "lover") how = (l == Lang::En) ? " died with their lover" : "因情侣殉情";
    else if (cause == "hunter") how = (l == Lang::En) ? " was shot by the hunter" : "被猎人开枪带走";
    else how = (l == Lang::En) ? " died" : "死亡";

    return who + how;
}

// 询问猎人开枪目标（0 表示放弃开枪）。
// 返回 false 表示应中止游戏。
bool AskHunterShot(int hunterSlot, int& target)
{
    // NPC 猎人：即时生成开枪决策（NIGHT_SHOOT|i，-1 语义同 0=不开枪），
    // 非法目标一律按放弃处理，不进入真人输入循环
    if (IsNpc(hunterSlot))
    {
        int t = ParseNpcTarget(
            NpcGetAction(hunterSlot, "hunter_shot", AliveTargetList(hunterSlot), ""),
            "NIGHT_SHOOT");

        if (!(t == 0 || t == -1 || (t >= 1 && t <= g_numPlayers && g_players[t].alive && t != hunterSlot)))
        {
            t = 0;
        }

        if (t == -1) t = 0;

        target = t;
        return true;
    }

    while (true)
    {
        string in;

        if (!AskChoiceL10n(hunterSlot - 1, "你被击杀了，请输入开枪目标编号（0 放弃开枪）：", "You died. Enter a target to shoot (0 = no shot):", in)) return false;

        int t = atoi(in.c_str());

        if (t == 0)
        {
            target = 0;
            return true;
        }

        if (t >= 1 && t <= g_numPlayers && g_players[t].alive && t != hunterSlot)
        {
            target = t;
            return true;
        }

        SendToClientL10n(hunterSlot, "输入不合法：目标必须是 1..N 的存活玩家或 0。请重新输入。", "Invalid target: must be an alive player 1..N or 0. Try again.");
    }
}

// 处死玩家并广播（处理情侣殉情、猎人开枪，均用队列逐个结算，
// 避免递归过深；猎人开枪还会引入新的待处死玩家）。
void KillPlayer(int slot, const string& cause)
{
    vector<pair<int, string>> pending;
    pending.push_back({ slot, cause });

    while (!pending.empty())
    {
        int s = pending.back().first;
        string c = pending.back().second;
        pending.pop_back();

        if (s < 1 || s > g_numPlayers) continue;

        if (!g_players[s].alive) continue;

        g_players[s].alive = false;
        Log("KillPlayer slot=" + to_string(s) + " cause=" + c);
        SendToAllL10nPair(DeathText(s, c, Lang::Zh), DeathText(s, c, Lang::En));

        // 死亡摘要入库供 NPC 决策使用（公开线索；死因映射与 DeathText 一致）
        {
            string how;

            if (c == "wolf") how = "被狼人击杀";
            else if (c == "poison") how = "被女巫毒杀";
            else if (c == "exile") how = "被放逐";
            else if (c == "bomb") how = "被白狼王自爆带走";
            else if (c == "bomber") how = "白狼王自爆身亡";
            else if (c == "lover") how = "因情侣殉情";
            else if (c == "hunter") how = "被猎人开枪带走";
            else how = "死亡";

            g_deathNotes.push_back(to_string(s) + "号" + g_players[s].name + "（" + JOBS[g_players[s].jobId].zhName + "）已死亡，" + how + "。");
        }

        // 仅狼刀/白狼王自爆致死时提示死者本人：放逐（遗言流程已有提示）、
        // 猎人/毒/殉情等路径不重复发；死者白天静默是规则，但客户端门控全程
        // 关闭会让玩家误以为"卡死"，需先告知已死亡可观看（需求 §14.1）
        if (c == "wolf" || c == "bomber" || c == "bomb")
        {
            SendToClientL10n(s, "你已死亡，白天无法发言，可观看局势。", "You died. You cannot speak during the day, but you can watch the game.");
        }

        // 猎人在放逐/被刀/被炸死时可以开枪，毒死不行（JOBS[4]）
        if (g_players[s].jobId == 4 && !g_players[s].hunterShootUsed && c != "poison")
        {
            int target = 0;

            if (AskHunterShot(s, target))
            {
                if (target > 0)
                {
                    g_players[s].hunterShootUsed = true;
                    SendToAllL10n("猎人%s开枪带走了%s！", "Hunter %s shot %s!", g_players[s].name.c_str(), g_players[target].name.c_str());
                    pending.push_back({ target, "hunter" });
                }
            }
            else
            {
                // 猎人无法输入（断线中止）：由调用方检查 g_gameAborted
                return;
            }
        }

        // 情侣一方死亡，另一方殉情
        if (s == g_loverA || s == g_loverB)
        {
            int partner = (s == g_loverA) ? g_loverB : g_loverA;

            if (partner >= 1 && partner <= g_numPlayers && g_players[partner].alive)
            {
                pending.push_back({ partner, "lover" });
            }
        }
    }
}

// ============ 身份分配 ============

// 构建职业池（总数必须等于人数 N，不足补村民；村民仅在开关开启时可用）。
// 狼 W：level>=2 时含 1 个白狼王，其余为普通狼人；level<2 全普通狼人。
// 中立 N：丘比特/盗贼仅 level>=2 启用，随机取（最多 2 个）。
// 神 G：预言家/女巫/猎人恒有，守卫/白痴仅 level>=1，每种最多 1 个。
vector<int> BuildJobPool()
{
    vector<int> pool;

    // 狼人部分
    if (g_wolfCount >= 1)
    {
        if (g_level >= 2)
        {
            pool.push_back(1); // 白狼王
        }

        int wolves = (g_level >= 2) ? g_wolfCount - 1 : g_wolfCount;

        for (int i = 0; i < wolves; ++i) pool.push_back(0);
    }

    // 中立部分：level>=2 才有候选，随机洗牌后取前 N 个
    vector<int> neutCand;

    if (g_level >= 2)
    {
        neutCand.push_back(7); // 丘比特
        neutCand.push_back(8); // 盗贼
    }

    random_shuffle(neutCand.begin(), neutCand.end());

    int neut = min(g_neutralCount, (int)neutCand.size());

    for (int i = 0; i < neut; ++i) pool.push_back(neutCand[i]);

    // 神职部分：每种最多 1 个，按固定顺序取
    vector<int> godCand;
    godCand.push_back(2); // 预言家
    godCand.push_back(3); // 女巫
    godCand.push_back(4); // 猎人

    if (g_level >= 1)
    {
        godCand.push_back(5); // 守卫
        godCand.push_back(6); // 白痴
    }

    int gods = min(g_godCount, (int)godCand.size());

    for (int i = 0; i < gods; ++i) pool.push_back(godCand[i]);

    // 防御：池子超长（比例与人数不符时）截断，避免数组越界；
    // 不足则补村民。村民开关关闭时仍补村民只为保住本局能开——
    // 正常流程下 Start.exe 会在开局前校验比例合法性。
    if ((int)pool.size() > g_numPlayers) pool.resize(g_numPlayers);

    while ((int)pool.size() < g_numPlayers) pool.push_back(9);

    return pool;
}

// 洗牌随机发身份；每个玩家获得 ROLE|<enName> 私信
void AssignRoles()
{
    // 身份介绍要从干净屏幕开始显示，先让所有客户端清屏一次
    SendToAll("__CLS__");

    vector<int> pool = BuildJobPool();
    random_shuffle(pool.begin(), pool.end());

    for (int i = 1; i <= g_numPlayers; ++i)
    {
        g_players[i].jobId = pool[i - 1];
        g_players[i].alive = true;
        SendToClient(i - 1, "ROLE|" + string(JOBS[g_players[i].jobId].enName));

        // 身份介绍保持完整（与 HELP 同源，属既有验收项），按玩家语言选 detail/detailEn
        SendToClientL10nPair(i,
            "你的身份是：" + string(JOBS[g_players[i].jobId].zhName) + "。" + JOBS[g_players[i].jobId].detail,
            "Your role is: " + string(JOBS[g_players[i].jobId].enName) + ". " + JOBS[g_players[i].jobId].detailEn);
        Log("Slot " + to_string(i) + " (" + g_players[i].name + ") = " + JOBS[g_players[i].jobId].zhName);
    }
}

// 丘比特选情侣（2 个槽位，可含自己）
bool SetupCupid()
{
    int cupid = FindAliveJob(7);

    if (cupid < 0) return true;

    // NPC 丘比特：决策模块无情侣动作行，服务端按本地默认随机结一对，
    // 否则开局会永远卡在等输入上（需求 §19.7 只约定夜晚/白天动作行）
    if (IsNpc(cupid))
    {
        vector<int> cand = AliveTargetList(0);

        if ((int)cand.size() < 2)
        {
            g_loverA = -1;
            g_loverB = -1;
            return true;
        }

        random_shuffle(cand.begin(), cand.end());
        g_loverA = cand[0];
        g_loverB = cand[1];
        SendToAllL10n("丘比特为玩家%s 与玩家%s 结成了情侣。", "Cupid paired %s with %s as lovers.", g_players[g_loverA].name.c_str(), g_players[g_loverB].name.c_str());
        return true;
    }

    while (true)
    {
        string in;

        if (!AskChoiceL10n(cupid - 1, "你是丘比特，请输入两个槽位编号（空格分隔，可含自己）：", "You are Cupid. Enter two slot numbers (space separated, may include yourself):", in)) return false;

        vector<string> tok = SplitTokens(in);

        if (tok.size() != 2)
        {
            SendToClientL10n(cupid, "输入格式不对：请用空格分隔两个槽位编号。", "Wrong format: two slot numbers separated by a space.");
            continue;
        }

        int a = atoi(tok[0].c_str());
        int b = atoi(tok[1].c_str());

        if (a < 1 || a > g_numPlayers || b < 1 || b > g_numPlayers || a == b)
        {
            SendToClientL10n(cupid, "槽位不合法：必须是两个不同的 1..N 编号。", "Invalid slots: two different numbers from 1..N.");
            continue;
        }

        g_loverA = a;
        g_loverB = b;
        SendToAllL10n("丘比特为玩家%s 与玩家%s 结成了情侣。", "Cupid paired %s with %s as lovers.", g_players[a].name.c_str(), g_players[b].name.c_str());
        return true;
    }
}

// 盗贼从 2 张额外随机卡（可含狼）二选一改职业
bool SetupThief()
{
    int thief = FindAliveJob(8);

    if (thief < 0) return true;

    // NPC 盗贼：决策模块无盗贼动作行，按本地默认保持原身份（不换牌），
    // 否则开局会永远卡在等输入上
    if (IsNpc(thief))
    {
        SendToAllL10nPair(
            "盗贼" + g_players[thief].name + " 选择保持原身份。",
            "Thief " + g_players[thief].name + " kept the original role.");
        return true;
    }

    // 从全职业表随机抽两张（排除盗贼自己），保证"可含狼"
    vector<int> cand;

    for (int j = 0; j < JOB_COUNT; ++j)
    {
        if (JOBS[j].id != 8) cand.push_back(JOBS[j].id);
    }

    random_shuffle(cand.begin(), cand.end());

    int cardA = cand[0];
    int cardB = cand[1];

    while (true)
    {
        string in;

        // 身份卡名按语言取词，占位不共用，走预渲染成对提示
        if (!AskChoicePair(thief - 1,
            "你是盗贼，两张身份卡：<1> " + string(JOBS[cardA].zhName) + " <2> " + string(JOBS[cardB].zhName) + "，请选择 1 或 2：",
            "You are the Thief. Two cards: <1> " + string(JOBS[cardA].enName) + " <2> " + string(JOBS[cardB].enName) + ". Choose 1 or 2:",
            in)) return false;

        int pick = atoi(in.c_str());

        if (pick != 1 && pick != 2)
        {
            SendToClientL10n(thief, "请输入 1 或 2。", "Enter 1 or 2.");
            continue;
        }

        int chosen = (pick == 1) ? cardA : cardB;

        g_players[thief].jobId = chosen;
        SendToClientL10nPair(thief,
            "你选择了：" + string(JOBS[chosen].zhName) + "。",
            "You chose: " + string(JOBS[chosen].enName) + ".");
        SendToAllL10nPair(
            "盗贼" + g_players[thief].name + " 选择了新身份：" + string(JOBS[chosen].zhName) + "。",
            "Thief " + g_players[thief].name + " chose a new role: " + string(JOBS[chosen].enName) + ".");
        return true;
    }
}

// ============ 夜晚阶段 ============

// 守卫守护：1..N 或 0；不可连续两晚守护同一人
int AskGuard(int guardSlot)
{
    // NPC 守卫：即时生成 NIGHT_GUARD|i（-1 不守），校验后直接生效，
    // 不进入真人输入循环；非法目标（含连续两晚守同一人）按不守处理
    if (IsNpc(guardSlot))
    {
        string extra = "前一夜你的守护对象：";

        if (g_players[guardSlot].guardLast >= 1) extra += to_string(g_players[guardSlot].guardLast) + "号" + g_players[g_players[guardSlot].guardLast].name;
        else extra += "无（无人）";

        int t = ParseNpcTarget(
            NpcGetAction(guardSlot, "night_guard", AliveTargetList(0), extra),
            "NIGHT_GUARD");

        if (t == -1) t = 0;

        if (!(t == 0 || (t >= 1 && t <= g_numPlayers && g_players[t].alive
            && t != g_players[guardSlot].guardLast)))
        {
            t = 0;
        }

        g_players[guardSlot].guardLast = t;
        GLog("NPC guard " + to_string(guardSlot) + " guards " + to_string(t));
        return t;
    }

    while (true)
    {
        string in;

        if (!AskChoiceL10n(guardSlot - 1,
            "你是守卫，请输入守护目标（1..N，0 不守；\n不可连续两晚守同一人）：",
            "You are the Guard. Enter a target (1..N, 0 = none;\ncannot guard the same player twice in a row):", in)) return -2;

        int t = atoi(in.c_str());

        if (t == 0) return 0;

        if (t >= 1 && t <= g_numPlayers && g_players[t].alive)
        {
            if (t == g_players[guardSlot].guardLast)
            {
                SendToClientL10n(guardSlot, "不能连续两晚守护同一人，请重新输入。", "You cannot guard the same player twice in a row. Try again.");
                continue;
            }

            return t;
        }

        SendToClientL10n(guardSlot, "目标不合法：必须是 1..N 的存活玩家或 0。", "Invalid target: an alive player 1..N or 0.");
    }
}

// 狼人归票：每存活狼输入一个目标，票多者被刀；平票随机
int AskWolfTarget()
{
    vector<int> wolves;

    for (int i = 1; i <= g_numPlayers; ++i)
    {
        if (g_players[i].alive && IsWolfCamp(g_players[i].jobId)) wolves.push_back(i);
    }

    if (wolves.empty()) return 0;

    vector<int> votes;

    for (int w : wolves)
    {
        // NPC 狼：即时生成 NIGHT_KILL|i（每狼一票），非法目标按弃票跳过，
        // 不进入真人输入循环（需求 §19.7：夜晚阶段不等 NPC 输入）
        if (IsNpc(w))
        {
            int t = ParseNpcTarget(
                NpcGetAction(w, "night_kill", AliveTargetList(w), ""),
                "NIGHT_KILL");

            if (t >= 1 && t <= g_numPlayers && g_players[t].alive && t != w)
            {
                votes.push_back(t);
            }

            continue;
        }

        while (true)
        {
            string in;

            if (!AskChoiceL10n(w - 1, "你是狼人，请输入今晚刀杀目标编号（1..N，不能是自己）：", "You are a wolf. Enter tonight's kill target (1..N, not yourself):", in)) return -2;

            int t = atoi(in.c_str());

            if (t >= 1 && t <= g_numPlayers && g_players[t].alive && t != w)
            {
                votes.push_back(t);
                break;
            }

            SendToClientL10n(w, "目标不合法：必须是 1..N 的存活玩家且不能是自己。", "Invalid target: an alive player 1..N, not yourself.");
        }
    }

    // 统计票数
    vector<int> cnt(g_numPlayers + 1, 0);

    for (int v : votes) cnt[v]++;

    int maxV = 0;

    for (int i = 1; i <= g_numPlayers; ++i) maxV = max(maxV, cnt[i]);

    vector<int> top;

    for (int i = 1; i <= g_numPlayers; ++i)
    {
        if (cnt[i] == maxV) top.push_back(i);
    }

    return top[rand() % top.size()];
}

// 预言家验人：结果只发本人
bool AskSeer()
{
    int seer = FindAliveJob(2);

    if (seer < 0) return true;

    int t = -1;

    // NPC 预言家：即时生成 NIGHT_CHECK|i 并直接出结果；
    // 非法目标回退验第一个非自己的存活玩家（防御性兜底，不等输入）
    if (IsNpc(seer))
    {
        t = ParseNpcTarget(
            NpcGetAction(seer, "night_check", AliveTargetList(0), ""),
            "NIGHT_CHECK");

        if (!(t >= 1 && t <= g_numPlayers && g_players[t].alive))
        {
            vector<int> cand = AliveTargetList(seer);

            if (cand.empty()) return true;

            t = cand[0];
        }
    }
    else
    {
        while (true)
        {
            string in;

            if (!AskChoiceL10n(seer - 1, "你是预言家，请输入要查验的玩家编号（1..N）：", "You are the Seer. Enter a player to check (1..N):", in)) return false;

            t = atoi(in.c_str());

            if (t >= 1 && t <= g_numPlayers && g_players[t].alive) break;

            SendToClientL10n(seer, "目标不合法：必须是 1..N 的存活玩家。", "Invalid target: an alive player 1..N.");
        }
    }

    // RESULT| 前缀是协议不动，正文按玩家语言渲染
    SendToClientL10nPair(seer,
        "RESULT|" + g_players[t].name + "是" + CampLabel(g_players[t].jobId, Lang::Zh) + "。",
        "RESULT|" + g_players[t].name + " is " + CampLabel(g_players[t].jobId, Lang::En) + ".");

    // 验人结果入库供预言家 NPC 后续决策（只对预言家身份可见，需求 §19.7）
    g_seerCheckNotes.push_back("第" + to_string(g_night) + "夜查验" + to_string(t) + "号" + g_players[t].name
        + "：" + CampLabel(g_players[t].jobId, Lang::Zh) + "。");
    return true;
}

// 女巫：先救（0=不用，首夜可自救）后毒（0/任意存活玩家）
// 解药毒药各限一次，同一夜不可双用
// wolfTarget 仅用于 NPC 决策上下文（女巫本应知道当夜被刀者），真人路径不变
bool AskWitch(int wolfTarget, int& saveTarget, int& poisonTarget)
{
    saveTarget = 0;
    poisonTarget = 0;

    int witch = FindAliveJob(3);

    if (witch < 0) return true;

    bool saved = false;

    // 解药
    if (!g_players[witch].witchSaveUsed)
    {
        // NPC 女巫：即时生成 NIGHT_SAVE|i（-1 不用解药），非法目标按不用处理
        if (IsNpc(witch))
        {
            string extra;

            if (wolfTarget >= 1 && wolfTarget <= g_numPlayers)
            {
                extra = "今夜被狼人击杀的玩家是" + to_string(wolfTarget) + "号" + g_players[wolfTarget].name;
            }
            else
            {
                extra = "今夜无人被狼人击杀";
            }

            int t = ParseNpcTarget(
                NpcGetAction(witch, "night_save", AliveTargetList(0), extra),
                "NIGHT_SAVE");

            if (t == -1) t = 0;

            if (t == 0 || !(t >= 1 && t <= g_numPlayers && g_players[t].alive
                && (t != witch || g_night == 1)))
            {
                t = 0;
            }

            if (t >= 1)
            {
                saveTarget = t;
                g_players[witch].witchSaveUsed = true;
                saved = true;
            }
        }
        else
        {
            while (true)
            {
                string in;

                if (!AskChoiceL10n(witch - 1,
                    "你是女巫，是否使用解药？\n（输入目标编号，首夜可自救；0 不使用）：",
                    "You are the Witch. Use the antidote?\n(enter a target, self-save only on night 1; 0 = no):", in)) return false;

                int t = atoi(in.c_str());

                if (t == 0) break;

                if (t >= 1 && t <= g_numPlayers && g_players[t].alive)
                {
                    if (t == witch && g_night > 1)
                    {
                        SendToClientL10n(witch, "只有首夜可以自救，请重新输入。", "You can only save yourself on night 1. Try again.");
                        continue;
                    }

                    saveTarget = t;
                    g_players[witch].witchSaveUsed = true;
                    saved = true;
                    break;
                }

                SendToClientL10n(witch, "目标不合法：必须是 1..N 的存活玩家或 0。", "Invalid target: an alive player 1..N or 0.");
            }
        }
    }

    // 同一夜不可双用：已用解药则不再给毒药
    if (saved) return true;

    // 毒药
    if (!g_players[witch].witchPoisonUsed)
    {
        // NPC 女巫：即时生成 NIGHT_POISON|i（-1 不用毒），非法目标按不用处理
        if (IsNpc(witch))
        {
            int t = ParseNpcTarget(
                NpcGetAction(witch, "night_poison", AliveTargetList(witch), ""),
                "NIGHT_POISON");

            if (t == -1) t = 0;

            if (!(t == 0 || (t >= 1 && t <= g_numPlayers && g_players[t].alive)))
            {
                t = 0;
            }

            if (t >= 1)
            {
                poisonTarget = t;
                g_players[witch].witchPoisonUsed = true;
            }
        }
        else
        {
            while (true)
            {
                string in;

                if (!AskChoiceL10n(witch - 1,
                    "你是女巫，是否使用毒药？（输入目标编号；0 不使用）：",
                    "You are the Witch. Use the poison? (enter a target; 0 = no):", in)) return false;

                int t = atoi(in.c_str());

                if (t == 0) break;

                if (t >= 1 && t <= g_numPlayers && g_players[t].alive)
                {
                    poisonTarget = t;
                    g_players[witch].witchPoisonUsed = true;
                    break;
                }

                SendToClientL10n(witch, "目标不合法：必须是 1..N 的存活玩家或 0。", "Invalid target: an alive player 1..N or 0.");
            }
        }
    }

    return true;
}

// 夜晚结算：守卫免刀 > 女巫救 > 狼刀 > 女巫毒；守+救冲突判死
// 返回 false 表示应中止游戏
bool ResolveNight(int guardTarget, int wolfTarget, int saveTarget, int poisonTarget)
{
    vector<pair<int, string>> deaths;

    if (wolfTarget >= 1)
    {
        bool guarded = (guardTarget == wolfTarget);
        bool saved = (saveTarget == wolfTarget);

        if (guarded && saved)
        {
            // 守救冲突：判死（JOBS[5] 说明）
            deaths.push_back({ wolfTarget, "wolf" });
        }
        else if (!guarded && !saved)
        {
            deaths.push_back({ wolfTarget, "wolf" });
        }
    }

    if (poisonTarget >= 1)
    {
        deaths.push_back({ poisonTarget, "poison" });
    }

    if (deaths.empty())
    {
        SendToAllL10n("平安夜，昨夜无人死亡。", "Peaceful night. No one died.");
        return true;
    }

    for (auto& d : deaths)
    {
        KillPlayer(d.first, d.second);
    }

    return true;
}

// 夜晚阶段完整流程
bool NightPhase()
{
    ++g_night;
    GLog("NightPhase entry: night=" + to_string(g_night));
    SendToAllL10n("天黑请闭眼，开始今夜行动。", "Night falls. Night actions begin.");
    Log("Night " + to_string(g_night) + " begins");

    if (!EnsureAliveConnected()) return false;

    // 守卫
    int guardTarget = 0;
    int guardSlot = FindAliveJob(5);

    if (guardSlot >= 0)
    {
        GLog("NightPhase: asking guard slot " + to_string(guardSlot));
        SendToAllL10n("守卫请睁眼。", "Guard, open your eyes.");
        guardTarget = AskGuard(guardSlot);

        if (guardTarget == -2) return false;

        g_players[guardSlot].guardLast = guardTarget;
    }

    // 狼人归票
    GLog("NightPhase: asking wolves");
    SendToAllL10n("狼人请睁眼。", "Werewolves, open your eyes.");
    int wolfTarget = AskWolfTarget();

    if (wolfTarget == -2) return false;

    GLog("NightPhase: wolfTarget=" + to_string(wolfTarget));

    // 狼队刀人记录入库供狼阵营 NPC 后续决策（只对狼阵营可见，需求 §19.7）
    if (wolfTarget >= 1 && wolfTarget <= g_numPlayers)
    {
        g_wolfKillNotes.push_back("第" + to_string(g_night) + "夜刀杀" + to_string(wolfTarget) + "号" + g_players[wolfTarget].name + "。");
    }

    // 预言家（职业不在场则不广播该段开场）
    if (FindAliveJob(2) >= 0)
    {
        SendToAllL10n("预言家请睁眼。", "Seer, open your eyes.");
    }

    if (!AskSeer()) return false;

    // 女巫（职业不在场则不广播该段开场）
    int saveTarget = 0;
    int poisonTarget = 0;

    if (FindAliveJob(3) >= 0)
    {
        SendToAllL10n("女巫请睁眼。", "Witch, open your eyes.");
    }

    if (!AskWitch(wolfTarget, saveTarget, poisonTarget)) return false;

    // 结算
    SendToAllL10n("天亮了……", "Dawn...");

    if (!ResolveNight(guardTarget, wolfTarget, saveTarget, poisonTarget)) return false;

    return true;
}

// ============ 白天阶段 ============

// 解析投票命令：VOTE n / 投票 n / V n / VOTE|n / 裸数字 n（V/v 为短别名，需求 §12.5）
bool ParseVoteCommand(const string& content, int& target)
{
    string line = content;

    for (char& ch : line)
    {
        if (ch == '|') ch = ' ';
    }

    vector<string> tok = SplitTokens(line);

    if (tok.empty()) return false;

    int numIdx = -1;

    if (tok[0] == "VOTE" || tok[0] == "vote" || tok[0] == "Vote" || tok[0] == "投票"
        || tok[0] == "V" || tok[0] == "v")
    {
        if (tok.size() < 2) return false;

        numIdx = 1;
    }
    else if (tok.size() == 1 && !tok[0].empty())
    {
        bool allDigits = true;

        for (char c : tok[0])
        {
            if (!isdigit((unsigned char)c))
            {
                allDigits = false;
                break;
            }
        }

        if (allDigits) numIdx = 0;
    }

    if (numIdx < 0) return false;

    target = atoi(tok[numIdx].c_str());
    return true;
}

// 解析白狼王自爆命令：BOMB n / 自爆 n / B n / BOMB|n（B/b 为短别名，需求 §12.5）
bool ParseBombCommand(const string& content, int& target)
{
    string line = content;

    for (char& ch : line)
    {
        if (ch == '|') ch = ' ';
    }

    vector<string> tok = SplitTokens(line);

    if (tok.empty()) return false;

    if (tok[0] != "BOMB" && tok[0] != "bomb" && tok[0] != "Bomb" && tok[0] != "自爆"
        && tok[0] != "B" && tok[0] != "b") return false;

    if (tok.size() < 2) return false;

    target = atoi(tok[1].c_str());
    return true;
}

// 收集白天投票与聊天；白狼王可随时自爆。
// 窗口最长 DAY_VOTE_TIMEOUT_SECONDS 秒，到期后未投票的存活玩家自动弃权；
// 每 10 秒给所有存活玩家重发 __DAY_OPEN__（防单播窗口指令丢失导致失声；
// 已投票玩家窗口本就常开，重复开窗幂等无害，需求 §14.1）。
// 返回 false 表示应中止游戏。bombTarget>0 表示白狼王自爆（跳过放逐）。
bool GatherDayVotes(int& exiled, int& bombTarget)
{
    exiled = -1;
    bombTarget = 0;

    // 重置投票状态
    for (int i = 1; i <= g_numPlayers; ++i)
    {
        g_players[i].voteTarget = -1;
    }

    int remaining = 0;

    for (int i = 1; i <= g_numPlayers; ++i)
    {
        // 已翻牌的白痴失去投票权
        if (g_players[i].alive && !(g_players[i].jobId == 6 && g_players[i].idiotFlipped)) ++remaining;
    }

    // NPC 投票：到点即投（需求 §19.7：不等真人、不占窗口）。在投票窗口
    // 与 deadline 建立之前一次性投出——在线模型再慢也不吃掉真人的窗口。
    // 广播文案与真人投票完全一致，计票走同一数据结构
    for (int i = 1; i <= g_numPlayers; ++i)
    {
        if (!g_players[i].alive || !IsNpc(i)) continue;
        if (g_players[i].jobId == 6 && g_players[i].idiotFlipped) continue;

        int v = ParseNpcTarget(NpcGetAction(i, "day_vote", AliveTargetList(0), ""), "VOTE");

        if (!(v == 0 || v == -1 || (v >= 1 && v <= g_numPlayers && g_players[v].alive)))
        {
            v = 0;
        }

        if (v == -1) v = 0;

        g_players[i].voteTarget = v;

        if (v == 0) SendToAllL10n("玩家%s 弃权。", "Player %s abstained.", g_players[i].name.c_str());
        else SendToAllL10n("玩家%s 投票给了玩家%s（槽%d）。", "Player %s voted for player %s (slot %d).", g_players[i].name.c_str(), g_players[v].name.c_str(), v);

        --remaining;
    }

    auto deadline = chrono::steady_clock::now() + chrono::seconds(DAY_VOTE_TIMEOUT_SECONDS);
    auto lastResend = chrono::steady_clock::now();

    while (remaining > 0)
    {
        auto now = chrono::steady_clock::now();

        // 投票超时：到期后尚未投票的存活玩家一律按弃权处理，白天不会卡死（需求 §12.4）
        if (now >= deadline)
        {
            for (int i = 1; i <= g_numPlayers; ++i)
            {
                if (g_players[i].alive && g_players[i].voteTarget < 0
                    && !(g_players[i].jobId == 6 && g_players[i].idiotFlipped))
                {
                    g_players[i].voteTarget = 0;
                    SendToAllL10n("玩家%s 超时未投票，按弃权处理。", "Player %s timed out and abstained.", g_players[i].name.c_str());
                }
            }

            remaining = 0;
            break;
        }

        // 兜底：每 10 秒对所有存活玩家重发对话窗口指令（需求 §14.1：
        // 已投票玩家窗口本就不关，重复开窗幂等，消除任何强制关窗路径的失声缺口）
        if (now - lastResend >= chrono::seconds(10))
        {
            lastResend = now;

            for (int i = 1; i <= g_numPlayers; ++i)
            {
                if (g_players[i].alive)
                {
                    SendToClient(i - 1, "__DAY_OPEN__");
                }
            }
        }

        string msg;

        // 带超时的队列等待：PING 不进队列，静默期也要周期性醒来复查投票死线，
        // 否则白天会因 PollAllForMessage 永久阻塞而挂死（需求 §12.4，2026-08-05 实测）
        int pollRc = PollAllForMessageTimed(msg, 100);

        if (pollRc < 0) return false;
        if (pollRc == 0) continue;

        int dis = ParseDisconnectSlot(msg);

        if (dis >= 0)
        {
            // 已死玩家的断线不影响本局
            if (g_players[dis + 1].alive)
            {
                if (!WaitForReconnect(dis)) return false;

                // 白天仍在进行：给重连者补开窗口并广播提示（需求 §12.4）
                SendToAllL10n("白天仍在进行，请继续发言或投票（VOTE <编号>/投票 <编号>，0 弃权）。", "Day is still in progress. Keep talking or vote (VOTE <n>, 0 to abstain).");
                SendToClient(dis, "__DAY_OPEN__");
            }

            continue;
        }

        int from;
        string content;

        if (!ParseClientMsg(msg, from, content)) continue;

        int slot = from + 1;

        if (!g_players[slot].alive) continue;

        // 白狼王自爆（只限白狼王本人在白天发动）
        if (g_players[slot].jobId == 1)
        {
            int bt = -1;

            if (ParseBombCommand(content, bt))
            {
                if (bt >= 1 && bt <= g_numPlayers && g_players[bt].alive && bt != slot)
                {
                    SendToAllL10n("白狼王%s自爆，带走了玩家%s！", "White Wolf King %s detonated, taking player %s!", g_players[slot].name.c_str(), g_players[bt].name.c_str());

                    KillPlayer(slot, "bomber");
                    KillPlayer(bt, "bomb");

                    bombTarget = bt;
                    return true;
                }

                SendToClientL10n(from + 1, "自爆目标不合法：必须是 1..N 的存活玩家且不能是自己。请重新输入。", "Invalid bomb target: an alive player 1..N, not yourself. Try again.");
                continue;
            }
        }

        int vote = -1;

        if (ParseVoteCommand(content, vote))
        {
            bool valid = (vote == 0) || (vote >= 1 && vote <= g_numPlayers && g_players[vote].alive);

            if (!valid)
            {
                SendToClientL10n(from + 1, "投票目标不合法：必须是 1..N 的存活玩家或 0。请重新输入。", "Invalid vote: an alive player 1..N or 0. Try again.");
                continue;
            }

            // 已投票的玩家重复投票直接忽略
            if (g_players[slot].voteTarget >= 0)
            {
                SendToClientL10n(from + 1, "你已经投过票了。", "You already voted.");
                continue;
            }

            g_players[slot].voteTarget = vote;

            if (vote == 0) SendToAllL10n("玩家%s 弃权。", "Player %s abstained.", g_players[slot].name.c_str());
            else SendToAllL10n("玩家%s 投票给了玩家%s（槽%d）。", "Player %s voted for player %s (slot %d).", g_players[slot].name.c_str(), g_players[vote].name.c_str(), vote);

            --remaining;
        }
        else
        {
            // 非投票内容一律视为聊天广播（玩家自己的内容原样透传，不翻译）
            SendToAll(g_players[slot].name + "：" + SanitizeChat(content));
        }
    }

    // 计票：票多者放逐，平票无人放逐
    vector<int> cnt(g_numPlayers + 1, 0);

    for (int i = 1; i <= g_numPlayers; ++i)
    {
        if (g_players[i].voteTarget >= 1) cnt[g_players[i].voteTarget]++;
    }

    int maxV = 0;

    for (int i = 1; i <= g_numPlayers; ++i) maxV = max(maxV, cnt[i]);

    vector<int> top;

    for (int i = 1; i <= g_numPlayers; ++i)
    {
        if (cnt[i] == maxV) top.push_back(i);
    }

    if (maxV == 0)
    {
        SendToAllL10n("无人得票，白天无人被放逐。", "No votes cast. Nobody is exiled.");
    }
    else if (top.size() > 1)
    {
        SendToAllL10n("最高票平票，白天无人被放逐。", "Tie on the highest votes. Nobody is exiled.");
    }
    else
    {
        exiled = top[0];
        SendToAllL10n("玩家%s（槽%d）被放逐。", "Player %s (slot %d) was exiled.", g_players[exiled].name.c_str(), exiled);
    }

    return true;
}

// 处理放逐结果：白痴翻牌免死；否则处死
void ResolveExile(int exiled)
{
    if (exiled < 1) return;

    if (g_players[exiled].jobId == 6 && !g_players[exiled].idiotFlipped)
    {
        g_players[exiled].idiotFlipped = true;
        SendToAllL10n("白痴%s 翻牌免死，继续存活但失去投票权。", "Idiot %s flipped and survived, but lost voting rights.", g_players[exiled].name.c_str());
        return;
    }

    KillPlayer(exiled, "exile");
}

// 被放逐者遗言：等待最多 LASTWORD_TIMEOUT_SECONDS 秒。
// 不能用 PollAllForMessage（它无超时会无限等下去），用带 deadline 的局部轮询；
// 断线事件转重连等待，放逐者本人重连后重发提示开窗；全部玩家失联中止游戏。
bool AskLastWords(int slot)
{
    if (slot < 1 || slot > g_numPlayers) return true;

    // NPC 遗言：死亡瞬间即时生成并广播（SPEECH|内容），不进入 10 秒等待
    if (IsNpc(slot))
    {
        string content = ParseNpcSpeech(NpcGetAction(slot, "lastword", vector<int>(), ""));

        if (!content.empty())
        {
            SendToAllL10n("遗言：%s", "Last words: %s", SanitizeChat(content).c_str());
        }

        return true;
    }

    SendToClientL10nPair(slot,
        "你被放逐了。请留下你的遗言（直接输入内容，直接回车放弃）：",
        "You were exiled. Leave your last words (type text, empty line to skip):");
    SendToClient(slot - 1, "__INPUT__");

    auto deadline = chrono::steady_clock::now() + chrono::seconds(LASTWORD_TIMEOUT_SECONDS);

    while (true)
    {
        if (chrono::steady_clock::now() >= deadline) return true;

        string msg;

        {
            lock_guard<mutex> lock(g_mutex);

            if (!g_msgQueue.empty())
            {
                msg = g_msgQueue.front();
                g_msgQueue.pop();
            }
            else
            {
                // 持锁期间直接查 g_connected：调 AnyConnected() 会递归锁抛 resource_deadlock
                bool anyOnline = false;

                for (int i = 0; i < g_numPlayers; ++i)
                {
                    if (g_connected[i])
                    {
                        anyOnline = true;
                        break;
                    }
                }

                if (!anyOnline) return false;
            }
        }

        if (msg.empty())
        {
            Sleep(100);
            continue;
        }

        int dis = ParseDisconnectSlot(msg);

        if (dis >= 0)
        {
            // 遗言只是可选的演出：投票早已收齐，任何玩家断线都不值得
            // 触发 25 秒重连等待甚至中止整局。放逐者本人断线直接跳过遗言，
            // 其他玩家断线忽略（夜晚行动阶段另有重连机制接管）。
            if (dis == slot - 1) return true;

            continue;
        }

        int from;
        string content;

        // 只认放逐者本人的输入；其他玩家的排队消息一律忽略
        if (ParseClientMsg(msg, from, content) && from == slot - 1)
        {
            if (content.empty()) return true;

            // 遗言内容属玩家自己的话，原样透传不翻译
            SendToAllL10n("遗言：%s", "Last words: %s", SanitizeChat(content).c_str());
            return true;
        }
    }
}

// 白天阶段完整流程
bool DayPhase()
{
    // 白天开场提示：全量精简定稿，中文短句 + 英文短句（需求 §12.6）
    SendToAllL10n(
        "白天发言阶段。\n可随意聊天，投票用 VOTE n（0 弃权）。\n全部投完票后进入放逐。",
        "Day phase.\nTalk freely; vote with VOTE n (0 abstain).\nExile comes after everyone votes.");

    // 白狼王的自爆能力只私发给白狼王本人，普通玩家看不到这条提示
    int bombWolf = FindAliveJob(1);

    if (bombWolf >= 0)
    {
        SendToClientL10n(bombWolf, "你是白狼王，可输入 BOMB n / 自爆 n 带走一人。", "You are the White Wolf King. BOMB n takes one player.");
    }

    // 白天窗口标志先置位：重连分支（接受线程）据此补发 __DAY_OPEN__（需求 §12.4）
    g_dayVoting = true;

    // 开窗指令必须在全部提示之后单独发，且只给存活玩家开窗；
    // 若与提示同批发送，客户端打印提示期间输入门尚未打开，先键入的字符会丢失
    for (int i = 1; i <= g_numPlayers; ++i)
    {
        if (g_players[i].alive) SendToClient(i - 1, "__DAY_OPEN__");
    }

    // 白天发言：每个存活 NPC 至少发 1 次言（需求 §19.7），阶段开始时依序
    // 立即生成并广播，走真人聊天同渠道（名字：内容）；安排在投票窗口开启
    // 之前，在线模型的等待不占用真人的投票时间
    for (int i = 1; i <= g_numPlayers; ++i)
    {
        if (!g_players[i].alive || !IsNpc(i)) continue;

        string content = ParseNpcSpeech(NpcGetAction(i, "day_speech", vector<int>(), ""));

        if (!content.empty())
        {
            SendToAll(g_players[i].name + "：" + SanitizeChat(content));
        }
    }

    int exiled = -1;
    int bombTarget = 0;

    if (!GatherDayVotes(exiled, bombTarget))
    {
        g_dayVoting = false;
        return false;
    }

    // 统一关闭所有玩家对话窗口（含死亡玩家），遗言环节需要窗口已关
    SendToAll("__DAY_CLOSE__");
    g_dayVoting = false;

    // 白狼王自爆后直接进入夜晚（跳过放逐），但窗口已在上面关闭
    if (bombTarget > 0)
    {
        return true;
    }

    // 先记下放逐前状态：白痴翻牌免死不算"放逐亡"则不得触发遗言
    bool exiledWasAlive = (exiled >= 1 && g_players[exiled].alive);

    ResolveExile(exiled);

    // 仅放逐真死才请遗言；遗言环节经超时/中止返回 false 时整局随之停止
    if (exiledWasAlive && exiled >= 1 && !g_players[exiled].alive)
    {
        if (!AskLastWords(exiled)) return false;
    }

    return true;
}

// ============ 胜负判定 ============

// 情侣特殊胜利：情侣双活且一狼一好人，且除情侣外存活者只剩单一阵营
bool CheckLoversVictory()
{
    if (g_loverA < 1 || g_loverB < 1) return false;

    if (!g_players[g_loverA].alive || !g_players[g_loverB].alive) return false;

    int campA = JOBS[g_players[g_loverA].jobId].camp;
    int campB = JOBS[g_players[g_loverB].jobId].camp;

    bool mix = (campA == CAMP_WOLF && IsGoodCamp(g_players[g_loverB].jobId))
            || (campB == CAMP_WOLF && IsGoodCamp(g_players[g_loverA].jobId));

    if (!mix) return false;

    vector<int> othersCamp;

    for (int i = 1; i <= g_numPlayers; ++i)
    {
        if (i == g_loverA || i == g_loverB) continue;

        if (g_players[i].alive) othersCamp.push_back(JOBS[g_players[i].jobId].camp);
    }

    if (othersCamp.empty()) return true;

    int first = othersCamp[0];

    for (int c : othersCamp)
    {
        if (c != first) return false;
    }

    return true;
}

// 检查胜负：返回胜方中文名；空串表示游戏继续
string CheckVictory()
{
    if (CheckLoversVictory()) return "情侣阵营";

    int wolves = CountAliveWolf();
    int good = CountAliveGood();

    if (wolves == 0) return "好人阵营";

    if (wolves >= good) return "狼人阵营";

    return "";
}

// ============ 通知房间管理器 ============

// 游戏结束时通知房间管理器。连接失败会重试 3 次。
// 全部玩家失联（g_releaseRoom）时追加 RELEASE 让房间管理器销毁房间。
void NotifyStartGameEnded()
{
    if (g_startIp.empty() || g_startPort == 0 || g_roomId.empty()) return;

    for (int attempt = 1; attempt <= 3; ++attempt)
    {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);

        if (s == INVALID_SOCKET)
        {
            Sleep(2000);
            continue;
        }

        sockaddr_in addr;
        addr.sin_family = AF_INET;
        inet_pton(AF_INET, g_startIp.c_str(), &addr.sin_addr);
        addr.sin_port = htons(g_startPort);

        if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0)
        {
            string msg = "HELLO|START\nGAME_ENDED|" + g_roomId + "\n";

            if (g_releaseRoom) msg += "RELEASE|" + g_roomId + "\n";

            send(s, msg.c_str(), msg.length(), 0);
            closesocket(s);
            return;
        }

        closesocket(s);

        if (attempt < 3)
        {
            Log("Failed to notify room manager (attempt " + to_string(attempt) + "), retrying in 2 s");
            Sleep(2000);
        }
    }

    Log("Unable to notify room manager (room manager may be offline)");
}

// ============ 主流程 ============

// UTF-8 从宽字符转换（命令行参数含中文名时必须走宽字符路径）
string WideToUtf8Local(const wstring& w)
{
    if (w.empty()) return "";

    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);

    if (len <= 0) return "";

    string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], len, nullptr, nullptr);
    return out;
}

int main(int argc, char* argv[])
{
    DisableConsoleQuickEdit();
    SetConsoleUtf8();
    Log("狼人杀游戏服务器启动...");

    // 测试注入：环境变量 WOLF_VOTE_TIMEOUT_SECONDS（1..300 的整数）
    // 可缩短白天投票窗口，仅供自动化测试用；非法值/缺省保持 90 秒，
    // 真人游戏行为不受影响（需求 §12.8 第 4 项验收依赖短窗口）。
    if (const char* envTimeout = getenv("WOLF_VOTE_TIMEOUT_SECONDS"))
    {
        int v = atoi(envTimeout);

        if (v >= 1 && v <= 300) DAY_VOTE_TIMEOUT_SECONDS = v;
    }

    // 用宽字符命令行解析，避免 ANSI 代码页破坏中文名
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    vector<string> args;
    args.reserve(wargc);

    for (int i = 0; i < wargc; ++i)
    {
        args.push_back(WideToUtf8Local(wargv[i]));
    }

    LocalFree(wargv);

    // 参数不足直接退出（固定 10 个 + 每玩家名与语言码各 1 个，N>=2 时至少 14 个）
    if ((int)args.size() < 14)
    {
        Log("参数不足：Server.exe <gamePort> <names...> <startIp> <startPort> <roomId> <W> <N> <G> <level> <villager> <langs...>");
        cout << "\n[ Pause ]\n";
        system("pause > nul");
        return 1;
    }

    int port = atoi(args[1].c_str());

    // 中间全部是玩家名；末尾还跟 N 个语言码（与名字一一对应），总数为 10 + 2N
    g_numPlayers = ((int)args.size() - 10) / 2;

    if (g_numPlayers < 2 || g_numPlayers > MAX_PLAYERS)
    {
        Log("玩家人数不合法：" + to_string(g_numPlayers) + "（要求 2.." + to_string(MAX_PLAYERS) + "）");
        cout << "\n[ Pause ]\n";
        system("pause > nul");
        return 1;
    }

    // 尾部固定 8 个参数（位于最后 N 个语言码之前）
    int tail = (int)args.size();
    g_startIp = args[tail - g_numPlayers - 8];
    g_startPort = atoi(args[tail - g_numPlayers - 7].c_str());
    g_roomId = args[tail - g_numPlayers - 6];
    g_wolfCount = atoi(args[tail - g_numPlayers - 5].c_str());
    g_neutralCount = atoi(args[tail - g_numPlayers - 4].c_str());
    g_godCount = atoi(args[tail - g_numPlayers - 3].c_str());
    g_level = atoi(args[tail - g_numPlayers - 2].c_str());
    g_villagerSwitch = (atoi(args[tail - g_numPlayers - 1].c_str()) == 1);

    // 最后 N 个参数为每玩家语言码（槽位 1..N 与名字顺序一一对应，需求 §12.1）。
    // NPC 用 npc（在线）/npc-off（离线）代替 zh/en（需求 §19.7）：NPC 无
    // socket 收不到任何提示、发言固定中文，语言码对其只承担"类型标记"作用。
    // 先存临时数组，g_players 建表后再写入结构
    int npcType[MAX_PLAYERS + 1] = { 0 };

    for (int i = 1; i <= g_numPlayers; ++i)
    {
        string code = args[tail - g_numPlayers + i - 1];

        if (_stricmp(code.c_str(), "npc") == 0) npcType[i] = 1;
        else if (_stricmp(code.c_str(), "npc-off") == 0) npcType[i] = 2;
        else g_playerLang[i] = ParseLang(code);
    }

    // 防御：人数与三方比例兜底
    if (g_wolfCount < 0) g_wolfCount = 0;
    if (g_neutralCount < 0) g_neutralCount = 0;
    if (g_godCount < 0) g_godCount = 0;
    if (g_level < 0 || g_level > 2) g_level = 0;

    // 初始化玩家表
    g_players.assign(g_numPlayers + 1, Player());

    for (int i = 1; i <= g_numPlayers; ++i)
    {
        g_players[i].name = SanitizeName(args[1 + i]);
        g_players[i].slot = i;
        g_players[i].lang = g_playerLang[i];
        g_players[i].npcType = npcType[i];

        if (g_players[i].npcType != 0)
        {
            Log("NPC slot " + to_string(i) + " name=" + g_players[i].name
                + " type=" + (g_players[i].npcType == 1 ? "online" : "offline"));
        }
    }

    Log("端口 " + to_string(port) + "，玩家 " + to_string(g_numPlayers) + " 人，狼/中/神 = "
        + to_string(g_wolfCount) + "/" + to_string(g_neutralCount) + "/" + to_string(g_godCount)
        + "，档位 " + to_string(g_level) + "，村民开关 " + (g_villagerSwitch ? "开" : "关"));

    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        Log("WSAStartup failed");
        _getch();
        return 1;
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);

    if (listenSock == INVALID_SOCKET)
    {
        Log("socket() failed, error: " + to_string(WSAGetLastError()));
        WSACleanup();
        _getch();
        return 1;
    }

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    // 全限定 ::bind：common.h 里有 using namespace std，直接写 bind 会匹配到 std::bind
    if (::bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        Log("bind() failed on port " + to_string(port) + ", error: " + to_string(WSAGetLastError()));
        closesocket(listenSock);
        WSACleanup();
        _getch();
        return 1;
    }

    if (listen(listenSock, 5) == SOCKET_ERROR)
    {
        Log("listen() failed, error: " + to_string(WSAGetLastError()));
        closesocket(listenSock);
        WSACleanup();
        _getch();
        return 1;
    }

    Log("Listening on port " + to_string(port));

    // 监听套接字保持打开：接受线程负责首次连接与断线重连
    thread acceptThread(AcceptThreadFunc, listenSock);

    // 接收线程尽早启动，这样游戏开始前的断线也能被感知
    thread recvThread(ReceiveThreadFunc);

    // 等待所有玩家连接（超时则中止）
    WaitForGameStart();

    if (!AnyConnected())
    {
        Log("等待开局超时：无玩家连接，中止");
        g_gameAborted = true;
        g_releaseRoom = AllLost();
        NotifyStartGameEnded();
        g_serverRunning = false;
        closesocket(listenSock);
        acceptThread.join();
        recvThread.join();
        WSACleanup();
        Log("游戏服务器退出");
        cout << "\n[ Pause ]\n";
        system("pause > nul");
        return 0;
    }

    srand((unsigned)time(0));

    // 身份分配（先广播身份后处理丘比特/盗贼，让所有玩家先看到自己身份）
    AssignRoles();

    // 玩家名单广播（编号序，不含职业信息）：客户端缓存后游戏内 LIST 本地
    // 重放显示。名字已在 Start 侧按白名单净化过（不含 |），可安全拼接
    {
        string roster = "PLAYER_LIST|" + to_string(g_numPlayers);

        for (int i = 1; i <= g_numPlayers; ++i)
        {
            roster += "|" + g_players[i].name;
        }

        SendToAll(roster);
        Log("PLAYER_LIST broadcast: " + roster);
    }

    if (!SetupCupid())
    {
        g_gameAborted = true;
    }

    if (!g_gameAborted && !SetupThief())
    {
        g_gameAborted = true;
    }

    if (!g_gameAborted)
    {
        SendToAllL10n("身份已分配，天黑请闭眼。", "Roles assigned. Night falls.");
    }

    string winner;

    try
    {
    while (g_serverRunning && !g_gameAborted)
    {
        if (!EnsureAliveConnected())
        {
            g_gameAborted = true;
            break;
        }

        if (!NightPhase())
        {
            g_gameAborted = true;
            break;
        }

        winner = CheckVictory();

        if (!winner.empty()) break;

        if (!DayPhase())
        {
            g_gameAborted = true;
            break;
        }

        winner = CheckVictory();

        if (!winner.empty()) break;
    }
    }
    catch (const exception& e)
    {
        Log(string("MAIN-LOOP EXCEPTION: ") + e.what());
        g_gameAborted = true;
    }
    catch (...)
    {
        Log("MAIN-LOOP UNKNOWN EXCEPTION");
        g_gameAborted = true;
    }

    if (g_gameAborted)
    {
        SendToAllL10n("本局中止，正在返回房间...", "Game aborted. Returning to the room...");
        Log("Game aborted");
    }
    else
    {
        // CheckVictory 返回中文胜方名，按语言映射成对文本广播
        string enWinner = "Good camp";

        if (winner == "狼人阵营") enWinner = "Wolf camp";
        else if (winner == "情侣阵营") enWinner = "Lovers camp";

        SendToAllL10nPair("本局结束，胜利方：" + winner + "！", "Game over. Winner: " + enWinner + "!");
        Log("Winner: " + winner);
    }

    // 全部失联 → 释放房间；否则保留房间供玩家回房重开
    g_releaseRoom = AllLost();

    NotifyStartGameEnded();

    SendToAllL10nPair("本局结束，即将返回房间。", "Game over. Returning to the room.");

    // 终态控制行：在关闭连接前告知客户端"这是正常收尾"而非断线，
    // 客户端收到后直接走回房流程、不再对已退出的服务器做必败重连（需求 §14.2）。
    // 裸行不走 L10n 翻译；已断开/失联的连接由 SendToClient 内部容错跳过
    // （g_connected 检查 + 发送失败 MarkDisconnected）。
    for (int i = 1; i <= g_numPlayers; ++i)
    {
        SendToClient(i - 1, "__GAME_OVER__");
    }

    // 关闭监听套接字，让接受线程退出
    g_serverRunning = false;
    closesocket(listenSock);
    acceptThread.join();
    recvThread.join();

    for (int i = 0; i < g_numPlayers; ++i)
    {
        if (g_connected[i]) closesocket(g_clients[i]);
    }

    WSACleanup();
    Log("游戏服务器退出");
    cout << "\n[ Pause ]\n";
    system("pause > nul");
    return 0;
}
