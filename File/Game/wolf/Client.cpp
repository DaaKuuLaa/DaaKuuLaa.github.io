// Client.cpp - 狼人杀客户端（大厅 → 房间 → 游戏）
//
// 线程分工：
//   主线程    ：消息分发与命令处理；GAME_PREPARE 时连游戏服务器，主循环每轮
//               调用 FlushGameInput() 把输入线程捕获的第一行非空输入发给服务器
//   输入线程  ：受输入门控（g_inputGate）驱动的常驻线程，唯一读控制台的线程；
//               Open（大厅/房间提示符、游戏 __INPUT__）→ 捕获一行并路由；
//               Closed（游戏演出）→ 读取并丢弃；Paused → 完全不读
//   房间接收线程：读取房间管理器（Start.exe）消息；每次连接大厅后重建
//   游戏接收线程：读取游戏服务器消息；每次进入/重连游戏后重建
//   心跳线程  ：每 HEARTBEAT_INTERVAL_SECONDS 秒向当前连接（大厅=Start /
//               游戏=Server）发一行 PING 保活；随大厅连接启停（重连时先
//               join 旧线程再重启），程序退出时 join
//
// 本文件实现的协议与参考项目（reference/demon）有三点差异：
//   1) 握手为 HELLO|3（协议版本号），随后 NAME|<名字>；
//   2) 房间命令通过 RM 转发（READY/STATUS/TRANSFER/PICK/BAN/UNBAN/LEVEL/
//      VILLAGER/RATIO/CONFIRM/START/AUTO）；
//   3) 游戏结束后回到原房间：重连房间管理器后自动发送 GAME_ENDED + REJOIN，
//      房间由 Start.exe 保留（若房间已被 RELEASE 销毁则回房失败落在大厅）。
//
// 断线规则：
//   1. 进入游戏后大厅连接被主动关闭，断线重连流程只针对游戏服务器；
//   2. 游戏连接中断 → 尝试重连 3 次（间隔 5 秒），成功则重发 PLAYER_ID
//      并等待服务端确认行（被拒/超时计失败，杜绝"连上→被踢"假成功）；
//   3. 3 次都失败 → 发送 GIVEUP|<playerId> 通知游戏服务器放弃，回原房间；
//   4. 服务器正常收尾会先发终态行 __GAME_OVER__ → 直接回房，不做重连；
//   5. 大厅断线 → 重连房间管理器 3 次（间隔 5 秒），仍失败 → 暂停并退出。
#include "common.h"

#include <deque>

// 双版本构建开关：WOLF_EN=1 为英文版，默认为中文版
#ifdef WOLF_EN
bool g_enMode = true;
#else
bool g_enMode = false;
#endif

// 当前客户端语言（由 WOLF_EN 编译开关决定）：本地提示按此取中英文案
Lang CurLang()
{
    return g_enMode ? Lang::En : Lang::Zh;
}

void ClientLog(const string& msg)
{
    LogMsg("client.log", msg);
}

// ============ 全局状态 ============

SOCKET g_sock = INVALID_SOCKET;        // 房间管理器连接
SOCKET g_gameSock = INVALID_SOCKET;    // 游戏服务器连接
atomic<bool> g_running(true);
queue<string> g_msgQueue;
mutex g_mutex;
condition_variable g_cv;

string g_roomId;                       // 当前房间号
bool g_inRoom = false;
string g_playerName = "Player";
bool g_isAdmin = false;

atomic<bool> g_inGame(false);
string g_gameServerIp;
int g_gameServerPort = 0;
int g_myGamePlayerId = 0;              // 本局玩家编号（RM 分配）

// 本局玩家名单（编号序，不含职业信息）：开局收 PLAYER_LIST| 广播缓存，
// 游戏内输入 LIST 时本地重放显示。重连不断线时保留（进程内缓存）
vector<string> g_gamePlayerNames;

// 玩家名单显示（定义在下文 DisplayWidth 之后），此处前置声明供消息处理调用
void ShowGamePlayerList();

// 已收到服务器终态控制行 __GAME_OVER__（正常收尾）：此后游戏连接断开
// 一律直接回房、不进入重连；只在收到下一局 GAME_PREPARE 时复位
bool g_gameOver = false;

// 游戏结束后的回房信息：重连房间管理器成功后据此发送 GAME_ENDED + REJOIN，
// 让 Start.exe 保留的房间把本玩家放回原槽位（房间已被销毁则回房失败落在大厅）。
// 发送后不立即清空：被拒（游戏仍在进行中）时还需要目标来启动自动重试；
// 回房成功（JOINED|）或终态失败（REJOIN_FAIL|非"进行中"）时由消息处理处清空
string g_rejoinRoomId;
int g_rejoinPlayerId = 0;

// 回房自动重试状态：REJOIN 被拒（游戏仍在进行中）后由主循环每 5 秒重发一次。
// 只在主线程访问（消息处理与主循环同一线程），无需加锁
bool g_rejoinRetrying = false;
string g_rejoinRetryRoomId;
int g_rejoinRetryPlayerId = 0;
chrono::steady_clock::time_point g_lastRejoinRetryTime;
int g_rejoinRetrySeconds = 5;   // main() 里可用环境变量 WOLF_REJOIN_RETRY_SECONDS 覆盖（测试注入）

string g_startIp = "127.0.0.1";        // 房间管理器地址
int g_startPort = 8888;

// 自动模式（§19.8，ADD USER 的开局自动窗口）：带 4 个命令行参数启动时置位，
// 跳过交互输入直接连接并自动 JOIN 指定房间；收 __GAME_OVER__ 后进程自动退出
bool g_autoMode = false;
string g_autoRoomPort;                 // 自动模式要加入的房间端口

// 游戏中继模式（§20.7）：游戏消息经房间管理器连接转发（GAME_FWD|<行>）而非
// 直连游戏端口。与直连互斥：g_relayMode=true 时 g_gameSock 保持 INVALID_SOCKET
atomic<bool> g_relayMode(false);

// 本局房间号（GAME_PREPARE 解析缓存）：中继申请/断线重连向 Start 指名房间用
string g_gameRoomId;

atomic<bool> g_switchingToGame(false); // 正在切换进游戏（抑制大厅断线提示）
atomic<bool> g_inputThreadRunning(true);
atomic<bool> g_inputThreadStarted(false);  // 输入线程是否已启动（PauseAndWait 依据）
atomic<bool> g_inputSolicited(false);  // 服务器已征求输入（__INPUT__），窗口已打开
atomic<bool> g_dayTalk(false);         // 白天自由发言连续输入模式（__DAY_OPEN__ 打开，__DAY_CLOSE__ 关闭）
bool g_promptDisplayed = true;

// 心跳保活控制。间隔常量 HEARTBEAT_INTERVAL_SECONDS 已在 common.h 统一
// 定义，此处不得重复定义（同一翻译单元重复定义 const 会编译报错 C2374）
atomic<bool> g_pingRunning(false);
thread g_pingThread;

// SendRaw/SendGameRaw 定义在下方"消息队列与发送"节，此处前置声明供心跳线程调用
void SendRaw(const string& msg);
void SendGameRaw(const string& msg);

// 心跳线程：每 HEARTBEAT_INTERVAL_SECONDS 秒向当前连接发一行 PING。
// 发送函数自身会跳过 INVALID_SOCKET，故连接切换/关闭时无需停线程
void PingThreadFunc()
{
    while (g_pingRunning)
    {
        Sleep(HEARTBEAT_INTERVAL_SECONDS * 1000);
        if (g_sock != INVALID_SOCKET) SendRaw("PING");
        if (g_gameSock != INVALID_SOCKET) SendGameRaw("PING");
        // 中继模式无直连 socket：游戏心跳必须显式走 GAME_FWD|PING 上行，
        // 否则 Server 会因长时间无字节判定本玩家失联（§20.7）
        if (g_relayMode && g_sock != INVALID_SOCKET) SendGameRaw("PING");
    }
}

// 启动心跳线程。ConnectToRoomManager 每次重连大厅都会调用本函数，必须先停
// 旧线程再启新线程：对仍 joinable 的 std::thread 重新赋值会触发 std::terminate
void StartPingThread()
{
    g_pingRunning = false;
    if (g_pingThread.joinable()) g_pingThread.join();
    g_pingRunning = true;
    g_pingThread = thread(PingThreadFunc);
}

// 输入门控：控制输入线程何时真正捕获玩家输入。
//   Open   - 大厅/房间提示符、游戏 __INPUT__：捕获一行并路由到对应队列
//   Closed - 游戏表演/打字机等：读取后丢弃（无回显），不捕获任何输入
//   Paused - 暂停等待期间：完全不读控制台，避免与暂停抢回车
enum class InputGate { Closed, Paused, Open };
atomic<InputGate> g_inputGate(InputGate::Closed);

// 输入线程的路由目标：游戏中 → g_gameCmdQueue，其余 → g_cmdQueue
// 用 deque：暂停需要只消费"回车产生的空行"并把非空输入留在队列里给 __INPUT__ 用
deque<string> g_cmdQueue;
mutex g_cmdMutex;
condition_variable g_cmdCV;

deque<string> g_gameCmdQueue;
mutex g_gameCmdMutex;
condition_variable g_gameCmdCV;

thread g_inputThread;
thread g_roomRecvThread;
thread g_gameRecvThread;

// ============ 消息队列与发送 ============

// 从共享队列取一条消息；timeoutMs<0 时无限等待。
bool PopMessage(string& out, int timeoutMs = -1)
{
    unique_lock<mutex> lock(g_mutex);

    if (timeoutMs < 0)
    {
        g_cv.wait(lock, [] { return !g_msgQueue.empty(); });
        out = g_msgQueue.front();
        g_msgQueue.pop();
        return true;
    }

    if (g_cv.wait_for(lock, chrono::milliseconds(timeoutMs), [] { return !g_msgQueue.empty(); }))
    {
        out = g_msgQueue.front();
        g_msgQueue.pop();
        return true;
    }

    return false;
}

// 按前缀丢弃消息队列中的残留，防止旧连接的"断开"消息与新连接重复触发重连。
void DropMessagesByPrefix(const string& prefix)
{
    lock_guard<mutex> lock(g_mutex);

    queue<string> kept;

    while (!g_msgQueue.empty())
    {
        string m = g_msgQueue.front();
        g_msgQueue.pop();
        if (m.find(prefix) != 0) kept.push(m);
    }

    g_msgQueue.swap(kept);
}

// 接收线程的回调：把一行原文入队交给主线程分发。
void RecvHandler(const string& line)
{
    lock_guard<mutex> lock(g_mutex);
    g_msgQueue.push(line);
    g_cv.notify_one();
}

void SendRaw(const string& msg)
{
    if (g_sock == INVALID_SOCKET) return;

    string out = msg + "\n";

    int total = 0;

    while (total < (int)out.length())
    {
        int sent = send(g_sock, out.c_str() + total, (int)out.length() - total, 0);

        if (sent <= 0) return;

        total += sent;
    }
}

void SendGameRaw(const string& msg)
{
    // 中继模式：游戏消息经 Start 转发（GAME_FWD|<行>），不直连游戏端口
    if (g_relayMode)
    {
        if (g_sock != INVALID_SOCKET) SendRaw("GAME_FWD|" + msg);
        return;
    }

    if (g_gameSock == INVALID_SOCKET) return;

    string out = msg + "\n";

    int total = 0;

    while (total < (int)out.length())
    {
        int sent = send(g_gameSock, out.c_str() + total, (int)out.length() - total, 0);

        if (sent <= 0)
        {
            ClientLog("GAME_SEND_FAIL errno=" + to_string(WSAGetLastError()));
            return;
        }

        total += sent;
    }
}

// 游戏连接读超时（毫秒）。正常对局中 Server 对每 3 秒的 PING 应答一行
// PING，客户端每 3 秒必有字节到达，15 秒远超心跳周期不会误判静默；对端
// 已死/网络中断（半开连接，无 FIN/RST）时收不到应答，recv 超时返回
// WSAETIMEDOUT → ReceiveLines 失败 → __CONN_LOST__ 触发重连流程，
// 修复"断线后客户端永久卡死无感知"（2026-08-07 稳定性修复）。
// 大厅连接不设此超时：Start 不回 PING 应答，静默期会误判
const int GAME_RECV_TIMEOUT_MS = 15000;

void SetGameRecvTimeout(SOCKET s)
{
    if (s == INVALID_SOCKET) return;

    DWORD timeout = GAME_RECV_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
}

// ============ 控制台 ============

// 有提示符挂着时先换行，避免把消息追加到提示符后面
void EnsureNewLine()
{
    if (g_promptDisplayed)
    {
        cout << endl;
        g_promptDisplayed = false;
    }
}

// 大厅/房间的输入提示符（游戏内不用提示符，由 __INPUT__ 窗口控制）。
// 只显示位置，不附加"输入 HELP"之类的话术，保持干净。
void ShowPrompt()
{
    if (g_promptDisplayed) return;

    const char* prompt = g_enMode
        ? (g_inRoom ? "[Room]> " : "[Lobby]> ")
        : (g_inRoom ? "[房间]> " : "[大厅]> ");

    cout << prompt << flush;
    g_promptDisplayed = true;
    ShowCursor(true);
}

// 切换输入门控并同步控制台输入模式（调用方：主线程）。
// Open 时行输入+回显；先设控制台模式、后置门控标志，
// 避免"门开了但模式还是原始模式"导致 ReadConsoleW 不等回车直接返回。
void SetInputGate(InputGate gate)
{
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;

    if (GetConsoleMode(hIn, &mode))
    {
        mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
        mode |= ENABLE_PROCESSED_INPUT;

        if (gate == InputGate::Open)
        {
            mode |= ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT;
        }

        SetConsoleMode(hIn, mode);
    }

    g_inputGate.store(gate);
}

// 控制台输入缓冲中是否存在真正的字符输入。
// 焦点变化/窗口大小变化等非字符事件也会让输入句柄变为有信号，
// 直接 ReadConsoleW 会被它们唤醒后一直等一个完整行（线程卡住）；
// 调用方需要先判定确有字符记录才读行。
bool HasCharInput(HANDLE hIn)
{
    INPUT_RECORD recs[16];
    DWORD n = 0;

    if (!PeekConsoleInput(hIn, recs, 16, &n) || n == 0) return false;

    for (DWORD i = 0; i < n; ++i)
    {
        if (recs[i].EventType == KEY_EVENT
            && recs[i].Event.KeyEvent.bKeyDown
            && recs[i].Event.KeyEvent.uChar.UnicodeChar != 0)
        {
            return true;
        }
    }

    return false;
}

