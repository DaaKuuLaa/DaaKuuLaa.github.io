// Server.cpp - 游戏服务器（每局游戏一个进程，由 Start.exe 启动）
//
// 命令行参数：
//   Server.exe <gamePort> "<name1>" "<name2>" <startIp> <startPort> <roomId>
//   startIp / startPort / roomId 用于游戏结束时通知房间管理器（Start.exe）。
//
// 线程分工：
//   主线程   ：游戏逻辑（回合、道具、胜负结算）
//   接受线程 ：持续监听，处理玩家首次连接、断线重连（PLAYER_ID|N）以及
//              放弃重连（GIVEUP|N）；只读首行，同包剩余数据交还接收线程
//   接收线程 ：读取两个玩家发送的游戏消息
//
// 发送约定：SendToAll/SendToClient 持锁快照目标、锁外 send（send 可能阻塞，
// 持锁会拖死其他线程）；失败经 MarkDisconnected 回锁标记断线。
//
// 断线重连规则（与 Client.exe 约定）：
//   1. 任一玩家断线 → 进入重连等待（默认 60 秒），并告知另一方；
//   2. 断线的客户端自行尝试 3 次重连，全部失败后发送 GIVEUP|N 通知本服务器；
//   3. 收到 GIVEUP、等待超时，或另一方也断线 → 本局中止；
//   4. 若只是单个玩家失联 → 只发 GAME_ENDED（保留房间，双方回房后重新开始）；
//   5. 若双方都失联 → 追加发送 RELEASE（让房间管理器销毁该房间）。
// 开局等待（WaitForGameStart）同样受 60 秒超时约束，防止玩家永不连接时挂死。
#include "common.h"
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

// 断线后等待重连的最长时间（秒）
// 重连等待上限。客户端断线后自行重试 3 次（约 15 秒），全部失败才发
// GIVEUP 放弃。因此服务器等待 25s 已足够覆盖正常闪断重连；旧值 60s 会让
// 另一方玩家"等待对方重连"空转一整分钟，体感像是无限挂起
// （2026-08-03 修复：对方永不回连时，剩余玩家约 25s 内被送回房间）。
const int RECONNECT_TIMEOUT_SECONDS = 25;

void Log(const string& msg)
{
    string s = LogMsg("server.log", msg);
    cout << s << endl;
}

// 游戏过程观测日志：相对开局时间、毫秒精度。普通 Log 只有秒级时间戳，
// 无法分辨"几十秒的卡顿"到底发生在哪个阶段（排查 2026-08-02 第二轮后
// 服务器静默卡死时加入；排查完毕可保留，日志量很小）。
auto g_gameClock = chrono::steady_clock::now();

void GLog(const string& msg)
{
    long long ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - g_gameClock).count();
    Log("[+ " + to_string(ms) + "ms] " + msg);
}

// ============ 全局游戏状态 ============

// 道具名称（下标 1..6 对应 T[] 六种道具）
const string Prop[] = { "0", "Handcuff", "Chocolate", "Saw", "Phone", "Drinks", "Smoke" };

struct Player
{
    string name;
    int Hp;
    int Num;   // 道具总数
    int F = 0; // 第三回合是否已触发"准备好了吗"演出
    int T[7];  // 六种道具数量（下标 1..6）
} P[3];        // P[1] 玩家1，P[2] 玩家2（下标 0 不使用）

long long Bonus = 10000000000; // 本局奖金（随事件扣减）
string B;                      // Bonus 字符化结果（_Bonus 生成，用于打字机演出）
int Gun[11];                   // 弹仓（下标 1..10，1=空弹 2=实弹）
int v[10];                     // 输入选项映射表
int hurt = 1;                  // 当前伤害（被锯子翻倍）
int Cnt = 1;                   // 弹仓中的弹数
int Pos = 2;                   // 当前指针位置（下一发）
int R = 1;                     // 实弹数量
int p = 1;                     // 当前回合玩家（1/2）
int Round = 0;                 // 当前回合数
int Handf = 0;                 // 是否有人被手铐

// ============ 网络与重连状态 ============

SOCKET g_clients[2] = { INVALID_SOCKET, INVALID_SOCKET };
bool g_connected[2] = { false, false };
string g_recvBuffers[2];       // 两个玩家各自的半行缓冲（仅接收线程访问）
queue<string> g_msgQueue;
mutex g_mutex;
condition_variable g_cv;
atomic<bool> g_serverRunning(true);

// 断线重连状态（见文件头注释）
bool g_waitingReconnect[2] = { false, false }; // 该玩家是否处于"等待重连"中
time_t g_reconnectDeadline[2] = { 0, 0 };      // 重连等待的截止时间
bool g_giveUp[2] = { false, false };           // 该玩家已明确放弃（收到 GIVEUP|N）

// 房间管理器通知参数（由 Start.exe 传入）
string g_startIp;
int g_startPort = 0;
string g_roomId;
string g_winnerName;

// 游戏结束时是否需要释放（销毁）房间：双方都失联时为 true
bool g_releaseRoom = false;

// ============ 基本发送 ============

// 向两个玩家发送消息。不持锁发送：send 可能阻塞（对端接收窗口满），
// 持锁会拖死接收线程/接受线程（它们也要抢同一把锁）。
// 改为持锁快照目标、锁外逐个发送；失败再回锁标记断线。
// 返回 false 表示连接已断开。不持有锁调用（由调用者保证）。
//
// 2026-08-02 修复：对端停止读取（TCP 接收窗口满）时，send 会一直返回
// WSAEWOULDBLOCK。旧代码无限 Sleep(10)+continue 重试——客户端若卡死，
// 游戏主线程就永久挂死在发送里（曾导致整局静默卡死）。现在超过
// SEND_TIMEOUT 视为连接异常，返回 false 走断线重连流程（正常发送的
// 消息只有几十字节，对端正常读取时绝不会触发超时）。
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
                    Log("SendAllData timeout: peer not reading (" + to_string(len - total) + " bytes unsent), treating as disconnected");
                    return false;
                }

                Sleep(10);
                continue;
            }

            auto ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - t0).count();
            Log("SendAllData FAIL sock=" + to_string(sock) + " err=" + to_string(WSAGetLastError()) + " after " + to_string(ms) + "ms (" + to_string(len - total) + " bytes unsent)");
            return false;
        }

        total += sent;
    }

    auto ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - t0).count();

    if (ms >= 100)
    {
        Log("SendAllData SLOW sock=" + to_string(sock) + " " + to_string(ms) + "ms for " + to_string(len) + " bytes");
    }

    return true;
}

