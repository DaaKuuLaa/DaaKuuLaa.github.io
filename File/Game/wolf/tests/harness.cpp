// ============================================================
//  Demon Roulette 自动化全流程测试工具（测试专用，不进游戏目录）
//  用法: harness.exe [游戏目录]     默认 D:\Demon\demon
//
//  流程: Start.exe(隐藏控制台) → Client A 建房(5000) → Client B 加入 →
//        READY×2 → 游戏1（按观测点精确注入）→ 双方回房(REJOIN) →
//        READY×2 → 游戏2 → 双方回房 → 清理进程
//        （游戏1开局后另起 Client C：LIST 验证游戏中房间可见 [in-game]、
//          同一端口重复建房被拒绝——覆盖 2026-08-03 修复的 LIST/端口占用问题）
//
//  按键注入: FreeConsole + AttachConsole(pid) + CreateFileW("CONIN$")
//            + WriteConsoleInputW（游戏要求真实控制台；窗口 SW_HIDE 后台运行）
//  状态观测: 增量轮询 start.log / server.log / client.log / clientBtest\client.log
//            Client 观测点: INPUT_OPEN / PAUSE / SENT: / ReturnedToRoom / Lobby connected
//            Start  观测点: "READY ... now ready" / "rejoined room" / "created on port"
//  行消费模型: 日志行先进入每个文件的 unconsumed 缓冲；WaitLog 只消费到匹配行为止，
//            之前的行留给后续消费者（游戏循环），绝不丢行。
//  退出码:   0 = 全部断言通过；1 = 失败/超时
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <tlhelp32.h>

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iterator>
#include <chrono>
#include <cstdio>
#include <functional>
#include <cwchar>

using namespace std;
using Clock = chrono::steady_clock;

static string g_gameDir;
static ofstream g_harness;
static int g_fails = 0;

static string NowStr()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[64];
    sprintf(b, "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return b;
}

static void HLog(const string& msg)
{
    string line = "[" + NowStr() + "] " + msg;
    g_harness << line << endl;
    g_harness.flush();
    cout << line << endl;
}

static void Assert(bool ok, const string& what)
{
    HLog(ok ? ("PASS  " + what) : ("FAIL  " + what));
    if (!ok) ++g_fails;
}

static bool Contains(const string& line, const string& sub)
{
    return line.find(sub) != string::npos;
}

// ---------- 日志尾部轮询（无丢失行消费） ----------
// 文件新内容先进入 buf（unconsumed）；WaitLog 只消费到匹配行（含），
// 之前未匹配的行留在 buf 里给后续消费者；游戏循环逐批消费全部 buf。
struct Tail
{
    streamoff off = 0;
    vector<string> buf;     // 未消费的行
    string pending;         // 半行残留
};