// 直接读控制台事件等待一个按键（不派生子进程，按任意键一次即继续）。
void PauseWaitConsole()
{
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    INPUT_RECORD rec;

    while (true)
    {
        DWORD n = 0;

        if (!ReadConsoleInputW(hIn, &rec, 1, &n) || n == 0)
        {
            Sleep(20);
            continue;
        }

        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) break;
    }
}

// 打印暂停提示并等待任意键。暂停期间输入线程（Paused）完全不读控制台，
// 主线程直接用 PauseWaitConsole 读控制台事件；前后 flush 掉残留击键，
// 防止暂停被瞬间跳过或暂停期间键入的内容污染之后的输入。
void PauseAndWait()
{
    cout << "\n" << Txt(CurLang(), "[ 暂停 ]", "[ Pause ]") << "\n";

    // 输入线程未启动（启动阶段连接失败退出路径）：无并发读，直接读事件
    if (!g_inputThreadStarted)
    {
        PauseWaitConsole();
        return;
    }

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);

    ShowCursor(true);
    SetInputGate(InputGate::Paused);
    FlushConsoleInputBuffer(hIn);

    PauseWaitConsole();
    FlushConsoleInputBuffer(hIn);

    SetInputGate(InputGate::Open);
    ShowCursor(true);
}

// ============ 宽字符转换 ============

// 宽字符 → UTF-8（ReadConsoleW 读取结果转换用）。
string WideToUtf8(const wstring& w)
{
    if (w.empty()) return "";

    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);

    if (len <= 0) return "";

    string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], len, nullptr, nullptr);
    return out;
}

// 控制台读一行（宽字符 + IME 中文输入正常，回车结束）。
// 不用 std::cin：cin 走 CRT 文本模式读，与暂停等待抢控制台输入，
// 且中文 IME 输入在 UTF-8 代码页下不可靠；ReadConsoleW 直接读宽字符。
string ReadConsoleUtf8Line()
{
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    wchar_t buf[1024];
    DWORD n = 0;

    if (!ReadConsoleW(hIn, buf, 1023, &n, nullptr)) return "";
    if (n == 0) return "";

    while (n > 0 && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n')) --n;

    return WideToUtf8(wstring(buf, n));
}

// ============ 输入线程 ============

// 输入线程：唯一读取控制台输入的线程，受 g_inputGate 驱动。
//   Open   - 捕获一行输入并路由（游戏中 → g_gameCmdQueue，其余 → g_cmdQueue）
//   Closed - 读走并丢弃缓冲（游戏演出期间；无回显，不捕获任何输入）。
//            必须清空缓冲，否则下次开门时残留击键会被当成游戏选择
//   Paused - 完全不读控制台（暂停独占输入，读走会让暂停等不到按键）
// 控制台输入句柄可等待（有输入时变为有信号），用 50ms 轮询保证退出及时。
void InputThreadFunc()
{
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    g_inputThreadStarted = true;

    while (g_inputThreadRunning && g_running)
    {
        if (WaitForSingleObject(hIn, 50) != WAIT_OBJECT_0) continue;

        // 唤醒后重新读取门控：主线程可能刚切换过门控（如 __PAUSE__ 刚置 Paused），
        // 若用唤醒前读到的旧值，本线程会把暂停开始后注入的回车当成演出期残留吞掉
        InputGate gate = g_inputGate.load();

        // 暂停态：暂停等待读控制台，这里绝不读
        if (gate == InputGate::Paused) continue;

        // 关闭态（游戏演出）：读走并丢弃全部输入记录。
        // 用 ReadConsoleInput 而非 ReadConsoleW：ReadConsoleW 只返回字符记录，
        // 遇到 Shift 等无字符按键会被唤醒后阻塞；ReadConsoleInput 返回所有记录。
        // 先 PeekConsoleInput 探测：只有确有记录才读，绝不在 ReadConsoleInput
        // 上长期阻塞——否则门控切换后本线程仍卡在读取里，既吞暂停回车，又取不到新输入。
        if (gate == InputGate::Closed)
        {
            INPUT_RECORD recs[32];
            DWORD peek = 0;

            if (!PeekConsoleInput(hIn, recs, 32, &peek) || peek == 0) continue;

            // 探测与读取之间门控可能已切换为 Paused：绝不读，记录留给暂停等待
            if (g_inputGate.load() == InputGate::Paused) continue;

            DWORD n = 0;
            ReadConsoleInput(hIn, recs, 32, &n);
            continue;
        }

        // 开门态：先确认确有字符输入再读行。
        // 焦点变化等非字符事件也会让句柄有信号，直接 ReadConsoleW 会一直等整行
        if (!HasCharInput(hIn))
        {
            INPUT_RECORD recs[32];
            DWORD n = 0;
            ReadConsoleInput(hIn, recs, 32, &n);
            continue;
        }

        string input = ReadConsoleUtf8Line();

        if (!g_running || !g_inputThreadRunning) break;

        if (g_inGame)
        {
            // 游戏中：仅当服务器已征求输入（__INPUT__ 一次性 / __DAY_OPEN__ 连续）
            // 时才入队，演出/等待期间键入的直接丢弃，防止击键错位到下一次选择
            if (!g_inputSolicited && !g_dayTalk) continue;

            lock_guard<mutex> lock(g_gameCmdMutex);
            g_gameCmdQueue.push_back(input);
            g_gameCmdCV.notify_one();
        }
        else
        {
            lock_guard<mutex> lock(g_cmdMutex);
            g_cmdQueue.push_back(input);
            g_cmdCV.notify_one();
        }
    }

    g_inputThreadStarted = false;
}

// ============ 接收线程 ============

// 启动房间管理器接收线程。旧线程必须已退出（join）后再启动新的。
void StartRoomRecvThread()
{
    if (g_roomRecvThread.joinable()) g_roomRecvThread.join();

    g_roomRecvThread = thread([]()
    {
        string buffer;

        while (g_running)
        {
            if (g_sock == INVALID_SOCKET)
            {
                this_thread::sleep_for(chrono::milliseconds(100));
                continue;
            }

            if (!ReceiveLines(g_sock, buffer, RecvHandler))
            {
                // 大厅连接断开：非切换进游戏过程中 → 通知主线程处理重连
                if (g_running && !g_switchingToGame)
                {
                    lock_guard<mutex> lock(g_mutex);
                    g_msgQueue.push("DISCONNECTED");
                    g_cv.notify_one();
                }

                break;
            }
        }
    });
}

// 启动游戏服务器接收线程。旧线程必须已退出（join）后再启动新的。
void StartGameRecvThread()
{
    if (g_gameRecvThread.joinable()) g_gameRecvThread.join();

    g_gameRecvThread = thread([]()
    {
        string buffer;
        ClientLog("GAME_RECV_START");

        while (g_running && g_inGame)
        {
            if (g_gameSock == INVALID_SOCKET)
            {
                this_thread::sleep_for(chrono::milliseconds(50));
                continue;
            }

            if (!ReceiveLines(g_gameSock, buffer, RecvHandler))
            {
                ClientLog("GAME_RECV_FAIL errno=" + to_string(WSAGetLastError()));

                // 游戏连接断开 → 通知主线程进入重连流程
                if (g_inGame)
                {
                    lock_guard<mutex> lock(g_mutex);
                    g_msgQueue.push("__CONN_LOST__");
                    g_cv.notify_one();
                }

                break;
            }
        }
    });
}

// ============ 连接大厅 ============

// 连接房间管理器（大厅）。失败时重试 3 次；仍失败则暂停并退出程序。
// 成功时自动发送 HELLO/NAME 并启动接收线程。
bool ConnectToRoomManager()
{
    const int maxTries = 3;

    for (int attempt = 1; attempt <= maxTries; ++attempt)
    {
        if (attempt > 1)
        {
            cout << FmtLang(CurLang(), "连接房间服务器失败（第 %d 次），5 秒后重试 ...", "Connect to lobby failed (try %d), retry in 5s ...", attempt - 1) << endl;
            Sleep(5000);
        }

        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);

        if (s == INVALID_SOCKET) continue;

        sockaddr_in addr;
        addr.sin_family = AF_INET;
        inet_pton(AF_INET, g_startIp.c_str(), &addr.sin_addr);
        addr.sin_port = htons(g_startPort);

        if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0)
        {
            closesocket(s);
            continue;
        }

        // 先等旧接收线程退出，再替换 g_sock，避免旧线程误读新连接
        if (g_roomRecvThread.joinable()) g_roomRecvThread.join();

        g_sock = s;

        SendRaw("HELLO|3");
        if (g_enMode) SendRaw("LANG|en");
        else SendRaw("LANG|zh");
        SendRaw("NAME|" + g_playerName);

    // 游戏刚结束：先告知 RM 本局已结束（重置房间状态），再请求回到原房间。
    // 顺序必须 GAME_ENDED 在前：REJOIN 要求房间 gameStarted=false。
    // 此处不能清空 g_rejoinRoomId/g_rejoinPlayerId：被拒（游戏仍在进行中）时
    // 还需要目标来启动自动重试；成功/终态失败由消息处理处清空
    if (!g_rejoinRoomId.empty() && g_rejoinPlayerId > 0)
    {
        SendRaw("GAME_ENDED|" + g_rejoinRoomId);
        SendRaw("REJOIN|" + g_rejoinRoomId + "|" + to_string(g_rejoinPlayerId));
    }

    StartRoomRecvThread();

    // 启动心跳线程（PING 保活）：内部先 join 旧线程再重启，重连大厅安全
    StartPingThread();

    return true;
}

    cout << Txt(CurLang(), "无法连接房间服务器，程序退出。", "Cannot connect to lobby, exiting.") << endl;

    // 自动模式（ADD USER 自动窗口）失败时直接退出关闭窗口，不等待按键
    if (!g_autoMode) PauseAndWait();

    g_running = false;
    return false;
}

// 大厅断线后的统一处理：关闭失效连接、清空房间状态、重连大厅。
// 连接成功后把残留的"断开"消息从队列中清掉，防止重复触发重连。
void HandleLobbyDisconnect()
{
    EnsureNewLine();
    cout << Txt(CurLang(), "与房间服务器的连接断开，正在重新连接 ...", "Lobby connection lost, reconnecting ...") << endl;

    if (g_sock != INVALID_SOCKET)
    {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }

    if (g_roomRecvThread.joinable()) g_roomRecvThread.join();

    g_inRoom = false;
    g_isAdmin = false;
    g_roomId = "";
    g_promptDisplayed = false;

    ConnectToRoomManager();

    if (!g_running) return;

    // 旧连接断开瞬间接收线程可能已把 DISCONNECTED 入队，与新连接重复触发重连
    DropMessagesByPrefix("DISCONNECTED");

    g_promptDisplayed = false;
    SetInputGate(InputGate::Open);

    if (g_running) ShowPrompt();
}

// ============ 游戏连接处理 ============

// 尽力通知游戏服务器"放弃重连"（连接一次、发送即走，结果无关紧要）。
// 中继模式改经 Start 转发：直连可能本来就失败，靠 GAME_FWD 上行才送得到
void SendGiveUp()
{
    if (g_relayMode)
    {
        if (g_sock != INVALID_SOCKET) SendRaw("GAME_FWD|GIVEUP|" + to_string(g_myGamePlayerId));
        return;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);

    if (s == INVALID_SOCKET) return;

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, g_gameServerIp.c_str(), &addr.sin_addr);
    addr.sin_port = htons(g_gameServerPort);

    if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0)
    {
        string msg = "GIVEUP|" + to_string(g_myGamePlayerId) + "\n";
        send(s, msg.c_str(), msg.length(), 0);
    }

    closesocket(s);
}

// 是否强制走中继（§20.7）：环境变量 WOLF_FORCE_PROXY=1 时跳过直连尝试，
// 游戏消息一律经房间管理器 Start 转发。仅在自动化测试注入
bool IsForceProxyEnv()
{
    const char* env = getenv("WOLF_FORCE_PROXY");
    return env != nullptr && strcmp(env, "1") == 0;
}

// 直连游戏服务器（GAME_PREPARE 路径）：最多 5 次（间隔 1 秒）。
// 成功返回 true 并完成：g_inGame 置位、读超时、PLAYER_ID 握手、启动游戏
// 接收线程；失败返回 false（g_gameSock 已清理，调用方改走中继或回房）
bool TryConnectGameDirect()
{
    g_gameSock = socket(AF_INET, SOCK_STREAM, 0);

    if (g_gameSock == INVALID_SOCKET)
    {
        ClientLog("Failed to create game socket");
        return false;
    }

    sockaddr_in gameAddr;
    gameAddr.sin_family = AF_INET;
    inet_pton(AF_INET, g_gameServerIp.c_str(), &gameAddr.sin_addr);
    gameAddr.sin_port = htons(g_gameServerPort);

    // 连接游戏服务器：带重试。失败的 connect 会使套接字失效，必须重建后重试
    const int connectTries = 5;
    bool connected = false;

    for (int attempt = 1; attempt <= connectTries && !connected; ++attempt)
    {
        if (attempt > 1)
        {
            Sleep(1000);

            if (g_gameSock != INVALID_SOCKET)
            {
                closesocket(g_gameSock);
                g_gameSock = INVALID_SOCKET;
            }

            g_gameSock = socket(AF_INET, SOCK_STREAM, 0);

            if (g_gameSock == INVALID_SOCKET)
            {
                ClientLog("Failed to recreate game socket");
                break;
            }
        }

        if (connect(g_gameSock, (sockaddr*)&gameAddr, sizeof(gameAddr)) == 0)
        {
            connected = true;
            break;
        }

        ClientLog("Game connect attempt " + to_string(attempt) + " failed, errno=" + to_string(WSAGetLastError()));
    }

    if (!connected)
    {
        if (g_gameSock != INVALID_SOCKET)
        {
            closesocket(g_gameSock);
            g_gameSock = INVALID_SOCKET;
        }

        return false;
    }

    g_inGame = true;

    // 读超时：半开死连检测（详见 SetGameRecvTimeout 说明）
    SetGameRecvTimeout(g_gameSock);

    string idMsg = "PLAYER_ID|" + to_string(g_myGamePlayerId) + "\n";
    send(g_gameSock, idMsg.c_str(), idMsg.length(), 0);

    ClientLog("Connected to game server, player ID: " + to_string(g_myGamePlayerId));

    StartGameRecvThread();

    return true;
}