// 发送失败后的统一处理：标记断线、关闭套接字、通知游戏逻辑。
// sock 身份校验：若标记期间该槽位已被接受线程重连为新套接字，则不误杀。
// 需在锁外收集日志文本、锁内修改状态，避免持锁做文件 I/O。
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

        for (int i = 0; i < 2; ++i)
        {
            if (g_connected[i]) targets.push_back(g_clients[i]);
        }
    }

    // 锁外发送（锁内 send 可能被阻塞的 TCP 发送拖住整个游戏）
    for (SOCKET s : targets)
    {
        if (!SendAllData(s, out.c_str(), (int)out.length()))
        {
            // 找到失败的玩家编号并标记断线（身份校验防误杀重连后的新套接字）
            int failedId = -1;

            {
                lock_guard<mutex> lock(g_mutex);

                for (int i = 0; i < 2; ++i)
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

void SendToClient(int id, const string& msg)
{
    if (id < 0 || id > 1) return;

    string out = msg;

    if (out.empty() || out.back() != '\n') out += '\n';

    SOCKET target = INVALID_SOCKET;

    {
        lock_guard<mutex> lock(g_mutex);

        if (g_connected[id]) target = g_clients[id];
    }

    if (target == INVALID_SOCKET) return;

    // 锁外发送（原因同上）；失败回锁标记断线（身份校验防误杀新连接）
    if (!SendAllData(target, out.c_str(), (int)out.length()))
    {
        MarkDisconnected(id, target);
    }
}

void SendMsg(const string& s)
{
    SendToAll(s);
}

// ============ 状态查询（全部带锁） ============

bool IsConnected(int idx)
{
    lock_guard<mutex> lock(g_mutex);
    return g_connected[idx];
}

bool AnyClientConnected()
{
    lock_guard<mutex> lock(g_mutex);

    for (int i = 0; i < 2; ++i)
    {
        if (g_connected[i]) return true;
    }

    return false;
}

bool BothConnected()
{
    lock_guard<mutex> lock(g_mutex);
    return g_connected[0] && g_connected[1];
}

bool BothLost()
{
    lock_guard<mutex> lock(g_mutex);
    return !g_connected[0] && !g_connected[1];
}

// ============ 消息队列 ============

// 从共享队列取一条消息；100ms Sleep 轮询（2026-08-02: 弃用 condition_variable。
// 观测证据：主线程 wait_for(100ms) 在运行期间被延迟 20-70s 唤醒（无任何输入心跳、
// 消息滞留队列），而同进程 recv 线程的 select(10ms) 从未延迟——CV 走 keyed-event
// 路径，Sleep 走普通内核定时器路径，与 select 同源；此改动若消除卡顿即证实。）
bool WaitForMessage(string& out)
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

            if (!g_connected[0] && !g_connected[1]) return false;
        }

        Sleep(100);
    }
}

// ============ 接收线程 ============

// 接收线程：持续读取两个玩家的消息，断线时标记并通知游戏逻辑。
void ReceiveThreadFunc()
{
    fd_set readSet;
    auto lastHeartbeat = chrono::steady_clock::now();

    while (g_serverRunning)
    {
        FD_ZERO(&readSet);
        SOCKET maxSock = 0;

        // 拷贝当前连接快照，避免与接受线程/游戏线程竞争
        {
            lock_guard<mutex> lock(g_mutex);

            for (int i = 0; i < 2; ++i)
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

        // 每 2 秒心跳：记录两套接字上"待读取字节数"（FIONREAD）。
        // 客户端数据迟迟不被处理时，pending>0 说明数据已到服务器缓冲
        // 但本线程没读到；pending==0 说明数据根本还没到达服务器。
        auto now = chrono::steady_clock::now();

        if (now - lastHeartbeat >= chrono::seconds(2))
        {
            lastHeartbeat = now;

            u_long pending0 = 0, pending1 = 0;
            SOCKET s0 = INVALID_SOCKET, s1 = INVALID_SOCKET;

            {
                lock_guard<mutex> lock(g_mutex);

                if (g_connected[0]) { s0 = g_clients[0]; ioctlsocket(s0, FIONREAD, &pending0); }
                if (g_connected[1]) { s1 = g_clients[1]; ioctlsocket(s1, FIONREAD, &pending1); }
            }

            GLog("recv-heartbeat: c0=" + to_string(s0) + " pend=" + to_string(pending0)
                + " c1=" + to_string(s1) + " pend=" + to_string(pending1)
                + " q=" + to_string(g_msgQueue.size()));
        }

        // tv 必须每次循环重置：select() 会把剩余时间写回 timeval，
        // 旧代码声明一次后 tv 被清零，select 退化成 0ms 忙等（100% CPU）
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;

        int sel = select(0, &readSet, NULL, NULL, &tv);

        if (sel > 0)
        {
            for (int i = 0; i < 2; ++i)
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

                    auto t0 = chrono::steady_clock::now();

                    if (!ReceiveLines(g_clients[i], g_recvBuffers[i], [](const string& line)
                    {
                        g_msgQueue.push(line);
                    }))
                    {
                        g_connected[i] = false;
                        closesocket(g_clients[i]);
                        g_clients[i] = INVALID_SOCKET;
                        g_msgQueue.push("__DISCONNECT__" + to_string(i));
                        g_cv.notify_one();
                    }

                    auto ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - t0).count();

                    // 观测点：select 报可读后 recv 仍卡住（锁内阻塞）会直接拖死主线程
                    if (ms >= 100)
                    {
                        GLog("recv-thread ReceiveLines SLOW sock=" + to_string(g_clients[i]) + " " + to_string(ms) + "ms");
                    }
                }
            }
        }
        else if (sel < 0)
        {
            GLog("recv-thread select failed: errno=" + to_string(WSAGetLastError()));
            Sleep(10);
        }
    }
}

