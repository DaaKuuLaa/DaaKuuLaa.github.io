// Client.cpp - 客户端（大厅 → 房间 → 游戏）
//
// 线程分工：
//   主线程       ：消息分发与命令处理；__INPUT__ 只打开输入窗口（g_inputSolicited）
//                  并立即返回（服务器先发菜单文本、最后发 __INPUT__，菜单打印期间
//                  门控关闭、键入被丢弃，不会破坏排版）；主循环每轮调用
//                  FlushGameInput() 把输入线程捕获的第一行非空输入发给服务器并关闭窗口
//   输入线程     ：受输入门控（g_inputGate）驱动的常驻线程；仅在门控打开时捕获
//                  一行输入（游戏中仅当 g_inputSolicited 时入队，演出期间丢弃；
//                  其余路由到 g_cmdQueue），门控关闭（游戏演出）时读取并丢弃、
//                  暂停时完全不读
//   房间接收线程 ：读取房间管理器（Start.exe）消息；每次连接大厅后重建
//   游戏接收线程 ：读取游戏服务器消息；每次进入/重连游戏后重建
//
// 输入门控与光标：
//   Open（大厅/房间提示符、游戏 __INPUT__）→ 显示光标、捕获一行输入；
//   Closed（游戏演出/打字机/进度条）→ 隐藏光标、不捕获任何输入；
//   Paused（system("pause") 期间）→ 输入线程完全不读控制台。
//   暂停统一输出 "\n[ Pause ]\n" 后执行 system("pause > nul")。
//
// 断线重连规则（与 Server.cpp 约定）：
//   1. 游戏连接中断 → 本地尝试重连 3 次（间隔 5 秒）；
//   2. 3 次都失败 → 发送 GIVEUP|N 通知服务器结束本局，然后自动回到原房间；
//   3. 回到房间 = 重新连接大厅 + 发送 GAME_ENDED / REJOIN <roomId> <playerId>；
//   4. 若大厅也连不上（3 次重试后）→ Pause 并退出程序。
#include "common.h"

#include <deque>

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

string g_roomId;                       // 当前房间号（自动回房依赖它）
bool g_inRoom = false;
string g_playerName = "Player";
bool g_isAdmin = false;

atomic<bool> g_inGame(false);
string g_gameServerIp;
int g_gameServerPort = 0;
int g_myGamePlayerId = 0;              // 本局玩家编号（1/2），回房 REJOIN 用

string g_startIp = "127.0.0.1";        // 房间管理器地址
int g_startPort = 8888;

atomic<bool> g_switchingToGame(false); // 正在切换进游戏（抑制大厅断线提示）
atomic<bool> g_inputThreadRunning(true);
atomic<bool> g_inputThreadStarted(false); // 输入线程是否已启动（PauseAndWait 判断依据）
atomic<bool> g_inputSolicited(false);  // 服务器已征求输入（__INPUT__ 已到，窗口打开）
bool g_promptDisplayed = true;

// 输入门控：控制输入线程何时真正捕获玩家输入（配合光标显隐）。
//   Open   - 大厅/房间提示符、游戏 __INPUT__：捕获一行并路由到对应队列
//   Closed - 游戏演出/打字机/进度条等：读取后丢弃（无回显），不捕获任何输入
//   Paused - system("pause") 执行期间：完全不读控制台，避免与 cmd.exe 抢回车
enum class InputGate { Closed, Paused, Open };
atomic<InputGate> g_inputGate(InputGate::Closed);

// 输入线程的路由目标：游戏中 → g_gameCmdQueue，其余 → g_cmdQueue
// 用 deque：pause 需要只消费"回车产生的空行"并把非空输入留在队列里给 __INPUT__ 用
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
    if (g_gameSock == INVALID_SOCKET) return;

    string out = msg + "\n";

    int total = 0;

    while (total < (int)out.length())
    {
        int sent = send(g_gameSock, out.c_str() + total, (int)out.length() - total, 0);

        if (sent <= 0)
        {
            ClientLog("SEND_FAIL: errno=" + to_string(WSAGetLastError()));
            return;
        }

        total += sent;
    }
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

void ShowPrompt()
{
    if (g_promptDisplayed) return;

    cout << (g_inRoom ? (g_isAdmin ? "[Admin]" : "[Room]") : "[Lobby]") << "> " << flush;
    g_promptDisplayed = true;
    ShowCursor(true);
}

// 切换输入门控并同步控制台输入模式（调用方：主线程）。
//   Open        - 行输入 + 回显（玩家能看到并编辑自己的输入）
//   Closed/Paused - 无回显、无行输入（原始模式），击键不显示、不被捕获
// 只改动输入相关的三个位，保留 DisableConsoleQuickEdit 清掉的位
// （快速编辑/鼠标），避免 SetConsoleMode 把它们重新打开。
// 先设控制台模式、后置门控标志：输入线程看到 Open 时行输入模式必然已生效，
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