// 等待 Start 对 PROXY_GAME 的应答（PROXY_OK|<房间号> 或 PROXY_FAIL|<原因>）。
// 只消费 PROXY_* 行；等待期间的其他行（中继建立前的零星流量）直接丢弃，
// 不打扰游戏状态；大厅断线则重连并返回失败。房接收线程与主线程同源走
// 消息队列，主线程独占消费无并发冲突
bool WaitProxyResponse(string& out, int timeoutSeconds)
{
    auto deadline = chrono::steady_clock::now() + chrono::seconds(timeoutSeconds);

    while (g_running)
    {
        long long remainMs = chrono::duration_cast<chrono::milliseconds>(deadline - chrono::steady_clock::now()).count();

        if (remainMs <= 0) return false;

        string m;

        if (PopMessage(m, (int)min<long long>(remainMs, 200)))
        {
            if (m.find("PROXY_OK|") == 0 || m.find("PROXY_FAIL|") == 0)
            {
                out = m;
                return true;
            }

            if (m == "DISCONNECTED")
            {
                // 大厅连接断了：先重连，由下一次尝试重发 PROXY_GAME
                HandleLobbyDisconnect();
                return false;
            }

            // 其他行：中继建立前的零星流量，丢弃
        }
    }

    return false;
}

// 经房间管理器 Start 中继进入游戏（§20.7）：不直连游戏端口，把大厅连接
// 变成游戏消息通道。PROXY_GAME|<roomId>|<playerId> → PROXY_OK 进入中继
// 模式（此后"发往游戏服务器"的写一律改为 GAME_FWD|<行>）；PROXY_FAIL
// 显示原因后重试（Start 可能在等游戏服务器就绪，稍后同请求可能成功）；
// 超时/断线也计一次失败。最多重试 3 次（间隔 2 秒），成功返回 true 并置
// g_relayMode/g_switchingToGame=false（大厅断线须能上报），否则返回 false
// （调用方回落大厅既有路径）
bool StartGameProxy(const string& roomId, int playerId)
{
    const int maxTries = 3;

    for (int attempt = 1; attempt <= maxTries; ++attempt)
    {
        if (attempt > 1)
        {
            cout << FmtLang(CurLang(), "  中继连接失败（第 %d 次），2 秒后重试 ...", "  Relay connect failed (try %d), retry in 2s ...", attempt - 1) << endl;
            Sleep(2000);
        }

        // 大厅连接是中继的唯一物理通道：失效则先重连
        if (g_sock == INVALID_SOCKET)
        {
            HandleLobbyDisconnect();

            if (g_sock == INVALID_SOCKET || !g_running) return false;
        }

        SendRaw("PROXY_GAME|" + roomId + "|" + to_string(playerId));
        ClientLog("PROXY_GAME|" + roomId + "|" + to_string(playerId));

        string resp;

        if (!WaitProxyResponse(resp, 10))
        {
            cout << Txt(CurLang(), "  等待中继应答超时。", "  Timeout waiting for the relay reply.") << endl;
            continue;
        }

        if (resp.find("PROXY_OK|") == 0)
        {
            // 中继建立：进入中继模式。PLAYER_ID 认领槽位也走 GAME_FWD 上行
            // （Start 的 GAME_FWD 注解明确 PLAYER_ID 认领走中继通道）
            g_relayMode = true;
            g_switchingToGame = false;

            SendGameRaw("PLAYER_ID|" + to_string(playerId));

            ClientLog("PROXY_OK for room " + roomId);
            return true;
        }

        // PROXY_FAIL|<原因>：显示中文原因后继续重试（最多 3 次）
        string reason = resp.substr(11);

        if (reason.empty()) reason = Txt(CurLang(), "未知原因", "unknown reason");

        EnsureNewLine();
        cout << FmtLang(CurLang(), "中继被拒绝：%s", "Relay rejected: %s", reason.c_str()) << endl;
    }

    return false;
}

// 结束游戏状态，关闭游戏连接，回到大厅（房间管理器连接会自动重建）。
void ReturnToRoom()
{
    ClientLog("ReturnedToRoom");

    // 记录待回房信息（房间保留可再开一局）：重连 RM 后自动 GAME_ENDED + REJOIN。
    // 此时 g_roomId 仍是本局房间号（GAME_PREPARE 时只清 inRoom 不清 roomId）。
    g_rejoinRoomId = g_roomId;
    g_rejoinPlayerId = g_myGamePlayerId;

    g_inGame = false;
    g_switchingToGame = false;
    g_inputSolicited = false;
    // 游戏状态整体收尾：白天连续输入状态一并复位，避免下一局残留
    g_dayTalk = false;

    // 清掉游戏输入队列残留，避免回房后被误当命令发送
    {
        lock_guard<mutex> lock(g_gameCmdMutex);
        g_gameCmdQueue.clear();
    }

    // 清掉大厅命令队列残留，避免回房后被误当命令发送
    {
        lock_guard<mutex> lock(g_cmdMutex);
        g_cmdQueue.clear();
    }

    // 丢弃控制台输入缓冲中未读的击键（含游戏中键入但没回车的半截行）
    FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));

    // 先置回房状态再关套接字，避免接收线程在关闭瞬间误推 __CONN_LOST__
    if (g_gameSock != INVALID_SOCKET)
    {
        closesocket(g_gameSock);
        g_gameSock = INVALID_SOCKET;
    }

    if (g_gameRecvThread.joinable()) g_gameRecvThread.join();

    // 中继模式：游戏消息经大厅连接收发，回房前必须先关掉该连接再重建——
    // 否则 ConnectToRoomManager 的 join 会卡在活 socket 的 recv 上（死锁）
    if (g_relayMode)
    {
        g_relayMode = false;

        if (g_sock != INVALID_SOCKET)
        {
            closesocket(g_sock);
            g_sock = INVALID_SOCKET;
        }

        if (g_roomRecvThread.joinable()) g_roomRecvThread.join();
    }

    g_inRoom = false;
    g_isAdmin = false;
    g_roomId = "";
    g_myGamePlayerId = 0;

    g_promptDisplayed = false;
    ConnectToRoomManager();

    if (!g_running) return;

    DropMessagesByPrefix("DISCONNECTED");

    g_promptDisplayed = false;
    SetInputGate(InputGate::Open);

    if (g_running) ShowPrompt();
}

// 开始回房自动重试：保存重试目标并提示用户（只提示一次，之后由主循环静默重发）
void StartRejoinRetry(const string& roomId, int playerId)
{
    g_rejoinRetrying = true;
    g_rejoinRetryRoomId = roomId;
    g_rejoinRetryPlayerId = playerId;
    g_lastRejoinRetryTime = chrono::steady_clock::now();

    EnsureNewLine();
    cout << Txt(CurLang(), "游戏仍在进行中，稍后自动重试回房", "Game still in progress; will retry returning soon") << endl;
}

// 停止回房自动重试：回房成功、终态失败（房间不存在/已拉黑/已满）或玩家另建/另入房间时调用
void StopRejoinRetry()
{
    g_rejoinRetrying = false;
    g_rejoinRetryRoomId.clear();
    g_rejoinRetryPlayerId = 0;
}

// 重连握手确认读取：发出 PLAYER_ID 后等待服务端第一行响应（带短超时）。
// 只在重连尝试窗口内被主线程调用——此时游戏接收线程已 join、g_gameSock
// 仍为 INVALID_SOCKET（心跳线程不会往新套接字发 PING），select+recv 独占
// 读取该临时套接字，无并发冲突；返回 true 表示收满一行（内容写入 out）。
bool WaitGameAckLine(SOCKET s, string& out, int timeoutSeconds)
{
    string buf;
    auto deadline = chrono::steady_clock::now() + chrono::seconds(timeoutSeconds);

    while (true)
    {
        long long remainMs = chrono::duration_cast<chrono::milliseconds>(deadline - chrono::steady_clock::now()).count();

        if (remainMs <= 0) return false;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);

        timeval tv;
        tv.tv_sec = (long)(remainMs / 1000);
        tv.tv_usec = (long)((remainMs % 1000) * 1000);

        // 可读才 recv：没有数据时不阻塞等待，把剩余时间留给下一次 select
        if (select(0, &rfds, nullptr, nullptr, &tv) <= 0) return false;

        char cbuf[256];
        int n = recv(s, cbuf, sizeof(cbuf), 0);

        if (n <= 0) return false;   // EOF 或出错

        buf.append(cbuf, n);

        size_t nl = buf.find('\n');

        if (nl != string::npos)
        {
            out = buf.substr(0, nl);
            return true;
        }
    }
}

// 游戏连接中断后的重连流程（主线程调用，可能阻塞 10~20 秒）。
// 最多尝试 3 次直连（间隔 5 秒）；全部失败（或 WOLF_FORCE_PROXY 强制）→
// 改经 Start 中继；中继也失败 → GIVEUP 并返回大厅
void HandleGameReconnect()
{
    const int maxTries = 3;
    bool forceProxy = IsForceProxyEnv();

    EnsureNewLine();
    cout << Txt(CurLang(), "与游戏服务器的连接断开，正在尝试重连 ...", "Game connection lost, reconnecting ...") << endl;

    // 中继残留防御性清理：中继模式下本就没有直连 socket/接收线程
    if (g_gameSock != INVALID_SOCKET)
    {
        closesocket(g_gameSock);
        g_gameSock = INVALID_SOCKET;
    }

    if (g_gameRecvThread.joinable()) g_gameRecvThread.join();

    if (!forceProxy)
    {
        for (int attempt = 1; attempt <= maxTries; ++attempt)
        {
            if (attempt > 1)
            {
                cout << FmtLang(CurLang(), "  第 %d 次重连失败，5 秒后重试 ...", "  Retry %d failed, waiting 5s ...", attempt - 1) << endl;
                Sleep(5000);
            }

            cout << FmtLang(CurLang(), "  正在尝试重连 %d/%d ...", "  Reconnecting %d/%d ...", attempt, maxTries) << endl;

            SOCKET s = socket(AF_INET, SOCK_STREAM, 0);

            if (s == INVALID_SOCKET) continue;

            sockaddr_in addr;
            addr.sin_family = AF_INET;
            inet_pton(AF_INET, g_gameServerIp.c_str(), &addr.sin_addr);
            addr.sin_port = htons(g_gameServerPort);

            if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0)
            {
                closesocket(s);
                continue;
            }

            // 重连套接字同样设读超时：半开死连检测（同 GAME_PREPARE 路径）
            SetGameRecvTimeout(s);

            // 重连握手：先告知服务器我们的玩家编号，再等待其确认行。
            // 任何一行都视为重连成功；"already connected"（被踢）或
            // 超时/EOF 计一次失败，杜绝旧的"连上即成功→被踢→再连"假成功循环。
            // 确认等待期间 g_gameSock 仍是 INVALID_SOCKET：心跳线程不会往
            // 新套接字发 PING，游戏接收线程也未启动，主线程独占读取无并发冲突
            string idMsg = "PLAYER_ID|" + to_string(g_myGamePlayerId) + "\n";
            send(s, idMsg.c_str(), idMsg.length(), 0);

            const int ackTimeoutSeconds = 3;
            string ack;
            bool ackOk = false;

            if (WaitGameAckLine(s, ack, ackTimeoutSeconds))
            {
                if (ack.find("already connected") != string::npos)
                {
                    closesocket(s);
                    continue;
                }

                ackOk = true;
            }

            if (!ackOk)
            {
                closesocket(s);
                continue;
            }

            // 确认行就是服务端欢迎文本（如"你被分配到 N 号位。欢迎回来！"），
            // 已被本次读取消费不再入队，直接回显以免丢失
            if (!ack.empty()) cout << "  " << ack << endl;

            g_gameSock = s;
            StartGameRecvThread();

            cout << Txt(CurLang(), "  重连成功，游戏继续！", "  Reconnected, game continues!") << endl;
            return;
        }
    }

    // 直连全部失败（或强制中继）：尝试经 Start 中继重连
    if (StartGameProxy(g_gameRoomId, g_myGamePlayerId))
    {
        cout << Txt(CurLang(), "  经房间服务器中继重连成功，游戏继续！", "  Reconnected via lobby relay, game continues!") << endl;
        return;
    }

    // 直连与中继都失败：通知服务器放弃（服务器据此立即结束本局），回到大厅
    cout << Txt(CurLang(), "  重连失败，通知服务器结束本局并返回大厅 ...", "  Reconnect failed, ending game and returning to lobby ...") << endl;
    SendGiveUp();
    ReturnToRoom();
}

// ============ 游戏内输入 ============

// 取一行输入中命令字之后的参数部分（去首尾空白）。无参数返回空串。
string GetLineArgs(const string& line)
{
    size_t pos = line.find_first_of(" \t");

    if (pos == string::npos) return "";

    size_t p = line.find_first_not_of(" \t", pos);

    if (p == string::npos) return "";

    string args = line.substr(p);
    size_t end = args.find_last_not_of(" \t");

    if (end == string::npos) return "";

    return args.substr(0, end + 1);
}

// 字符串是否全部由数字组成（比例/投票等参数校验用）。
bool IsAllDigits(const string& s)
{
    if (s.empty()) return false;

    for (char c : s)
    {
        if (!isdigit((unsigned char)c)) return false;
    }

    return true;
}

// 把游戏内的一行输入包装成发给游戏服务器的完整协议行。
// VOTE/BOMB 是游戏命令，这里转换成 "PLAYER_<id>|VOTE|<n>" 的结构，
// 其余一律按聊天/回答原样发送（游戏服务器的等待输入窗口只在 __INPUT__ 时有效）。
string BuildGamePayload(const string& line)
{
    vector<string> tokens = SplitTokens(line);

    if (!tokens.empty())
    {
        // 英文版不匹配中文别名（§12.5）；alias 命中后 cmd->en 仍是全名（如 V→VOTE）
        const CommandEntry* cmd = FindCommand(tokens[0], !g_enMode);

        // LIST 是本地名单功能：服务器不处理（收到会当聊天），返回空串
        // 表示"不发送"，由 FlushGameInput 本地重放玩家名单（窗口保持）
        if (cmd != nullptr && _stricmp(cmd->en, "LIST") == 0)
        {
            return "";
        }

        if (cmd != nullptr && _stricmp(cmd->en, "VOTE") == 0)
        {
            string args = GetLineArgs(line);

            if (args.empty()) args = "0"; // 未指定投票目标按弃权处理

            return "PLAYER_" + to_string(g_myGamePlayerId) + "|VOTE|" + args;
        }

        if (cmd != nullptr && _stricmp(cmd->en, "BOMB") == 0)
        {
            return "PLAYER_" + to_string(g_myGamePlayerId) + "|BOMB|" + SanitizeChat(GetLineArgs(line));
        }
    }

    return "PLAYER_" + to_string(g_myGamePlayerId) + "|" + SanitizeChat(line);
}