// ============ 接受线程（首次连接 / 重连 / 放弃） ============

// 接受线程：接受新连接，读取首行命令并处理。
//   PLAYER_ID|N  ：首次连接或断线重连，占用/恢复 1 号或 2 号位置
//   GIVEUP|N     ：断线玩家明确放弃重连，通知游戏逻辑立即中止
// 只处理首行：不能复用 ReceiveLines 一路读下去——它会把同一包中首行之后
// 的内容也读走，且处理完首行后仍在读的数据会被静默丢弃（吞掉玩家的
// 首个游戏消息）。这里手动拼接首行，剩余字节交还接收线程的缓冲继续解析。
void AcceptThreadFunc(SOCKET listenSock)
{
    while (g_serverRunning)
    {
        sockaddr_in clientAddr;
        int addrLen = sizeof(clientAddr);

        SOCKET clientSock = accept(listenSock, (sockaddr*)&clientAddr, &addrLen);

        if (clientSock == INVALID_SOCKET)
        {
            // 监听套接字被关闭（服务器退出）或临时错误 → 稍后再试
            Sleep(100);
            continue;
        }

        // 发送超时：对端停止读取（接收窗口满）时，阻塞 send 会无限等待，
        // 即使 SendAllData 有 WSAEWOULDBLOCK 超时逻辑也永远不会触发
        // （阻塞套接字不返回 WSAEWOULDBLOCK，而是直接挂起）。
        // 设置 5s SO_SNDTIMEO：对端不读则 send 以 WSAETIMEDOUT 失败，
        // 走断线重连流程，游戏主线程不再被无限挂死（2026-08-02）。
        int sndTimeout = 5000;
        setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));

        string buf;
        string line;
        string leftover;
        bool handled = false;

        while (!handled)
        {
            // 首行读等待带 30 秒超时：连接后从不发数据的客户端不能永久占住
            // 接受线程（该线程还要服务另一名玩家的断线重连）。
            // 阻塞 recv 没有超时机制，先 select 判定可读再 recv（2026-08-03 修复）。
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(clientSock, &readSet);

            timeval tv;
            tv.tv_sec = 30;
            tv.tv_usec = 0;

            int sel = select(0, &readSet, NULL, NULL, &tv);

            if (sel <= 0)
            {
                if (sel < 0)
                {
                    GLog("accept first-line select failed: errno=" + to_string(WSAGetLastError()));
                }
                else
                {
                    GLog("accept first-line timeout: client connected but never sent, closing");
                }

                closesocket(clientSock);
                break;
            }

            char data[4096];
            int bytes = recv(clientSock, data, sizeof(data) - 1, 0);

            if (bytes <= 0)
            {
                // 未等来首行命令就断线
                closesocket(clientSock);
                break;
            }

            data[bytes] = '\0';
            buf += data;

            // 防御：首行不可能这么长，超限直接断开（防内存膨胀）
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

            // 空行（仅回车）不构成命令，忽略后继续等首行
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

        // 非法首行 → 拒绝
        if (pid != 1 && pid != 2)
        {
            string reject = "ERROR:Invalid PLAYER_ID\n";
            send(clientSock, reject.c_str(), reject.length(), 0);
            closesocket(clientSock);
            continue;
        }

        int idx = pid - 1;
        bool isReconnect = false;
        string notifyOther; // 需要发给另一名玩家的消息（锁外发送）

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

            // 首行之后的同包剩余数据交还接收线程的缓冲继续解析，
            // 避免被本线程吞掉（客户端正常不会这样发，防御处理）
            g_recvBuffers[idx].clear();

            if (!leftover.empty())
            {
                g_recvBuffers[idx] = leftover;
            }

            if (isReconnect)
            {
                Log("Player " + to_string(pid) + " reconnected");
                notifyOther = " !!! " + P[pid].name + " reconnected, game continues ...";
            }
            else
            {
                Log("Client assigned as Player " + to_string(pid));
            }
        }

        // 欢迎消息（在锁外发送，避免持锁 send）
        string msg = "You are Player " + to_string(pid) + "\n";

        if (isReconnect) msg += "Welcome back to Demon Roulette!\n";
        else msg += "Welcome to Demon Roulette!\n";

        send(clientSock, msg.c_str(), msg.length(), 0);

        if (!notifyOther.empty())
        {
            SendToClient(1 - idx, notifyOther);
        }

        {
            lock_guard<mutex> lock(g_mutex);
            g_cv.notify_all(); // 唤醒等待双方连接/重连的主线程
        }
    }
}

// ============ 重连等待 ============

// 进入重连等待：标记断线玩家正在等待，并给出截止时间。
// 若另一方也已断线（双方都失联）→ 返回 false，无需等待。
bool EnterReconnectWait(int playerId)
{
    int otherId = 1 - playerId;

    lock_guard<mutex> lock(g_mutex);

    if (!g_connected[otherId]) return false;

    g_waitingReconnect[playerId] = true;
    g_reconnectDeadline[playerId] = time(nullptr) + RECONNECT_TIMEOUT_SECONDS;
    return true;
}