// 显示帮助：根据当前状态（大厅/房间/游戏）输出所有可用指令及用法。
void PrintHelp()
{
    EnsureNewLine();

    cout << "=== Help ===" << endl;

    if (g_inGame)
    {
        cout << "In-game commands are sent raw to the game server." << endl;
        cout << "EXIT          - Quit the current game and return to the room" << endl;
        cout << "(number)      - Choose action prompted by the server (1/2/...)" << endl;
        cout << "-1 / -2       - Debug: swap current shell for real/blank (kept deliberately)" << endl;
    }
    else if (g_inRoom)
    {
        cout << "READY         - Toggle ready state" << endl;
        cout << "STATUS        - Refresh room ready status" << endl;
        cout << "PICK          - (Admin) Start the game when both are ready" << endl;
        cout << "NAME|<name>   - Change your display name (or NAME <name>)" << endl;
        cout << "EXIT          - Leave the room and return to lobby" << endl;
        cout << "HELP          - Show this help" << endl;
        cout << "cls / clear   - Clear the console" << endl;
        cout << "(any text)     - Send chat to the room" << endl;
    }
    else
    {
        cout << "LIST          - List available rooms" << endl;
        cout << "CREATE <port> - Create a room on the given port (1024-65535)" << endl;
        cout << "JOIN <port>   - Join a room on the given port" << endl;
        cout << "NAME|<name>   - Change your display name (or NAME <name>)" << endl;
        cout << "EXIT          - Quit the client" << endl;
        cout << "HELP          - Show this help" << endl;
        cout << "cls / clear   - Clear the console" << endl;
    }

    cout << "=============" << endl;
}

// 暂停等待：直接读控制台事件等待一个按键，不派生子进程。
// 旧实现 system("pause > nul") 会拉起 cmd.exe 共享控制台，在自动化注入
// （WriteConsoleInputW）/多控制台场景下，cmd.exe 的 pause 可能拿不到按键
// 导致客户端主线程无限卡死。输入线程处于 Paused（不读控制台）时调用，
// 无读取竞争，按键事件（含注入的 \r）都能被本函数收到（2026-08-02）。
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

// 暂停等待：打印提示并等待任意按键。
// 之前用"等待输入线程回车入队"的 CV 方案：暂停期间玩家提前敲的选择会被
// 保留给下次 __INPUT__（没错），但按 Enter 常被输入线程抢先读走，表现为
// "要狂按好几次才能继续"。
// 现在配合输入门控：暂停期间输入线程完全不读控制台（InputGate::Paused），
// 主线程直接读控制台事件等待按键，按任意键一次即继续（不再拉起 cmd.exe）。
void PauseAndWait()
{
    cout << "\n[ Pause ]\n";

    // 输入线程未启动（启动阶段连接失败退出路径）：无并发读控制台，直接读事件
    if (!g_inputThreadStarted)
    {
        PauseWaitConsole();
        return;
    }

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);

    ShowCursor(true);
    SetInputGate(InputGate::Paused);

    // 清掉缓冲残留（暂停前的击键/半行），防止 pause 被瞬间跳过
    FlushConsoleInputBuffer(hIn);

    PauseWaitConsole();

    // 丢弃暂停期间键入的内容（无回显），避免污染暂停后的输入
    FlushConsoleInputBuffer(hIn);

    // 暂停后回到大厅/房间提示符：重新打开门控
    SetInputGate(InputGate::Open);
    ShowCursor(true);
}

// ============ 输入线程 ============