// 把输入线程捕获到的游戏输入发给服务器（仅当输入窗口/白天对话打开时）。
// 都只发送第一行非空输入（误按回车产生的空行丢弃，窗口保持状态）：
//  __INPUT__ 一次性模式       - 窗口期内多敲的剩余行丢弃，发完即关窗；
//  __DAY_OPEN__ 白天对话模式  - 保留剩余行，发完保持窗口，供玩家连续聊天/投票。
void FlushGameInput()
{
    if (!g_inputSolicited && !g_dayTalk) return;

    // 快照本次发送模式：白天连续对话时 g_inputSolicited 恒为 false，
    // 遇到 __INPUT__ 恰在白天对话期间到达（两者同时为真）按一次性语义处理
    bool oneShot = g_inputSolicited;

    string input;

    {
        lock_guard<mutex> lock(g_gameCmdMutex);

        while (!g_gameCmdQueue.empty() && g_gameCmdQueue.front().empty())
        {
            g_gameCmdQueue.pop_front();
        }

        if (g_gameCmdQueue.empty()) return;

        input = g_gameCmdQueue.front();
        g_gameCmdQueue.pop_front();

        // 仅一次性模式丢弃剩余行（同一窗口期内多敲的），防止错位到下一次 __INPUT__；
        // 白天对话模式必须保留，逐行发送不走会丢词的连续发言
        if (oneShot) g_gameCmdQueue.clear();
    }

    string payload = BuildGamePayload(input);

    // 空串 = 本地命令（游戏内 LIST 名单重放）：只显示不发送，且保持当前
    // 输入窗口状态——白天发言继续聊、一次性 __INPUT__（如投票）窗口不浪费，
    // 玩家看完名单还能正常作答
    if (payload.empty())
    {
        ShowGamePlayerList();
        return;
    }

    SendGameRaw(payload);
    ClientLog("SENT:" + input);

    if (oneShot)
    {
        // 一次性输入已发出：关闭窗口并隐藏光标（游戏演出期间不再捕获输入）
        g_inputSolicited = false;
        SetInputGate(InputGate::Closed);
        ShowCursor(false);

        // 若 __INPUT__ 恰在白天对话期间到达，说明服务端白天窗口已结束，
        // 不得再连续发言，一并复位白天对话状态避免残留
        g_dayTalk = false;
    }
    // 白天对话模式：保持窗口与光标，等待下一行输入
}

// ============ 游戏消息处理 ============

bool ProcessGameMessage(string& msg)
{
    // 供测试 harness 观测：把收到的每条游戏消息写入 client.log
    ClientLog("GAME_MSG:" + msg);

    // 服务器对心跳 PING 的应答行：只作存活证明，直接忽略（不显示、不处理）
    if (IsPingLine(msg)) return true;

    // 游戏结束（服务器通知或连接彻底丢失）。
    // 终态广播文案中英文都可能出现，包含匹配（"本局结束"/"Game over"）
    // 作双保险：旧的精确匹配永不命中，是死代码（服务端实际文案是
    // "本局结束，即将返回房间。" / "Game over. Returning to the room."）
    if (msg.find("LEFT_GAME") == 0
        || msg.find("本局结束") != string::npos
        || msg.find("Game over") != string::npos)
    {
        EnsureNewLine();
        cout << Txt(CurLang(), "本局游戏结束。", "Game over.") << endl;
        ReturnToRoom();
        return true;
    }

    // 服务器征求输入。注意：只能"打开窗口"后立即返回，绝不能在这里阻塞等待！
    // 服务器在 __INPUT__ 之后紧跟菜单/提示文本，若阻塞，菜单要等输入后才显示。
    // 实际发送由主循环的 FlushGameInput() 完成：取输入线程捕获的第一行非空输入。
    if (msg == "__INPUT__")
    {
        g_inputSolicited = true;
        ShowCursor(true);
        SetInputGate(InputGate::Open);
        ClientLog("INPUT_OPEN");
        return true;
    }

    // 白天自由发言：服务端广播 __DAY_OPEN__ 打开连续输入窗口。
    // 与 __INPUT__ 一次性窗口不同，发送一行后窗口保持打开（见 FlushGameInput）
    if (msg == "__DAY_OPEN__")
    {
        g_dayTalk = true;
        g_inputSolicited = false;
        ShowCursor(true);
        SetInputGate(InputGate::Open);
        ClientLog("DAY_OPEN");
        return true;
    }

    // 白天发言窗口结束：复位连续输入状态并关闭输入门，
    // 防止表演/夜晚期间键入的内容错位到下一轮
    if (msg == "__DAY_CLOSE__")
    {
        g_dayTalk = false;
        SetInputGate(InputGate::Closed);
        ShowCursor(false);
        ClientLog("DAY_CLOSE");
        return true;
    }

    if (msg == "__CLS__")
    {
        ClearScreen();
        return true;
    }

    if (msg == "__PAUSE__")
    {
        // 配合输入门控：暂停期间输入线程（Paused）完全不读控制台，
        // 主线程直接读控制台事件等待按键，按任意键一次即继续。
        // 先清掉缓冲残留（暂停前的击键/半行），防止暂停被瞬间跳过。
        EnsureNewLine();
        cout << "\n" << Txt(CurLang(), "[ 暂停 ]", "[ Pause ]") << "\n";
        ShowCursor(true);
        SetInputGate(InputGate::Paused);

        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        FlushConsoleInputBuffer(hIn);

        PauseWaitConsole();
        FlushConsoleInputBuffer(hIn);

        // 恢复门控：暂停前若处于白天发言/服务器征求输入窗口则重新打开，
        // 否则保持演出期关闭——硬置 Closed 会让暂停后的输入永久锁死
        if (g_dayTalk || g_inputSolicited)
        {
            SetInputGate(InputGate::Open);
            ShowCursor(true);
        }
        else
        {
            SetInputGate(InputGate::Closed);
            ShowCursor(false);
        }
        return true;
    }

    if (msg.find("__PROGRESS__:") == 0)
    {
        int pct = atoi(msg.substr(13).c_str());

        // 防御：clamp 到 0-100，防止损坏/恶意消息导致 string(负数) 抛异常
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;

        cout << "\r[" << string(pct / 2, '=') << string(50 - pct / 2, ' ') << "] " << pct << "/100 " << flush;
        return true;
    }

    if (msg.find("__TYPEWRITER__") == 0)
    {
        cout << msg.substr(14);
        return true;
    }

    // 身份分配：服务器发 ROLE|<enName>，按自身语言展示职业名与详细介绍
    if (msg.find("ROLE|") == 0)
    {
        string enName = msg.substr(5);
        const JobDef* job = FindJob(enName);

        if (job != nullptr)
        {
            cout << Txt(CurLang(), "你的身份：", "Your role: ") << (g_enMode ? job->enName : job->zhName) << endl;
            cout << (g_enMode ? job->detailEn : job->detail) << endl;
        }
        else
        {
            cout << msg << endl;
        }

        return true;
    }

    // 结果消息（如验人结果）：本地只回显
    if (msg.find("RESULT|") == 0)
    {
        cout << msg.substr(7) << endl;
        return true;
    }

    // 玩家名单（开局广播一次）：按编号序缓存，显示给玩家；游戏内 LIST
    // 命令由输入处理处本地重放（ShowGamePlayerList），不发给游戏服务器
    // 行格式 PLAYER_LIST|总数|名1|...|名N：必须**先跳过头字段「总数」**，
    // 否则会把总数当第一个玩家名入队，缓存 N+1 项、名字整体错位 1 槽
    if (msg.find("PLAYER_LIST|") == 0)
    {
        string rest = msg.substr(12);
        g_gamePlayerNames.clear();
        size_t pos = rest.find('|');

        // 畸形行（总数后没有名字分隔符）直接丢弃，不缓存任何名字
        if (pos == string::npos) return true;

        int expected = atoi(rest.substr(0, pos).c_str());

        // 总数越界视为异常行丢弃，防止恶意/损坏行撑爆缓存
        if (expected < 0 || expected > MAX_PLAYERS) return true;

        rest.erase(0, pos + 1);

        // 只取前 expected 个名字：字段多了丢弃、少了取到哪算哪（防御 Server 异常行）
        for (int i = 0; i < expected; ++i)
        {
            pos = rest.find('|');

            if (pos == string::npos)
            {
                if (!rest.empty()) g_gamePlayerNames.push_back(rest);

                break;
            }

            g_gamePlayerNames.push_back(rest.substr(0, pos));
            rest.erase(0, pos + 1);
        }

        ShowGamePlayerList();
        return true;
    }

    // 其余：演出文本直接显示
    cout << msg << endl;
    return true;
}

// ============ 命令解析（双语，大厅/房间） ============

// UTF-8 字符串的显示宽度（ASCII 占 1 列，CJK 全角占 2 列）。
// 控制台等宽字体下汉字占 2 个单元格，列对齐必须按此计算，不能数字节。
int DisplayWidth(const string& s)
{
    int w = 0;

    for (unsigned char c : s)
    {
        w += (c >= 0x80) ? 2 : 1;
    }

    return w;
}

// 左对齐补齐到目标显示宽度（不足补空格），供帮助表格列对齐用。
string PadTo(const string& s, int width)
{
    string out = s;
    int pad = width - DisplayWidth(s);

    while (pad-- > 0) out += ' ';

    return out;
}

// 显示本局玩家名单（ID | NAME，自己槽位追加"你"标记）。
// 数据源：开局 PLAYER_LIST| 广播缓存的 g_gamePlayerNames（编号序）。
// 游戏内输入 LIST 时由输入处理处本地重放（服务器不处理 LIST 命令）。
void ShowGamePlayerList()
{
    EnsureNewLine();

    // 名单宽度上限：最长名字的显示列数（名字最多 10 码点，全角计 2 列）
    int nameW = 4;

    for (const string& n : g_gamePlayerNames)
    {
        nameW = max(nameW, DisplayWidth(n));
    }

    cout << "ID | " << PadTo("NAME", nameW) << endl;

    for (size_t i = 0; i < g_gamePlayerNames.size(); ++i)
    {
        string name = g_gamePlayerNames[i];

        // 自己槽位加标记，方便对位（编号即槽位，与投票编号一致）
        if ((int)(i + 1) == g_myGamePlayerId) name += Txt(CurLang(), "（你）", " (you)");

        // 编号右对齐 2 列，与房间 STATUS 表格风格一致
        char idBuf[8];
        sprintf(idBuf, "%2d", (int)(i + 1));
        cout << idBuf << " | " << name << endl;
    }
}

// 输出命令帮助：按分组（通用/大厅/房间/游戏）排序。
// 中文版四列：英文/中文/别名/参数/说明；英文版三列：英文/别名/参数/说明。
// 别名列为短别名（VOTE→V 等，§11.2），无别名的命令该列留空；列宽全表统一计算。
void ShowCommandHelp()
{
    int wEn = 0, wZh = 0, wAlias = 0, wArgs = 0;

    for (int i = 0; i < COMMAND_COUNT; ++i)
    {
        wEn = max(wEn, DisplayWidth(COMMANDS[i].en));
        wZh = max(wZh, DisplayWidth(COMMANDS[i].zh));
        wAlias = max(wAlias, DisplayWidth(COMMANDS[i].alias));
        wArgs = max(wArgs, DisplayWidth(COMMANDS[i].args));
    }

    const char* groupNames[4] = { "通用", "大厅", "房间", "游戏" };
    const char* groupNamesEn[4] = { "Common", "Lobby", "Room", "Game" };

    cout << Txt(CurLang(), "===== 命令说明 =====", "===== Commands =====") << endl;

    for (int g = 0; g < 4; ++g)
    {
        cout << (g_enMode ? groupNamesEn[g] : groupNames[g]) << ":" << endl;

        for (int i = 0; i < COMMAND_COUNT; ++i)
        {
            if (COMMANDS[i].group != g) continue;

            cout << "  " << PadTo(COMMANDS[i].en, wEn + 2);

            // 中文版多一列中文别名；英文版不显示中文列（§11.5）
            if (!g_enMode)
            {
                cout << PadTo(COMMANDS[i].zh, wZh + 2);
            }

            cout << PadTo(COMMANDS[i].alias, wAlias + 2)
                 << PadTo(COMMANDS[i].args, wArgs + 2)
                 << (g_enMode ? COMMANDS[i].descEn : COMMANDS[i].desc) << endl;
        }
    }

    cout << Txt(CurLang(), "注：【房主】命令仅房主可用；房间命令需先进入房间。", "Note: [Host] commands are host-only; room commands need a room.") << endl;
    cout << Txt(CurLang(), "HELP ALL 查看全部职业列表。", "HELP ALL lists all roles.") << endl;
    cout << "===================" << endl;
}

// 输出全部职业列表：按阵营分组排序。中文版中文名/英文名两列对齐；
// 英文版只显示英文名与英文介绍，避免混入中文（§11.5）。
void ShowAllJobsHelp()
{
    const char* campNames[4] = { "狼人阵营", "中立阵营", "神职阵营", "好人阵营" };
    const char* campNamesEn[4] = { "Wolf camp", "Neutral camp", "God camp", "Good side" };
    int wZh = 0, wEn = 0;

    for (int i = 0; i < JOB_COUNT; ++i)
    {
        wZh = max(wZh, DisplayWidth(JOBS[i].zhName));
        wEn = max(wEn, DisplayWidth(JOBS[i].enName));
    }

    cout << Txt(CurLang(), "===== 职业列表 =====", "===== Roles =====") << endl;

    for (int c = 0; c < CAMP_COUNT; ++c)
    {
        cout << (g_enMode ? campNamesEn[c] : campNames[c]) << ":" << endl;

        for (int i = 0; i < JOB_COUNT; ++i)
        {
            if (JOBS[i].camp != c) continue;

            if (g_enMode)
            {
                cout << "  " << PadTo(JOBS[i].enName, wEn + 2) << JOBS[i].detailEn << endl;
            }
            else
            {
                cout << "  "
                     << PadTo(JOBS[i].zhName, wZh + 2)
                     << PadTo(JOBS[i].enName, wEn + 2)
                     << JOBS[i].detail << endl;
            }
        }
    }

    cout << Txt(CurLang(), "HELP <职业名>（中/英）查看该职业详细介绍。", "HELP <role name> (en/zh) for details.") << endl;
    cout << "===================" << endl;
}