// 等待断线玩家重连。
// 返回 true  = 玩家已重连，游戏继续；
// 返回 false = 对方放弃（GIVEUP）、等待超时或双方都断线，游戏应中止。
bool WaitForReconnect(int playerId)
{
    int otherId = 1 - playerId;

    GLog("WaitForReconnect entry: player " + to_string(playerId + 1) + " lost, waiting up to " + to_string(RECONNECT_TIMEOUT_SECONDS) + "s");

    if (!EnterReconnectWait(playerId)) return false;

    // 告知另一方"正在等待重连"
    SendToClient(otherId, " !!! " + P[playerId + 1].name + " connection lost, waiting for reconnect ...");

    while (g_serverRunning)
    {
        bool reconnected = false;
        bool gaveUp = false;
        bool otherGone = false;
        bool timedOut = false;

        {
            lock_guard<mutex> lock(g_mutex);

            reconnected = g_connected[playerId];
            gaveUp = g_giveUp[playerId];
            otherGone = !g_connected[otherId];
            timedOut = time(nullptr) > g_reconnectDeadline[playerId];

            if (reconnected || gaveUp || otherGone || timedOut)
            {
                g_waitingReconnect[playerId] = false;

                if (timedOut) g_giveUp[playerId] = true;
            }
        }

        if (reconnected) return true;

        if (gaveUp || otherGone || timedOut)
        {
            GLog("WaitForReconnect exit: player " + to_string(playerId + 1) + " gaveUp=" + to_string(gaveUp) + " otherGone=" + to_string(otherGone) + " timedOut=" + to_string(timedOut));

            if (!otherGone)
            {
                SendToClient(otherId, " !!! " + P[playerId + 1].name + " failed to reconnect, this game ends ...");
            }

            return false;
        }

        Sleep(200);
    }

    return false;
}

// 确保双方都在线；若有人断线则阻塞等待重连。
// 返回 false 表示应中止游戏。
bool WaitBothConnected()
{
    while (true)
    {
        bool both;

        {
            lock_guard<mutex> lock(g_mutex);
            both = g_connected[0] && g_connected[1];
        }

        if (both) return true;

        // 找到断线的那一位，进入重连等待
        for (int i = 0; i < 2; ++i)
        {
            if (!IsConnected(i))
            {
                if (!WaitForReconnect(i)) return false;
            }
        }
    }
}

// 游戏开始前等待两个玩家都连接（接受线程负责 assign 并唤醒）。
// 带超时（RECONNECT_TIMEOUT_SECONDS）：若一方玩家迟迟不连接
//（如客户端在 GAME_PREPARE 后崩溃、从未连上），不再让进程永久挂起；
// 超时后按正常流程走：主循环发现无人连接 → 中止本局 → 通知房间管理器。
void WaitForGameStart()
{
    unique_lock<mutex> lock(g_mutex);

    while (!(g_connected[0] && g_connected[1]))
    {
        if (g_cv.wait_for(lock, chrono::seconds(RECONNECT_TIMEOUT_SECONDS))
            == cv_status::timeout)
        {
            Log("Timed out waiting for both players to connect");
            return;
        }
    }

    // 清空开局前积累的过期消息（如开局瞬间的 __DISCONNECT__），
    // 避免后续被 WaitForPlayerInput 误当成当前状态。
    while (!g_msgQueue.empty()) g_msgQueue.pop();
}

// ============ 玩家输入 ============

// 等待指定玩家输入一行（形如 PLAYER_N:...，N=1/2）。
// 玩家断线时进入重连等待；返回 false 表示游戏应中止。
bool WaitForPlayerInput(string& out, int playerId)
{
    auto lastWaitBeat = chrono::steady_clock::now();

    while (true)
    {
        // 每 2 秒心跳：等待玩家输入期间主线程一直轮询消息队列，
        // 若某条客户端消息在队列里停留数秒才被弹出，这里能直接看出来
        auto now = chrono::steady_clock::now();

        if (now - lastWaitBeat >= chrono::seconds(2))
        {
            lastWaitBeat = now;
            GLog("input-wait heartbeat: player " + to_string(playerId + 1) + " q=" + to_string(g_msgQueue.size()));
        }

        if (!IsConnected(playerId))
        {
            if (!WaitForReconnect(playerId)) return false;
            continue;
        }

        string msg;

        if (!WaitForMessage(msg)) return false;

        GLog("WaitForPlayerInput recv(" + to_string(playerId + 1) + "): " + msg);

        if (msg.find("__DISCONNECT__") == 0)
        {
            // 注意：g_connected 已由接收线程/发送函数置为 false，
            // 这里不要再写，否则可能覆盖"已经重连成功"的状态。
            // 安全解析断开编号：定位最后的数字字符（格式 "__DISCONNECT__0" 或 "__DISCONNECT__1"）
            int id = -1;
            for (size_t k = msg.size(); k > 0; --k)
            {
                char c = msg[k - 1];
                if (c >= '0' && c <= '9') { id = c - '0'; break; }
            }
            if (id != 0 && id != 1)
            {
                Log("Invalid __DISCONNECT__ message format: " + msg);
                continue;
            }

            if (id == playerId)
            {
                if (!WaitForReconnect(playerId)) return false;

                // 玩家重连成功：重新提示输入（断线期间原来的提示可能已丢失）
                SendToClient(playerId, "__INPUT__");
            }
            else
            {
                // 对方断线：转去等待对方重连，期间当前玩家的输入仍在队列中排队；
                // 对方放弃/超时 → 本局中止。旧代码这里直接 continue，
                // 双方会同时挂着等输入而无人感知对方失联（2026-08-03 修复）。
                if (!WaitForReconnect(id)) return false;
            }

            continue;
        }

        if (msg.find("PLAYER_") == 0)
        {
            size_t pipePos = msg.find('|');

            if (pipePos != string::npos && pipePos > 7)
            {
                string prefix = msg.substr(0, pipePos);

                if (prefix == "PLAYER_1" || prefix == "PLAYER_2")
                {
                    int senderId = prefix[7] - '0' - 1;
                    string content = msg.substr(pipePos + 1);

                    if (senderId == playerId)
                    {
                        out = content;
                        return true;
                    }
                }
            }
        }
    }
}

// ============ 游戏基础操作 ============

void SetHp(int hp)
{
    P[1].Hp = hp;
    P[2].Hp = hp;
}

void SetProp(int num)
{
    P[1].Num = P[2].Num = 6 * num;

    for (int i = 1; i <= 6; ++i)
    {
        P[1].T[i] = P[2].T[i] = num;
    }
}