// 宽字符 → UTF-8（ReadConsoleW 读取结果转换用）
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
// 不用 std::cin：cin 走 CRT 文本模式读，与 system("pause") 抢控制台输入，
// 且中文 IME 输入在 UTF-8 代码页下不可靠；ReadConsoleW 直接读宽字符。
string ReadConsoleUtf8Line()
{
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    wchar_t buf[1024];
    DWORD n = 0;

    if (!ReadConsoleW(hIn, buf, 1023, &n, nullptr)) return "";
    if (n == 0) return "";

    // ReadConsoleW 会带回行尾的 \r\n，去掉
    while (n > 0 && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n')) --n;

    return WideToUtf8(wstring(buf, n));
}

// 输入线程：唯一读取控制台输入的线程，受输入门控（g_inputGate）驱动。
//   Open   - 捕获一行输入并路由（游戏中 → g_gameCmdQueue，其余 → g_cmdQueue）
//   Closed - 读走并丢弃缓冲（游戏演出期间；无回显，不捕获任何输入）。
//            必须清空缓冲，否则下次开门时残留击键会被当成游戏选择，
//            表现为"输入错乱/无反应"。
//   Paused - 完全不读控制台（system("pause") 独占输入，抢回车会导致
//            暂停需要按多次）
// 控制台输入句柄可等待（有输入时变为有信号），用 50ms 轮询保证退出及时。
void InputThreadFunc()
{
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    g_inputThreadStarted = true;

    while (g_inputThreadRunning && g_running)
    {
        if (WaitForSingleObject(hIn, 50) != WAIT_OBJECT_0) continue;

        // 唤醒后重新读取门控：主线程可能刚切换过门控（如 __PAUSE__ 刚设置
        // Paused），若用唤醒前读到的旧值，本线程会把暂停开始后注入的回车
        // 当成演出期残留吞掉，system("pause") 就永远等不到按键了。
        InputGate gate = g_inputGate.load();

        // 暂停态：cmd.exe 的 pause 在读控制台，这里绝不读
        if (gate == InputGate::Paused) continue;

        // 关闭态（游戏演出）：读走并丢弃全部输入记录。
        // 用 ReadConsoleInput 而非 ReadConsoleW：ReadConsoleW 只返回字符记录，
        // 遇到 Shift 等无字符按键会被唤醒后阻塞；ReadConsoleInput 返回所有记录。
        // 先用 PeekConsoleInput 探测：只有确实有记录才读，绝不在 ReadConsoleInput
        // 上长期阻塞——否则门控切换后（Paused/Open）本线程仍卡在读取里，
        // 既吞掉暂停期间的回车，也让输入窗口打开后取不到新键入的内容。
        if (gate == InputGate::Closed)
        {
            INPUT_RECORD recs[32];
            DWORD peek = 0;
            if (!PeekConsoleInput(hIn, recs, 32, &peek) || peek == 0) continue;

            // 探测与读取之间门控可能已切换：若已进入 Paused，绝不读，
            // 记录留给 cmd.exe 的 pause（读走就会让暂停等不到按键）。
            if (g_inputGate.load() == InputGate::Paused) continue;

            DWORD n = 0;
            ReadConsoleInput(hIn, recs, 32, &n);
            continue;
        }

        // 开门态：先确认确有字符输入再读行。焦点变化等非字符事件
        // 会让句柄有信号，直接 ReadConsoleW 会一直等一个完整行（线程卡住）
        if (!HasCharInput(hIn))
        {
            INPUT_RECORD recs[32];
            DWORD n = 0;
            ReadConsoleInput(hIn, recs, 32, &n); // 丢弃非字符事件后继续轮询
            continue;
        }

        string input = ReadConsoleUtf8Line();

        if (!g_running || !g_inputThreadRunning) break;

        if (g_inGame)
        {
            // 游戏中：仅当服务器已征求输入（__INPUT__ 已到，窗口打开）时才入队，
            // 由主线程 FlushGameInput() 发送；演出/等待期间键入的直接丢弃，
            // 防止击键错位到下一次选择。
            if (!g_inputSolicited) continue;

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

// ============ 接收线程（可重建） ============

// 启动房间管理器接收线程。旧的线程必须已退出（加入）后再启动新的。
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

            if (!ReceiveLines(g_sock, buffer, [](const string& line)
            {
                lock_guard<mutex> lock(g_mutex);
                g_msgQueue.push(line);
                g_cv.notify_one();
            }))
            {
                // 大厅连接断开：非切换游戏过程中 → 通知主线程退出
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

// 启动游戏服务器接收线程。旧的线程必须已退出（加入）后再启动新的。
void StartGameRecvThread()
{
    if (g_gameRecvThread.joinable()) g_gameRecvThread.join();

    g_gameRecvThread = thread([]()
    {
        string buffer;
        ClientLog("RECV_START");

        while (g_running && g_inGame)
        {
            if (g_gameSock == INVALID_SOCKET)
            {
                this_thread::sleep_for(chrono::milliseconds(50));
                continue;
            }

            if (!ReceiveLines(g_gameSock, buffer, [](const string& line)
            {
                lock_guard<mutex> lock(g_mutex);
                g_msgQueue.push(line);
                g_cv.notify_one();
            }))
            {
                // 观测点：接收失败（连接断开/异常）——排查"客户端不再读取
                // 游戏数据"（服务器发送被 TCP 窗口卡死）时区分线程是否已退出
                ClientLog("RECV_FAIL: errno=" + to_string(WSAGetLastError()));

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

// ============ 连接大厅与回房 ============

// 连接房间管理器（大厅）。失败时重试 3 次，仍失败则 Pause 并退出程序。
// 成功时自动发送 HELLO/NAME；若刚结束游戏，则先发 GAME_ENDED 再发 REJOIN 回到原房间。
bool ConnectToRoomManager()
{
    const int maxTries = 3;

    for (int attempt = 1; attempt <= maxTries; ++attempt)
    {
        if (attempt > 1)
        {
            cout << "Failed to connect to lobby (attempt " << (attempt - 1) << "), retrying in 5 s ..." << endl;
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

        g_sock = s;

        SendRaw("HELLO|START");
        SendRaw("NAME|" + g_playerName);

        // 刚结束游戏：通知大厅释放房间状态，并请求回到原房间
        if (!g_roomId.empty())
        {
            SendRaw("GAME_ENDED|" + g_roomId);
            SendRaw("REJOIN|" + g_roomId + "|" + to_string(g_myGamePlayerId));
        }

        StartRoomRecvThread();
        return true;
    }

    // 3 次都失败：Pause 后退出（输入线程可能已启动，走线程驱动暂停）
    cout << "Unable to connect to the lobby server. The program will exit." << endl;
    PauseAndWait();
    g_running = false;
    return false;
}

// ============ 离开游戏与重连 ============

// 尽力通知游戏服务器"放弃重连"（连接一次、发送即走，结果无关紧要）。
void SendGiveUp()
{
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

// 结束游戏状态，回到大厅并自动加入原房间。
void ReturnToRoom()
{
    // 观测点：游戏结束/放弃，进入回房流程（自动化测试据此判断游戏结束）
    ClientLog("ReturnedToRoom");

    g_inGame = false;
    g_switchingToGame = false;

    // 输入窗口关闭：游戏已结束，不再征求输入
    g_inputSolicited = false;

    // 清掉游戏输入队列残留（演出/暂停期间误敲的键、还没被 __INPUT__ 消费的行），
    // 避免回房后被误当命令发送
    {
        lock_guard<mutex> lock(g_gameCmdMutex);
        g_gameCmdQueue.clear();
    }

    // 清掉大厅命令队列残留（GAME_PREPARE 连接期间、游戏演出期间误敲的键，
    // 输入线程可能已按 g_inGame=false 路由到这里）
    {
        lock_guard<mutex> lock(g_cmdMutex);
        g_cmdQueue.clear();
    }

    // 丢弃控制台输入缓冲中未读的击键（含游戏中键入但没回车的半截行），
    // 防止回房后这些输入错位进入大厅命令队列。
    // 不能用"注入回车"方案：那会把半截输入补成一行，同样造成错位。
    FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));

    // 先置标志再关套接字，避免游戏接收线程在关闭瞬间误推 __CONN_LOST__
    if (g_gameSock != INVALID_SOCKET)
    {
        closesocket(g_gameSock);
        g_gameSock = INVALID_SOCKET;
    }

    // 输入线程常驻，无需重启

    // 连接大厅（内部失败会重试 3 次后 Pause 退出）；
    // 连接期间保持门控关闭，键入内容会被输入线程丢弃
    ConnectToRoomManager();

    // 重置提示符状态：否则 g_promptDisplayed 仍为 true，提示符不会重新显示
    g_promptDisplayed = false;

    // 回到大厅/房间：重新打开输入门控（提示符等待命令输入）
    SetInputGate(InputGate::Open);

    if (g_running)
    {
        ShowPrompt();
    }
}

// 游戏连接中断后的重连流程（由主线程调用，可能阻塞 10~20 秒）。
// 最多尝试 3 次；全部失败 → 发送 GIVEUP|N 通知服务器，然后返回房间。
void HandleGameReconnect()
{
    const int maxTries = 3;

    EnsureNewLine();
    cout << "Connection to the game server lost, attempting to reconnect ..." << endl;

    // 关闭已失效的旧连接并等待旧接收线程退出
    if (g_gameSock != INVALID_SOCKET)
    {
        closesocket(g_gameSock);
        g_gameSock = INVALID_SOCKET;
    }

    if (g_gameRecvThread.joinable()) g_gameRecvThread.join();

    for (int attempt = 1; attempt <= maxTries; ++attempt)
    {
        if (attempt > 1)
        {
            cout << "  Reconnect attempt " << (attempt - 1) << " failed, retrying in 5 s ..." << endl;
            Sleep(5000);
        }

        cout << "  Attempting reconnect " << attempt << "/" << maxTries << " ..." << endl;

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

        // 重连成功：告知服务器我们的玩家编号
        string idMsg = "PLAYER_ID|" + to_string(g_myGamePlayerId) + "\n";
        send(s, idMsg.c_str(), idMsg.length(), 0);

        g_gameSock = s;
        StartGameRecvThread();

        cout << "  Reconnected successfully, game continues!" << endl;
        return;
    }

    // 3 次都失败：通知服务器放弃（服务器据此立即结束本局，对方回到房间）
    cout << "  Reconnect failed, notifying server and returning to room ..." << endl;
    SendGiveUp();
    ReturnToRoom();
}

// ============ 游戏消息处理 ============

// 把输入线程捕获到的游戏输入发给服务器（仅当输入窗口打开时）。
// 只发送第一行非空输入：误按回车产生的空行丢弃（窗口保持打开），
// 窗口期内多敲的剩余行也丢弃，防止错位到下一次 __INPUT__。
// 发送完成后关闭输入窗口（游戏演出期间不再捕获输入）。
void FlushGameInput()
{
    if (!g_inputSolicited) return;

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

        g_gameCmdQueue.clear(); // 剩余行是同一窗口期内多敲的，全部丢弃
    }

    auto sendStartMs = GetTickCount64();
    SendGameRaw("PLAYER_" + to_string(g_myGamePlayerId) + "|" + input);
    auto sendElapsedMs = GetTickCount64() - sendStartMs;

    // 观测点：send() 若长时间阻塞（对端接收窗口满/数据卡在发送缓冲），
    // 说明服务器没在读我们的数据——这是"输入到达服务器迟了几十秒"的直接证据
    if (sendElapsedMs > 200)
    {
        ClientLog("SEND_SLOW_MS:" + to_string(sendElapsedMs));
    }

    // 观测点：已把玩家输入发给游戏服务器（自动化测试据此核对 I/O 全链路）
    ClientLog("SENT:" + input);

    // 输入已发出：关闭窗口并隐藏光标
    g_inputSolicited = false;
    SetInputGate(InputGate::Closed);
    ShowCursor(false);
}

bool ProcessGameMessage(string& msg)
{
    // 游戏结束（服务器通知或连接彻底丢失）
    if (msg.find("LEFT_GAME|") == 0 || msg == "Game over." || msg == "Game over")
    {
        EnsureNewLine();
        cout << "Game ended." << endl;
        ReturnToRoom();
        return true;
    }

    // 服务器征求输入。注意：只能"打开窗口"后立即返回，绝不能在这里阻塞等待！
    // 服务器在 __INPUT__ 之后紧跟菜单/提示文本（先发 __INPUT__ 再发提示，
    // 让玩家在提示显示期间就能输入），若阻塞，菜单要等输入后才显示，
    // 表现为"菜单在输入后才出现、输入后无反应"。
    // 实际发送由主循环的 FlushGameInput() 完成：取输入线程捕获的第一行
    // 非空输入发给服务器，然后关闭窗口。
    if (msg == "__INPUT__")
    {
        g_inputSolicited = true;
        ShowCursor(true);
        SetInputGate(InputGate::Open);

        // 观测点：输入窗口打开（自动化测试据此注入按键，也便于排查"输入无反应"）
        ClientLog("INPUT_OPEN");
        return true;
    }

    if (msg == "__CLS__")
    {
        ClearScreen();
        return true;
    }

    if (msg == "__PAUSE__")
    {
        // 配合输入门控：暂停期间输入线程（InputGate::Paused）完全不读控制台，
        // 主线程直接读控制台事件等待按键（PauseWaitConsole，不拉 cmd.exe），
        // 按任意键一次即继续。先清掉缓冲残留（暂停前的击键/半行），防止
        // 暂停被瞬间跳过。
        EnsureNewLine();
        cout << "\n[ Pause ]\n";
        ShowCursor(true);
        SetInputGate(InputGate::Paused);

        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        FlushConsoleInputBuffer(hIn);

        // 观测点：暂停开始等待按键（自动化测试据此注入一个回车跳过暂停）
        ClientLog("PAUSE");

        PauseWaitConsole();

        // 丢弃暂停期间键入的内容（无回显），避免污染暂停后的输入
        FlushConsoleInputBuffer(hIn);

        // 暂停结束：回到游戏演出状态（关闭门控，隐藏光标）
        SetInputGate(InputGate::Closed);
        ShowCursor(false);
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

    // 服务器欢迎语（"You are Player N"）忽略
    if (msg.find("You are Player ") == 0) return true;

    cout << msg << endl;
    return true;
}

// ============ 主流程 ============

int main()
{
    DisableConsoleQuickEdit();
    SetConsoleUtf8();
    SetConsoleFont();
    ShowCursor(true);

    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        cout << "WSAStartup failed" << endl;
        _getch();
        return 1;
    }

    ClientLog("Client started");
    ClearScreen();

    cout << "=== Demon Roulette Client ===" << endl;
    cout << "Enter your name: ";

    g_playerName = SanitizeName(ReadConsoleUtf8Line());

    cout << "Enter server IP (default 127.0.0.1): ";
    g_startIp = ReadConsoleUtf8Line();
    if (g_startIp.empty()) g_startIp = "127.0.0.1";

    // 校验 IP 格式：非法地址会让 inet_pton 失败（连接落在 0.0.0.0 上），
    // 只会白白重试 3 次后退出。给玩家重新输入的机会。
    while (true)
    {
        sockaddr_in test;
        if (inet_pton(AF_INET, g_startIp.c_str(), &test.sin_addr) == 1) break;

        cout << "Invalid IP address format. Enter server IP (default 127.0.0.1): ";
        g_startIp = ReadConsoleUtf8Line();
        if (g_startIp.empty()) g_startIp = "127.0.0.1";
    }

    cout << "Enter server port (default 8888): ";

    string portStr = ReadConsoleUtf8Line();
    if (portStr.empty()) portStr = "8888";

    // 端口必须为 1024-65535 的纯数字，否则连接必然失败，尽早纠正
    while (!IsValidPort(portStr))
    {
        cout << "Invalid port (must be 1024-65535). Enter server port (default 8888): ";
        portStr = ReadConsoleUtf8Line();
        if (portStr.empty()) portStr = "8888";
    }

    g_startPort = atoi(portStr.c_str());

    // 连接房间管理器（失败会重试 3 次后 Pause 退出）
    if (!ConnectToRoomManager())
    {
        WSACleanup();
        return 0;
    }

    // 丢弃启动阶段最后的击键残留（玩家连续按回车输入的尾巴），
    // 防止它们进入输入线程的第一次读取（"启动击键渗漏"问题）
    FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));

    g_inputThread = thread(InputThreadFunc);

    // 大厅/房间：输入门控常开（提示符等待命令输入）
    SetInputGate(InputGate::Open);

    // 观测点：已连上大厅、输入线程就绪（自动化测试据此注入 CREATE/JOIN 等命令）
    ClientLog("Lobby connected");

    ShowPrompt();

    while (g_running)
    {
        string msg;
        bool msgHandled = false;

        while (PopMessage(msg, 10))
        {
            // 观测点：主循环弹出消息（进度条刷屏除外），用于定位客户端
            // 处理延迟/卡顿发生在哪条消息上
            if (g_inGame && msg.find("__PROGRESS__") != 0) ClientLog("G_POP: " + msg);

            if (msg == "DISCONNECTED")
            {
                EnsureNewLine();
                cout << "Disconnected from the room manager" << endl;
                g_running = false;
                break;
            }

            if (msg == "__CONN_LOST__")
            {
                // 游戏连接中断 → 关闭输入窗口（断线期间键入内容丢弃），
                // 进入重连流程（内部会阻塞一段时间）；重连成功后
                // 服务器会重新发 __INPUT__ 征求输入
                if (g_inGame)
                {
                    g_inputSolicited = false;
                    SetInputGate(InputGate::Closed);
                    ShowCursor(false);
                    HandleGameReconnect();
                }
                continue;
            }

            if (g_inGame)
            {
                ProcessGameMessage(msg);
                continue;
            }

            // ---- 开始游戏：连接房间管理器指定的游戏服务器 ----
            if (msg.find("GAME_PREPARE|") == 0)
            {
                string rest = msg.substr(13);
                size_t p1 = rest.find('|');
                size_t p2 = rest.find('|', p1 + 1);
                size_t p3 = rest.find('|', p2 + 1);

                if (p1 == string::npos || p2 == string::npos || p3 == string::npos) continue;

                g_gameServerPort = atoi(rest.substr(0, p1).c_str());
                g_gameServerIp = rest.substr(p2 + 1, p3 - p2 - 1);
                g_myGamePlayerId = atoi(rest.substr(p3 + 1).c_str());
                g_switchingToGame = true;

                // 清掉上一局残留的游戏输入（输入线程按 g_inGame 路由，可能留底）
                {
                    lock_guard<mutex> lock(g_gameCmdMutex);
                    g_gameCmdQueue.clear();
                }

                // 清掉大厅命令队列：连接游戏期间键入的命令若残留，
                // 回房后会被当大厅命令自动执行（如 EXIT 导致回房后被踢）
                {
                    lock_guard<mutex> lock(g_cmdMutex);
                    g_cmdQueue.clear();
                }

                // 游戏演出文本不能追在大厅提示符后面
                g_promptDisplayed = false;

                // 进入游戏：关闭输入门控并隐藏光标。
                // 连接失败走 ReturnToRoom 会重新打开门控，无需在此恢复
                g_inputSolicited = false;
                SetInputGate(InputGate::Closed);
                ShowCursor(false);

                // 关闭大厅连接（游戏期间不再接收大厅消息）
                closesocket(g_sock);
                g_sock = INVALID_SOCKET;
                g_inRoom = false;

                EnsureNewLine();
                cout << "Connecting to the game server ..." << endl;

                g_gameSock = socket(AF_INET, SOCK_STREAM, 0);

                if (g_gameSock == INVALID_SOCKET)
                {
                    ClientLog("Failed to create game socket");
                    g_switchingToGame = false;
                    ReturnToRoom();
                    continue;
                }

                sockaddr_in gameAddr;
                gameAddr.sin_family = AF_INET;
                inet_pton(AF_INET, g_gameServerIp.c_str(), &gameAddr.sin_addr);
                gameAddr.sin_port = htons(g_gameServerPort);

                // 连接游戏服务器：带重试。房间管理器现在是"先 spawn 再通知"，
                // 正常首连即成功；万一服务器监听尚未就绪，重试兜底。
                // 失败的 connect 会使套接字失效，必须重建后重试（2026-08-03）。
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
                            ClientLog("Failed to recreate game socket (attempt " + to_string(attempt) + ")");
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

                if (connected)
                {
                    g_inGame = true;

                    string idMsg = "PLAYER_ID|" + to_string(g_myGamePlayerId) + "\n";
                    send(g_gameSock, idMsg.c_str(), idMsg.length(), 0);

                    ClientLog("Connected to game server, player ID: " + to_string(g_myGamePlayerId));

                    StartGameRecvThread();

                    // 输入线程常驻，无需暂停/重建；
                    // 门控已在 GAME_PREPARE 处关闭，仅 __INPUT__ 时打开
                }
                else
                {
                    ClientLog("Failed to connect to game server");
                    cout << "Failed to connect to the game server." << endl;
                    g_switchingToGame = false;
                    ReturnToRoom();
                }

                continue;
            }

            // ---- 大厅/房间消息 ----
            EnsureNewLine();

            if (msg.find("ROOM_EVENT|") == 0)
            {
                string content = msg.substr(11);
                if (content.empty()) cout << endl;
                else cout << content << endl;
                msgHandled = true;
            }
            else if (msg.find("ROOM_STATUS|") == 0)
            {
                // 房间准备状态：name1|ready1|name2|ready2
                string data = msg.substr(12);
                vector<string> parts;
                size_t pos = 0;

                while ((pos = data.find('|')) != string::npos)
                {
                    parts.push_back(data.substr(0, pos));
                    data.erase(0, pos + 1);
                }

                parts.push_back(data);

                cout << "--- Room status ---" << endl;

                if (parts.size() >= 2)
                {
                    cout << "  " << parts[0] << " (P1): " << (parts[1] == "1" ? "READY" : "not ready") << endl;
                }

                if (parts.size() >= 4 && !parts[2].empty())
                {
                    cout << "  " << parts[2] << " (P2): " << (parts[3] == "1" ? "READY" : "not ready") << endl;
                }
                else
                {
                    cout << "  Player 2: not joined" << endl;
                }

                msgHandled = true;
            }
            else if (msg.find("REJOIN_FAIL|") == 0)
            {
                // 原房间已被释放 → 留在大厅，并清空回房信息
                cout << msg.substr(12) << endl;
                g_roomId = "";
                g_myGamePlayerId = 0;
                msgHandled = true;
            }
            else if (msg.find("LEFT_ROOM|") == 0)
            {
                cout << msg.substr(10) << endl;
                msgHandled = true;
            }
            else if (msg.find("ROOMS_LIST|") == 0)
            {
                string data = msg.substr(11);

                // 观测点：记录收到的房间列表原样（自动化测试据此断言
                // 游戏期间房间是否仍在列表、重复建房是否被拒绝）
                ClientLog("ROOMS_LIST:" + data);

                if (data == "EMPTY")
                {
                    cout << "No rooms available." << endl;
                }
                else
                {
                    cout << "Room List" << endl;

                    size_t pos = 0;
                    string item;

                    while ((pos = data.find('|')) != string::npos)
                    {
                        item = data.substr(0, pos);
                        data.erase(0, pos + 1);
                        cout << item << endl;
                    }

                    if (!data.empty()) cout << data << endl;
                }

                msgHandled = true;
            }
            else if (msg.find("CREATED|") == 0)
            {
                string rest = msg.substr(8);
                size_t pos = rest.find('|');

                if (pos != string::npos)
                {
                    g_roomId = rest.substr(0, pos);
                    g_inRoom = true;
                    g_inGame = false;
                    g_isAdmin = false; // ADMIN| 消息会重新置为 true
                }

                msgHandled = true;
            }
            else if (msg.find("JOINED|") == 0)
            {
                g_roomId = msg.substr(7);
                g_inRoom = true;
                g_inGame = false;
                g_isAdmin = false; // 若收到 ADMIN| 会重新置为 true
                msgHandled = true;
            }
            else if (msg.find("ADMIN|") == 0)
            {
                g_isAdmin = true;
                msgHandled = true;
            }
            else if (msg.find("NAME_SET|") == 0)
            {
                g_playerName = msg.substr(9);
                msgHandled = true;
            }
            else if (msg.find("ERROR:") == 0)
            {
                // 观测点：记录房间管理器错误回复（如 "Port already in use"）
                ClientLog("ERROR:" + msg.substr(6));

                cerr << "Error: " << msg.substr(6) << endl;
                msgHandled = true;
            }
            else if (msg.find("WELCOME|") == 0 || msg.find("COMMANDS|") == 0)
            {
                continue;
            }
            else if (!msg.empty())
            {
                cout << msg << endl;
                msgHandled = true;
            }
        }

        if (g_inGame)
        {
            // 游戏中：每轮把捕获到的游戏输入发给服务器（窗口打开时）
            FlushGameInput();
            continue;
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

            // 提示符已随输入行"消费"：重置标志，让底部路径（本地处理）
            // 或响应派发（msgHandled）能真实重绘提示符。否则 g_promptDisplayed
            // 仍为 true，ShowPrompt() 空转，会出现"输入后无提示符、需再按回车"
            // 的问题（2026-08-03）。
            g_promptDisplayed = false;

            // 命令是否交给了服务端等待响应。交给服务端的命令不立即重绘提示
            //（由响应到达后的 msgHandled 分支重绘）；本地处理/未识别的命令走
            // 底部路径立即重绘——保证每条输入都像命令行一样回到提示符，
            // 而不是必须等一次回车（2026-08-03）。
            bool cmdAwaitReply = false;

            if (!cmd.empty())
            {
                if (cmd == "HELP")
                {
                    PrintHelp();
                    ShowPrompt();
                    continue;
                }
                else if (cmd == "EXIT")
                {
                    if (g_inGame)
                    {
                        // 游戏中退出：先通知服务器放弃，再回到房间
                        SendGiveUp();
                        ReturnToRoom();
                    }
                    else if (g_inRoom)
                    {
                        SendRaw("EXIT");
                        g_inRoom = false;
                        g_isAdmin = false;
                        g_roomId = "";
                        g_myGamePlayerId = 0;
                    }
                    else
                    {
                        SendRaw("EXIT");
                        g_running = false;
                        break;
                    }
                }
                else if (cmd == "cls" || cmd == "clear")
                {
                    ClearScreen();
                    g_promptDisplayed = false;
                    ShowPrompt();
                    continue;
                }
                else if (g_inGame)
                {
                    SendGameRaw(cmd);
                }
                else if (!g_inRoom)
                {
                    if (cmd == "LIST")
                    {
                        SendRaw("LIST");
                        cmdAwaitReply = true;
                    }
                    else if (cmd.find("CREATE") == 0)
                    {
                        istringstream iss(cmd);
                        string t;
                        string p;
                        iss >> t >> p;

                        if (IsValidPort(p))
                        {
                            SendRaw("CREATE " + p);
                            cmdAwaitReply = true;
                        }
                        else
                        {
                            cout << "Invalid port (must be 1024-65535)." << endl;
                        }
                    }
                    else if (cmd.find("JOIN") == 0)
                    {
                        istringstream iss(cmd);
                        string t;
                        string p;
                        iss >> t >> p;

                        if (IsValidPort(p))
                        {
                            SendRaw("JOIN " + p);
                            cmdAwaitReply = true;
                        }
                        else
                        {
                            cout << "Invalid port (must be 1024-65535)." << endl;
                        }
                    }
                    else if (cmd.find("NAME") == 0)
                    {
                        // 支持 NAME|<name> 和 NAME <name> 两种格式（向后兼容）
                        string n;
                        size_t pipePos = cmd.find('|');
                        if (pipePos != string::npos)
                            n = cmd.substr(pipePos + 1);
                        else
                        {
                            istringstream iss(cmd);
                            string t;
                            iss >> t;
                            getline(iss, n);
                            if (!n.empty() && n[0] == ' ') n = n.substr(1);
                        }

                        if (!n.empty())
                        {
                            g_playerName = SanitizeName(n);
                            SendRaw("NAME|" + g_playerName);
                            cmdAwaitReply = true;
                        }
                    }
                }
                else if (g_inRoom)
                {
                    if (cmd == "READY")
                    {
                        SendRaw("READY");
                        cmdAwaitReply = true;
                    }
                    else if (cmd == "PICK" && g_isAdmin)
                    {
                        SendRaw("PICK");
                        cmdAwaitReply = true;
                    }
                    else if (cmd == "STATUS")
                    {
                        SendRaw("STATUS");
                        cmdAwaitReply = true;
                    }
                    else
                    {
                        // 房间聊天：清理换行，防止伪造协议
                        SendRaw(SanitizeChat(cmd));
                        cmdAwaitReply = true;
                    }
                }
            }
            else
            {
                // 空命令（直接回车）：强制换行以重新显示路径提示符
                g_promptDisplayed = false;
            }

            // 本地处理/未识别的命令立即回到提示符；交给服务端的命令
            // 由响应派发路径（msgHandled）负责重绘，这里不再画
            if (g_running && !g_inGame && !cmdAwaitReply) ShowPrompt();
        }

        Sleep(10);
    }

    g_running = false;
    g_inputThreadRunning = false;

    // 关闭输入门控：输入线程不再读取/路由任何输入
    SetInputGate(InputGate::Closed);

    // 若输入线程正阻塞在 ReadConsoleW（键入了字符但没回车），
    // 向控制台输入缓冲注入一个回车事件，让它及时返回并退出。
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
    cout << "\n[ Pause ]\n";

    // 输入线程已退出，此时无并发读控制台，直接读控制台事件等待按键
    PauseWaitConsole();
    return 0;
}