// ============ HELP 指令用法（§19.5） ============

// HELP <指令名> 的详细用法表：覆盖全部指令，含第九轮新增的 SHOW/LOOK/ADD
// （它们尚未进入 common.h 的 COMMANDS 命令表，仅在此描述用法）。
// zh/en 为双语用法说明，与客户端其他文本一样按自身语言取一条显示
struct HelpDetail
{
    const char* name;   // 指令英文名（查找时大小写不敏感）
    const char* zh;     // 中文用法说明
    const char* en;     // 英文用法说明
};

static const HelpDetail HELP_DETAILS[] = {
    { "HELP", "HELP [ALL|指令名|职业名]：无参数显示命令表；HELP ALL 显示全部职业列表；HELP <指令名> 显示指令用法；HELP <职业名> 显示职业介绍。中文别名「帮助」。",
               "HELP [ALL|command|role]: no arg shows the command list; HELP ALL lists all roles; HELP <command> shows its usage; HELP <role> shows role info. Chinese alias: 帮助." },
    { "SHOW", "SHOW <子项>：查看房间信息。子项：BAN 黑名单（房主）、RATIO 比例、LEVEL 档位、VILLAGER 村民开关、AUTO 自动开局、ADD 本地用户与 NPC（房主）、MUTE 禁言名单（房主）、NPCKEY AI key 配置状态。无参数或未知子项输出用法。与 LOOK 完全等效。",
               "SHOW <item>: view room info. Items: BAN blacklist (host), RATIO ratio, LEVEL role level, VILLAGER toggle, AUTO auto-start, ADD local users and NPCs (host), MUTE mute list (host), NPCKEY AI key config status. No arg or unknown item prints usage. Fully equivalent to LOOK." },
    { "LOOK", "LOOK <子项>：查看房间信息，与 SHOW 完全等效（用法见 HELP SHOW）。",
               "LOOK <item>: view room info, fully equivalent to SHOW (see HELP SHOW)." },
    { "ADD", "ADD USER <username> [-u] <玩家名或槽位>：添加本地用户占槽位（开局自动开窗进游戏；无 -u 默认由房主控制，-u 指定控制者）。ADD NPC [NPCname] on|off：添加 NPC（on 在线 AI / off 离线逻辑）。【房主】【大厅可用】",
               "ADD USER <username> [-u] <player or slot>: add a local user taking a slot (a client window auto-launches at game start; control defaults to the host, -u picks the controller). ADD NPC [NPCname] on|off: add an NPC (on=online AI, off=offline logic). [Host][Lobby]" },
    { "UNADD", "UNADD <槽号/名字/...>（短别名 UA，* 移除全部）：移除 NPC 或本地用户槽，真人玩家请用 PICK；空格分隔多项；游戏进行中不可用，游戏结束后允许。【房主】",
               "UNADD <slot/name/...> (alias UA, * removes all): remove NPC or local-user slots; use PICK for real players; space-separated; unavailable during a game, allowed after it ends. [Host]" },
    { "BAN", "BAN <名字/IP/通配模式或 .ban 文件>：拉黑玩家或 IP，命中即拒绝入房并踢出房内同匹配者。支持 *（任意位数）与 ?（1 位）通配（全角 ＊？等效）；空格分隔批量；.ban 文件逐行导入。【房主】",
               "BAN <name/IP/wildcard or .ban file>: ban players or IPs; matched joins are rejected and in-room matches are kicked. Wildcards * (any digits) and ? (one digit), full-width ＊？equivalent; space-separated batch; .ban files import line by line. [Host]" },
    { "UNBAN", "UNBAN <名字/IP/通配模式或 .ban 文件>：取消拉黑，按模式串精确删除；批量与 .ban 文件导入同 BAN。【房主】",
                 "UNBAN <name/IP/wildcard or .ban file>: remove bans, exact pattern-string match; batch and .ban file import same as BAN. [Host]" },
    { "MUTE", "MUTE <槽号/名字/通配模式/ALL>...：禁言玩家，空格分隔多项。被禁言者的聊天不会广播（命令照常可用）；ALL 禁言全部（含今后加入者）；离房/被踢自动解除精确名项。【房主】",
              "MUTE <slot/name/wildcard/ALL>...: mute players, space-separated. Muted players' chat is not broadcast (commands still work); ALL mutes everyone including future joiners; exact-name entries auto-lift on leave/kick. [Host]" },
    { "UNMUTE", "UNMUTE <名字/通配模式/ALL>...：解除禁言，空格分隔多项；ALL 清空整个禁言名单。【房主】",
                "UNMUTE <name/wildcard/ALL>...: lift mutes, space-separated; ALL clears the whole mute list. [Host]" },
    { "START", "START：全员准备后开始游戏。START /F（或 /FORCE）强制开局：跳过全员准备检查，比例不符时自动设置合理组合并直接采用。【房主】",
                 "START: start the game when all players are ready. START /F (or /FORCE) forces the start: skips the ready check; a mismatched ratio is auto-adjusted and applied directly. [Host]" },
    { "AUTO", "AUTO：切换「全员准备自动开局」开关，开启后全员准备即自动开始。【房主】",
               "AUTO: toggle auto-start when all players are ready; with it on, full readiness starts the game automatically. [Host]" },
    { "READY", "READY：准备 / 取消准备。全员准备且比例合法时房主可 START（或自动开局）。",
                 "READY: toggle ready status. When all are ready and the ratio is valid the host can START (or auto-start kicks in)." },
    { "STATUS", "STATUS（短别名 ST）：查看本房成员列表与准备状态。",
                  "STATUS (alias ST): show room members and ready status." },
    { "LEVEL", "LEVEL <0|1|2>：设置职业档位：0 基础（狼/预言家/女巫/猎人）、1 经典（+守卫/白痴）、2 豪华（+白狼王/丘比特/盗贼）。【房主】",
                 "LEVEL <0|1|2>: set the role level: 0 basic (wolf/seer/witch/hunter), 1 classic (+guard/idiot), 2 deluxe (+whitewolf/cupid/thief). [Host]" },
    { "VILLAGER", "VILLAGER <0|1>（短别名 VG）：开关村民职业（1=启用，默认关闭）。【房主】",
                    "VILLAGER <0|1> (alias VG): toggle the villager role (1=on, off by default). [Host]" },
    { "RATIO", "RATIO <狼> <中立> <神>：设置三阵营真实人数（村民关闭时三方之和须等于房间人数）；非法输入不设置。【房主】",
                 "RATIO <wolf> <neutral> <god>: set the three-camp headcounts (the sum must equal the room size when villager is off); invalid input is not applied. [Host]" },
    { "CONFIRM", "CONFIRM <1|0>（短别名 CF）：全员准备时比例不符触发自动配置建议，1=同意按建议开局，0=拒绝并全员取消准备。【房主】",
                   "CONFIRM <1|0> (alias CF): when full readiness meets a ratio mismatch the auto-config suggestion fires; 1=accept and start, 0=reject and everyone un-readies. [Host]" },
    { "PICK", "PICK <槽号或名字>：把指定玩家踢出房间（禁入 10 秒）。【房主】",
                "PICK <slot or name>: kick the specified player from the room (blocked for 10s). [Host]" },
    { "TRANSFER", "TRANSFER <槽号或名字>（短别名 TF）：把房主转移给指定玩家，原房主失去配置权限。【房主】",
                    "TRANSFER <slot or name> (alias TF): transfer the host role to the specified player; the old host loses config rights. [Host]" },
    { "IP", "IP <玩家名>：查询指定玩家当前连接 IP（游戏中保留槽位也可查）。【房主】",
              "IP <name>: show the specified player's current connection IP (retained slots queryable in-game). [Host]" },
    { "LG", "LG：查看本房玩家进出日志（[in]/[out] 与 IP，每人最多 3 条）。【房主】",
              "LG: show the room's entry/exit log ([in]/[out] with IP, up to 3 entries per player). [Host]" },
    { "LIST", "LIST：查看房间列表（含人数与 [游戏中] 标记；房间内也可用）。",
                "LIST: list rooms (with player counts and an [in-game] tag; usable inside a room too)." },
    { "CREATE", "CREATE <端口>（短别名 CR）：创建房间，端口须为 1024-65535 且未被占用。",
                  "CREATE <port> (alias CR): create a room; the port must be 1024-65535 and unused." },
    { "JOIN", "JOIN <端口>：加入指定端口的房间（满员或已被拉黑会被拒绝）。",
                "JOIN <port>: join the room on that port (rejected if full or banned)." },
    { "NAME", "NAME <新名字>：改名（全服唯一，重名被拒；仅中英文/数字/下划线，限 10 码点，至少 2 个字符，不能是 IP 格式）。",
                "NAME <new name>: rename (unique server-wide; letters/digits/CJK/underscore only, at most 10 code points, at least 2 chars, no IP-like names)." },
    { "NPCKEY", "NPCKEY <key>：设置/查询 AI key（全局配置，任意连接可用）。空参数=查询状态（不回显 key）；key 仅允许字母数字/连字符/下划线/点。设置后在线 NPC 立即启用（DPAPI 加密落盘 npc_key.bin）。",
                "NPCKEY <key>: set/query the AI key (global config, usable from any connection). No arg shows status (key never echoed); key allows letters/digits/hyphen/underscore/dot. Online NPCs activate right after setting (stored DPAPI-encrypted as npc_key.bin)." },
    { "VOTE", "VOTE <编号>（短别名 V）：白天投票放逐，编号=玩家槽位，0=弃权。",
                "VOTE <number> (alias V): vote to exile during the day; the number is the player's slot, 0=abstain." },
    { "BOMB", "BOMB <编号>（短别名 B）：白狼王白天自爆，带走一名玩家并立即进入夜晚。",
                "BOMB <number> (alias B): the White Wolf King self-detonates during the day, taking one player into the night." },
};

const int HELP_DETAIL_COUNT = sizeof(HELP_DETAILS) / sizeof(HELP_DETAILS[0]);

// 查 HELP <参数> 对应的指令用法：先按命令表（英文名/短别名/中文别名）匹配指令，
// 未命中再按英文名直查新增指令表（SHOW/LOOK/ADD 不在 COMMANDS 表内）。
// allowZh 语义与 FindCommand 一致：英文版客户端不认中文别名
const HelpDetail* FindHelpDetail(const string& arg, bool allowZh)
{
    const CommandEntry* cmd = FindCommand(arg, allowZh);

    if (cmd != nullptr)
    {
        for (int i = 0; i < HELP_DETAIL_COUNT; ++i)
        {
            if (_stricmp(HELP_DETAILS[i].name, cmd->en) == 0) return &HELP_DETAILS[i];
        }
    }

    for (int i = 0; i < HELP_DETAIL_COUNT; ++i)
    {
        if (_stricmp(HELP_DETAILS[i].name, arg.c_str()) == 0) return &HELP_DETAILS[i];
    }

    // SHOW/LOOK 的中文别名「查看」（§19.4）不在 COMMANDS 表内，单独匹配
    if (allowZh && arg == "查看")
    {
        for (int i = 0; i < HELP_DETAIL_COUNT; ++i)
        {
            if (_stricmp(HELP_DETAILS[i].name, "SHOW") == 0) return &HELP_DETAILS[i];
        }
    }

    return nullptr;
}

// 输出单条指令用法：按客户端语言显示中文或英文说明
void ShowHelpDetail(const HelpDetail& d)
{
    cout << d.name << Txt(CurLang(), "：", ": ") << endl;
    cout << "  " << (g_enMode ? d.en : d.zh) << endl;
}