void Clear()
{
    SendToAll("__CLS__");
}

// 游戏节奏控制（原为忙等循环，改为 Sleep 避免占用 CPU）：
//   f == -100 : 进入暂停（客户端显示 Pause 等待按键）
//   f <  0    : 每单位 400ms
//   f == 0    : 50ms
//   0 < f <100: 每单位 100ms
//   f >= 100  : 每单位 5ms
void wait(int f)
{
    if (f == -100)
    {
        wait(3);
        GLog("wait(-100): sending __PAUSE__");
        SendMsg("__PAUSE__");
        return;
    }

    if (f < 0)
    {
        Sleep(400 * (-f));
        return;
    }

    if (f == 0)
    {
        Sleep(50);
        return;
    }

    if (f < 100)
    {
        Sleep(100 * f);
        return;
    }

    Sleep(5 * f);
}

// 加载进度条（客户端显示 0-100）
void Loading()
{
    Clear();
    SendMsg(" Loading... ");

    for (int i = 0; i <= 100; ++i)
    {
        SendToAll("__PROGRESS__:" + to_string(i));
        this_thread::sleep_for(chrono::milliseconds(30));
    }

    SendMsg(" Loading complete. ");
    wait(5);
    Clear();
}

void Menu()
{
    SendToAll(" Welcome to the Demon Roulette ! ! ");
    wait(10);
}

void ShowRound()
{
    ++Round;
    GLog("Round " + to_string(Round) + " start (Pos=" + to_string(Pos) + " Cnt=" + to_string(Cnt) + ")");
    SendToAll(" Round " + to_string(Round));
}

// 血条显示：第一回合上限 3，其余回合上限 5。
// 第三回合只是把格子样式换成 '-'（装饰），上限仍是 5——
// "第三回合低血量禁烟"是 Ask() 里的独立规则（决胜局高压设计）。
void ShowHP(int id)
{
    stringstream ss;

    if (P[id].Hp <= 0)
    {
        ss << " " << setw(5) << P[id].name << " HP X";
    }
    else
    {
        ss << " " << setw(5) << P[id].name << " HP ";

        char c = (Round == 3) ? '-' : '+';
        int t = (Round == 1) ? 3 : 5;

        for (int i = 1; i <= t - P[id].Hp; ++i) ss << " ";
        for (int i = 1; i <= P[id].Hp - 2; ++i) ss << "+";

        for (int i = 1; i <= min(2, P[id].Hp); ++i) ss << c;
    }

    SendMsg(ss.str());
}

// 展示某玩家剩余道具
void ShowProps(int id)
{
    SendMsg(" There are " + P[id].name + " 's Props .");
    wait(3);

    for (int i = 1; i <= 6; ++i)
    {
        if (!P[id].T[i]) continue;

        SendMsg("        " + to_string(P[id].T[i]) + " -> " + Prop[i]);
        wait(0);
    }

    wait(-100);
    Clear();
}

// 扣血（第三回合低血量时触发"准备好了吗"演出）
void Hurt(int id)
{
    P[id].Hp -= hurt;
    GLog("Hurt id=" + to_string(id) + " hp=" + to_string(P[id].Hp));
    ShowHP(id);
    wait(6);
    Clear();
    wait(4);

    if (Round == 3 && P[id].Hp <= 2 && P[id].Hp > 0 && !P[id].F)
    {
        SendMsg(" ");

        P[id].F = 1;
        string ts = "Are you ready ? !";

        for (char c : ts)
        {
            // id 是玩家编号（1/2），SendToClient 需要 0/1 下标
            SendToClient(id - 1, "__TYPEWRITER__" + string(1, c));
            wait(0);
        }

        SendMsg(" ");
        wait(6);
        wait(-100);
    }

    Clear();
}

// ============ 动作实现 ============

// 开枪：选择打自己还是对方
void Shoot()
{
    GLog("Shoot entry p=" + to_string(p) + " Pos=" + to_string(Pos) + " Gun[Pos]=" + to_string(Gun[Pos]));
    wait(2);

    int currentId = p - 1;
    int op = 0;

    do
    {
        // 提示文本先发送、__INPUT__ 最后发：客户端在菜单打印期间门控是关闭的
        //（原始模式、无回显、读取即丢弃），提前键入不会混进菜单里破坏排版；
        // 只有 __INPUT__ 到达时才打开输入窗口，接受一行（回车结束）后关闭
        //（2026-08-03）。
        SendToClient(currentId, " Please choose your shooting target .");
        wait(2);
        SendToClient(currentId, "        < 1 > Self .");
        wait(0);
        SendToClient(currentId, "        < 2 > Other .");
        wait(0);

        if (IsConnected(1 - currentId))
        {
            SendToClient(1 - currentId, " " + P[p].name + " is choosing target ...");
        }

        SendToClient(currentId, "__INPUT__");

        string input;

        if (!WaitForPlayerInput(input, currentId)) return;

        op = atoi(input.c_str());

        // 输入校验：只接受 1/2，防止非法输入导致逻辑混乱
        if (op != 1 && op != 2)
        {
            GLog("Shoot invalid input: " + input);
            SendToClient(currentId, " -ERROR-");
            SendToClient(currentId, "__PAUSE__");
            SendToClient(currentId, "__CLS__");
            continue;
        }

        break;
    } while (true);

    Clear();
    GLog("Shoot target=" + to_string(op) + " (self/other)");
    SendToAll(" " + P[p].name + " chose target " + (op == 1 ? "Self" : "Other"));
    wait(2);

    // 玩家 2 的选择映射到真实目标编号（1=自己 2=对方 → 对玩家2要反转）
    if (p == 2) op = op % 2 + 1;

    if (Gun[Pos] == 2)
    {
        SendToAll("----------");
        wait(0);
        SendToAll("  BOOM!!!");
        wait(2);
        Hurt(op);

        if (!Handf) p = p % 2 + 1;
        else Handf = 0;

        Bonus -= 70000000;
    }
    else if (Gun[Pos] == 1)
    {
        SendToAll(" ----------");
        wait(0);
        SendToAll("  False .");
        wait(4);
        Clear();

        if (op != p && !Handf) p = p % 2 + 1;
        if (op != p && Handf) Handf = 0;

        Bonus -= 8000000;
    }

    Pos++;
    hurt = 1;
    GLog("Shoot done: p now " + to_string(p) + " Pos=" + to_string(Pos));
}