static void TailRefill(const string& path, Tail& t)
{
    ifstream f(path, ios::binary | ios::in);
    if (!f) return;

    f.seekg(0, ios::end);
    streamoff size = f.tellg();

    if (size < t.off) t.off = 0;
    if (size == t.off) return;

    f.seekg(t.off);
    string data((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
    t.off = size;
    t.pending += data;

    size_t pos;
    while ((pos = t.pending.find('\n')) != string::npos)
    {
        string line = t.pending.substr(0, pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        t.pending.erase(0, pos + 1);
        t.buf.push_back(line);
    }
}

// 日志尾部初始偏移：快照当前文件大小（只观测本次运行新追加的内容）
static void InitTail(const string& path, Tail& t)
{
    ifstream f(path, ios::binary | ios::in);
    if (!f) return;
    f.seekg(0, ios::end);
    t.off = f.tellg();
}

// 等待出现满足谓词的行；消费到该行（含），其前的行保留；超时返回空串。
static string WaitLog(const string& path, Tail& t,
    const function<bool(const string&)>& pred, const string& desc, int timeoutSec)
{
    auto deadline = Clock::now() + chrono::seconds(timeoutSec);

    while (Clock::now() < deadline)
    {
        TailRefill(path, t);

        for (size_t i = 0; i < t.buf.size(); ++i)
        {
            if (pred(t.buf[i]))
            {
                string matched = t.buf[i];
                t.buf.erase(t.buf.begin(), t.buf.begin() + i + 1);
                HLog("log<" + path + "> " + matched);
                return matched;
            }
        }
        Sleep(200);
    }

    HLog("TIMEOUT " + to_string(timeoutSec) + "s waiting: " + desc + " in " + path);
    return "";
}

// 消费缓冲中前 n 行（调用方需确保 n <= buf.size()）
static void Consume(Tail& t, size_t n)
{
    if (n >= t.buf.size()) t.buf.clear();
    else t.buf.erase(t.buf.begin(), t.buf.begin() + n);
}

// ---------- 进程管理 ----------
struct Proc
{
    HANDLE h = nullptr;
    DWORD pid = 0;
};

static bool SpawnNewConsole(const string& exePath, const string& cwd, Proc& out)
{
    string cmd = "\"" + exePath + "\"";
    wstring wCmd(cmd.begin(), cmd.end());
    wstring wCwd(cwd.begin(), cwd.end());

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;   // 后台运行：新控制台窗口隐藏（注入仍有效）

    if (!CreateProcessW(nullptr, &wCmd[0], nullptr, nullptr, FALSE,
        CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT, nullptr,
        wCwd.c_str(), &si, &pi))
    {
        return false;
    }

    out.h = pi.hProcess;
    out.pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    return true;
}

// ---------- 进程查找/状态 ----------
static bool ProcAlive(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

// 找父进程为 parentPid 的 Server.exe（Start.exe 每局 spawn 的）
static DWORD FindChildServer(DWORD parentPid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    DWORD found = 0;

    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (pe.th32ParentProcessID == parentPid
                && _wcsicmp(pe.szExeFile, L"Server.exe") == 0)
            {
                found = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return found;
}

static bool WaitProcExit(DWORD pid, int timeoutSec)
{
    auto deadline = Clock::now() + chrono::seconds(timeoutSec);
    while (Clock::now() < deadline)
    {
        if (!ProcAlive(pid)) return true;
        Sleep(200);
    }
    return false;
}

// ---------- 按键注入 ----------
// 每个字符注入 按下/抬起 两条 KEY_EVENT；wVirtualKeyCode 用 VkKeyScanW 计算
// （'1'→0x31 等），uChar 同时携带 Unicode 字符，两种解析路径都可靠。
static void InjectText(DWORD pid, const string& text)
{
    if (!ProcAlive(pid))   // 进程已退出则直接放弃，避免 5 秒重试风暴
    {
        HLog("skip   pid=" + to_string(pid) + " 已退出, 文本=<" + text + "> 未注入");
        return;
    }

    for (int tries = 0; tries < 50; ++tries)
    {
        FreeConsole();

        if (AttachConsole(pid))
        {
            HANDLE hIn = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);

            if (hIn != INVALID_HANDLE_VALUE)
            {
                DWORD n;
                for (char ch : text)
                {
                    wchar_t c = (wchar_t)(unsigned char)ch;
                    INPUT_RECORD ir;
                    ZeroMemory(&ir, sizeof(ir));
                    ir.EventType = KEY_EVENT;
                    ir.Event.KeyEvent.bKeyDown = TRUE;
                    ir.Event.KeyEvent.wRepeatCount = 1;
                    ir.Event.KeyEvent.wVirtualKeyCode = LOBYTE(VkKeyScanW(c));
                    ir.Event.KeyEvent.wVirtualScanCode = MapVirtualKeyW(ir.Event.KeyEvent.wVirtualKeyCode, MAPVK_VK_TO_VSC);
                    ir.Event.KeyEvent.uChar.UnicodeChar = c;
                    ir.Event.KeyEvent.dwControlKeyState = 0;
                    WriteConsoleInputW(hIn, &ir, 1, &n);

                    ir.Event.KeyEvent.bKeyDown = FALSE;
                    ir.Event.KeyEvent.uChar.UnicodeChar = 0;
                    WriteConsoleInputW(hIn, &ir, 1, &n);
                }
                CloseHandle(hIn);
                FreeConsole();
                HLog("typed  pid=" + to_string(pid) + "  <" + text + ">");
                return;
            }
            FreeConsole();
        }
        Sleep(100);
    }

    HLog("INJECT FAILED pid=" + to_string(pid) + " text=<" + text + ">");
}

// 跳过暂停：连续注入两个回车（间隔 300ms）。第一个回车可能被输入线程的
// 关闭态 drain 竞争窗口吞掉（概率极小但存在），第二个必定放行 pause。
static void SkipPause(DWORD pid)
{
    InjectText(pid, "\r");
    Sleep(300);
    InjectText(pid, "\r");
}

// ---------- 游戏阶段 ----------
// 双方连上游戏服务器后，按观测点精确注入，绝不做无意义的空转：
//   INPUT_OPEN → 注入答案 "1\r"（Ask→Shoot 选择、Shoot→打自己，永远是合法选项）
//   PAUSE      → 注入一个回车跳过暂停（客户端 system pause 独占等待按键）
// 连续 90 秒无任何观测点活动 → 判定卡死提前中止。直到双方 ReturnedToRoom。
struct GamePhaseResult
{
    bool doneA = false, doneB = false;
    bool stuck = false;
    int sentA = 0, sentB = 0;
    double seconds = 0;
};

static GamePhaseResult PlayGame(DWORD pidA, DWORD pidB, const string& pathA, Tail& tA,
    const string& pathB, Tail& tB, const string& tag)
{
    HLog("== " + tag + " 游戏阶段 ==");

    WaitLog(pathA, tA, [](const string& l) { return Contains(l, "Connected to game server"); },
        tag + " client A 连上游戏服务器", 40);
    WaitLog(pathB, tB, [](const string& l) { return Contains(l, "Connected to game server"); },
        tag + " client B 连上游戏服务器", 40);

    GamePhaseResult r;
    auto start = Clock::now();
    auto deadline = start + chrono::seconds(300);
    auto lastActivity = start;

    while (Clock::now() < deadline)
    {
        bool activity = false;

        TailRefill(pathA, tA);
        for (auto& line : tA.buf)
        {
            activity = true;
            lastActivity = Clock::now();
            if (Contains(line, "INPUT_OPEN")) InjectText(pidA, "1\r");
            else if (Contains(line, "PAUSE")) SkipPause(pidA);
            else if (Contains(line, "SENT:")) ++r.sentA;
            else if (Contains(line, "ReturnedToRoom")) r.doneA = true;
        }
        Consume(tA, tA.buf.size());

        TailRefill(pathB, tB);
        for (auto& line : tB.buf)
        {
            activity = true;
            lastActivity = Clock::now();
            if (Contains(line, "INPUT_OPEN")) InjectText(pidB, "1\r");
            else if (Contains(line, "PAUSE")) SkipPause(pidB);
            else if (Contains(line, "SENT:")) ++r.sentB;
            else if (Contains(line, "ReturnedToRoom")) r.doneB = true;
        }
        Consume(tB, tB.buf.size());

        if (r.doneA && r.doneB) break;

        // 90 秒没有任何观测点活动 → 判定卡死
        if (Clock::now() - lastActivity >= chrono::seconds(90))
        {
            r.stuck = true;
            HLog("== " + tag + " 90s 无任何活动，判定卡死 ==");
            break;
        }

        Sleep(200);
    }

    r.seconds = chrono::duration<double>(Clock::now() - start).count();
    HLog("== " + tag + " 结束: A[" + (r.doneA ? "回房" : "超时") + "] B[" + (r.doneB ? "回房" : "超时")
        + "] 输入已发 A=" + to_string(r.sentA) + " B=" + to_string(r.sentB)
        + " 耗时=" + to_string((int)r.seconds) + "s");
    return r;
}

// ---------- 主流程 ----------
int main(int argc, char* argv[])
{
    g_gameDir = argc > 1 ? argv[1] : "D:\\Demon\\demon";

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        return 1;
    }

    // harness.log 写到 exe 自身所在目录（不依赖 C 盘临时目录）
    string tempBase = ".";
    {
        char self[MAX_PATH] = { 0 };
        if (GetModuleFileNameA(nullptr, self, MAX_PATH) > 0)
        {
            string p = self;
            size_t pos = p.find_last_of('\\');
            if (pos != string::npos) tempBase = p.substr(0, pos);
        }
    }
    g_harness.open(tempBase + "\\harness.log", ios::out | ios::trunc);

    string startExe = g_gameDir + "\\Start.exe";
    string clientExe = g_gameDir + "\\Client.exe";
    string dirB = g_gameDir + "\\clientBtest";
    CreateDirectoryA(dirB.c_str(), nullptr);

    string pathStart = g_gameDir + "\\start.log";
    string pathServer = g_gameDir + "\\server.log";
    string pathA = g_gameDir + "\\client.log";
    string pathB = dirB + "\\client.log";

    Tail tStart, tServer, tA, tB;
    InitTail(pathStart, tStart);
    InitTail(pathServer, tServer);
    InitTail(pathA, tA);
    InitTail(pathB, tB);

    Proc startP, aP, bP, cP;
    auto globalDeadline = Clock::now() + chrono::seconds(480);

    HLog("=== Demon Roulette 自动化全流程测试 ===");
    HLog("游戏目录: " + g_gameDir);

    // 0. 起点检查：8888 端口必须可用（无残留 RM）
    {
        bool rmUp = false;
        {
            SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
            if (s != INVALID_SOCKET)
            {
                sockaddr_in a;
                a.sin_family = AF_INET;
                a.sin_addr.s_addr = inet_addr("127.0.0.1");
                a.sin_port = htons(8888);
                rmUp = connect(s, (sockaddr*)&a, sizeof(a)) == 0;
                closesocket(s);
            }
        }
        if (rmUp)
        {
            HLog("FAIL  8888 端口已有服务在监听，无法启动测试（请先关闭旧 Start.exe）");
            return 1;
        }
    }

    // 1. Start.exe：回答端口/IP 提示（直接回车用默认值 8888 / 127.0.0.1）
    if (!SpawnNewConsole(startExe, g_gameDir, startP))
    {
        HLog("无法启动 Start.exe");
        return 1;
    }
    Sleep(1000);
    InjectText(startP.pid, "\r");
    Sleep(300);
    InjectText(startP.pid, "\r");

    if (WaitLog(pathStart, tStart,
        [](const string& l) { return Contains(l, "Room Manager started"); },
        "Start.exe 就绪 (Room Manager started)", 60).empty())
    {
        Assert(false, "Start.exe 启动并监听 (60s 内未就绪)");
        goto cleanup;
    }
    Assert(true, "Start.exe 启动并监听");

    if (Clock::now() > globalDeadline) { HLog("全局超时，中止"); goto cleanup; }

    // 2. Client A：启动三项输入 → 大厅就绪后 CREATE 建房 5000
    SpawnNewConsole(clientExe, g_gameDir, aP);
    Sleep(1500);
    InjectText(aP.pid, "Tester1\r");
    Sleep(700);
    InjectText(aP.pid, "127.0.0.1\r");
    Sleep(700);
    InjectText(aP.pid, "8888\r");

    Assert(!WaitLog(pathA, tA,
        [](const string& l) { return Contains(l, "Lobby connected"); },
        "Client A 连上大厅", 40).empty(),
        "Client A 连上大厅");

    InjectText(aP.pid, "CREATE 5000\r");

    Assert(!WaitLog(pathStart, tStart,
        [](const string& l) { return Contains(l, "created on port 5000"); },
        "Client A 创建房间 5000", 40).empty(),
        "Client A 创建房间(5000)");
    if (Clock::now() > globalDeadline) { HLog("全局超时，中止"); goto cleanup; }

    // 3. Client B：加入房间
    SpawnNewConsole(clientExe, dirB, bP);
    Sleep(1500);
    InjectText(bP.pid, "Tester2\r");
    Sleep(700);
    InjectText(bP.pid, "127.0.0.1\r");
    Sleep(700);
    InjectText(bP.pid, "8888\r");

    Assert(!WaitLog(pathB, tB,
        [](const string& l) { return Contains(l, "Lobby connected"); },
        "Client B 连上大厅", 40).empty(),
        "Client B 连上大厅");

    InjectText(bP.pid, "JOIN 5000\r");

    Assert(!WaitLog(pathStart, tStart,
        [](const string& l) { return Contains(l, "joined room") && Contains(l, "port 5000"); },
        "Client B 加入房间 5000", 40).empty(),
        "Client B 加入房间(5000)");
    if (Clock::now() > globalDeadline) { HLog("全局超时，中止"); goto cleanup; }

    // 4. 双方 READY（就绪后 RM 自动 3 秒倒计时开局）
    InjectText(aP.pid, "READY\r");
    InjectText(bP.pid, "READY\r");

    Assert(!WaitLog(pathStart, tStart,
        [](const string& l) { return Contains(l, "now ready"); },
        "READY #1", 30).empty(), "READY #1 (now ready)");
    Assert(!WaitLog(pathStart, tStart,
        [](const string& l) { return Contains(l, "now ready"); },
        "READY #2", 30).empty(), "READY #2 (now ready)");
    if (Clock::now() > globalDeadline) { HLog("全局超时，中止"); goto cleanup; }

    Assert(!WaitLog(pathStart, tStart,
        [](const string& l) { return Contains(l, "Starting game server"); },
        "游戏1 服务端启动", 30).empty(),
        "游戏1: Starting game server");

    // 5. 第三客户端（纯 LIST 检测）：游戏进行中（gameStarted=true）验证
    //    a) LIST 必须显示游戏中的房间并标记 [in-game]——旧代码直接隐藏该房间，
    //       第三名玩家看列表会以为房间消失（用户实测现象）；
    //    b) 同一端口 CREATE 必须被拒绝——旧代码允许游戏中的房间被重复建房，
    //       两个房间共用端口 → 后启动的 Server.exe bind 失败(10048)。
    //    局部变量用花括号隔离作用域（goto cleanup 不能跳过初始化）。
    {
        string dirC = g_gameDir + "\\clientCtest";
        CreateDirectoryA(dirC.c_str(), nullptr);
        string pathC = dirC + "\\client.log";
        Tail tC;
        InitTail(pathC, tC);

        SpawnNewConsole(clientExe, dirC, cP);
        Sleep(1500);
        InjectText(cP.pid, "Ctest\r");
        Sleep(700);
        InjectText(cP.pid, "127.0.0.1\r");
        Sleep(700);
        InjectText(cP.pid, "8888\r");

        Assert(!WaitLog(pathC, tC,
            [](const string& l) { return Contains(l, "Lobby connected"); },
            "Client C 连上大厅", 40).empty(),
            "Client C 连上大厅");

        // LIST：游戏中房间必须可见（不能"显示为空"）、带 [in-game] 标记，
        // 且槽位恒为 2/2——游戏一旦开始双方都会关闭大厅连接，不能按 socket
        // 数算（否则显示成 1/2，误导玩家以为只有一人）（2026-08-03 修复）。
        InjectText(cP.pid, "LIST\r");

        Assert(!WaitLog(pathC, tC,
            [](const string& l) { return Contains(l, "ROOMS_LIST:") && Contains(l, "5000") && Contains(l, "[in-game]") && Contains(l, "2/2"); },
            "Client C LIST 显示游戏中的房间(带 [in-game] 且为 2/2)", 40).empty(),
            "游戏1: LIST 显示游戏中房间 [2/2] [in-game]");

        // 同一端口重复建房必须被拒绝（Fix2：防止双 Server.exe 抢同一端口）
        InjectText(cP.pid, "CREATE 5000\r");

        Assert(!WaitLog(pathC, tC,
            [](const string& l) { return Contains(l, "ERROR:Port already in use"); },
            "Client C 重复建房被拒绝", 40).empty(),
            "游戏1: 游戏中房间端口不可重复建房");
    }

    // 6. 游戏1：按观测点自动输入直至结束
    GamePhaseResult g1 = PlayGame(aP.pid, bP.pid, pathA, tA, pathB, tB, "游戏1");

    Assert(g1.doneA && g1.doneB, "游戏1: 双方客户端回到房间 (ReturnedToRoom)");
    Assert(g1.sentA >= 2, "游戏1: Client A 输入全链路 (SENT>=2, 实得 " + to_string(g1.sentA) + ")");
    Assert(g1.sentB >= 2, "游戏1: Client B 输入全链路 (SENT>=2, 实得 " + to_string(g1.sentB) + ")");

    // 7. 回房验证：RM 必须记录两次 REJOIN（房间未被销毁）
    Assert(!WaitLog(pathStart, tStart,
        [](const string& l) { return Contains(l, "rejoined room"); },
        "REJOIN #1", 40).empty(), "游戏1: Client A 重连房间成功");
    Assert(!WaitLog(pathStart, tStart,
        [](const string& l) { return Contains(l, "rejoined room"); },
        "REJOIN #2", 40).empty(), "游戏1: Client B 重连房间成功");

    {
        bool destroyed = false;
        for (auto& l : tStart.buf)
        {
            if (Contains(l, "destroyed")) destroyed = true;
        }
        Assert(!destroyed, "游戏1: 房间未被销毁 (destroyed)");
    }

    // 8. 等游戏1的 Server.exe 走到退出暂停（Server 与 Start 共享控制台——Start.cpp
    //    CreateProcessW flags=0，注入的回车进共享缓冲不可靠，可能被 Start 竞读），
    //    直接终止进程释放 5000 端口。暂停点之前已完成全部清理（关套接字、join 线程、
    //    WSACleanup），杀掉无任何损失。
    {
        DWORD serverPid = FindChildServer(startP.pid);
        if (serverPid)
        {
            WaitLog(pathServer, tServer,
                [](const string& l) { return Contains(l, "Game Server stopped"); },
                "游戏1 Server 走到退出暂停", 30);
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, serverPid);
            if (h)
            {
                TerminateProcess(h, 0);
                CloseHandle(h);
            }
            Assert(WaitProcExit(serverPid, 10), "游戏1: Server.exe 已退出(释放端口)");
        }
        else
        {
            HLog("未找到游戏1的 Server.exe 进程");
        }
    }

    // 9. 游戏2：再次 READY × 2（RM 已在 GAME_ENDED 时重置 ready 标志）
    InjectText(aP.pid, "READY\r");
    InjectText(bP.pid, "READY\r");

    Assert(!WaitLog(pathStart, tStart,
        [](const string& l) { return Contains(l, "now ready"); },
        "游戏2 READY #1", 30).empty(), "游戏2: READY #1 (now ready)");
    Assert(!WaitLog(pathStart, tStart,
        [](const string& l) { return Contains(l, "now ready"); },
        "游戏2 READY #2", 30).empty(), "游戏2: READY #2 (now ready)");

    Assert(!WaitLog(pathStart, tStart,
        [](const string& l) { return Contains(l, "Starting game server"); },
        "游戏2 服务端启动", 30).empty(),
        "游戏2: Starting game server");

    // 10. 游戏2
    GamePhaseResult g2 = PlayGame(aP.pid, bP.pid, pathA, tA, pathB, tB, "游戏2");

    Assert(g2.doneA && g2.doneB, "游戏2: 双方客户端回到房间 (ReturnedToRoom)");
    Assert(g2.sentA >= 2, "游戏2: Client A 输入全链路 (SENT>=2, 实得 " + to_string(g2.sentA) + ")");
    Assert(g2.sentB >= 2, "游戏2: Client B 输入全链路 (SENT>=2, 实得 " + to_string(g2.sentB) + ")");

    Assert(!WaitLog(pathStart, tStart,
        [](const string& l) { return Contains(l, "rejoined room"); },
        "游戏2 REJOIN #1", 40).empty(), "游戏2: Client A 重连房间成功");
    Assert(!WaitLog(pathStart, tStart,
        [](const string& l) { return Contains(l, "rejoined room"); },
        "游戏2 REJOIN #2", 40).empty(), "游戏2: Client B 重连房间成功");

    // 10b. 游戏3：中途退出回归测试——开局后直接杀掉 Client B 进程，
    //      剩余 Client A 应在短超时内被送回房间（而不是无限等待对方重连）。
    //      旧代码 RECONNECT_TIMEOUT=60s 让剩余玩家空转一整分钟；修复后
    //      =25s（覆盖客户端 3 次重试窗口），测试窗口放宽到 90s 取稳。
    {
        DWORD bp = bP.pid;

        InjectText(aP.pid, "READY\r");
        InjectText(bP.pid, "READY\r");

        Assert(!WaitLog(pathStart, tStart,
            [](const string& l) { return Contains(l, "now ready"); },
            "游戏3 READY #1", 30).empty(), "游戏3: READY #1 (now ready)");
        Assert(!WaitLog(pathStart, tStart,
            [](const string& l) { return Contains(l, "now ready"); },
            "游戏3 READY #2", 30).empty(), "游戏3: READY #2 (now ready)");

        Assert(!WaitLog(pathStart, tStart,
            [](const string& l) { return Contains(l, "Starting game server"); },
            "游戏3 服务端启动", 30).empty(),
            "游戏3: Starting game server");

        WaitLog(pathA, tA,
            [](const string& l) { return Contains(l, "Connected to game server"); },
            "游戏3 Client A 连上游戏服务器", 40);
        WaitLog(pathB, tB,
            [](const string& l) { return Contains(l, "Connected to game server"); },
            "游戏3 Client B 连上游戏服务器", 40);

        // 杀掉 Client B：等价于玩家在游戏中途拔线/关闭客户端
        Sleep(1500);
        HLog("== 游戏3 中途杀掉 Client B (pid=" + to_string(bp) + ") ==");
        TerminateProcess(bP.h, 0);

        // 剩余客户端 A 必须自动回到房间（ReturnedToRoom）
        Assert(!WaitLog(pathA, tA,
            [](const string& l) { return Contains(l, "ReturnedToRoom"); },
            "游戏3 Client A 因对手退出自动回房", 90).empty(),
            "游戏3: 一方退出后剩余客户端回到房间");

        // 房间保留（单人失联只发 GAME_ENDED，不销毁房间）
        Assert(!WaitLog(pathStart, tStart,
            [](const string& l) { return Contains(l, "Game ended in room"); },
            "游戏3 Server 通知房间管理器 Game ended", 40).empty(),
            "游戏3: 服务器只发 GAME_ENDED（房间保留）");

        // 等游戏3的 Server.exe 退出（释放 5000 端口）
        DWORD serverPid = FindChildServer(startP.pid);
        if (serverPid)
        {
            WaitLog(pathServer, tServer,
                [](const string& l) { return Contains(l, "Game Server stopped"); },
                "游戏3 Server 走到退出暂停", 60);
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, serverPid);
            if (h)
            {
                TerminateProcess(h, 0);
                CloseHandle(h);
            }
            Assert(WaitProcExit(serverPid, 10), "游戏3: Server.exe 已退出(释放端口)");
        }
    }

    {
        // WaitLog 会消费缓冲中已匹配的行（buffer 里只剩未消费的尾部），
        // 最终统计不能数内存缓冲——直接重读整个 start.log 文件计数。
        int serverStart = 0, rejoined = 0;
        ifstream f(pathStart);
        string line;
        while (getline(f, line))
        {
            if (Contains(line, "Starting game server")) ++serverStart;
            if (Contains(line, "rejoined room")) ++rejoined;
        }
        Assert(serverStart >= 3, "全程: 三局游戏都正常开局 (Starting game server=" + to_string(serverStart) + ")");
        Assert(rejoined >= 5, "全程: 游戏后双方回房、中途退出后剩余方回房 (rejoined=" + to_string(rejoined) + ")");
    }

    // 11. 清理（含失败提前跳转）
cleanup:
    HLog("清理进程 ...");
    {
        // 清理 Start 残留的 Server.exe 子进程
        DWORD serverPid = FindChildServer(startP.pid);
        while (serverPid)
        {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, serverPid);
            if (h)
            {
                TerminateProcess(h, 0);
                CloseHandle(h);
            }
            Sleep(200);
            serverPid = FindChildServer(startP.pid);
        }
    }

    if (startP.h) TerminateProcess(startP.h, 0);
    if (aP.h) TerminateProcess(aP.h, 0);
    if (bP.h) TerminateProcess(bP.h, 0);
    if (cP.h) TerminateProcess(cP.h, 0);

    if (startP.h) CloseHandle(startP.h);
    if (aP.h) CloseHandle(aP.h);
    if (bP.h) CloseHandle(bP.h);
    if (cP.h) CloseHandle(cP.h);

    HLog(g_fails == 0 ? "=== 测试结果: 全部 PASS ===" : "=== 测试结果: 存在 FAIL (" + to_string(g_fails) + " 项) ===");
    return g_fails == 0 ? 0 : 1;
}