// 处理一行控制台输入（来自 g_cmdQueue）。命中命令则执行（发协议消息或本地显示），
// 未命中命令时：房间内按聊天原样发送，大厅给出"未知命令"提示。
void HandleCommand(const string& line)
{
    if (line.empty()) return;

    vector<string> tokens = SplitTokens(line);

    if (tokens.empty()) return;

    const CommandEntry* cmd = FindCommand(tokens[0], !g_enMode);
    string args = GetLineArgs(line);

    // ==== HELP/帮助：任何状态可用 ====
    if (cmd != nullptr && _stricmp(cmd->en, "HELP") == 0)
    {
        // HELP ALL：全部职业列表（"全部"是中文别名，英文版不认）
        if (!args.empty() && (_stricmp(args.c_str(), "ALL") == 0 || (!g_enMode && args == "全部")))
        {
            ShowAllJobsHelp();
            return;
        }

        if (!args.empty())
        {
            // 带参数：优先按职业名查（中英都支持，既有行为不回归）；
            // 未命中再按指令名查详细用法（§19.5），都未命中给出统一提示
            const JobDef* job = FindJob(args);

            if (job == nullptr)
            {
                const HelpDetail* detail = FindHelpDetail(args, !g_enMode);

                if (detail != nullptr)
                {
                    ShowHelpDetail(*detail);
                    return;
                }

                cout << FmtLang(CurLang(),
                    "没有这个指令或职业：%s。HELP 查看命令说明；HELP <指令名> 查看指令用法；HELP ALL 查看全部职业列表。",
                    "No such command or role: %s. HELP shows commands; HELP <command> shows its usage; HELP ALL lists all roles.",
                    args.c_str()) << endl;
                return;
            }

            const char* campName = g_enMode ? "Unknown" : "未知";

            if (job->camp == CAMP_WOLF)         campName = g_enMode ? "Wolf" : "狼人";
            else if (job->camp == CAMP_NEUTRAL) campName = g_enMode ? "Neutral" : "中立";
            else if (job->camp == CAMP_GOD)     campName = g_enMode ? "God" : "神职";
            else if (job->camp == CAMP_VILLAGER)campName = g_enMode ? "Good" : "好人";

            cout << Txt(CurLang(), "职业：", "Role: ") << (g_enMode ? job->enName : job->zhName) << endl;
            cout << Txt(CurLang(), "阵营：", "Camp: ") << campName << endl;
            cout << Txt(CurLang(), "介绍：", "Info: ") << (g_enMode ? job->detailEn : job->detail) << endl;
        }
        else
        {
            ShowCommandHelp();
        }

        return;
    }

    // ==== NAME：改名（任何状态都可改，客户端即时生效，重名由 RM 拒绝）====
    if (cmd != nullptr && _stricmp(cmd->en, "NAME") == 0)
    {
        // 特殊符号在净化前直接驳回（需求 §14.6）：旧净化会把 "a b" 净化成
        // 合法名 "ab" 而容忍非法字符，新规则对原始输入做白名单校验后才放行
        if (!IsValidNameChars(args))
        {
            cout << Txt(CurLang(), "名字只能包含中英文、数字与下划线", "Name may only contain letters, digits, CJK chars and underscore") << endl;
            return;
        }

        string n = args.empty() ? "Player" : args;
        n = SanitizeName(n);

        // 单字符/单数字名字拒绝：净化后不足 2 码点无区分度（空名已回退 Player）
        if (CountUtf8Chars(n) < 2)
        {
            cout << Txt(CurLang(), "名字至少需要 2 个字符", "Name needs at least 2 characters") << endl;
            return;
        }

        // 名字禁止 IP 格式（RM 同样校验；此处提前拦截避免来回）。
        // 用截断前判定，防止 11 位 IP 被限长切成非 IP 串而漏检。
        if (LooksLikeIpName(args))
        {
            cout << Txt(CurLang(), "名字不能是 IP 格式，请换一个。", "Name cannot be an IP, try another.") << endl;
            return;
        }

        g_playerName = n;
        SendRaw("NAME|" + n);
        return;
    }

    // ==== NPCKEY：设置/查询 AI key（任何状态可用，转发给 Start 处理）====
    // 字符校验与 Start 一致：协议分隔符 | 与空白都不进协议行，防止 key 拆坏协议
    if (cmd != nullptr && _stricmp(cmd->en, "NPCKEY") == 0)
    {
        for (char c : args)
        {
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';

            if (!ok)
            {
                cout << Txt(CurLang(), "AI key 只能包含字母数字/连字符/下划线/点",
                            "AI key may only contain letters, digits, hyphen, underscore and dot") << endl;
                return;
            }
        }

        SendRaw("NPCKEY|" + args);
        return;
    }

    // ==== EXIT：区分游戏/房间/大厅 ====
    if (cmd != nullptr && _stricmp(cmd->en, "EXIT") == 0)
    {
        if (g_inGame)
        {
            // 游戏中退出：先通知服务器放弃，再回到大厅
            SendGiveUp();
            ReturnToRoom();
        }
        else if (g_inRoom)
        {
            SendRaw("EXIT");
            g_inRoom = false;
            g_isAdmin = false;
            g_roomId = "";
        }
        else
        {
            // 大厅退出：中止回房自动重试并清掉回房信息，程序即将整体结束
            StopRejoinRetry();
            g_rejoinRoomId.clear();
            g_rejoinPlayerId = 0;
            SendRaw("EXIT");
            g_running = false;
        }

        return;
    }

    // ==== SHOW/LOOK/ADD：房间/大厅/游戏三场景分发（§20.x） ====
    // 这三条不能被游戏内兜底当聊天发出，也不能落进"Game only."兜底：
    // 房间内原样转发给 Start（权限与校验都由服务端做），大厅给用法说明，
    // 游戏内给本地提示
    if (cmd != nullptr
        && (_stricmp(cmd->en, "SHOW") == 0 || _stricmp(cmd->en, "ADD") == 0))
    {
        if (g_inGame)
        {
            // 游戏内不可用：只本地提示，绝不发给游戏服务器
            cout << Txt(CurLang(), "SHOW/LOOK/ADD 游戏内不可用，请回房后使用。", "SHOW/LOOK/ADD are not available in game; use them back in the room.") << endl;
            return;
        }

        if (g_inRoom)
        {
            // 房间内：原文转发（命令字保留键入的 SHOW/LOOK/ADD，中文别名
            // 归一为英文全名；Look→SHOW 条目），Start 按命令表统一解析
            string fwd = tokens[0];

            if (_stricmp(fwd.c_str(), "SHOW") != 0
                && _stricmp(fwd.c_str(), "LOOK") != 0
                && _stricmp(fwd.c_str(), "ADD") != 0)
            {
                fwd = cmd->en;
            }

            SendRaw(fwd + "|" + SanitizeChat(args));
            return;
        }

        // 大厅：SHOW/LOOK 打印用法说明，ADD 提示先入房
        if (_stricmp(cmd->en, "SHOW") == 0)
        {
            cout << Txt(CurLang(),
                "SHOW <子项>（LOOK 等效）：查看房间信息。子项：BAN 黑名单 / RATIO 比例 / LEVEL 档位 / VILLAGER 村民开关 / AUTO 自动开局 / ADD 本地用户与 NPC / MUTE 禁言名单。进入房间后可查看；游戏内不可用。",
                "SHOW <item> (LOOK equivalent): view room info. Items: BAN blacklist / RATIO ratio / LEVEL role level / VILLAGER villager toggle / AUTO auto-start / ADD local users & NPCs / MUTE mute list. Viewable after joining a room; not in game.") << endl;
        }
        else
        {
            cout << Txt(CurLang(),
                "ADD：添加本地用户（新窗口，由指定玩家控制）或 NPC（on=在线 AI / off=离线逻辑）。请先创建或加入房间后再使用 ADD。",
                "ADD: add a local user (new window, controlled by a player) or an NPC (on=online AI / off=offline logic). Please create or join a room before using ADD.") << endl;
        }

        return;
    }

    // ==== 游戏内兜底：任何输入都发给游戏服务器 ====
    if (g_inGame)
    {
        SendGameRaw(BuildGamePayload(line));
        return;
    }

    // ==== 房间内命令 ====
    if (g_inRoom)
    {
        if (cmd == nullptr)
        {
            // 未匹配命令：直接作为聊天发送
            SendRaw(SanitizeChat(line));
            return;
        }

        if (_stricmp(cmd->en, "READY") == 0)
        {
            SendRaw("READY");
            return;
        }

        if (_stricmp(cmd->en, "STATUS") == 0)
        {
            SendRaw("STATUS");
            return;
        }

        if (_stricmp(cmd->en, "TRANSFER") == 0)
        {
            if (args.empty())
            {
                cout << Txt(CurLang(), "用法：TRANSFER <槽号或名字>（转移房主）", "Usage: TRANSFER <slot or name> (transfer host)") << endl;
                return;
            }

            SendRaw("TRANSFER|" + SanitizeChat(args));
            return;
        }

        if (_stricmp(cmd->en, "PICK") == 0)
        {
            if (args.empty())
            {
                cout << Txt(CurLang(), "用法：PICK <槽号或名字>（踢出房间）", "Usage: PICK <slot or name> (kick from room)") << endl;
                return;
            }

            SendRaw("PICK|" + SanitizeChat(args));
            return;
        }

        if (_stricmp(cmd->en, "LEVEL") == 0)
        {
            if (args != "0" && args != "1" && args != "2")
            {
                cout << Txt(CurLang(), "档位参数必须是 0/1/2（0 基础 / 1 经典 / 2 豪华）", "Level must be 0/1/2 (0 basic / 1 classic / 2 deluxe)") << endl;
                return;
            }

            SendRaw("LEVEL|" + args);
            return;
        }

        if (_stricmp(cmd->en, "VILLAGER") == 0)
        {
            if (args != "0" && args != "1")
            {
                cout << Txt(CurLang(), "村民开关参数必须是 0（禁用）或 1（启用）", "Villager must be 0 (off) or 1 (on)") << endl;
                return;
            }

            SendRaw("VILLAGER|" + args);
            return;
        }

        if (_stricmp(cmd->en, "RATIO") == 0)
        {
            // 比例是真实人数：狼 中立 神，三个纯数字
            vector<string> parts = SplitTokens(args);

            if (parts.size() != 3)
            {
                cout << Txt(CurLang(), "比例参数应为三个数字：RATIO <狼> <中立> <神>", "Ratio needs 3 numbers: RATIO <wolf> <neutral> <god>") << endl;
                return;
            }

            for (size_t i = 0; i < parts.size(); ++i)
            {
                if (!IsAllDigits(parts[i]))
                {
                    cout << Txt(CurLang(), "比例参数必须是纯数字：RATIO <狼> <中立> <神>", "Ratio must be digits: RATIO <wolf> <neutral> <god>") << endl;
                    return;
                }
            }

            SendRaw("RATIO|" + parts[0] + "|" + parts[1] + "|" + parts[2]);
            return;
        }

        if (_stricmp(cmd->en, "CONFIRM") == 0)
        {
            if (args != "1" && args != "0")
            {
                cout << Txt(CurLang(), "同意参数必须是 1（同意）或 0（拒绝）", "Confirm must be 1 (agree) or 0 (reject)") << endl;
                return;
            }

            SendRaw("CONFIRM|" + args);
            return;
        }

        if (_stricmp(cmd->en, "START") == 0)
        {
            // 原样转发：无参数发裸 START；带参数（START /F、/FORCE，尾随
            // 空格已被 GetLineArgs 裁掉）拼进参数区，准备检查/强制开局等
            // 校验全部由 Start 侧完成，客户端不做本地拦截
            if (args.empty())
            {
                SendRaw("START");
            }
            else
            {
                SendRaw("START|" + SanitizeChat(args));
            }

            return;
        }

        if (_stricmp(cmd->en, "AUTO") == 0)
        {
            SendRaw("AUTO");
            return;
        }

        if (_stricmp(cmd->en, "BAN") == 0)
        {
            if (args.empty())
            {
                cout << Txt(CurLang(), "用法：BAN <槽号/名字/IP>（拉黑玩家或 IP，禁止其加入本房）", "Usage: BAN <slot/name/IP> (ban from room)") << endl;
                return;
            }

            SendRaw("BAN|" + SanitizeChat(args));
            return;
        }

        if (_stricmp(cmd->en, "UNBAN") == 0)
        {
            if (args.empty())
            {
                cout << Txt(CurLang(), "用法：UNBAN <名字/IP>（取消拉黑）", "Usage: UNBAN <name/IP> (unban)") << endl;
                return;
            }

            SendRaw("UNBAN|" + SanitizeChat(args));
            return;
        }

        if (_stricmp(cmd->en, "MUTE") == 0 || _stricmp(cmd->en, "UNMUTE") == 0)
        {
            // 禁言/解禁（房主专属）：参数原样转发，不做本地拦截——空参数、
            // 权限与名单校验全部由 Start 侧完成/应答（§20.4）
            SendRaw(string(cmd->en) + "|" + SanitizeChat(args));
            return;
        }

        if (_stricmp(cmd->en, "UNADD") == 0)
        {
            // 移除 NPC/本地用户（房主专属，§21）：参数原样转发，不做本地拦截
            // ——空参数、权限、存在性与游戏期门全部由 Start 侧完成/应答
            SendRaw(string(cmd->en) + "|" + SanitizeChat(args));
            return;
        }

        if (_stricmp(cmd->en, "IP") == 0)
        {
            // 房主查询玩家 IP：转发给 Start，权限（房主专属）由服务端校验
            if (args.empty())
            {
                cout << Txt(CurLang(), "用法：IP <玩家名>（查询玩家 IP）", "Usage: IP <player name> (show IP)") << endl;
                return;
            }

            SendRaw("IP|" + SanitizeChat(args));
            return;
        }

        if (_stricmp(cmd->en, "LG") == 0)
        {
            // 房主查看进出记录：无参数，转发给 Start
            SendRaw("LG");
            return;
        }

        // 大厅建房/入房在房间里没意义，给出提示避免误发（LIST 除外：§11.1 房间内放行）
        if (_stricmp(cmd->en, "CREATE") == 0
            || _stricmp(cmd->en, "JOIN") == 0)
        {
            cout << Txt(CurLang(), "该命令仅在大厅可用，请先退出房间（EXIT）", "Lobby only. Leave the room first (EXIT)") << endl;
            return;
        }

        // 房间内 LIST 放行：转发给 Start，返回房间列表照常展示（§11.1）
        if (_stricmp(cmd->en, "LIST") == 0)
        {
            SendRaw("LIST");
            return;
        }

        cout << Txt(CurLang(), "该命令仅能在游戏中使用。", "Game only.") << endl;
        return;
    }

    // ==== 大厅命令 ====
    if (cmd == nullptr)
    {
        cout << Txt(CurLang(), "未知命令，输入 HELP 或 帮助 查看命令说明", "Unknown command. Type HELP for help") << endl;
        return;
    }

    if (_stricmp(cmd->en, "LIST") == 0)
    {
        SendRaw("LIST");
        return;
    }

    if (_stricmp(cmd->en, "CREATE") == 0)
    {
        if (!IsValidPort(args))
        {
            cout << Txt(CurLang(), "端口无效（须为 1024-65535 的纯数字）", "Invalid port (1024-65535, digits only)") << endl;
            return;
        }

        SendRaw("CREATE|" + args);
        return;
    }

    if (_stricmp(cmd->en, "JOIN") == 0)
    {
        if (!IsValidPort(args))
        {
            cout << Txt(CurLang(), "端口无效（须为 1024-65535 的纯数字）", "Invalid port (1024-65535, digits only)") << endl;
            return;
        }

        SendRaw("JOIN|" + args);
        return;
    }

    // 房间命令在大厅没有意义，明确提示
    if (_stricmp(cmd->en, "READY") == 0
        || _stricmp(cmd->en, "STATUS") == 0
        || _stricmp(cmd->en, "TRANSFER") == 0
        || _stricmp(cmd->en, "PICK") == 0
        || _stricmp(cmd->en, "BAN") == 0
        || _stricmp(cmd->en, "UNBAN") == 0
        || _stricmp(cmd->en, "MUTE") == 0
        || _stricmp(cmd->en, "UNMUTE") == 0
        || _stricmp(cmd->en, "UNADD") == 0
        || _stricmp(cmd->en, "IP") == 0
        || _stricmp(cmd->en, "LG") == 0
        || _stricmp(cmd->en, "LEVEL") == 0
        || _stricmp(cmd->en, "VILLAGER") == 0
        || _stricmp(cmd->en, "RATIO") == 0
        || _stricmp(cmd->en, "CONFIRM") == 0
        || _stricmp(cmd->en, "START") == 0
        || _stricmp(cmd->en, "AUTO") == 0)
    {
        cout << Txt(CurLang(), "该命令仅能在进入房间后使用。", "Room only. Join a room first.") << endl;
        return;
    }

    cout << Txt(CurLang(), "该命令仅能在游戏中使用。", "Game only.") << endl;
}