// 手铐：对方下回合无法行动
void Handcuff()
{
    wait(2);
    SendToAll(" " + P[p % 2 + 1].name + " was handcuffed .");

    Handf = 1;
    wait(6);
    Clear();

    P[p].Num--;
    P[p].T[1]--;
    Bonus -= 60000000;
}

// 巧克力：以自己的一件道具为代价，替对方使用一件道具
void Chocolate()
{
    wait(2);
    ShowProps(p % 2 + 1);

    int op;
    int f = 0;
    int currentId = p - 1;

    do
    {
        for (int i = 0; i <= 9; ++i) v[i] = 0;

        wait(4);

        // 选择期间的清屏只发给正在选择的玩家（2026-08-03）
        SendToClient(currentId, "__CLS__");

        // 提示文本先发送、__INPUT__ 最后发：客户端在菜单打印期间门控是关闭的
        //（无回显，键入被丢弃），不会破坏菜单排版；__INPUT__ 到达时才打开
        // 输入窗口，接受一行（回车结束）后关闭（2026-08-03）。
        SendToClient(currentId, " Please choose target .");
        wait(2);
        SendToClient(currentId, "        < 0 > Exit .");
        wait(0);

        int j = 0;

        for (int i = 1; i <= 6; ++i)
        {
            if (P[p % 2 + 1].T[i] && i != 2)
            {
                SendToClient(currentId, "        < " + to_string(++j) + " > " + Prop[i] + " .");
                v[j] = i;
                wait(0);
            }
        }

        if (IsConnected(1 - currentId))
        {
            SendToClient(1 - currentId, " " + P[p].name + " is choosing a prop ...");
        }

        SendToClient(currentId, "__INPUT__");

        string input;

        if (!WaitForPlayerInput(input, currentId)) return;

        op = atoi(input.c_str());

        // 选择未结束前的清屏同样只发给选择者（2026-08-03）
        SendToClient(currentId, "__CLS__");

        // 输入校验：只允许 0-9，越界按非法处理（防止数组越界）
        if (op < 0 || op > 9) op = -1;
        else op = v[op];

        if (op == 0) return;

        if (op == 1 && P[p % 2 + 1].T[op])
        {
            if (Handf)
            {
                SendToClient(currentId, " -ERROR-");
            }
            else
            {
                f = 1;
                wait(2);
                SendToAll(" " + P[p % 2 + 1].name + " was handcuffed .");
                Handf = 1;
                wait(6);
                Clear();
                P[p % 2 + 1].Num--;
                P[p % 2 + 1].T[1]--;
            }
        }
        else if (op == 3 && P[p % 2 + 1].T[op])
        {
            f = 1;
            wait(2);
            SendToAll(" The gun damage doubled !! ");
            hurt *= 2;
            wait(6);
            Clear();
            P[p % 2 + 1].Num--;
            P[p % 2 + 1].T[3]--;
        }
        else if (op == 4 && P[p % 2 + 1].T[op])
        {
            f = 1;
            wait(2);
            SendToAll(" The shell has been switched .");
            Gun[Pos] = Gun[Pos] % 2 + 1;
            wait(6);
            Clear();
            P[p % 2 + 1].Num--;
            P[p % 2 + 1].T[4]--;
        }
        else if (op == 5 && P[p % 2 + 1].T[op])
        {
            f = 1;
            wait(2);
            SendToAll(" A ");

            if (Gun[Pos] == 2) SendToAll("true shell ");
            else SendToAll("false shell ");

            SendToAll("has been unloaded .");
            Pos++;
            wait(6);
            Clear();
            P[p % 2 + 1].Num--;
            P[p % 2 + 1].T[5]--;
        }
        else if (op == 6 && P[p % 2 + 1].T[op])
        {
            f = 1;
            wait(2);
            SendToAll(" Your HP +1 .");
            P[p].Hp = min(5, P[p].Hp + 1);
            wait(3);
            ShowHP(p);
            wait(3);
            Clear();
            P[p % 2 + 1].Num--;
            P[p % 2 + 1].T[6]--;
        }
        else
        {
            SendToClient(currentId, " -ERROR-");
        }
    } while (!f);

    P[p].Num--;
    P[p].T[2]--;
    Bonus -= 80000000;
}

// 锯子：本发子弹伤害翻倍
void Saw()
{
    wait(2);
    SendToAll(" The gun damage doubled !! ");
    hurt *= 2;
    wait(6);
    Clear();

    P[p].Num--;
    P[p].T[3]--;
    Bonus -= 40000000;
}

// 手机：查看并反转当前一发子弹
void Phone()
{
    wait(2);
    SendToAll(" The shell has been switched .");
    Gun[Pos] = Gun[Pos] % 2 + 1;
    wait(6);
    Clear();

    P[p].Num--;
    P[p].T[4]--;
    Bonus -= 70000000;
}

// 饮料：卸掉当前一发子弹
void Drinks()
{
    wait(2);
    SendToAll(" A ");

    if (Gun[Pos] == 2) SendToAll("true shell ");
    else SendToAll("false shell ");

    SendToAll("has been unloaded .");
    Pos++;
    wait(6);
    Clear();

    P[p].Num--;
    P[p].T[5]--;
    Bonus -= 54000000;
}

// 烟：回复 1 点生命
void Smoke()
{
    wait(2);
    SendToAll(" Your HP +1 .");
    P[p].Hp = min(5, P[p].Hp + 1);
    wait(3);
    ShowHP(p);
    wait(3);
    Clear();

    P[p].Num--;
    P[p].T[6]--;
    Bonus -= 63000000;
}

// 装弹：Cnt 增加 n 发，随机排列实弹/空弹
void Shell(int n)
{
    Cnt = min(10, Cnt + n);

    // 防御：弹数必须严格大于回合数，否则 rand()%0 会除零（致命）
    // 注意：Cnt 也可能在 Shell 外部被改小（如道具消耗），故每次装弹前都校验
    if (Cnt <= Round) Cnt = Round + 1;
    if (Cnt > 10) Cnt = 10;

    R = rand() % (Cnt - Round) + Round;
    wait(0);

    int c = Cnt - R;

    for (int i = 1; i <= 10; ++i) Gun[i] = 0;

    SendToAll(" True: " + to_string(R));
    wait(0);
    SendToAll(" False: " + to_string(c));
    wait(1);

    for (int i = 1; i <= Cnt; ++i)
    {
        int t;

        do
        {
            t = rand() % Cnt + 1;
        } while (Gun[t]);

        if (R)
        {
            Gun[t] = 2;
            --R;
        }
        else
        {
            Gun[t] = 1;
            --c;
        }
    }

    wait(2);
    Pos = 1;
    SendToAll(" The gun is loaded .");
}

// 随机发放道具
void RandProps(int id, int num)
{
    wait(2);
    SendToAll(" Now , it is randing " + P[id].name + " 's Props .");

    P[id].Num += num;

    while (num--)
    {
        int t = rand() % 6 + 1;
        P[id].T[t]++;
    }
}

// 行动选择：开枪或使用道具（调试命令 -1/-2 可交换当前弹种位置，保留勿删）
void Ask()
{
    int currentId = p - 1;
    int otherId = 1 - currentId;
    int op;

    GLog("Ask entry p=" + to_string(p) + " Pos=" + to_string(Pos) + " Cnt=" + to_string(Cnt));

    do
    {
        for (int i = 0; i <= 9; ++i) v[i] = 0;

        v[1] = 1;
        wait(6);

        // 选择期间的清屏只发给正在选择的玩家：对端屏幕保持"is choosing"状态，
        // 只有最终选择的结果才全员广播（2026-08-03）。
        SendToClient(currentId, "__CLS__");

        // 提示文本先发送、__INPUT__ 最后发：客户端在菜单打印期间门控是关闭的
        //（无回显，键入被丢弃），不会破坏菜单排版；__INPUT__ 到达时才打开
        // 输入窗口，接受一行（回车结束）后关闭（2026-08-03）。
        SendToClient(currentId, " - " + P[p].name);
        SendToClient(currentId, " Please choose your OP .");
        wait(0);

        SendToClient(currentId, "        < 1 > Shoot .");
        wait(0);

        int j = 1;

        for (int i = 1; i <= 6; ++i)
        {
            if (P[p].T[i])
            {
                SendToClient(currentId, "        < " + to_string(++j) + " > " + Prop[i]);
                v[j] = i + 1;
                wait(0);
            }
        }

        if (IsConnected(otherId))
        {
            SendToClient(otherId, " " + P[p].name + " is choosing ...");
        }

        SendToClient(currentId, "__INPUT__");

        // 观测点：菜单发送完成、即将进入等待（卡顿发生在发送序列 → 之前的
        // SendAllData SLOW/FAIL 日志会指明是哪条消息；发生在等待 → 心跳会冒出来）
        GLog("Ask menu sent, waiting input (currentId=" + to_string(currentId) + ")");

        string input;

        if (!WaitForPlayerInput(input, currentId)) return;

        op = atoi(input.c_str());

        GLog("Ask got input: " + input + " -> op " + to_string(op));

        // 调试命令：-1 换入实弹，-2 换入空弹（针对当前指针位置）
        if (op == -1)
        {
            SendToClient(currentId, "__CLS__");

            for (int i = Pos + 1; i <= Cnt; ++i)
            {
                if (Gun[i] == 2)
                {
                    swap(Gun[Pos], Gun[i]);
                    break;
                }
            }

            continue;
        }

        if (op == -2)
        {
            SendToClient(currentId, "__CLS__");

            for (int i = Pos + 1; i <= Cnt; ++i)
            {
                if (Gun[i] == 1)
                {
                    swap(Gun[Pos], Gun[i]);
                    break;
                }
            }

            continue;
        }

        // 选择未结束前的清屏同样只发给选择者（2026-08-03）
        SendToClient(currentId, "__CLS__");

        // 输入校验：只允许 0-9，越界按非法处理（防止数组越界）
        if (op < 0 || op > 9) op = -1;
        else op = v[op];

        if (op == 2 && P[p].T[op - 1])
        {
            if (Handf)
            {
                SendToClient(currentId, " -ERROR-");
                SendToClient(currentId, "__PAUSE__");
                SendToClient(currentId, "__CLS__");
            }
            else
            {
                Handcuff();
            }
        }
        else if (op == 3 && P[p].T[op - 1])
        {
            if (!P[p % 2 + 1].Num)
            {
                SendToClient(currentId, " -ERROR-");
                SendToClient(currentId, "__PAUSE__");
                SendToClient(currentId, "__CLS__");
            }
            else
            {
                Chocolate();
            }
        }
        else if (op == 4 && P[p].T[op - 1])
        {
            Saw();
        }
        else if (op == 5 && P[p].T[op - 1])
        {
            Phone();
        }
        else if (op == 6 && P[p].T[op - 1])
        {
            Drinks();
        }
        else if (op == 7 && P[p].T[op - 1])
        {
            // 第三回合决胜局高压设计：低血量（<=2）禁止回血
            if (P[p].Hp <= 2 && Round == 3)
            {
                SendToClient(currentId, " You can't use it !! ");
                SendToClient(currentId, "__PAUSE__");
                SendToClient(currentId, "__CLS__");
            }
            else
            {
                Smoke();
            }
        }
        else if (op != 1)
        {
            SendToClient(currentId, " -ERROR-");
            SendToClient(currentId, "__PAUSE__");
            SendToClient(currentId, "__CLS__");
        }
    } while (op != 1);

    Shoot();
}