// ============ 房间消息处理 ============

// 处理房间管理器发来的消息（大厅/房间状态）。
bool HandleRoomMessage(const string& msg)
{
    // 供测试 harness 观测：把收到的每条房间消息写入 client.log
    ClientLog("RM_MSG:" + msg);

    EnsureNewLine();

    if (msg.find("WELCOME|") == 0)
    {
        string content = msg.substr(8);
        cout << (content.empty() ? Txt(CurLang(), "欢迎来到狼人杀客户端！", "Welcome to Werewolf!") : content) << endl;
        return true;
    }

    if (msg.find("ROOMS_LIST|") == 0)
    {
        string data = msg.substr(11);
        ClientLog("ROOMS_LIST:" + data);

        if (data == "EMPTY")
        {
            cout << Txt(CurLang(), "当前没有可用的房间。", "No rooms available.") << endl;
        }
        else
        {
            cout << Txt(CurLang(), "--- 房间列表 ---", "--- Rooms ---") << endl;

            size_t pos = 0;

            while ((pos = data.find('|')) != string::npos)
            {
                cout << data.substr(0, pos) << endl;
                data.erase(0, pos + 1);
            }

            if (!data.empty()) cout << data << endl;
        }

        return true;
    }

    if (msg.find("CREATED|") == 0)
    {
        string rest = msg.substr(8);
        size_t pos = rest.find('|');

        if (pos != string::npos)
        {
            g_roomId = rest.substr(0, pos);
            string port = rest.substr(pos + 1);
            cout << FmtLang(CurLang(), "建房成功！房间号：%s，端口：%s", "Room created! ID: %s, port: %s", g_roomId.c_str(), port.c_str()) << endl;
        }
        else if (!rest.empty())
        {
            g_roomId = rest;
            cout << FmtLang(CurLang(), "建房成功！房间号：%s", "Room created! ID: %s", g_roomId.c_str()) << endl;
        }

        g_inRoom = true;
        g_inGame = false;
        g_isAdmin = false; // ADMIN| 消息会重新置为 true

        // 房主另建新房间：放弃原房间回房，停止自动重试并清掉一次性回房信息
        StopRejoinRetry();
        g_rejoinRoomId.clear();
        g_rejoinPlayerId = 0;
        return true;
    }

    if (msg.find("JOINED|") == 0)
    {
        g_roomId = msg.substr(7);
        g_inRoom = true;
        g_inGame = false;
        g_isAdmin = false;
        cout << FmtLang(CurLang(), "已成功加入房间：%s", "Joined room: %s", g_roomId.c_str()) << endl;

        // 回房成功（或已另入新房间）：目标达成，停止自动重试并清掉一次性回房信息，
        // 防止之后大厅断线重连误发 GAME_ENDED 中止本房间新开的局
        StopRejoinRetry();
        g_rejoinRoomId.clear();
        g_rejoinPlayerId = 0;
        return true;
    }

    if (msg.find("ADMIN|") == 0)
    {
        g_isAdmin = true;
        cout << Txt(CurLang(), "你已成为房主！", "You are the host now!") << endl;
        return true;
    }

    if (msg.find("ROOM_MSG|") == 0)
    {
        cout << msg.substr(9) << endl;
        return true;
    }

    if (msg.find("ROOM_STATUS|") == 0)
    {
        string data = msg.substr(12);
        cout << (data.empty() ? Txt(CurLang(), "房间暂无状态信息。", "No room status.") : data) << endl;
        return true;
    }

    if (msg.find("READY_STATUS|") == 0)
    {
        string v = msg.substr(13);

        if (v == "1")
        {
            cout << Txt(CurLang(), "你已准备完毕！", "You are ready.") << endl;
        }
        else if (v == "0")
        {
            cout << Txt(CurLang(), "你已取消准备。", "You are not ready.") << endl;
        }
        else
        {
            cout << msg << endl;
        }

        return true;
    }

    if (msg.find("LEFT_ROOM|") == 0)
    {
        string content = msg.substr(10);
        cout << (content.empty() ? Txt(CurLang(), "你已离开房间，回到大厅。", "You left the room, back to lobby.") : content) << endl;

        g_inRoom = false;
        g_isAdmin = false;
        g_roomId = "";
        return true;
    }

    if (msg.find("KICKED|") == 0)
    {
        string content = msg.substr(7);
        cout << (content.empty() ? Txt(CurLang(), "你已被房主踢出房间！", "You were kicked from the room!") : content) << endl;

        g_inRoom = false;
        g_isAdmin = false;
        g_roomId = "";

        // 断开与房间服务器的连接并重连大厅（禁入期间 RM 会拒绝加入）
        if (g_sock != INVALID_SOCKET)
        {
            closesocket(g_sock);
            g_sock = INVALID_SOCKET;
        }

        if (g_roomRecvThread.joinable()) g_roomRecvThread.join();

        ConnectToRoomManager();

        if (g_running)
        {
            DropMessagesByPrefix("DISCONNECTED");
            g_promptDisplayed = false;
        }

        return true;
    }

    if (msg.find("REJOIN_FAIL|") == 0)
    {
        string content = msg.substr(12);

        // 游戏仍在进行中是可恢复状态：已进入自动重试的保持静默，避免每 5 秒刷屏
        bool retryable = content.find("游戏仍在进行中") != string::npos
            || content.find("Game still in progress") != string::npos;

        if (retryable && g_rejoinRetrying) return true;

        EnsureNewLine();
        g_inRoom = false;
        g_isAdmin = false;
        g_roomId = "";

        if (retryable && !g_rejoinRoomId.empty() && g_rejoinPlayerId > 0)
        {
            // 首次被拒：保存目标并进入每 5 秒自动重试（游戏结束、RM 收到 Server 通知后即成功）
            StartRejoinRetry(g_rejoinRoomId, g_rejoinPlayerId);
        }
        else
        {
            // 终态失败（房间不存在/已拉黑/已满等）或重试目标缺失：提示并停止，无限重试无意义
            cout << (content.empty() ? Txt(CurLang(), "回房失败，请留在大厅。", "Failed to rejoin, stay in lobby.") : content) << endl;
            StopRejoinRetry();
        }

        // 重试目标仅服务首次 REJOIN：清空后大厅断线重连不会再发 GAME_ENDED，避免误中止新开的局
        g_rejoinRoomId.clear();
        g_rejoinPlayerId = 0;
        return true;
    }

    if (msg.find("ERROR|") == 0)
    {
        string content = msg.substr(6);
        cout << Txt(CurLang(), "错误：", "Error: ") << (content.empty() ? Txt(CurLang(), "未知错误", "Unknown error") : content) << endl;
        return true;
    }

    if (msg.find("CONFIG_NEED_CONFIRM|") == 0)
    {
        // 自动配置建议：狼|中立|神，需要房主确认
        string rest = msg.substr(20);
        vector<string> parts;

        size_t pos = 0;

        while ((pos = rest.find('|')) != string::npos)
        {
            parts.push_back(rest.substr(0, pos));
            rest.erase(0, pos + 1);
        }

        parts.push_back(rest);

        string w = parts.size() > 0 ? parts[0] : "?";
        string n = parts.size() > 1 ? parts[1] : "?";
        string g = parts.size() > 2 ? parts[2] : "?";

        cout << FmtLang(CurLang(), "自动配置建议：狼 %s / 中立 %s / 神 %s。", "Auto config: wolf %s / neutral %s / god %s.", w.c_str(), n.c_str(), g.c_str()) << endl;
        cout << Txt(CurLang(), "是否同意？输入 CONFIRM 1 或 同意 1（1=同意，0=拒绝）", "Agree? Type CONFIRM 1 (1=yes, 0=no)") << endl;
        return true;
    }

    // 其他文本原样显示（如房间公告等）
    cout << msg << endl;
    return true;
}

// ============ 主流程 ============

// 统一收尾清理：停心跳/输入/接收线程、关连接、反初始化 Winsock。
// 抽出为独立函数是因为自动模式（§19.8）在 __GAME_OVER__ 处要直接
// return 0 关闭窗口，不能走主循环末尾的暂停等待路径
void ShutdownClient()
{
    g_running = false;
    g_inputThreadRunning = false;
    g_pingRunning = false;
    if (g_pingThread.joinable()) g_pingThread.join();

    // 关闭输入门控：输入线程不再读取/路由任何输入
    SetInputGate(InputGate::Closed);

    // 若输入线程正阻塞在 ReadConsoleW（键入了字符但没回车），
    // 注入一个回车事件让它及时返回并退出
    {
        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        INPUT_RECORD rec = { 0 };
        rec.EventType = KEY_EVENT;
        rec.Event.KeyEvent.bKeyDown = TRUE;
        rec.Event.KeyEvent.uChar.UnicodeChar = L'\r';
        DWORD written = 0;
        WriteConsoleInputW(hIn, &rec, 1, &written);
    }

    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    if (g_gameSock != INVALID_SOCKET) closesocket(g_gameSock);

    if (g_inputThread.joinable()) g_inputThread.join();
    if (g_roomRecvThread.joinable()) g_roomRecvThread.join();
    if (g_gameRecvThread.joinable()) g_gameRecvThread.join();

    WSACleanup();
    ClientLog("Client stopped");
}