// ============ 结算 ============

void _Bonus()
{
    int l = 0;

    B.clear(); // B 是全局变量，必须清空以免残留上一局（防御）

    // 用局部副本逐位取出，避免把全局 Bonus 除到 0：
    // 旧代码直接对 Bonus 做 /=10，Save() 随后写入 data.txt 的永远是 0
    //（2026-08-03 修复：data.txt 曾出现 9 行连续 0 奖金记录）
    long long remain = Bonus;

    if (remain == 0)
    {
        B = "0";
        return;
    }

    while (remain)
    {
        B += (char)(remain % 10 + 48);
        remain /= 10;
        l++;

        if (remain && l % 3 == 0) B += ',';
    }
}

void End()
{
    Clear();
    wait(10);

    if (P[1].Hp <= 0)
    {
        g_winnerName = P[2].name;
        SendToAll(" The Player <" + P[2].name + "> is winner !! ");
    }
    else
    {
        g_winnerName = P[1].name;
        SendToAll(" The Player <" + P[1].name + "> is winner !! ");
    }

    wait(-100);
    Clear();
    wait(3);
    SendToAll(" Settling . . . ");
    wait(20);
    Clear();
    wait(6);
    SendToAll(" " + g_winnerName + "'s Bonus has ... \n ");
    wait(-100);
    Clear();
    wait(4);

    _Bonus();

    // B 是"低位在前"（_Bonus 逐位 append 个位开始），
    // 倒序输出从 B.size()-1（最高位）开始才是正确顺序；
    // 旧代码从 size()-2 开始，会漏掉最高位（如 10,000,000,000 显示成 0,000,000,000）。
    for (int i = (int)B.size() - 1; i >= 0; i--)
    {
        SendToAll("__TYPEWRITER__" + string(1, B[i]));
        wait(0);
    }

    wait(20);
}

void Save()
{
    ofstream fout("data.txt", ios::app);
    fout << g_winnerName << " " << Bonus << "\n";
    fout.close();
}

// ============ 通知房间管理器 ============

// 游戏结束时通知房间管理器。连接失败会重试 3 次。
// 若双方都失联（g_releaseRoom），追加发送 RELEASE 让房间管理器销毁房间。
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

// UTF-8 从宽字符转换（用于命令行参数）
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
    Log("Game Server starting...");

    // Parse command line as wide chars to avoid ANSI codepage corruption (CJK names)
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    vector<string> args;
    args.reserve(wargc);
    for (int i = 0; i < wargc; ++i)
        args.push_back(WideToUtf8Local(wargv[i]));
    LocalFree(wargv);

    int port = 8888;
    string player1Name = "Player1";
    string player2Name = "Player2";

    if ((int)args.size() >= 2) port = atoi(args[1].c_str());
    if ((int)args.size() >= 3) player1Name = SanitizeName(args[2]);
    if ((int)args.size() >= 4) player2Name = SanitizeName(args[3]);

    if ((int)args.size() >= 7)
    {
        g_startIp = args[4];
        g_startPort = atoi(args[5].c_str());
        g_roomId = args[6];
    }

    Log("Port: " + to_string(port) + " Names: " + player1Name + " / " + player2Name);

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

    // 接收线程尽早启动，这样游戏开始前的断线也能被感知并等待重连
    thread recvThread(ReceiveThreadFunc);

    // 等待两个玩家都连接
    WaitForGameStart();

    P[1].name = player1Name;
    P[2].name = player2Name;
    Log("Names set: " + P[1].name + " / " + P[2].name);

    srand((unsigned)time(0));
    SetHp(3);
    SetProp(0);

    Clear();
    Menu();
    wait(-100);
    Clear();
    wait(3);
    Loading();
    Loading();

    bool gameRunning = true;

    for (int i = 1; i <= 3 && gameRunning; ++i)
    {
        ShowRound();
        wait(10);
        Clear();

        while (P[1].Hp > 0 && P[2].Hp > 0)
        {
            // 有人断线时先等待重连；失败则中止本局
            if (!WaitBothConnected())
            {
                gameRunning = false;
                break;
            }

            if (Pos > Cnt)
            {
                GLog("main loop: Pos(" + to_string(Pos) + ") > Cnt(" + to_string(Cnt) + "), Round " + to_string(Round) + " (reload props/shell)");

                if (Round > 1)
                {
                    RandProps(1, min(8 - P[1].Num, 4));
                    wait(6);
                    Clear();
                    ShowProps(1);
                    RandProps(2, min(8 - P[2].Num, 4));
                    wait(6);
                    Clear();
                    ShowProps(2);
                }

                Shell(1);
                wait(-100);
            }

            Clear();
            ShowHP(1);
            wait(0);
            ShowHP(2);
            wait(10);
            GLog("main loop: asking p=" + to_string(p));
            Ask();
        }

        GLog("round " + to_string(Round) + " while-loop ended");

        Pos = Cnt + 1;
        wait(8);

        if (Round < 3 && gameRunning)
        {
            SetHp(5);
            SetProp(0);
        }
    }

    // 双方都失联 → 释放房间；否则保留房间供玩家回房重开
    g_releaseRoom = BothLost();

    GLog("main loop finished: gameRunning=" + to_string(gameRunning) + " releaseRoom=" + to_string(g_releaseRoom));

    if (gameRunning)
    {
        GLog("End() called");
        End();
        Save();
    }
    else
    {
        SendToAll("Game aborted.\n");
    }

    NotifyStartGameEnded();

    SendToAll("Game over.\n");

    // 关闭监听套接字，让接受线程退出
    g_serverRunning = false;
    closesocket(listenSock);
    acceptThread.join();
    recvThread.join();

    for (int i = 0; i < 2; ++i)
    {
        if (g_connected[i]) closesocket(g_clients[i]);
    }

    WSACleanup();
    Log("Game Server stopped");
    cout << "\n[ Pause ]\n";
    system("pause > nul");
    return 0;
}