int main()
{
    DisableConsoleQuickEdit();
    SetConsoleUtf8();
    SetConsoleFont();
    ShowCursor(true);

    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        cout << Txt(CurLang(), "网络初始化失败（WSAStartup）。", "Network init failed (WSAStartup).") << endl;
        _getch();
        return 1;
    }

    // 测试注入：环境变量 WOLF_REJOIN_RETRY_SECONDS（1..300 的整数）可缩短
    // 回房自动重试间隔，仅供自动化测试用；非法值/缺省保持 5 秒
    if (const char* envRetry = getenv("WOLF_REJOIN_RETRY_SECONDS"))
    {
        int v = atoi(envRetry);

        if (v >= 1 && v <= 300) g_rejoinRetrySeconds = v;
    }

    ClientLog("Client started");
    ClearScreen();

    cout << Txt(CurLang(), "=== 狼人杀 客户端 ===", "=== Werewolf Client ===") << endl;

    // ==== 自动模式（§19.8，ADD USER 的开局自动窗口）====
    // 带 4 个参数（IP Start端口 用户名 房间端口）时进入：跳过下方交互输入，
    // 按参数连接后自动 JOIN 指定房间；此后交互与普通客户端一致；
    // 收 __GAME_OVER__（本局正常结束）后进程自动退出。参数个数不对不进入
    // 自动模式（既有交互流程完全不变）。
    // 解析用宽字符命令行：Start 以 CreateProcessW 传参，CRT 窄化的 argv 会按
    // ACP（GBK）转码中文用户名变乱码，宽命令行转 UTF-8 才是原始值
    {
        int wargc = 0;
        LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);

        if (wargc == 5)
        {
            vector<string> a;

            for (int i = 1; i < wargc; ++i) a.push_back(WideToUtf8(wargv[i]));

            // 参数防御性校验：自动窗口无交互重输机会，非法输出原因直接退出。
            // 用户名规则与 NAME 命令完全一致（白名单/长度/IP 形似）
            sockaddr_in ipTest;

            if (inet_pton(AF_INET, a[0].c_str(), &ipTest.sin_addr) != 1)
            {
                cout << FmtLang(CurLang(), "自动模式：服务器 IP 无效：%s", "Auto mode: invalid server IP: %s", a[0].c_str()) << endl;
                WSACleanup();
                return 1;
            }

            if (!IsValidPort(a[1]))
            {
                cout << FmtLang(CurLang(), "自动模式：服务器端口无效：%s（须为 1024-65535 的纯数字）", "Auto mode: invalid server port: %s (1024-65535 digits)", a[1].c_str()) << endl;
                WSACleanup();
                return 1;
            }

            if (!IsValidPort(a[3]))
            {
                cout << FmtLang(CurLang(), "自动模式：房间端口无效：%s（须为 1024-65535 的纯数字）", "Auto mode: invalid room port: %s (1024-65535 digits)", a[3].c_str()) << endl;
                WSACleanup();
                return 1;
            }

            if (!IsValidNameChars(a[2]) || LooksLikeIpName(a[2]))
            {
                cout << FmtLang(CurLang(), "自动模式：用户名无效：%s", "Auto mode: invalid username: %s", a[2].c_str()) << endl;
                WSACleanup();
                return 1;
            }

            g_playerName = SanitizeName(a[2]);

            if (CountUtf8Chars(g_playerName) < 2)
            {
                cout << FmtLang(CurLang(), "自动模式：用户名无效：%s（至少 2 个字符）", "Auto mode: invalid username: %s (at least 2 chars)", a[2].c_str()) << endl;
                WSACleanup();
                return 1;
            }

            g_autoMode = true;
            g_startIp = a[0];
            g_startPort = atoi(a[1].c_str());
            g_autoRoomPort = a[3];

            // 标题带用户名便于控制者在多窗口间区分（仅自动模式，不影响验收）
            SetConsoleTitleA(("Werewolf - " + g_playerName).c_str());
        }

        if (wargv != nullptr) LocalFree(wargv);
    }

    // 交互输入（自动模式跳过：参数已就位）
    if (!g_autoMode)
    {
        // 服务器 IP：格式非法会让 inet_pton 失败，连接只会白白重试后退出，尽早纠正
        cout << Txt(CurLang(), "输入服务器 IP（默认 127.0.0.1）：", "Server IP (default 127.0.0.1): ");
        g_startIp = ReadConsoleUtf8Line();

        if (g_startIp.empty()) g_startIp = "127.0.0.1";

        while (true)
        {
            sockaddr_in test;

            if (inet_pton(AF_INET, g_startIp.c_str(), &test.sin_addr) == 1) break;

            cout << Txt(CurLang(), "IP 地址格式不正确，请重新输入（默认 127.0.0.1）：", "Invalid IP, retry (default 127.0.0.1): ");
            g_startIp = ReadConsoleUtf8Line();

            if (g_startIp.empty()) g_startIp = "127.0.0.1";
        }

        // 服务器端口：必须为 1024-65535 的纯数字，否则连接必然失败
        cout << Txt(CurLang(), "输入服务器端口（默认 8888）：", "Server port (default 8888): ");
        string portStr = ReadConsoleUtf8Line();

        if (portStr.empty()) portStr = "8888";

        while (!IsValidPort(portStr))
        {
            cout << Txt(CurLang(), "端口无效（须为 1024-65535 的纯数字），请重新输入：", "Invalid port (1024-65535), retry: ");
            portStr = ReadConsoleUtf8Line();

            if (portStr.empty()) portStr = "8888";
        }

        g_startPort = atoi(portStr.c_str());

        // 玩家名字：空则用默认名（重名会在连接后被 RM 拒绝并可再次改名）。
        // 名字禁止 IP 格式（RM 同样校验，此处提前纠正避免来回）。
        cout << Txt(CurLang(), "输入你的名字（默认 Player）：", "Your name (default Player): ");
        string nameLine = ReadConsoleUtf8Line();

        if (!nameLine.empty()) g_playerName = SanitizeName(nameLine);

        // 初始名字与 NAME 命令同规则：白名单 → 长度 → IP（本处提前纠正避免来回）
        while (!nameLine.empty() && !IsValidNameChars(nameLine))
        {
            cout << Txt(CurLang(), "名字只能包含中英文、数字与下划线，请重新输入：", "Name may only contain letters, digits, CJK chars and underscore, retry: ");
            nameLine = ReadConsoleUtf8Line();

            if (nameLine.empty()) break;

            g_playerName = SanitizeName(nameLine);
        }

        while (LooksLikeIpName(nameLine))
        {
            cout << Txt(CurLang(), "名字不能是 IP 格式，请重新输入：", "Name cannot be an IP, retry: ");
            nameLine = ReadConsoleUtf8Line();

            if (nameLine.empty()) { g_playerName = "Player"; break; }

            g_playerName = SanitizeName(nameLine);
        }

        while (!nameLine.empty() && CountUtf8Chars(g_playerName) < 2)
        {
            cout << Txt(CurLang(), "名字至少需要 2 个字符，请重新输入：", "Name needs at least 2 characters, retry: ");
            nameLine = ReadConsoleUtf8Line();

            if (nameLine.empty()) { g_playerName = "Player"; break; }

            g_playerName = SanitizeName(nameLine);
        }
    }

    cout << Txt(CurLang(), "你的名字：", "Your name: ") << g_playerName << endl;

    // 连接房间管理器（失败会重试 3 次后退出）
    if (!ConnectToRoomManager())
    {
        WSACleanup();
        return 0;
    }

    // 丢弃启动阶段最后的击键残留（玩家连续按回车输入的尾巴），
    // 防止它们进入输入线程的第一次读取
    FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));

    g_inputThread = thread(InputThreadFunc);

    // 大厅/房间：输入门控常开（提示符等待命令输入）
    SetInputGate(InputGate::Open);
    ClientLog("Lobby connected");

    // 自动模式：把 JOIN 注入命令队列，由主循环走 HandleCommand 的 JOIN 分支
    // （端口校验与发送路径与手动输入完全一致）；注入在用户输入之前入队，
    // 保证先自动入房、后接管操作者输入。
    // 命令用空格分隔（SplitTokens/GetLineArgs 按空格分词），不能用竖线——
    // 竖线会把整行当成一个未知命令 token，JOIN 永远发不出去（D 段实测）
    if (g_autoMode)
    {
        lock_guard<mutex> lock(g_cmdMutex);
        g_cmdQueue.push_back("JOIN " + g_autoRoomPort);
    }

    ShowPrompt();

    while (g_running)
    {
        string msg;
        bool msgHandled = false;

        while (PopMessage(msg, 10))
        {
            if (msg == "DISCONNECTED")
            {
                // 中继模式下大厅连接是游戏消息的唯一通道：断开必须重连大厅
                // 并恢复游戏链路——清中继标志后走既有游戏重连（直连→失败再中继）
                if (g_inGame && g_relayMode)
                {
                    g_relayMode = false;
                    g_inputSolicited = false;
                    g_dayTalk = false;
                    SetInputGate(InputGate::Closed);
                    ShowCursor(false);

                    HandleLobbyDisconnect();

                    if (!g_running) continue;

                    // HandleLobbyDisconnect 按大厅视角重开了输入门，游戏重连
                    // 期间必须关回，等服务器 __INPUT__ 再开
                    g_inputSolicited = false;
                    g_dayTalk = false;
                    SetInputGate(InputGate::Closed);
                    ShowCursor(false);

                    HandleGameReconnect();
                    continue;
                }

                // 游戏期间大厅连接本就已关闭，误收到的断开消息忽略
                if (g_inGame) continue;

                HandleLobbyDisconnect();
                continue;
            }

            if (msg == "__CONN_LOST__")
            {
                // 已收终态标记（服务器正常收尾后关闭连接）：不重连直接回房。
                // 正常时序下 __GAME_OVER__ 先行处理已回房，此分支仅防御兜底
                if (g_gameOver)
                {
                    if (g_inGame) ReturnToRoom();

                    continue;
                }

                // 游戏连接中断 → 关闭输入窗口（断线期间键入内容丢弃），
                // 进入重连流程；重连成功后服务器会重新发 __INPUT__ 征求输入
                if (g_inGame)
                {
                    g_inputSolicited = false;
                    // 断线后白天连续发言窗口一并关闭，重连由服务器重新征求
                    g_dayTalk = false;
                    SetInputGate(InputGate::Closed);
                    ShowCursor(false);
                    HandleGameReconnect();
                }

                continue;
            }

            if (msg == "__GAME_OVER__")
            {
                // 终态控制行：服务器正常收尾发来（发完即关连接），置终态标记
                // 并直接回房、不进入游戏重连，也绝不发 GIVEUP（Server 已退出收尾）。
                // 可能已因"本局结束"广播的包含匹配先行回房，此处幂等无害；
                // 标记保留到下一局 GAME_PREPARE 才复位
                g_gameOver = true;

                // 自动模式（§19.8）：本局已正常结束，进程直接退出（return 0）
                // 并关闭窗口——窗口由 ADD USER 自动打开，不随玩家回房流程走
                if (g_autoMode)
                {
                    ClientLog("AUTO_MODE exit on __GAME_OVER__");
                    ShutdownClient();
                    return 0;
                }

                if (g_inGame) ReturnToRoom();

                continue;
            }

            // ---- 中继模式消息分流：游戏行（GAME_FWD|）与大厅行各走各的 ----
            if (g_inGame && g_relayMode)
            {
                if (msg.find("GAME_FWD|") == 0)
                {
                    // 剥前缀交给游戏消息处理（与直连同入口：ROLE/PLAYER_LIST/
                    // RESULT/__INPUT__/白天广播等）
                    string gm = msg.substr(9);

                    // 终态行：与直连一致的 __GAME_OVER__ 处理（正常收尾直接回房）
                    if (gm == "__GAME_OVER__")
                    {
                        g_gameOver = true;
                        ClientLog("RELAY_GAME_OVER");

                        // 自动模式（§19.8）：本局已正常结束，进程直接退出关窗
                        if (g_autoMode)
                        {
                            ClientLog("AUTO_MODE exit on __GAME_OVER__ (relay)");
                            ShutdownClient();
                            return 0;
                        }

                        if (g_inGame) ReturnToRoom();
                        continue;
                    }

                    ProcessGameMessage(gm);
                    continue;
                }

                if (msg.find("PROXY_FAIL|") == 0)
                {
                    // Start 拒绝/撤销中继：显示原因，清中继标志后走既有游戏
                    // 重连（直连 → 失败再中继）
                    string reason = msg.substr(11);

                    EnsureNewLine();
                    cout << FmtLang(CurLang(), "游戏中继被拒绝：%s", "Game relay rejected: %s", reason.c_str()) << endl;

                    g_relayMode = false;
                    g_inputSolicited = false;
                    g_dayTalk = false;
                    SetInputGate(InputGate::Closed);
                    ShowCursor(false);

                    HandleGameReconnect();
                    continue;
                }

                // 其他行（RELEASE/ROOM_MSG/ERROR 等）走既有房间处理
                HandleRoomMessage(msg);
                continue;
            }

            if (g_inGame)
            {
                ProcessGameMessage(msg);
                continue;
            }

            // ---- 开始游戏：连接房间管理器指定的游戏服务器（或经 Start 中继）----
            if (msg.find("GAME_PREPARE|") == 0)
            {
                string rest = msg.substr(13);
                size_t p1 = rest.find('|');
                size_t p2 = rest.find('|', p1 + 1);
                size_t p3 = rest.find('|', p2 + 1);

                if (p1 == string::npos || p2 == string::npos || p3 == string::npos) continue;

                g_gameServerPort = atoi(rest.substr(0, p1).c_str());
                g_gameRoomId = rest.substr(p1 + 1, p2 - p1 - 1);
                g_gameServerIp = rest.substr(p2 + 1, p3 - p2 - 1);
                g_myGamePlayerId = atoi(rest.substr(p3 + 1).c_str());
                g_switchingToGame = true;

                // 清掉上一局残留的游戏输入与大厅命令
                {
                    lock_guard<mutex> lock(g_gameCmdMutex);
                    g_gameCmdQueue.clear();
                }

                {
                    lock_guard<mutex> lock(g_cmdMutex);
                    g_cmdQueue.clear();
                }

                // 游戏演出文本不能追在大厅提示符后面
                g_promptDisplayed = false;

                // 进入游戏：关闭输入门控并隐藏光标。
                // 连接失败走 ReturnToRoom 会重新打开门控，无需在此恢复
                g_inputSolicited = false;
                // 跨局切换时清除上一局可能残留的白天连续发言状态
                g_dayTalk = false;
                // 终态标记在新局开始复位：此后收尾断线应走重连流程而非直接回房
                g_gameOver = false;
                SetInputGate(InputGate::Closed);
                ShowCursor(false);

                EnsureNewLine();
                cout << Txt(CurLang(), "正在连接游戏服务器 ...", "Connecting to game server ...") << endl;

                // 直连 vs 中继：WOLF_FORCE_PROXY=1 强制中继（测试注入）；
                // 否则先走既有直连（5 次 × 1 秒），全部失败再经 Start 中继
                bool forceProxy = IsForceProxyEnv();

                if (!forceProxy && TryConnectGameDirect())
                {
                    // 直连成功：按既有流程关闭大厅连接（游戏期间不再接收
                    // 大厅消息），回房时由重连流程重建
                    closesocket(g_sock);
                    g_sock = INVALID_SOCKET;
                    g_inRoom = false;

                    // 输入线程常驻，无需暂停/重建；
                    // 门控已在 GAME_PREPARE 处关闭，仅 __INPUT__ 时打开
                }
                else
                {
                    // 直连失败（或强制中继）：改经 Start 中继。中继复用的是
                    // 大厅连接，不能关闭；大厅断线由 DISCONNECTED 流程恢复
                    if (g_gameSock != INVALID_SOCKET)
                    {
                        closesocket(g_gameSock);
                        g_gameSock = INVALID_SOCKET;
                    }

                    if (g_gameRecvThread.joinable()) g_gameRecvThread.join();

                    // 中继阶段大厅连接意外断开必须上报（DISCONNECTED）以恢复游戏链路
                    g_switchingToGame = false;
                    g_inRoom = false;

                    if (forceProxy)
                    {
                        cout << Txt(CurLang(), "已启用强制中继模式，正在连接游戏服务器 ...", "Force-proxy mode enabled; connecting to the game server ...") << endl;
                    }
                    else
                    {
                        cout << Txt(CurLang(), "直连游戏服务器失败，改由房间服务器中继 ...", "Direct game connect failed; relaying via the lobby ...") << endl;
                    }

                    if (StartGameProxy(g_gameRoomId, g_myGamePlayerId))
                    {
                        // 中继成功：游戏消息改经大厅连接收发（g_relayMode 已置位）
                        g_inGame = true;
                        ClientLog("Connected to game server via proxy, player ID: " + to_string(g_myGamePlayerId));
                    }
                    else
                    {
                        ClientLog("Failed to connect to game server (direct and proxy)");
                        cout << Txt(CurLang(), "连接游戏服务器失败。", "Failed to connect to game server.") << endl;
                        g_switchingToGame = false;
                        ReturnToRoom();
                    }
                }

                continue;
            }

            // ---- 大厅/房间消息 ----
            msgHandled = HandleRoomMessage(msg) || msgHandled;
        }

        if (g_inGame)
        {
            // 游戏中：每轮把捕获到的游戏输入发给服务器（窗口打开时）
            FlushGameInput();
            continue;
        }

        // 回房自动重试：已重连大厅且未进任何房间时，每 5 秒重发一次 REJOIN，
        // 直到成功（JOINED|）、终态失败（REJOIN_FAIL|非"进行中"）或 EXIT/手动操作打断。
        // 放在主循环而非独立线程：阻塞式重连大厅与 EXIT（g_running=false）
        // 都会让本检查自然失效，无需额外线程同步
        if (g_rejoinRetrying && !g_inRoom && g_sock != INVALID_SOCKET)
        {
            auto now = chrono::steady_clock::now();

            if (chrono::duration_cast<chrono::seconds>(now - g_lastRejoinRetryTime).count() >= g_rejoinRetrySeconds)
            {
                SendRaw("REJOIN|" + g_rejoinRetryRoomId + "|" + to_string(g_rejoinRetryPlayerId));
                ClientLog("REJOIN_RETRY|" + g_rejoinRetryRoomId + "|" + to_string(g_rejoinRetryPlayerId));
                g_lastRejoinRetryTime = now;
            }
        }

        if (msgHandled) ShowPrompt();

        // ---- 处理控制台命令 ----
        string cmd;
        bool gotCmd = false;

        {
            lock_guard<mutex> lock(g_cmdMutex);

            if (!g_cmdQueue.empty())
            {
                cmd = g_cmdQueue.front();
                g_cmdQueue.pop_front();
                gotCmd = true;
            }
        }

        if (gotCmd)
        {
            ShowCursor(true);

            // 提示符已随输入行"消费"：重置标志，让底部 ShowPrompt 能真实重绘
            g_promptDisplayed = false;

            HandleCommand(cmd);

            if (g_running && !g_inGame) ShowPrompt();
        }

        Sleep(10);
    }

    // ---- 退出清理 ----
    ShutdownClient();

    // 自动模式（ADD USER 自动窗口）退出不等待按键，直接关闭窗口
    if (!g_autoMode)
    {
        cout << "\n" << Txt(CurLang(), "[ 暂停 ]", "[ Pause ]") << "\n";

        // 输入线程已退出，此时无并发读控制台，直接读控制台事件等待按键
        PauseWaitConsole();
    }

    return 0;
}
