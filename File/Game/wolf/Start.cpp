// Start.cpp - 房间管理器（大厅服务器，监听端口：参数指定或交互输入）
//
// 职责：
//   - 维护房间列表（12 槽，按玩家自选端口建房）；
//   - 房主可：转移房主（TRANSFER）、点名踢人（PICK）、拉黑/取消拉黑
//     （BAN/UNBAN，按名字或 IP）、设置职业档位/村民开关/阵营比例；
//   - 开局由房主指令驱动：START 手动开局，或 AUTO 开启"全员准备自动开局"；
//   - 全员准备且比例不符时进入自动配置确认流程（CONFIRM）；
//   - 处理 GAME_ENDED（游戏结束，房间恢复等待状态）、RELEASE（全部玩家
//     失联时销毁房间）与 REJOIN（游戏结束后玩家自动回房再开一局）。
//
// 第三轮（需求 §11/§12）：
//   - 每连接记录语言（LANG|zh|en），发给客户端的所有用户可见文本按
//     接收者语言输出（SendToClientL10n/SendToAllL10n）；拉起 Server.exe
//     时命令行尾部追加每玩家语言码（顺序与玩家名列表一一对应）；
//   - 心跳保活：收到任意字节刷新 lastSeen，超过 3 秒无字节判定失联、
//     走既有断线清理（§19.3 从 10s 收紧）；PING 行忽略（不算聊天/命令）；
//   - 握手改为带超时的 select + 半包拼接，修掉"只连接不发数据"的
//     客户端永久占用线程（参考 reference/demon 已验证模式）；
//   - 房间内 LIST 可用（服务端 LIST 本无状态检查）；命令解析走
//     FindCommand（英文全名/短别名/中文别名等效）。
//
// 参考实现：reference/demon/Start.cpp（恶魔轮盘），复用其线程模型、握手、
// 发送超时、CreateProcessW 传递 CJK 名字、房间状态机等已验证设计。
#include "common.h"
#include "npc_bot.h"

#include <signal.h>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

void Log(const string& msg)
{
    string s = LogMsg("start.log", msg);
    cout << s << endl;
}

// ============ 数据结构 ============

// Room 定义在下方（含槽位、黑名单等），进/出与窗口管理函数需要前向声明
struct Room;

struct Slot
{
    SOCKET sock;
    bool ready;
    string name;
    string ip;
    int gamePid;           // 本局游戏玩家编号（压缩名单序号 1..N，开局分配；未开局为 0）
    Lang lang;             // 该玩家的语言（LANG| 上报；缺省中文）
    bool isNpc;            // NPC 槽位：无 socket，Server 端 bot 决策（§19.7）
    bool npcOnline;        // NPC 在线模式（true=AI 大模型；false=离线逻辑）
    bool isLocalUser;      // 本地用户槽位：由真实 Client 窗口占用、控制者操纵（§19.6）
    int ownerSlot;         // 本地用户的控制者槽号（isLocalUser 时有效）

    // 注：原 inGame 字段（判断目标是否在游戏中）为死代码——进游戏后该玩家
    // 的大厅连接已断开，sock 为 INVALID，ResolveSlotOrName 根本找不到该目标，
    // 相关检查（TRANSFER/PICK）已删除（需求 §13.2）。
    Slot() : sock(INVALID_SOCKET), ready(false), gamePid(0), lang(Lang::Zh),
             isNpc(false), npcOnline(false), isLocalUser(false), ownerSlot(-1) {}
};

// 槽位是否被真实玩家/NPC 占用：NPC 无 socket，但占槽、占名单、占人数，
// 凡"该槽有人"的判断都必须用 SlotOccupied 而非裸 sock 判空（§19.7）
bool SlotOccupied(const Slot& s)
{
    return s.sock != INVALID_SOCKET || s.isNpc;
}

// 本地用户记录（ADD USER，§19.6）：随房间生命周期管理；游戏结束/房间销毁
// 时按 pid 关闭对应 Client 窗口进程（TerminateProcess 兜底——Client 自动
// 模式收 __GAME_OVER__ 会自行退出，此时 OpenProcess 失败忽略即可）
struct LocalUserRec
{
    string name;
    int ownerSlot;
    DWORD pid;
};

// 关闭该房间全部本地用户窗口进程并清空记录（调用者须持有 g_roomsMutex）。
// 游戏结束/房间销毁/兜底回滚共用；进程已自行退出时杀失败无害。
// 定义在 Room 结构之后（函数体访问 Room 成员）
void CloseLocalUserWindows(Room* room);

// ============ 通配符匹配（BAN/UNBAN，§19.1） ============

// 全角通配符（＊ U+FF0A = EF BC 8A、？ U+FF1F = EF BC 9F）规范化为半角：
// 需求半角全角等效，匹配前统一成半角，匹配逻辑只认半角
string NormalizeWildcards(const string& s)
{
    string out;

    for (size_t i = 0; i < s.size(); ++i)
    {
        unsigned char c = (unsigned char)s[i];

        if (c == 0xEF && i + 2 < s.size() &&
            (unsigned char)s[i + 1] == 0xBC &&
            ((unsigned char)s[i + 2] == 0x8A || (unsigned char)s[i + 2] == 0x9F))
        {
            out += ((unsigned char)s[i + 2] == 0x8A) ? '*' : '?';
            i += 2;
        }
        else
        {
            out += (char)c;
        }
    }

    return out;
}

// 是否含通配符（半角或全角），决定该 BAN 项按模式处理（不做名字净化）
bool HasWildcard(const string& s)
{
    for (size_t i = 0; i < s.size(); ++i)
    {
        unsigned char c = (unsigned char)s[i];

        if (c == '*' || c == '?') return true;

        if (c == 0xEF && i + 2 < s.size() &&
            (unsigned char)s[i + 1] == 0xBC &&
            ((unsigned char)s[i + 2] == 0x8A || (unsigned char)s[i + 2] == 0x9F))
        {
            return true;
        }
    }

    return false;
}

// glob 模式匹配：* = 任意位数（含 0），? = 恰好 1 位，大小写不敏感。
// 动态规划迭代版（O(p*t)），无递归爆栈风险；模式已由调用方规范化半角
bool GlobMatch(const string& pattern, const string& text)
{
    vector<bool> prev(text.size() + 1, false);
    vector<bool> cur(text.size() + 1, false);
    prev[0] = true;

    for (char pc : pattern)
    {
        cur.assign(text.size() + 1, false);

        if (pc == '*') cur[0] = prev[0];

        for (size_t i = 0; i < text.size(); ++i)
        {
            unsigned char tc = (unsigned char)text[i];

            if (pc == '*')
            {
                cur[i + 1] = prev[i + 1] || cur[i];
            }
            else if (pc == '?')
            {
                cur[i + 1] = prev[i];
            }
            else
            {
                unsigned char pp = (unsigned char)pc;
                if (pp >= 'A' && pp <= 'Z') pp += 32;
                if (tc >= 'A' && tc <= 'Z') tc += 32;
                cur[i + 1] = (pp == tc) && prev[i];
            }
        }

        prev = cur;
    }

    return prev[text.size()];
}

// 模式串是否为"点分数字+通配"的 IP 形似（如 10.129.*、10.*.5.1）：
// 段=纯数字段或单通配段、2-4 段、总长受限——命中则归 IP 黑名单匹配玩家 IP，
// 否则归名字黑名单匹配玩家名字（§19.1）
bool LooksLikeIpPattern(const string& s)
{
    if (s.empty() || s.size() > 19) return false;

    int dots = 0;
    string seg;

    for (size_t i = 0; i < s.size(); ++i)
    {
        char c = s[i];

        if (c == '.')
        {
            if (seg.empty() || seg.size() > 3) return false;
            seg.clear();
            ++dots;
        }
        else if (c >= '0' && c <= '9')
        {
            seg += c;
            if (seg.size() > 3) return false;
        }
        else if (c == '*')
        {
            if (!seg.empty()) return false;
            seg = "*";
        }
        else
        {
            return false;
        }
    }

    if (seg.empty() || seg.size() > 3) return false;

    return dots >= 1 && dots <= 3;
}

// 模式项注入清理：模式不做 NAME 白名单净化，但 |/引号/换行/控制字符仍要
// 剔除（防伪造协议行），并限长 ≤63。返回空串=该项非法
string CleanBanPattern(const string& s)
{
    string out;

    for (char c : s)
    {
        if (c == '"' || c == '|' || c == '\n' || c == '\r' || (unsigned char)c < 32)
        {
            continue;
        }
        out += c;
    }

    if (out.size() > 63) out.resize(63);

    return out;
}

// ============ 房间玩家进出记录（LG 查询用，需求 §14.4）： ============
struct PlayerLog
{
    string name;
    string ip;
    bool in;
};

struct Room
{
    string roomId;
    string port;
    vector<Slot> slots;       // 固定 MAX_PLAYERS 大小，槽 0 = 房主
    int playerCount;
    bool gameStarted;
    bool gameEnded;
    int gamePlayerCount;      // 开局时的人数（回房时判定"全员回房"，§13.3）
    int hostPid;              // 房主在游戏中的 pid（开局时槽 0 必为原房主，恒 1；回房保护用）
    DWORD serverPid;          // 本房 Server.exe 进程 id（0=未开局/已回收；房间销毁时杀孤儿防端口泄漏，§16.3）
    ULONGLONG gameWaitStart;  // 全员进游戏后启动等待兜底的计时起点（0 = 未计时）
    int level;                // 职业档位 0/1/2
    bool villager;            // 村民开关
    int ratioW, ratioN, ratioG;
    string banName;           // 被 PICK 踢出者的名字
    time_t banUntil;
    bool needConfirm;         // 等待房主确认自动配置
    int confirmW, confirmN, confirmG;
    bool autoStart;                 // AUTO 开关：全员准备后自动开局
    vector<string> bannedNames;     // 拉黑名单（按名字；房间销毁时一并清除）
    vector<string> bannedIps;       // 拉黑名单（按 IP；房间销毁时一并清除）
    vector<PlayerLog> logs;         // 进出记录（LG 查询；随房间销毁）
    vector<LocalUserRec> localUsers; // 本地用户（ADD USER，§19.6；随房间销毁）

    Room()
        : playerCount(0), gameStarted(false), gameEnded(false),
          gamePlayerCount(0), hostPid(0), serverPid(0), gameWaitStart(0),
          level(0), villager(false),
          ratioW(0), ratioN(0), ratioG(0),
          banUntil(0), needConfirm(false),
          confirmW(0), confirmN(0), confirmG(0),
          autoStart(false)
    {
        slots.resize(MAX_PLAYERS);
    }
};

// 关闭该房间全部本地用户窗口进程并清空记录（调用者须持有 g_roomsMutex）。
// 游戏结束/房间销毁/兜底回滚共用；进程已自行退出时杀失败无害
void CloseLocalUserWindows(Room* room)
{
    if (!room) return;

    for (const LocalUserRec& lu : room->localUsers)
    {
        if (lu.pid != 0 && lu.pid != GetCurrentProcessId())
        {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, lu.pid);

            if (h)
            {
                TerminateProcess(h, 1);
                CloseHandle(h);
            }
        }
    }

    room->localUsers.clear();
}

map<string, shared_ptr<Room>> g_rooms;
mutex g_roomsMutex;
SOCKET g_listenSock = INVALID_SOCKET;
bool g_running = true;
int g_listenPort = 0;      // Start 实际监听端口（main 确定后赋值；Server 回连用，§17 非 8888 启动）

// 开局等待兜底时长（秒）：拉起 Server.exe 且全员进游戏后，若持续该时长
// 未收到 GAME_ENDED/RELEASE 通知，判定 Server 启动失败并回滚 gameStarted。
// 环境变量 WOLF_GAME_WAIT_SECONDS 可注入 1-600（非法/缺省回默认 120），
// 测试脚本用其缩短等待来验证回滚路径。
int g_gameWaitSeconds = 120;

void LoadGameWaitSeconds()
{
    const char* env = getenv("WOLF_GAME_WAIT_SECONDS");

    if (env && *env)
    {
        int v = atoi(env);

        if (v >= 1 && v <= 600)
        {
            g_gameWaitSeconds = v;
        }
    }
}

struct ClientInfo
{
    SOCKET sock;
    string roomId;
    int slot;
    bool inRoom;
    string name;
    string ip;
    bool isAdmin;
    Lang lang;             // 该连接的语言（握手后 LANG| 上报；缺省中文）
};

map<SOCKET, ClientInfo> g_clients;
mutex g_clientsMutex;

// ============ 基础工具 ============

string GetClientIp(SOCKET sock)
{
    sockaddr_in addr;
    int len = sizeof(addr);

    if (getpeername(sock, (sockaddr*)&addr, &len) == 0)
    {
        return inet_ntoa(addr.sin_addr);
    }

    return "unknown";
}

// UTF-8 字符串的显示宽度：半角字符 1 列、CJK 汉字 2 列（等宽字体下
// 汉字占 2 个单元格，列对齐必须按显示宽度计，不能数字节数）。
int DisplayWidth(const string& s)
{
    int w = 0;
    size_t i = 0;

    while (i < s.size())
    {
        unsigned char c = (unsigned char)s[i];
        int len = 1;

        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;

        // 防御：尾部残缺序列不越界，按 1 列计
        if (i + len > s.size()) len = (int)(s.size() - i);

        if (c >= 0xE0 && c < 0xF0 && len == 3 &&
            (unsigned char)s[i + 1] >= 0x80 && (unsigned char)s[i + 1] <= 0xBF &&
            (unsigned char)s[i + 2] >= 0x80 && (unsigned char)s[i + 2] <= 0xBF)
        {
            unsigned int cp = ((c & 0x0F) << 12) |
                              (((unsigned char)s[i + 1] & 0x3F) << 6) |
                              ((unsigned char)s[i + 2] & 0x3F);

            w += (cp >= 0x4E00 && cp <= 0x9FFF) ? 2 : 1;
        }
        else
        {
            w += 1;
        }

        i += len;
    }

    return w;
}

// 左对齐补齐到目标显示宽度（不足补空格），供 LG 列表列对齐。
string PadToWidth(const string& s, int width)
{
    string out = s;
    int pad = width - DisplayWidth(s);

    while (pad-- > 0) out += ' ';

    return out;
}

// 强制结束该房间的 Server.exe（孤儿进程回收，§16.3）。Server.exe 由
// StartGameServer 拉起后 Start 不持有句柄，游戏端口由其监听；若房间销毁
// （空房/RELEASE）或开局兜底回滚时该进程仍未退出，端口会一直占用，
// 同端口重建房后开局 bind 失败。进程已自行退出时 OpenProcess/
// TerminateProcess 失败，忽略即可。不能杀自己（防御 pid 异常）。
void KillGameServer(Room* room)
{
    if (!room || room->serverPid == 0) return;

    if (room->serverPid != GetCurrentProcessId())
    {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, room->serverPid);

        if (h)
        {
            TerminateProcess(h, 1);
            CloseHandle(h);
            Log("KILL game server pid=" + to_string(room->serverPid));
        }
    }

    room->serverPid = 0;
}

// 该房间的 Server.exe 进程是否仍存活。对局正常进行时 Server 进程必然存在
// （玩家连的是游戏端口，不是大厅），按"全员进游戏 + 超时"强杀会把进行中
// 的长局误杀（2026-08-07 实测根因：对局 120s+ 被兜底误杀，全员断线重连失败）；
// 只有进程已退出（启动即死/崩溃）才允许 CheckGameWaitTimeouts 回滚。
// 进程退出后 PID 可能被系统复用，误判窗口极短且回滚无害，不做二次确认。
bool GameServerProcessAlive(Room* room)
{
    if (!room || room->serverPid == 0) return false;

    // 防御：PID 异常指向自己（理论不可能，CreateProcessW 返回的是子进程 id）
    if (room->serverPid == GetCurrentProcessId()) return false;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, room->serverPid);

    // 打开失败即视为进程不存在（已退出）
    if (!h) return false;

    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);

    return ok && code == STILL_ACTIVE;
}

void SendToClient(SOCKET sock, const string& msg)
{
    if (sock == INVALID_SOCKET) return;

    if (msg.find("批量拉黑") != string::npos || msg.find("被移出") != string::npos)
    {
        Log("SENDCHECK sock=" + to_string(sock) + " msg=[" + msg + "]");
    }

    string out = msg + "\n";
    int total = 0;

    while (total < (int)out.length())
    {
        int sent = send(sock, out.c_str() + total, (int)out.length() - total, 0);

        if (sent <= 0)
        {
            Log("SEND FAIL sock=" + to_string(sock) + " err=" + to_string(WSAGetLastError()) + " msg=[" + msg + "]");
            closesocket(sock);
            return;
        }

        total += sent;
    }
}

// 持锁前先快照目标 socket 再锁外发送（send 可能阻塞，不能持锁）
void SendToRoomMembers(Room* room, const string& msg, SOCKET exclude = INVALID_SOCKET)
{
    if (!room) return;

    vector<SOCKET> targets;

    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        if (room->slots[i].sock != INVALID_SOCKET && room->slots[i].sock != exclude)
        {
            targets.push_back(room->slots[i].sock);
        }
    }

    for (SOCKET s : targets)
    {
        SendToClient(s, msg);
    }
}

// ============ 双语输出（需求 §12.3：按接收者语言渲染） ============

// 单播双语提示：按该连接的语言渲染后发送。prefix 为语言中性的协议前缀
// （"ERROR|"/"ROOM_MSG|" 等，不翻译）；zh/en 为双语正文（printf 风格，
// %s 传 .c_str()，两套格式串占位符必须一致）。语言取自 g_clients，持锁
// 读取（锁序 rooms→clients，与断线收尾一致，不会死锁）。
void SendToClientL10n(SOCKET sock, const char* prefix, const char* zh, const char* en, ...)
{
    if (sock == INVALID_SOCKET) return;

    Lang lang = Lang::Zh;

    {
        lock_guard<mutex> lock(g_clientsMutex);
        auto it = g_clients.find(sock);

        if (it != g_clients.end())
        {
            lang = it->second.lang;
        }
    }

    const char* fmt = (lang == Lang::En) ? en : zh;
    char buf[4096];
    va_list args;

    va_start(args, en);
    vsnprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);

    SendToClient(sock, string(prefix) + buf);
}

// 广播双语提示：逐个 socket 按其语言渲染（同一消息不同语言也能正确分送）。
// exclude 为要跳过的收件人（TRANSFER 广播给除新房主外的所有人），无则传
// INVALID_SOCKET。语言取自已入座的槽位（与 ClientInfo 在 LANG| 时同步）。
void SendToAllL10n(Room* room, SOCKET exclude, const char* prefix, const char* zh, const char* en, ...)
{
    if (!room) return;

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

    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        if (room->slots[i].sock != INVALID_SOCKET && room->slots[i].sock != exclude)
        {
            SendToClient(room->slots[i].sock,
                string(prefix) + (room->slots[i].lang == Lang::En ? enBuf : zhBuf));
        }
    }
}

void UpdateClientAdmin(SOCKET sock, bool admin)
{
    auto it = g_clients.find(sock);

    if (it != g_clients.end())
    {
        it->second.isAdmin = admin;
    }
}

// ============ 房间内消息 ============

// 房间内广播（房内连接都可能收不到，发送失败时由 HandleClient 收尾清理）
void RoomMsg(Room* room, const string& text, SOCKET exclude = INVALID_SOCKET)
{
    SendToRoomMembers(room, "ROOM_MSG|" + text, exclude);
}

// ============ 重名检查 ============

// 全服务器唯一：遍历所有房间占用槽 + 当前连接。NPC 槽也占名字（§19.7）
bool NameTaken(const string& name, SOCKET self)
{
    for (auto& kv : g_rooms)
    {
        for (int i = 0; i < MAX_PLAYERS; ++i)
        {
            if (SlotOccupied(kv.second->slots[i]) &&
                NameEquals(kv.second->slots[i].name, name))
            {
                return true;
            }
        }
    }

    for (auto& kv : g_clients)
    {
        if (kv.first != self && NameEquals(kv.second.name, name))
        {
            return true;
        }
    }

    return false;
}

// 拉黑名单内查找（大小写不敏感），BAN 去重与 UNBAN 移除共用，避免
// "grace" 已拉黑后 "Grace" 又重复入单或移除失败。
bool ContainsName(const vector<string>& v, const string& n)
{
    for (const string& s : v)
    {
        if (NameEquals(s, n)) return true;
    }

    return false;
}

// ============ 开局 ============

// 锁外调用；锁定状态由调用者保证（g_roomsMutex 已持）
void StartGameServer(Room* room)
{
    // 先回收上一局可能残留的 Server.exe（兜底回滚后未退出的孤儿进程会
    // 继续占用游戏端口，导致本局 bind 失败，§16.3）
    KillGameServer(room);

    // 组装命令行：变长名字 + 配置参数
    string cmd = "Server.exe " + room->port;

    vector<string> names;

    // 压缩名单：跳过未连接槽位按序编号，gamePid 即该玩家在 Server 名单中的
    // 位置 1..N（与槽号无关，槽位有空洞时两者错位，§背景）。槽 0 必在首位
    // 所以恒得 1，hostPid=1 的语义不因压缩而改变。NPC 槽（§19.7）无 socket
    // 但占名单：同样分配 gamePid、进名字列表（Server 名单即 PLAYER_LIST）
    int seq = 0;

    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        if (SlotOccupied(room->slots[i]))
        {
            ++seq;
            room->slots[i].gamePid = seq;
            names.push_back(room->slots[i].name);
            cmd += " \"" + room->slots[i].name + "\"";
        }
    }

    // startIp startPort：Server.exe 回连 Start 用；startPort 必须用实际监听
    // 端口（非 8888 启动时用硬编码值会让 GAME_ENDED/RELEASE 通知发丢）
    cmd += " 127.0.0.1 " + to_string(g_listenPort) + " " + room->roomId + " " +
           to_string(room->ratioW) + " " + to_string(room->ratioN) + " " +
           to_string(room->ratioG) + " " + to_string(room->level) + " " +
           (room->villager ? "1" : "0");

    // 尾部追加每玩家语言码（槽位顺序与上面名字列表一一对应，需求 §12.1）；
    // Server.exe 从 argv 尾部解析 N 个语言码到 g_playerLang[1..N]。
    // NPC 槽位用 npc（在线）/npc-off（离线）标记（§19.7），参数总数不变
    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        if (SlotOccupied(room->slots[i]))
        {
            cmd += " ";

            if (room->slots[i].isNpc)
            {
                cmd += room->slots[i].npcOnline ? "npc" : "npc-off";
            }
            else
            {
                cmd += LangCode(room->slots[i].lang);
            }
        }
    }

    // CJK 名字必须用宽字符命令行传递（GBK 控制台 ANSI 转换会毁掉 UTF-8 名）
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
    wstring wideCmd(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, &wideCmd[0], wideLen);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    BOOL ok = CreateProcessW(nullptr, &wideCmd[0], nullptr, nullptr, FALSE,
                             0, nullptr, nullptr, &si, &pi);

    if (!ok)
    {
        // 回滚状态，让玩家可以重试
        room->gameStarted = false;
        room->gameEnded = false;
        room->serverPid = 0;

        for (int i = 0; i < MAX_PLAYERS; ++i)
        {
            if (room->slots[i].sock != INVALID_SOCKET)
            {
                room->slots[i].ready = false;
            }
        }

        SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|", "开局失败，请重试", "Failed to start the game, retry");
        return;
    }

    // 记录 Server.exe 进程 id：房间销毁/兜底回滚时杀孤儿释放游戏端口（§16.3）
    room->serverPid = pi.dwProcessId;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // 先置位再通知，否则客户端关闭大厅连接时房间会被误删
    room->gameStarted = true;
    room->gameEnded = false;

    // 记录开局人数与房主 pid（槽 0 必为原房主，游戏 pid 恒为 1）：
    // 回房时据此判定"全员回房"（gameEnded 解锁）与房主保护（§13.2/13.3）
    room->gamePlayerCount = room->playerCount;
    room->hostPid = 1;
    room->gameWaitStart = 0;

    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        if (room->slots[i].sock != INVALID_SOCKET)
        {
            // pid 用开局分配的 gamePid（压缩名单序号），不是槽号+1：
            // 槽位有空洞时（如 5 人房槽 2 掉线后 4 人开局）槽号与名单错位，
            // 而 Server 的 playerId = 名单位置，客户端 REJOIN 要拿同一编号
            int pid = room->slots[i].gamePid;
            SendToClient(room->slots[i].sock,
                "GAME_PREPARE|" + room->port + "|" + room->roomId + "|127.0.0.1|" + to_string(pid));
        }
    }
}

// 拉起一个 Client.exe 自动模式窗口（ADD USER / 本地用户兜底共用）：
// 参数 = <ip> <startPort> <username> <roomPort>（§19.8），新控制台窗口。
// 成功返回 true 并回填进程 id；失败返回 false（不弹错误，调用方处理）
bool SpawnClientWindow(const string& username, const string& roomPort, DWORD& outPid)
{
    string cmd = "127.0.0.1 " + to_string(g_listenPort) + " \"" + username + "\" " + roomPort;
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
    wstring wideArgs(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, &wideArgs[0], wideLen);

    // Client.exe 与 Start.exe 同目录：用本进程全路径推导，防工作目录漂移
    wchar_t selfPath[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, selfPath, MAX_PATH);

    while (n > 0 && selfPath[n - 1] != L'\\') --n;

    wstring exePath(selfPath, n);
    exePath += L"Client.exe";

    wstring fullCmd = L"\"" + exePath + L"\" " + wideArgs;

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    BOOL ok = CreateProcessW(exePath.c_str(), &fullCmd[0], nullptr, nullptr, FALSE,
                             CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);

    if (!ok) return false;

    outPid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// 开局前给每个本地用户保证有 Client 自动模式窗口（ADD USER 时已启动，
// 这里只是兜底：若窗口进程已死则补启动，保证 Server 名单里的本地用户
// 真的有窗口连进来。GameStarted 前调用，调用者持有 g_roomsMutex）
void EnsureLocalUserWindows(Room* room)
{
    if (!room) return;

    for (size_t i = 0; i < room->localUsers.size(); ++i)
    {
        LocalUserRec& lu = room->localUsers[i];

        // 进程还活着就跳过；入房/改名状态由 Client 自行完成
        if (lu.pid != 0)
        {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, lu.pid);

            if (h)
            {
                DWORD code = 0;
                BOOL ok = GetExitCodeProcess(h, &code);
                CloseHandle(h);

                if (ok && code == STILL_ACTIVE) continue;
            }
        }

        // 进程已死：先清理可能残留的旧进程（pid 复用竞态下防双窗口），再拉起
        if (lu.pid != 0 && lu.pid != GetCurrentProcessId())
        {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, lu.pid);

            if (h)
            {
                TerminateProcess(h, 1);
                CloseHandle(h);
            }
        }

        DWORD pid = 0;

        if (SpawnClientWindow(lu.name, room->port, pid))
        {
            lu.pid = pid;
            Log("LOCALUSER respawn " + lu.name + " pid=" + to_string(pid));
        }
        else
        {
            Log("LOCALUSER spawn failed for " + lu.name);
        }
    }
}

// 全部玩家就绪 → 检查比例 → 需要时请求房主确认自动配置 → 开局
void TryStart(Room* room)
{
    if (!room || room->gameStarted || room->needConfirm) return;

    if (room->playerCount < 4) return;

    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        // NPC 恒视为已准备（无 READY 指令来源，§19.7）
        if (SlotOccupied(room->slots[i]) && !room->slots[i].isNpc &&
            !room->slots[i].ready)
        {
            return;
        }
    }

    int P = room->playerCount;
    int W = room->ratioW, N = room->ratioN, G = room->ratioG;
    bool ok = false;

    if (room->villager)
    {
        // 村民模式：三阵营之和可小于人数，差额由村民填充
        ok = (W >= 0 && N >= 0 && G >= 0 && W + N + G <= P);
    }
    else
    {
        ok = (W >= 1 && N >= 0 && G >= 1 && W + N + G == P);
    }

    if (ok)
    {
        StartGameServer(room);
        return;
    }

    // 自动建议：狼约 1/3，神约剩余一半，中立补余
    int aw = P / 3; if (aw < 1) aw = 1;
    int ag = (P - aw) / 2; if (ag < 1) ag = 1;
    int an = P - aw - ag;
    if (an < 0) an = 0;

    room->confirmW = aw;
    room->confirmN = an;
    room->confirmG = ag;
    room->needConfirm = true;

    // 只给房主发确认请求
    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        if (room->slots[i].sock != INVALID_SOCKET && i == 0)
        {
            SendToClient(room->slots[i].sock,
                "CONFIG_NEED_CONFIRM|" + to_string(aw) + "|" + to_string(an) + "|" + to_string(ag));
        }
    }

    SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|",
        "玩家已全部准备，但比例与人数不符，请房主确认自动配置",
        "All ready, but the ratio does not match; host must confirm the auto config");
}

// ============ 玩家进出 ============

// ============ 进出记录（需求 §14.4 LG） ============

// 进出记录 upsert（建房/加入/回房与离开/被踢共用）：按 SanitizeName
// 规范化后的名字做大小写不敏感比对，已存在则更新 ip/in，不存在则追加。
// 离开只置 in=false、记录保留（历史可查）。
void UpsertPlayerLog(Room* room, const string& name, const string& ip, bool in)
{
    if (!room) return;

    string norm = SanitizeName(name);

    if (norm.empty()) return;

    for (PlayerLog& pl : room->logs)
    {
        if (NameEquals(pl.name, norm))
        {
            pl.ip = ip;
            pl.in = in;
            return;
        }
    }

    PlayerLog pl;
    pl.name = norm;
    pl.ip = ip;
    pl.in = in;
    room->logs.push_back(pl);
}

// 组装 LG 输出文本（多行，\n 分隔）。名字列宽 = max(12, 最长名字显示宽)，
// IP 列宽 = max(16, 最长 IP 宽+1)；半角 1 列、全角 2 列按显示宽度补齐。
string BuildLogText(Room* room)
{
    string out = "Log List";

    if (room->logs.empty())
    {
        out += "\n（暂无进出记录）";
        return out;
    }

    int maxNameW = 12;
    int maxIpW = 16;

    for (const PlayerLog& pl : room->logs)
    {
        maxNameW = max(maxNameW, DisplayWidth(pl.name));
        maxIpW = max(maxIpW, DisplayWidth(pl.ip) + 1);
    }

    for (const PlayerLog& pl : room->logs)
    {
        out += "\n" + PadToWidth(pl.name, maxNameW) + PadToWidth(pl.ip, maxIpW) +
               (pl.in ? "[in]" : "[out]");
    }

    return out;
}

// 从房间移除玩家。需调用者持有 g_roomsMutex 与 g_clientsMutex
void RemovePlayerFromRoom(SOCKET sock)
{
    auto cit = g_clients.find(sock);
    if (cit == g_clients.end()) return;

    ClientInfo& ci = cit->second;
    if (ci.roomId.empty()) return;

    auto rit = g_rooms.find(ci.roomId);
    if (rit == g_rooms.end()) return;

    Room* room = rit->second.get();
    int slot = ci.slot;

    // 是否离开者是房主（决定要不要顶替新房主）
    bool wasAdmin = ci.isAdmin;

    // 游戏进行中/已结束等待回房：大厅连接断开是预期行为（进游戏即关闭大厅
    // 连接），不是真离开房间 → 只清 sock/ready，保留名字/IP/语言供 REJOIN、
    // 不减人数、不顶替房主、不销毁房间（§13.2）
    bool inGamePhase = room->gameStarted || room->gameEnded;

    if (slot >= 0 && slot < MAX_PLAYERS)
    {
        bool wasReady = room->slots[slot].ready;

        // 幂等：PICK 踢人时槽位已清空，这里不再重复占位清理
        bool occupied = room->slots[slot].sock != INVALID_SOCKET;

        // 非游戏阶段要记进出记录，而名字/IP 随即被清空，先快照
        string leavingName = room->slots[slot].name;
        string leavingIp = room->slots[slot].ip;

        room->slots[slot].sock = INVALID_SOCKET;
        room->slots[slot].ready = false;

        // 非游戏阶段清空名字/IP；inGamePhase 保留（回房 REJOIN 要用）。
        // gamePid 只对本局有效，同名字/IP 一起清（防止旧局编号串到新局）
        if (!inGamePhase)
        {
            room->slots[slot].name.clear();
            room->slots[slot].ip.clear();
            room->slots[slot].gamePid = 0;
            room->slots[slot].isNpc = false;
            room->slots[slot].npcOnline = false;
            room->slots[slot].isLocalUser = false;
            room->slots[slot].ownerSlot = -1;

            // 本地用户窗口断开（被控制者关闭/进程退出）：同步删记录，
            // 防 SHOW ADD 残留已不存在的本地用户
            for (size_t li = 0; li < room->localUsers.size();)
            {
                if (NameEquals(room->localUsers[li].name, leavingName))
                {
                    room->localUsers.erase(room->localUsers.begin() + li);
                }
                else
                {
                    ++li;
                }
            }

            // 真正离开（游戏外断开/主动退出）：进出记录置 out，记录保留；
            // 游戏期断开属预期行为（进游戏），不改 in（§14.4）
            if (!leavingName.empty())
            {
                UpsertPlayerLog(room, leavingName, leavingIp, false);
            }
        }

        // 槽位空闲说明已被 PICK 分支清理过，不能再扣人数；游戏阶段也不减
        if (occupied && !inGamePhase)
        {
            room->playerCount = max(0, room->playerCount - 1);
        }
    }

    ci.roomId.clear();
    ci.slot = -1;
    ci.inRoom = false;
    ci.isAdmin = false;

    // 只有房主本人离开且未处于游戏阶段（含已结束等待回房）→ 最小槽顶替为房主
    // 非房主离开（含被 PICK 踢出）绝不触发顶替
    if (wasAdmin && !inGamePhase)
    {
        for (int i = 0; i < MAX_PLAYERS; ++i)
        {
            if (room->slots[i].sock != INVALID_SOCKET)
            {
                UpdateClientAdmin(room->slots[i].sock, true);
                SendToClient(room->slots[i].sock, "ADMIN|你已成为房主");
                SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|",
                    "%s 已成为房主", "%s is now the host", room->slots[i].name.c_str());
                break;
            }
        }
    }

    // 房间空了且未开局、未结束 → 销毁（inGamePhase 期间绝不销毁）。
    // 销毁前回收可能残留的 Server.exe（开局失败/兜底回滚后未退出的孤儿
    // 进程会占用游戏端口，§16.3）
    if (room->playerCount == 0 && !inGamePhase)
    {
        KillGameServer(room);
        g_rooms.erase(rit);
    }
}

// 把指定槽位的玩家直接移出房间（PICK 踢人与 BAN 拉黑共用）。
// 被移出者收 KICKED 提示并立即从 g_clients 清条目（防其重连时重名误判），
// 再用 shutdown 让原 HandleClient 线程收尾关闭（不能在这 closesocket：
// 句柄可能被测试脚本立即重连的新连接复用，导致误杀新连接）。
// 调用者需持有 g_roomsMutex；内部按锁序再取 g_clientsMutex（与
// HandleClient 收尾一致：rooms 在上、clients 在下，不会死锁）。
// reasonZh/reasonEn 为双语理由：KICKED| 正文客户端会原样显示，故按
// 被踢者自己的语言渲染。
void EjectPlayerFromRoom(Room* room, int slot, const char* reasonZh, const char* reasonEn)
{
    if (!room || slot < 0 || slot >= MAX_PLAYERS) return;
    if (room->slots[slot].sock == INVALID_SOCKET) return;

    Log("EJECT " + room->slots[slot].name + " tick=" + to_string(GetTickCount64()));

    SOCKET kickedSock = room->slots[slot].sock;
    string kickedName = room->slots[slot].name;
    string kickedIp = room->slots[slot].ip;
    Lang kickedLang = room->slots[slot].lang;

    room->slots[slot].sock = INVALID_SOCKET;
    room->slots[slot].ready = false;
    room->slots[slot].name.clear();
    room->slots[slot].ip.clear();
    room->slots[slot].isNpc = false;
    room->slots[slot].npcOnline = false;
    room->slots[slot].isLocalUser = false;
    room->slots[slot].ownerSlot = -1;
    room->playerCount = max(0, room->playerCount - 1);

    // 被 PICK/BAN 踢出：进出记录置 out（记录保留）
    if (!kickedName.empty())
    {
        UpsertPlayerLog(room, kickedName, kickedIp, false);
    }

    SendToClient(kickedSock, string("KICKED|") + Txt(kickedLang, reasonZh, reasonEn));
    Sleep(200);

    // 被踢者是本地用户（PICK/BAN 路径）→ 同步关窗口并删记录（§19.6）；
    // PICK 分支已删过记录，这里按名字再查一遍是幂等兜底
    for (size_t li = 0; li < room->localUsers.size();)
    {
        if (NameEquals(room->localUsers[li].name, kickedName))
        {
            DWORD pid = room->localUsers[li].pid;

            if (pid != 0 && pid != GetCurrentProcessId())
            {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);

                if (h)
                {
                    TerminateProcess(h, 1);
                    CloseHandle(h);
                }
            }

            room->localUsers.erase(room->localUsers.begin() + li);
        }
        else
        {
            ++li;
        }
    }

    {
        lock_guard<mutex> lock(g_clientsMutex);
        g_clients.erase(kickedSock);
    }

    shutdown(kickedSock, SD_BOTH);

    SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|",
        "%s 被移出房间", "%s was removed from the room", kickedName.c_str());
}

// ============ 解析助手 ============

// 解析 "槽号或名字" 参数为槽下标；失败返回 -1
int ResolveSlotOrName(Room* room, const string& arg, SOCKET caller)
{
    // 尝试纯数字槽号（玩家编号 = 槽号 + 1）
    bool isNum = true;
    for (char c : arg)
    {
        if (!isdigit((unsigned char)c)) { isNum = false; break; }
    }

    if (isNum && !arg.empty())
    {
        int num = atoi(arg.c_str());
        if (num >= 1 && num <= MAX_PLAYERS)
        {
            int idx = num - 1;
            if (room->slots[idx].sock != INVALID_SOCKET && room->slots[idx].sock != caller)
            {
                return idx;
            }
        }
        return -1;
    }

    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        if (room->slots[i].sock != INVALID_SOCKET && room->slots[i].sock != caller &&
            NameEquals(room->slots[i].name, arg))
        {
            return i;
        }
    }

    return -1;
}

// ============ BAN/UNBAN 批量工具（需求 §14.5） ============

// 参数是否以 .ban 结尾（大小写不敏感）→ 视为黑名单文件路径。
bool IsBanFileSuffix(const string& s)
{
    if (s.size() < 4) return false;

    return _stricmp(s.substr(s.size() - 4).c_str(), ".ban") == 0;
}

// 对单个拉黑项（名字或 IP）执行操作，BAN 与 UNBAN 共用。返回结果码：
//   1 = IP 项成功；2 = 名字项成功；0 = 失败（UNBAN 未在名单中）；
//   -1 = 被拒（BAN 拉黑自己）。
// 名字统一按 SanitizeName 规范化 + 大小写不敏感比对（与 NAME 同规则，
// 防止超长/变体绕过黑名单）；在房玩家被拉黑时一并移出（EjectPlayerFromRoom
// 内部会把进出记录置 out）。
int ApplyBanItem(Room* room, const string& item, bool isBan, const string& selfName)
{
    // 通配模式项（§19.1）：不做名字白名单净化（模式串本就可含 *?），只做
    // 注入清理+限长；点分数字形似模式归 IP 名单（匹配玩家 IP），否则归
    // 名字名单（匹配规范化后的名字）。半角/全角通配符先统一成半角
    if (HasWildcard(item))
    {
        string pat = NormalizeWildcards(item);
        pat = CleanBanPattern(pat);

        if (pat.empty()) return 0;

        bool ipLike = LooksLikeIpPattern(pat);
        vector<string>& list = ipLike ? room->bannedIps : room->bannedNames;

        if (isBan)
        {
            // 去重：IP 模式字面量精确比；名字模式大小写不敏感比
            bool dup = false;

            if (ipLike)
            {
                dup = find(list.begin(), list.end(), pat) != list.end();
            }
            else
            {
                dup = ContainsName(list, pat);
            }

            if (!dup) list.push_back(pat);

            // 房内已有匹配者立即踢出（房主槽 0 除外，与 IP 拉黑行为一致）
            for (int i = 1; i < MAX_PLAYERS; ++i)
            {
                if (!SlotOccupied(room->slots[i])) continue;

                const string& target = ipLike ? room->slots[i].ip : room->slots[i].name;

                if (GlobMatch(pat, target))
                {
                    EjectPlayerFromRoom(room, i,
                        ipLike ? "你的 IP 已被房主拉黑" : "你已被房主拉黑并移出房间",
                        ipLike ? "Your IP was banned by the host" : "You were banned and removed by the host");
                }
            }

            return ipLike ? 1 : 2;
        }
        else
        {
            // UNBAN 模式：按模式串本身精确删除（名字大小写不敏感）
            if (ipLike)
            {
                auto it = find(list.begin(), list.end(), pat);

                if (it == list.end()) return 0;

                list.erase(it);
            }
            else
            {
                for (size_t i = 0; i < list.size(); ++i)
                {
                    if (NameEquals(list[i], pat))
                    {
                        list.erase(list.begin() + i);
                        return 2;
                    }
                }

                return 0;
            }

            return 1;
        }
    }

    if (IsIpAddress(item))
    {
        if (isBan)
        {
            // 去重入单后把房内同 IP 玩家（房主除外）一并移出
            if (find(room->bannedIps.begin(), room->bannedIps.end(), item) == room->bannedIps.end())
            {
                room->bannedIps.push_back(item);
            }

            for (int i = 1; i < MAX_PLAYERS; ++i)
            {
                if (room->slots[i].sock != INVALID_SOCKET && room->slots[i].ip == item)
                {
                    EjectPlayerFromRoom(room, i, "你的 IP 已被房主拉黑", "Your IP was banned by the host");
                }
            }

            return 1;
        }
        else
        {
            auto it = find(room->bannedIps.begin(), room->bannedIps.end(), item);

            if (it == room->bannedIps.end()) return 0;

            room->bannedIps.erase(it);
            return 1;
        }
    }

    string targetName = SanitizeName(item);

    if (targetName.empty()) return 0;

    if (isBan)
    {
        // 拉黑自己逐项拒绝，不中断同命令其他项
        if (NameEquals(targetName, selfName)) return -1;

        // 在房则踢出（拉黑其槽位名，防大小写变体绕过）；不在房按规范化名入单
        int target = ResolveSlotOrName(room, item, INVALID_SOCKET);

        if (target >= 0)
        {
            string bannedName = room->slots[target].name;

            if (!ContainsName(room->bannedNames, bannedName))
            {
                room->bannedNames.push_back(bannedName);
            }

            EjectPlayerFromRoom(room, target, "你已被房主拉黑并移出房间", "You were banned and removed by the host");
        }
        else
        {
            if (!ContainsName(room->bannedNames, targetName))
            {
                room->bannedNames.push_back(targetName);
            }
        }

        return 2;
    }
    else
    {
        for (size_t i = 0; i < room->bannedNames.size(); ++i)
        {
            if (NameEquals(room->bannedNames[i], targetName))
            {
                room->bannedNames.erase(room->bannedNames.begin() + i);
                return 2;
            }
        }

        return 0;
    }
}

// 读取 .ban 文件并按 isBan 语义逐行处理（行首尾空白裁剪、空行跳过、
// UTF-8 BOM 跳过；每行一个名字或 IP，规则同参数项；相对路径基于
// Start.exe 工作目录）。返回成功条目数；文件打不开返回 -1（调用方按
// "该项失败"报错，不影响同命令其他项）。
int ProcessBanFile(Room* room, const string& path, bool isBan, const string& selfName)
{
    ifstream f(path);

    if (!f.is_open()) return -1;

    string line;
    int ok = 0;
    bool firstLine = true;

    while (getline(f, line))
    {
        // 首行可能带 UTF-8 BOM（记事本保存），剥离后按普通行处理
        if (firstLine && line.size() >= 3 &&
            (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF)
        {
            line = line.substr(3);
        }

        firstLine = false;

        // CRLF 的 \r 在此剥离（TrimWhitespace 只去空格/Tab）
        if (!line.empty() && line.back() == '\r') line.pop_back();

        line = TrimWhitespace(line);
        if (line.empty()) continue;

        int r = ApplyBanItem(room, line, isBan, selfName);

        if (r > 0) ++ok;
    }

    return ok;
}

// ============ 命令处理 ============

void HandleCommand(SOCKET sock, const string& line)
{
    lock_guard<mutex> lockRooms(g_roomsMutex);

    auto cit = g_clients.find(sock);
    if (cit == g_clients.end()) return;

    ClientInfo& ci = cit->second;

    // 心跳保活行：协议保留字，不算聊天/命令，只刷新 lastSeen（recv 层已刷）
    if (IsPingLine(line)) return;

    // 统一解析：命令字与参数。协议标准分隔符是 '|'（NAME|abc、BAN|P1 P2），
    // 空格分隔是兼容写法（RATIO 2 0 2）。两者同时出现时按更靠前的一个切分：
    // "BAN|P1 P2" 必须切在 '|'，否则首项 P1 会被当成命令字一部分丢掉
    // （§14.5 批量拉黑第一项失效，2026-08-06 实测修复）
    string cmdStr;
    string argStr;
    size_t space = line.find(' ');
    size_t bar = line.find('|');

    if (bar != string::npos && (space == string::npos || bar < space))
    {
        cmdStr = line.substr(0, bar);
        argStr = line.substr(bar + 1);
    }
    else if (space != string::npos)
    {
        cmdStr = line.substr(0, space);
        argStr = line.substr(space + 1);
    }
    else
    {
        cmdStr = line;
    }

    // 参数统一裁剪首尾空白：尾随空格会让 NAME|Grace␣ 解析出"Grace "变体，
    // 既绕过拉黑名单也产生难查的重复名，故所有命令的参数在此规整。
    argStr = TrimWhitespace(argStr);

    // 命令字大写化（中文命令不受影响，用于匹配英文）
    string upper;
    for (char c : cmdStr)
    {
        upper += (char)toupper((unsigned char)c);
    }

    Room* room = nullptr;
    if (!ci.roomId.empty())
    {
        auto rit = g_rooms.find(ci.roomId);
        if (rit != g_rooms.end())
        {
            room = rit->second.get();
        }
    }

    // 说明：argStr 是 '|' 或首个空格之后的所有内容，未二次截断
    // （否则 RATIO|2|0|2 会被误切成 "0|2"）

    // LANG|<code>：记录语言（大小写不敏感，缺省中文），并同步到所在槽位
    if (upper == "LANG")
    {
        Lang l = ParseLang(argStr);
        ci.lang = l;

        if (room && ci.slot >= 0 && ci.slot < MAX_PLAYERS)
        {
            room->slots[ci.slot].lang = l;
        }

        return;
    }

    // 内部协议：Server.exe 通知与客户端回房（不在玩家命令表中）
    if (upper == "GAME_ENDED")
    {
        // 兼容 "GAME_ENDED|<roomId>" 与 "GAME_ENDED <roomId>"
        string rid = argStr;
        if (rid.empty() && line.find('|') != string::npos)
        {
            rid = line.substr(line.find('|') + 1);
        }

        auto rit = g_rooms.find(rid);
        if (rit == g_rooms.end()) return;

        Room* r = rit->second.get();
        r->gameStarted = false;
        r->gameEnded = true;

        // 收到 Server 通知说明它活着，取消启动等待兜底计时
        r->gameWaitStart = 0;

        for (int i = 0; i < MAX_PLAYERS; ++i)
        {
            if (r->slots[i].sock != INVALID_SOCKET)
            {
                r->slots[i].ready = false;
            }
        }

        return;
    }

    if (upper == "RELEASE")
    {
        // 全部玩家失联时由 Server.exe 发送：销毁房间（黑名单随房间一并销毁）
        string rid = argStr;
        if (rid.empty() && line.find('|') != string::npos)
        {
            rid = line.substr(line.find('|') + 1);
        }

        // 房间销毁，等待计时随之作废（无残留状态）。
        // RELEASE 由 Server.exe 在全部玩家失联时发送，正常它发完即自行退出；
        // 若未退出（孤儿），在此强制回收释放游戏端口（§16.3）
        auto rit = g_rooms.find(rid);
        if (rit != g_rooms.end())
        {
            KillGameServer(rit->second.get());
            g_rooms.erase(rit);
        }

        return;
    }

    if (upper == "REJOIN")
    {
        // REJOIN|<roomId>|<playerId>
        vector<string> toks;
        string rest = argStr;

        size_t bar2 = rest.find('|');
        if (bar2 == string::npos)
        {
            SendToClientL10n(sock, "REJOIN_FAIL|", "格式错误", "Bad format");
            return;
        }

        string rid = rest.substr(0, bar2);
        string pidStr = rest.substr(bar2 + 1);

        auto rit = g_rooms.find(rid);
        if (rit == g_rooms.end())
        {
            SendToClientL10n(sock, "REJOIN_FAIL|", "房间不存在", "Room not found");
            return;
        }

        Room* r = rit->second.get();
        if (r->gameStarted)
        {
            SendToClientL10n(sock, "REJOIN_FAIL|", "游戏仍在进行中", "Game still in progress");
            return;
        }

        // 拉黑检查：被拉黑的玩家不允许回房，与 JOIN 一致（含通配模式，§19.1）
        {
            string rejoinIp = GetClientIp(sock);

            for (const string& bn : r->bannedNames)
            {
                if (NameEquals(bn, ci.name) || GlobMatch(bn, ci.name))
                {
                    SendToClientL10n(sock, "REJOIN_FAIL|", "你已被拉黑，无法回到该房间", "You are banned from this room");
                    return;
                }
            }

            for (const string& bip : r->bannedIps)
            {
                if (bip == rejoinIp || GlobMatch(bip, rejoinIp))
                {
                    SendToClientL10n(sock, "REJOIN_FAIL|", "你的 IP 已被拉黑，无法回到该房间", "Your IP is banned from this room");
                    return;
                }
            }
        }

        // REJOIN 的 pid 语义 = 开局分配的 gamePid（压缩名单序号 1..N），
        // 不是槽号+1：槽位有空洞时槽号与名单序号错位，按槽号减 1 会张冠李戴
        int pid = atoi(pidStr.c_str());
        int target = -1;

        // 房主保护（§13.2）：房主（pid == hostPid，开局记 1）只允许回槽 0，
        // 防游戏结束后被非房主顶替；非房主即便槽 0 空着也不能占
        bool isHost = (r->hostPid != 0 && pid == r->hostPid);

        if (isHost)
        {
            if (r->slots[0].sock == INVALID_SOCKET)
            {
                target = 0;
            }
        }
        else
        {
            // 原槽优先：按 gamePid 找该玩家本局原槽（槽位须空，已回房的不再
            // 占）；从槽 1 起扫，槽 0 只属于房主（非房主的 gamePid 恒 ≥2）
            for (int i = 1; i < MAX_PLAYERS; ++i)
            {
                if (r->slots[i].gamePid == pid && r->slots[i].sock == INVALID_SOCKET)
                {
                    target = i;
                    break;
                }
            }

            // 找不到（伪造 pid、gamePid 全 0 的纯新局场景）才回落其他空槽
            // （不含槽 0，房主保护不放松）
            if (target < 0)
            {
                for (int i = 1; i < MAX_PLAYERS; ++i)
                {
                    if (r->slots[i].sock == INVALID_SOCKET) { target = i; break; }
                }
            }
        }

        if (target < 0)
        {
            SendToClientL10n(sock, "REJOIN_FAIL|", "房间已满", "Room is full");
            return;
        }

        r->slots[target].sock = sock;
        r->slots[target].name = ci.name;
        r->slots[target].ip = GetClientIp(sock);
        r->slots[target].lang = ci.lang;
        r->slots[target].isLocalUser = false;
        r->slots[target].ownerSlot = -1;

        // 本地用户回房：按名字恢复控制关系（ownerSlot 记的是上局的槽号，
        // 本局可能已变，但本地用户记录不随局清除、控制者槽号以记录为准）
        for (const LocalUserRec& lu : r->localUsers)
        {
            if (NameEquals(lu.name, ci.name))
            {
                r->slots[target].isLocalUser = true;
                r->slots[target].ownerSlot = lu.ownerSlot;
                break;
            }
        }

        // 进游戏断开已不再减人数（§13.2），回房也不再加；只做防御性同步：
        // 万一历史数据漏减，把 playerCount 抬高到实际槽位占用数（不超员）
        int occupied = 0;

        for (int i = 0; i < MAX_PLAYERS; ++i)
        {
            if (r->slots[i].sock != INVALID_SOCKET) ++occupied;
        }

        if (occupied > r->playerCount)
        {
            r->playerCount = occupied;
        }

        ci.roomId = r->roomId;
        ci.slot = target;
        ci.inRoom = true;
        ci.isAdmin = isHost;

        // 回房成功：进出记录 upsert（in=true）
        UpsertPlayerLog(r, ci.name, r->slots[target].ip, true);

        // 全员回房（槽位占用数 == 开局人数）才解除 gameEnded；
        // 否则保持结束态，外人不能趁半局加入（JOIN 拦截维持，§13.3）
        if (occupied == r->gamePlayerCount)
        {
            r->gameEnded = false;
        }

        SendToClient(sock, "JOINED|" + r->roomId);
        if (ci.isAdmin)
        {
            SendToClient(sock, "ADMIN|你已成为房主");
        }
        SendToAllL10n(r, INVALID_SOCKET, "ROOM_MSG|", "%s 回到房间", "%s returned to the room", ci.name.c_str());
        return;
    }

    // 玩家命令统一按命令表分发：英文全名/英文短别名/中文别名等效（§11.2）
    const CommandEntry* cmd = FindCommand(upper);

    if (!cmd)
    {
        // 兜底：房间内视为聊天（聊天内容原样透传，名字前缀+全角冒号 §10.1）
        if (room)
        {
            string chat = SanitizeChat(line);
            RoomMsg(room, ci.name + "：" + chat, sock);
        }
        else
        {
            SendToClientL10n(sock, "ERROR|", "不支持的命令", "Unknown command");
        }

        return;
    }

    if (strcmp(cmd->en, "NAME") == 0)
    {
        // 特殊符号在净化前直接驳回（需求 §14.6）：旧行为会把 "a b" 净化成
        // 合法名 "ab" 而容忍非法字符，新规则对原始输入做白名单校验
        if (!IsValidNameChars(argStr))
        {
            SendToClientL10n(sock, "ERROR|", "名字只能包含中英文、数字与下划线",
                "Name may only contain letters, digits, CJK chars and underscore");
            return;
        }

        string name = SanitizeName(argStr);

        // 空名回退默认 Player（与客户端同语义；pen_test 5a 断言该行为）。
        // 能走到这里且净化后为空只可能是参数本身为空（白名单已拦截非法字符）
        if (name.empty())
        {
            name = "Player";
        }

        // 单字符/单数字名字拒绝（需求 §16.2）：净化后不足 2 码点无区分度
        if (CountUtf8Chars(name) < 2)
        {
            SendToClientL10n(sock, "ERROR|", "名字至少需要 2 个字符", "Name needs at least 2 characters");
            return;
        }

        // 名字禁止用 IP 格式：拉黑按 IP 判断，避免玩家拿 IP 当名字绕过黑名单。
        // 判定在长度截断前进行，防止 11 位 IP 被截断成非 IP 串而漏检。
        if (LooksLikeIpName(argStr))
        {
            SendToClientL10n(sock, "ERROR|", "名字不能是 IP 格式", "Name cannot be an IP address");
            return;
        }

        if (NameTaken(name, sock))
        {
            SendToClientL10n(sock, "ERROR|", "名字已被占用，请换一个", "Name already taken, try another");
            return;
        }

        // 改名也要过黑名单检查（含通配模式 §19.1）：改名能绕过拉黑的话，
        // 玩家只需换名重连即可躲过 BAN，与 JOIN 检查同一套规则
        if (room && ci.slot >= 0 && ci.slot < MAX_PLAYERS)
        {
            string myIp = room->slots[ci.slot].ip;

            for (const string& bn : room->bannedNames)
            {
                if (NameEquals(bn, name) || GlobMatch(bn, name))
                {
                    SendToClientL10n(sock, "ERROR|", "你已被拉黑，无法使用该名字", "You are banned from using this name");
                    return;
                }
            }

            for (const string& bip : room->bannedIps)
            {
                if (bip == myIp || GlobMatch(bip, myIp))
                {
                    SendToClientL10n(sock, "ERROR|", "你的 IP 已被拉黑，无法改名", "Your IP is banned; rename denied");
                    return;
                }
            }
        }

        ci.name = name;
        SendToClient(sock, "NAME_SET|" + name);

        if (room && ci.slot >= 0 && ci.slot < MAX_PLAYERS)
        {
            // 房内改名：旧名记 out、新名记 in，避免进出记录残留"永久 in"
            string oldName = room->slots[ci.slot].name;

            room->slots[ci.slot].name = name;

            if (!oldName.empty() && !NameEquals(oldName, name))
            {
                UpsertPlayerLog(room, oldName, room->slots[ci.slot].ip, false);
                UpsertPlayerLog(room, name, room->slots[ci.slot].ip, true);
            }
        }
        return;
    }

    if (strcmp(cmd->en, "LIST") == 0)
    {
        string list;
        int count = 0;

        for (auto& kv : g_rooms)
        {
            Room* r = kv.second.get();

            // 空房才跳过；游戏进行中/已结束等待回房的房间人数已定格
            // （进游戏断开不减数），双保险不让它们从列表消失（§13.2）
            if (r->playerCount == 0 && !r->gameStarted && !r->gameEnded) continue;

            string line2 = r->port + "\t" + to_string(r->playerCount) + "/" + to_string(MAX_PLAYERS);
            if (r->gameStarted)
            {
                // 游戏中标记按请求者语言输出（EN 客户端不显示中文方括号）
                line2 += Txt(ci.lang, " [游戏中]", " [in-game]");
            }
            if (!list.empty()) list += "|";
            list += line2;
            ++count;
        }

        if (count == 0)
        {
            SendToClient(sock, "ROOMS_LIST|EMPTY");
        }
        else
        {
            SendToClient(sock, "ROOMS_LIST|" + list);
        }
        return;
    }

    if (strcmp(cmd->en, "CREATE") == 0)
    {
        if (ci.inRoom)
        {
            SendToClientL10n(sock, "ERROR|", "你已在房间中", "You are already in a room");
            return;
        }

        if (!IsValidPort(argStr))
        {
            SendToClientL10n(sock, "ERROR|", "端口必须为 1024-65535 的纯数字", "Port must be digits 1024-65535");
            return;
        }

        for (auto& kv : g_rooms)
        {
            if (kv.second->port == argStr)
            {
                SendToClientL10n(sock, "ERROR|", "端口已被占用", "Port already in use");
                return;
            }
        }

        if (ci.name.empty())
        {
            ci.name = "Player";
        }

        auto room2 = make_shared<Room>();
        room2->port = argStr;
        room2->playerCount = 1;
        room2->slots[0].sock = sock;
        room2->slots[0].name = ci.name;
        room2->slots[0].ip = GetClientIp(sock);
        room2->slots[0].lang = ci.lang;

        // 6 位大写随机房间号
        srand((unsigned)time(nullptr) ^ (unsigned)sock);
        for (int i = 0; i < 6; ++i)
        {
            room2->roomId += (char)('A' + rand() % 26);
        }

        ci.roomId = room2->roomId;
        ci.slot = 0;
        ci.inRoom = true;
        ci.isAdmin = true;

        g_rooms[room2->roomId] = room2;

        // 房主建房即入进出记录（in=true）
        UpsertPlayerLog(room2.get(), ci.name, room2->slots[0].ip, true);

        SendToClient(sock, "CREATED|" + room2->roomId + "|" + room2->port);
        SendToClient(sock, "ADMIN|你成为房主");
        SendToAllL10n(room2.get(), INVALID_SOCKET, "ROOM_MSG|",
            "%s 创建了房间", "%s created the room", ci.name.c_str());
        return;
    }

    if (strcmp(cmd->en, "JOIN") == 0)
    {
        if (ci.inRoom)
        {
            SendToClientL10n(sock, "ERROR|", "你已在房间中", "You are already in a room");
            return;
        }

        if (!IsValidPort(argStr))
        {
            SendToClientL10n(sock, "ERROR|", "端口必须为 1024-65535 的纯数字", "Port must be digits 1024-65535");
            return;
        }

        for (auto& kv : g_rooms)
        {
            Room* r = kv.second.get();
            if (r->port != argStr) continue;

            if (r->gameStarted || r->gameEnded)
            {
                SendToClientL10n(sock, "ERROR|", "该房间正在游戏中", "Game in progress in that room");
                return;
            }

            if (!r->banName.empty() && NameEquals(r->banName, ci.name) && time(nullptr) < r->banUntil)
            {
                SendToClientL10n(sock, "ERROR|", "你被移出房间，10 秒内不能加入", "Kicked from the room; wait 10s to rejoin");
                return;
            }

            // 拉黑名单：名字或 IP 命中即拒绝加入（不以 10 秒为限，直到取消
            // 拉黑）。名字项含通配模式（§19.1）时按 glob 匹配规范化后的名字
            {
                string joinIp = GetClientIp(sock);

                for (const string& bn : r->bannedNames)
                {
                    if (NameEquals(bn, ci.name) || GlobMatch(bn, ci.name))
                    {
                        SendToClientL10n(sock, "ERROR|", "你已被拉黑，无法加入该房间", "You are banned from this room");
                        return;
                    }
                }

                for (const string& bip : r->bannedIps)
                {
                    if (bip == joinIp || GlobMatch(bip, joinIp))
                    {
                        SendToClientL10n(sock, "ERROR|", "你的 IP 已被拉黑，无法加入该房间", "Your IP is banned from this room");
                        return;
                    }
                }
            }

            if (r->playerCount >= MAX_PLAYERS)
            {
                SendToClientL10n(sock, "ERROR|", "房间已满", "Room is full");
                return;
            }

            for (int i = 0; i < MAX_PLAYERS; ++i)
            {
                if (r->slots[i].sock != INVALID_SOCKET && NameEquals(r->slots[i].name, ci.name))
                {
                    SendToClientL10n(sock, "ERROR|", "名字已被占用，请换一个", "Name already taken, try another");
                    return;
                }
            }

            if (ci.name.empty())
            {
                ci.name = "Player";
            }

            int target = -1;
            for (int i = 0; i < MAX_PLAYERS; ++i)
            {
                if (r->slots[i].sock == INVALID_SOCKET) { target = i; break; }
            }

            r->slots[target].sock = sock;
            r->slots[target].name = ci.name;
            r->slots[target].ip = GetClientIp(sock);
            r->slots[target].lang = ci.lang;
            r->slots[target].isNpc = false;
            r->slots[target].npcOnline = false;
            r->slots[target].isLocalUser = false;
            r->slots[target].ownerSlot = -1;

            // 本地用户窗口入房（ADD USER spawn 的 Client 自动模式）：按名字
            // 打上控制关系标记，SHOW ADD 与 PICK 联动据此识别（§19.6）。
            // 注意用目标房间 r（room 是调用者自己的房间，此时必为 nullptr，
            // 解引用即崩——2026-08-08 实测：JOIN 玩家在进房前 room==nullptr，
            // 空指针读 localUsers 直接访问违例带崩整个 Start 进程）
            for (const LocalUserRec& lu : r->localUsers)
            {
                if (NameEquals(lu.name, ci.name))
                {
                    r->slots[target].isLocalUser = true;
                    r->slots[target].ownerSlot = lu.ownerSlot;
                    break;
                }
            }

            r->playerCount++;

            // 加入成功：进出记录 upsert（in=true）
            UpsertPlayerLog(r, ci.name, r->slots[target].ip, true);

            ci.roomId = r->roomId;
            ci.slot = target;
            ci.inRoom = true;

            SendToClient(sock, "JOINED|" + r->roomId);
            SendToAllL10n(r, INVALID_SOCKET, "ROOM_MSG|", "%s 加入房间", "%s joined the room", ci.name.c_str());
            return;
        }

        SendToClientL10n(sock, "ERROR|", "房间不存在", "Room not found");
        return;
    }

    if (strcmp(cmd->en, "READY") == 0)
    {
        if (!room || ci.slot < 0 || ci.slot >= MAX_PLAYERS)
        {
            SendToClientL10n(sock, "ERROR|", "你不在房间中", "You are not in a room");
            return;
        }

        if (room->gameStarted || room->gameEnded)
        {
            SendToClientL10n(sock, "ERROR|", "游戏已在进行中", "Game already in progress");
            return;
        }

        room->slots[ci.slot].ready = !room->slots[ci.slot].ready;

        if (room->slots[ci.slot].ready)
        {
            SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|", "%s 已准备", "%s is ready", ci.name.c_str());
        }
        else
        {
            SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|", "%s 已取消准备", "%s is not ready", ci.name.c_str());
        }

        SendToClient(sock, "READY_STATUS|" + to_string(room->slots[ci.slot].ready ? 1 : 0));

        // 仅 AUTO 开启时才全员准备即走开局流程；否则等房主输入 START
        if (room->autoStart) TryStart(room);
        return;
    }

    if (strcmp(cmd->en, "STATUS") == 0)
    {
        if (!room)
        {
            SendToClientL10n(sock, "ERROR|", "你不在房间中", "You are not in a room");
            return;
        }

        // 竖排等宽表（需求 §16.5）：ID 右对齐 2 宽、NAME 左对齐
        // （列宽 max(12, 最长名字显示宽)，全角算 2 宽）、ST 右对齐 2 宽；
        // 多行经单条 ROOM_STATUS| 下发（\n 分隔），客户端按行打印
        int nameW = 12;
        vector<int> ids;

        for (int i = 0; i < MAX_PLAYERS; ++i)
        {
            if (SlotOccupied(room->slots[i]))
            {
                ids.push_back(i);
                nameW = max(nameW, DisplayWidth(room->slots[i].name));
            }
        }

        string st = "ID | " + PadToWidth("NAME", nameW) + " | ST";

        for (int idx : ids)
        {
            Slot& sl = room->slots[idx];
            string idStr = to_string(idx + 1);
            while ((int)idStr.size() < 2) idStr = " " + idStr;

            // NPC 无 READY 概念，ST 列显示 "-" 而非 0/1（§19.7）
            string readyStr = sl.isNpc ? "-" : to_string(sl.ready ? 1 : 0);
            while ((int)readyStr.size() < 2) readyStr = " " + readyStr;
            st += "\n" + idStr + " | " + PadToWidth(sl.name, nameW) + " | " + readyStr;
        }

        SendToClient(sock, "ROOM_STATUS|" + st);
        return;
    }

    if (strcmp(cmd->en, "IP") == 0)
    {
        // 查询房间内玩家 IP（需求 §14.3）：纯查询不改状态；
        // 槽位含游戏进行中保留的（sock 已断但名字/IP 仍在，进游戏后也可查）
        if (!room || !ci.isAdmin)
        {
            SendToClientL10n(sock, "ERROR|", "只有房主可以执行该操作", "Only the host can do that");
            return;
        }

        if (argStr.empty())
        {
            SendToClientL10n(sock, "ERROR|", "请指定玩家名", "Specify a player name");
            return;
        }

        for (int i = 0; i < MAX_PLAYERS; ++i)
        {
            if (!room->slots[i].name.empty() && NameEquals(room->slots[i].name, argStr))
            {
                SendToClient(sock, "ROOM_MSG|" + room->slots[i].name + " 的 IP：" + room->slots[i].ip);
                return;
            }
        }

        SendToClientL10n(sock, "ERROR|", "未找到玩家 %s", "Player %s not found", argStr.c_str());
        return;
    }

    if (strcmp(cmd->en, "LG") == 0)
    {
        // 房间玩家进出记录（需求 §14.4）：多行文本经单条 ROOM_MSG| 下发，
        // 客户端按行打印；内容为数据，不走 L10n 翻译
        if (!room || !ci.isAdmin)
        {
            SendToClientL10n(sock, "ERROR|", "只有房主可以执行该操作", "Only the host can do that");
            return;
        }

        SendToClient(sock, "ROOM_MSG|" + BuildLogText(room));
        return;
    }

    if (strcmp(cmd->en, "TRANSFER") == 0 || strcmp(cmd->en, "PICK") == 0)
    {
        if (!room || !ci.isAdmin)
        {
            SendToClientL10n(sock, "ERROR|", "只有房主可以执行该操作", "Only the host can do that");
            return;
        }

        if (argStr.empty())
        {
            SendToClientL10n(sock, "ERROR|", "请指定目标玩家（槽号或名字）", "Specify a target (slot or name)");
            return;
        }

        int target = ResolveSlotOrName(room, argStr, sock);

        if (target < 0)
        {
            // 提示带原始参数与重输指引：参数既可能是槽号也可能是名字，
            // 给出原文让房主对照自己输入哪里不对；%s 传参防格式串注入
            SendToClientL10n(sock, "ERROR|",
                "目标玩家不存在：%s（不是房内玩家的编号或名字），请重新输入",
                "Target not found: %s (not a slot number or a name in this room). Try again.",
                argStr.c_str());
            return;
        }

        if (strcmp(cmd->en, "TRANSFER") == 0)
        {
            // NPC 无大厅连接，收不到房主通知，转移房主给 NPC 无意义（§19.7）
            if (room->slots[target].isNpc)
            {
                SendToClientL10n(sock, "ERROR|", "不能把房主转给 NPC", "Cannot transfer host to an NPC");
                return;
            }

            // 原"目标在游戏中"检查已删：进游戏后目标的大厅连接已断开，
            // sock 无效，ResolveSlotOrName 根本找不到该目标（死代码，§13.2）
            UpdateClientAdmin(sock, false);
            UpdateClientAdmin(room->slots[target].sock, true);

            SendToClient(room->slots[target].sock, "ADMIN|你已成为房主");
            SendToAllL10n(room, room->slots[target].sock, "ROOM_MSG|",
                "%s 已转交房主给 %s", "%s transferred the host to %s",
                ci.name.c_str(), room->slots[target].name.c_str());
            SendToClientL10n(room->slots[target].sock, "ROOM_MSG|",
                "%s 已转交房主给你", "%s transferred the host to you", ci.name.c_str());
        }
        else
        {
            // 同上：游戏中的玩家大厅 sock 已断，此处不可达（死代码，§13.2）
            string kickedName = room->slots[target].name;

            if (room->slots[target].isNpc)
            {
                // PICK NPC = 移除 NPC（无 socket 走不了 Eject，直接清标记减人数）
                room->slots[target].isNpc = false;
                room->slots[target].npcOnline = false;
                room->slots[target].name.clear();
                room->slots[target].ip.clear();
                room->slots[target].gamePid = 0;
                room->playerCount = max(0, room->playerCount - 1);

                SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|",
                    "房主移除了 NPC：%s", "Host removed NPC: %s", kickedName.c_str());
                return;
            }

            room->banName = kickedName;
            room->banUntil = time(nullptr) + BAN_SECONDS;

            // 控制者被 PICK → 其本地用户一并移除（§19.6）：杀窗口进程（断线
            // 清理会清槽减数），并从记录删除；本地用户被 PICK 同样处理
            for (size_t li = 0; li < room->localUsers.size();)
            {
                bool match = NameEquals(room->localUsers[li].name, kickedName)
                    || room->localUsers[li].ownerSlot == target;

                if (match)
                {
                    DWORD pid = room->localUsers[li].pid;

                    if (pid != 0 && pid != GetCurrentProcessId())
                    {
                        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);

                        if (h)
                        {
                            TerminateProcess(h, 1);
                            CloseHandle(h);
                        }
                    }

                    room->localUsers.erase(room->localUsers.begin() + li);
                }
                else
                {
                    ++li;
                }
            }

            EjectPlayerFromRoom(room, target, "你已被房主移出房间", "You were kicked by the host");
            SendToClientL10n(sock, "ROOM_MSG|", "已移出 %s", "Removed %s", kickedName.c_str());
        }
        return;
    }

    if (strcmp(cmd->en, "BAN") == 0 || strcmp(cmd->en, "UNBAN") == 0)
    {
        if (!room || !ci.isAdmin)
        {
            SendToClientL10n(sock, "ERROR|", "只有房主可以执行该操作", "Only the host can do that");
            return;
        }

        if (argStr.empty())
        {
            SendToClientL10n(sock, "ERROR|", "请指定目标（名字或 IP）", "Specify a target (name or IP)");
            return;
        }

        bool isBan = (strcmp(cmd->en, "BAN") == 0);

        Log("BANCMD enter " + argStr + " tick=" + to_string(GetTickCount64()) + " sock=" + to_string(sock) + " hostName=" + ci.name);

        // 游戏中禁 BAN：整条命令拒绝（UNBAN 无此约束，维持现状）
        if (isBan && (room->gameStarted || room->gameEnded))
        {
            SendToClientL10n(sock, "ERROR|", "游戏已在进行中，不能拉黑", "Cannot ban during a game");
            return;
        }

        vector<string> items = SplitTokens(argStr);

        // 单参数（且非 .ban 文件）：完全走既有单参数实现，文案与行为
        // 保持不变（既有测试断言依赖「已拉黑」「已取消拉黑」等文案）
        if (items.size() == 1 && !IsBanFileSuffix(items[0]))
        {
            // 通配模式（§19.1）：复用 ApplyBanItem 的模式分支，再补确认文案
            if (HasWildcard(argStr))
            {
                int r = ApplyBanItem(room, argStr, isBan, ci.name);

                if (r < 0)
                {
                    SendToClientL10n(sock, "ERROR|", "不能拉黑自己", "You cannot ban yourself");
                    return;
                }

                if (r == 0)
                {
                    SendToClientL10n(sock, "ERROR|", "该名字/IP 不在拉黑名单中", "Not in the ban list");
                    return;
                }

                if (isBan)
                {
                    SendToClientL10n(sock, "ROOM_MSG|", "已拉黑 %s", "Banned %s", argStr.c_str());
                    SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|", "房主拉黑了 %s", "Host banned %s", argStr.c_str());
                }
                else
                {
                    SendToClientL10n(sock, "ROOM_MSG|", "已取消拉黑：%s", "Unbanned: %s", argStr.c_str());
                    SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|", "房主取消了对 %s 的拉黑", "Host unbanned %s", argStr.c_str());
                }

                return;
            }

            if (isBan)
            {
                if (argStr == ci.name)
                {
                    SendToClientL10n(sock, "ERROR|", "不能拉黑自己", "You cannot ban yourself");
                    return;
                }

                if (IsIpAddress(argStr))
                {
                    // 按 IP 拉黑：去重入黑名单；房内同 IP 玩家（房主除外）一并移出，
                    // 避免房主拉黑自己 IP 把整房清空
                    if (find(room->bannedIps.begin(), room->bannedIps.end(), argStr) == room->bannedIps.end())
                    {
                        room->bannedIps.push_back(argStr);
                    }

                    for (int i = 1; i < MAX_PLAYERS; ++i)
                    {
                        if (room->slots[i].sock != INVALID_SOCKET && room->slots[i].ip == argStr)
                        {
                            EjectPlayerFromRoom(room, i, "你的 IP 已被房主拉黑", "Your IP was banned by the host");
                        }
                    }

                    SendToClientL10n(sock, "ROOM_MSG|", "已拉黑 IP：%s", "Banned IP: %s", argStr.c_str());
                    SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|", "房主拉黑了 IP：%s", "Host banned IP: %s", argStr.c_str());
                    return;
                }

                // 非 IP 参数按名字处理。先按与 NAME 相同的规则规范化（净化+截断），
                // 否则超长名（15 字符被截成 10）或带尾随空格的变体无法命中黑名单。
                string targetName = SanitizeName(argStr);

                if (NameEquals(targetName, ci.name))
                {
                    SendToClientL10n(sock, "ERROR|", "不能拉黑自己", "You cannot ban yourself");
                    return;
                }

                // 先按房间内玩家（槽号或名字）解析，在房则踢出并拉黑；
                // 不在房则按名字直接入黑名单（之后无法加入）
                int target = ResolveSlotOrName(room, argStr, sock);

                if (target >= 0)
                {
                    string bannedName = room->slots[target].name;

                    if (!ContainsName(room->bannedNames, bannedName))
                    {
                        room->bannedNames.push_back(bannedName);
                    }

                    EjectPlayerFromRoom(room, target, "你已被房主拉黑并移出房间", "You were banned and removed by the host");
                    SendToClientL10n(sock, "ROOM_MSG|", "已拉黑 %s", "Banned %s", bannedName.c_str());
                }
                else
                {
                    if (!ContainsName(room->bannedNames, targetName))
                    {
                        room->bannedNames.push_back(targetName);
                    }

                    SendToClientL10n(sock, "ROOM_MSG|", "已拉黑 %s", "Banned %s", targetName.c_str());
                    SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|", "房主拉黑了 %s", "Host banned %s", targetName.c_str());
                }
                return;
            }
            else
            {
                // UNBAN：IP 格式按 IP 从黑名单移除，否则按名字移除
                bool removed = false;

                if (IsIpAddress(argStr))
                {
                    auto it = find(room->bannedIps.begin(), room->bannedIps.end(), argStr);

                    if (it != room->bannedIps.end())
                    {
                        room->bannedIps.erase(it);
                        removed = true;
                    }
                }
                else
                {
                    // 按名字移除：大小写不敏感（与入单规则一致，防止拉黑后变体解不了）
                    for (size_t i = 0; i < room->bannedNames.size(); ++i)
                    {
                        if (NameEquals(room->bannedNames[i], argStr))
                        {
                            room->bannedNames.erase(room->bannedNames.begin() + i);
                            removed = true;
                            break;
                        }
                    }
                }

                if (!removed)
                {
                    SendToClientL10n(sock, "ERROR|", "该名字/IP 不在拉黑名单中", "Not in the ban list");
                    return;
                }

                SendToClientL10n(sock, "ROOM_MSG|", "已取消拉黑：%s", "Unbanned: %s", argStr.c_str());
                SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|", "房主取消了对 %s 的拉黑", "Host unbanned %s", argStr.c_str());
                return;
            }
        }

        // 批量/文件导入：逐项独立处理（一项失败不中断其他项），最后输出汇总行
        int okCount = 0;
        int nameCount = 0;
        int ipCount = 0;
        int rejectCount = 0;

        for (const string& item : items)
        {
            if (IsBanFileSuffix(item))
            {
                int fileOk = ProcessBanFile(room, item, isBan, ci.name);

                if (fileOk < 0)
                {
                    SendToClientL10n(sock, "ERROR|", "无法读取黑名单文件 %s", "Cannot read ban list file %s", item.c_str());
                }
                else
                {
                    okCount += fileOk;
                }

                continue;
            }

            int r = ApplyBanItem(room, item, isBan, ci.name);

            if (r == 1)
            {
                ++okCount;
                ++ipCount;
            }
            else if (r == 2)
            {
                ++okCount;
                ++nameCount;
            }
            else if (r < 0)
            {
                ++rejectCount;
            }
        }

        // 汇总行（参照既有「已拉黑」文案风格；拒绝项（拉黑自己）非零才列出）
        Log("BANCMD loopdone ok=" + to_string(okCount) + " rej=" + to_string(rejectCount) + " tick=" + to_string(GetTickCount64()));
        if (rejectCount > 0)
        {
            SendToClientL10n(sock, "ROOM_MSG|",
                "批量%s完成：成功 %d 项（名字 %d、IP %d），拒绝 %d 项",
                "Batch %s done: %d succeeded (%d names, %d IPs), %d rejected",
                isBan ? "拉黑" : "取消拉黑", okCount, nameCount, ipCount, rejectCount);
        }
        else
        {
            SendToClientL10n(sock, "ROOM_MSG|",
                "批量%s完成：成功 %d 项（名字 %d、IP %d）",
                "Batch %s done: %d succeeded (%d names, %d IPs)",
                isBan ? "拉黑" : "取消拉黑", okCount, nameCount, ipCount);
        }
        return;
    }

    if (strcmp(cmd->en, "LEVEL") == 0 || strcmp(cmd->en, "VILLAGER") == 0 || strcmp(cmd->en, "RATIO") == 0)
    {
        if (!room || !ci.isAdmin)
        {
            SendToClientL10n(sock, "ERROR|", "只有房主可以执行该操作", "Only the host can do that");
            return;
        }

        if (room->gameStarted)
        {
            SendToClientL10n(sock, "ERROR|", "游戏已在进行中，不能修改配置", "Cannot change config during a game");
            return;
        }

        if (strcmp(cmd->en, "LEVEL") == 0)
        {
            bool isNum = true;
            for (char c : argStr)
            {
                if (!isdigit((unsigned char)c)) { isNum = false; break; }
            }

            if (!isNum || argStr.empty())
            {
                SendToClientL10n(sock, "ERROR|", "档位必须为 0、1 或 2", "Level must be 0, 1 or 2");
                return;
            }

            int lv = atoi(argStr.c_str());
            if (lv < 0 || lv > 2)
            {
                SendToClientL10n(sock, "ERROR|", "档位必须为 0、1 或 2", "Level must be 0, 1 or 2");
                return;
            }

            room->level = lv;
            SendToClientL10n(sock, "ROOM_MSG|", "职业档位已设为：档位 %d", "Role level set: level %d", lv);
            SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|",
                "房主把职业档位设为：档位 %d", "Host set role level: %d", lv);
        }
        else if (strcmp(cmd->en, "VILLAGER") == 0)
        {
            if (argStr != "0" && argStr != "1")
            {
                SendToClientL10n(sock, "ERROR|", "村民开关必须为 0 或 1", "Villager switch must be 0 or 1");
                return;
            }

            room->villager = (argStr == "1");

            if (room->villager)
            {
                SendToClientL10n(sock, "ROOM_MSG|", "村民职业已启用", "Villager role enabled");
                SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|", "房主启用了村民职业", "Host enabled the villager role");
            }
            else
            {
                SendToClientL10n(sock, "ROOM_MSG|", "村民职业已禁用", "Villager role disabled");
                SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|", "房主禁用了村民职业", "Host disabled the villager role");
            }
        }
        else
        {
            // RATIO <狼> <中立> <神>；协议按 '|' 分隔，也容忍空格（如 RATIO 2 0 2）
            string norm = argStr;
            for (char& c : norm)
            {
                if (c == '|') c = ' ';
            }
            vector<string> toks = SplitTokens(norm);
            if (toks.size() != 3)
            {
                SendToClientL10n(sock, "ERROR|", "比例需三个数字：狼 中立 神", "Ratio needs 3 numbers: wolf neutral god");
                return;
            }

            bool allNum = true;
            for (const string& t : toks)
            {
                for (char c : t)
                {
                    if (!isdigit((unsigned char)c)) { allNum = false; break; }
                }
                if (!allNum) break;
            }

            if (!allNum)
            {
                SendToClientL10n(sock, "ERROR|", "比例不合法：需三个非负整数", "Invalid ratio: three non-negative integers");
                return;
            }

            int w = atoi(toks[0].c_str());
            int n = atoi(toks[1].c_str());
            int g = atoi(toks[2].c_str());

            if (!room->villager)
            {
                // 村民关闭：总和必须等于当前人数
                if (w + n + g != room->playerCount)
                {
                    SendToClientL10n(sock, "ERROR|", "比例不合法：三数之和须等于当前人数 %d", "Invalid ratio: sum must equal the %d players", room->playerCount);
                    return;
                }
                if (w < 1 || g < 1)
                {
                    SendToClientL10n(sock, "ERROR|", "比例不合法：狼与神至少各 1", "Invalid ratio: at least 1 wolf and 1 god");
                    return;
                }
            }

            room->ratioW = w;
            room->ratioN = n;
            room->ratioG = g;

            SendToClientL10n(sock, "ROOM_MSG|", "比例已设为：狼 %d / 中立 %d / 神 %d",
                "Ratio set: wolf %d / neutral %d / god %d", w, n, g);
            SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|", "房主把比例设为：狼 %d / 中立 %d / 神 %d",
                "Host set ratio: wolf %d / neutral %d / god %d", w, n, g);
        }
        return;
    }

    if (strcmp(cmd->en, "CONFIRM") == 0)
    {
        if (!room || !ci.isAdmin)
        {
            SendToClientL10n(sock, "ERROR|", "只有房主可以执行该操作", "Only the host can do that");
            return;
        }

        if (!room->needConfirm)
        {
            SendToClientL10n(sock, "ERROR|", "当前没有待确认的自动配置", "No auto config to confirm");
            return;
        }

        if (argStr == "1")
        {
            room->ratioW = room->confirmW;
            room->ratioN = room->confirmN;
            room->ratioG = room->confirmG;
            room->needConfirm = false;

            SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|",
                "房主同意了自动配置：狼 %d / 中立 %d / 神 %d",
                "Host accepted the auto config: wolf %d / neutral %d / god %d",
                room->ratioW, room->ratioN, room->ratioG);
            StartGameServer(room);
        }
        else
        {
            room->needConfirm = false;
            SendToClientL10n(sock, "ROOM_MSG|", "已保持当前配置", "Keeping the current config");

            // 需要重新准备
            for (int i = 0; i < MAX_PLAYERS; ++i)
            {
                if (room->slots[i].sock != INVALID_SOCKET)
                {
                    room->slots[i].ready = false;
                }
            }

            SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|",
                "房主拒绝了自动配置，请修改比例后重新准备",
                "Host rejected the auto config; adjust the ratio and ready up");
        }
        return;
    }

    if (strcmp(cmd->en, "START") == 0)
    {
        if (!room || !ci.isAdmin)
        {
            SendToClientL10n(sock, "ERROR|", "只有房主可以执行该操作", "Only the host can do that");
            return;
        }

        if (room->gameStarted)
        {
            SendToClientL10n(sock, "ERROR|", "游戏已在进行中", "Game already in progress");
            return;
        }

        if (room->gameEnded)
        {
            // 上一局刚结束：清掉结束标记，允许同房间再开一局
            room->gameEnded = false;
        }

        // START /F（或 /FORCE，大小写不敏感）：跳过全员准备检查强制开局
        // （§19.2）；比例不匹配时直接采用自动配置，不再询问 CONFIRM
        bool force = false;
        string upArg = argStr;

        for (char& c : upArg) c = (char)toupper((unsigned char)c);

        if (upArg == "/F" || upArg == "/FORCE")
        {
            force = true;
        }
        else if (!argStr.empty())
        {
            SendToClientL10n(sock, "ERROR|",
                "START 参数无效：%s（可用 START /F 强制开局）",
                "Invalid START argument: %s (use START /F to force)",
                argStr.c_str());
            return;
        }

        // 普通开局至少 4 人；强制开局放宽到 2 人（含 NPC/本地用户，§19.2）
        if (room->playerCount < (force ? 2 : 4))
        {
            SendToClientL10n(sock, "ERROR|", "至少 %d 人才能开局", "Need at least %d players", force ? 2 : 4);
            return;
        }

        if (!force)
        {
            for (int i = 0; i < MAX_PLAYERS; ++i)
            {
                if (room->slots[i].sock != INVALID_SOCKET && !room->slots[i].ready)
                {
                    SendToClientL10n(sock, "ERROR|", "还有玩家未准备，不能开局", "Not all players are ready");
                    return;
                }
            }

            // 校验与自动配置统一走 TryStart：比例合法直接开局，不合法进确认流程
            TryStart(room);
            return;
        }

        // 强制开局：本地用户窗口兜底补启动（§19.6），比例非法则直接套用
        // 自动配置（与 TryStart 同一套建议算法），然后开局
        EnsureLocalUserWindows(room);

        int P = room->playerCount;
        int W = room->ratioW, N = room->ratioN, G = room->ratioG;
        bool ok;

        if (room->villager)
        {
            ok = (W >= 0 && N >= 0 && G >= 0 && W + N + G <= P);
        }
        else
        {
            ok = (W >= 1 && N >= 0 && G >= 1 && W + N + G == P);
        }

        if (!ok)
        {
            int aw = P / 3; if (aw < 1) aw = 1;
            int ag = (P - aw) / 2; if (ag < 1) ag = 1;
            int an = P - aw - ag;
            if (an < 0) an = 0;

            room->ratioW = aw;
            room->ratioN = an;
            room->ratioG = ag;
            room->needConfirm = false;

            SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|",
                "房主强制开局，比例自动设为：狼 %d / 中立 %d / 神 %d",
                "Host forced start; ratio auto-set: wolf %d / neutral %d / god %d",
                aw, an, ag);
        }

        SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|",
            "房主强制开局", "Host forced the game to start");
        StartGameServer(room);
        return;
    }

    if (strcmp(cmd->en, "AUTO") == 0)
    {
        if (!room || !ci.isAdmin)
        {
            SendToClientL10n(sock, "ERROR|", "只有房主可以执行该操作", "Only the host can do that");
            return;
        }

        room->autoStart = !room->autoStart;

        if (room->autoStart)
        {
            SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|",
                "自动开局已开启：全员准备后自动开始", "Auto-start on: starts when all ready");
        }
        else
        {
            SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|",
                "自动开局已关闭：需房主输入 START 开始", "Auto-start off: host must type START");
        }

        // 开启瞬间若已全员准备，立即走开局流程，不等下一次 READY
        if (room->autoStart) TryStart(room);
        return;
    }

    if (strcmp(cmd->en, "SHOW") == 0)
    {
        // SHOW/LOOK（等效，§19.4）：查看黑名单/比例/配置/本地用户与 NPC。
        // 无参数、未知子项或无可显示内容 → 打印用法与作用（不报错）
        string sub = argStr;

        for (char& c : sub) c = (char)toupper((unsigned char)c);

        if (!room)
        {
            SendToClientL10n(sock, "ROOM_MSG|",
                "SHOW 用法：SHOW <BAN|RATIO|LEVEL|VILLAGER|AUTO|ADD>——查看黑名单、比例、职业档位、村民开关、自动开局、本地用户与 NPC（LOOK 同效）",
                "SHOW usage: SHOW <BAN|RATIO|LEVEL|VILLAGER|AUTO|ADD> - view ban list, ratio, role level, villager switch, auto-start, local users and NPCs (LOOK works too)");
            return;
        }

        if (sub == "BAN")
        {
            if (!ci.isAdmin)
            {
                SendToClientL10n(sock, "ERROR|", "只有房主可以执行该操作", "Only the host can do that");
                return;
            }

            if (room->bannedNames.empty() && room->bannedIps.empty())
            {
                SendToClientL10n(sock, "ROOM_MSG|",
                    "当前没有拉黑项。SHOW BAN 用法：查看本房拉黑名单（名字/IP 与通配模式）",
                    "No bans currently. SHOW BAN usage: show this room's ban list (names/IPs and wildcard patterns)");
                return;
            }

            string out = "Banned List";

            for (const string& bn : room->bannedNames)
            {
                out += "\n名字：" + bn;
            }

            for (const string& bip : room->bannedIps)
            {
                out += "\nIP：" + bip;
            }

            SendToClient(sock, "ROOM_MSG|" + out);
            return;
        }

        if (sub == "RATIO")
        {
            SendToClient(sock, "ROOM_MSG|狼 " + to_string(room->ratioW) + " / 中立 " +
                to_string(room->ratioN) + " / 神 " + to_string(room->ratioG) +
                Txt(ci.lang,
                    string(room->villager ? "（村民：开）" : "（村民：关）").c_str(),
                    string(room->villager ? " (villager: on)" : " (villager: off)").c_str()));
            return;
        }

        if (sub == "LEVEL")
        {
            SendToClient(sock, string("ROOM_MSG|") + Txt(ci.lang,
                (string("职业档位：档位 ") + to_string(room->level) + "（0 基础 / 1 经典 / 2 豪华）").c_str(),
                (string("Role level: ") + to_string(room->level) + " (0 basic / 1 classic / 2 deluxe)").c_str()));
            return;
        }

        if (sub == "VILLAGER")
        {
            SendToClient(sock, string("ROOM_MSG|") + Txt(ci.lang,
                string(room->villager ? "村民职业：已启用" : "村民职业：已禁用").c_str(),
                string(room->villager ? "Villager role: enabled" : "Villager role: disabled").c_str()));
            return;
        }

        if (sub == "AUTO")
        {
            SendToClient(sock, string("ROOM_MSG|") + Txt(ci.lang,
                string(room->autoStart ? "自动开局：已开启（全员准备后自动开始）" : "自动开局：已关闭（需房主输入 START）").c_str(),
                string(room->autoStart ? "Auto-start: on (starts when all ready)" : "Auto-start: off (host must type START)").c_str()));
            return;
        }

        if (sub == "ADD")
        {
            if (!ci.isAdmin)
            {
                SendToClientL10n(sock, "ERROR|", "只有房主可以执行该操作", "Only the host can do that");
                return;
            }

            string out = "Local Users & NPCs";

            if (room->localUsers.empty())
            {
                bool anyNpc = false;

                for (int i = 0; i < MAX_PLAYERS; ++i)
                {
                    if (room->slots[i].isNpc) { anyNpc = true; break; }
                }

                if (!anyNpc)
                {
                    SendToClientL10n(sock, "ROOM_MSG|",
                        "当前没有本地用户与 NPC。SHOW ADD 用法：查看本地用户（控制者）与 NPC 列表",
                        "No local users or NPCs. SHOW ADD usage: show local users (with owner) and NPC list");
                    return;
                }
            }

            for (const LocalUserRec& lu : room->localUsers)
            {
                string owner = "?";

                for (int i = 0; i < MAX_PLAYERS; ++i)
                {
                    if (i == lu.ownerSlot && SlotOccupied(room->slots[i]))
                    {
                        owner = room->slots[i].name;
                        break;
                    }
                }

                out += "\n本地用户：" + lu.name + "（控制者：" + owner + "）";
            }

            for (int i = 0; i < MAX_PLAYERS; ++i)
            {
                if (room->slots[i].isNpc)
                {
                    out += "\nNPC：" + room->slots[i].name +
                        (room->slots[i].npcOnline ? "（在线）" : "（离线）");
                }
            }

            SendToClient(sock, "ROOM_MSG|" + out);
            return;
        }

        SendToClientL10n(sock, "ROOM_MSG|",
            "SHOW 用法：SHOW <BAN|RATIO|LEVEL|VILLAGER|AUTO|ADD>——查看黑名单、比例、职业档位、村民开关、自动开局、本地用户与 NPC（LOOK 同效）",
            "SHOW usage: SHOW <BAN|RATIO|LEVEL|VILLAGER|AUTO|ADD> - view ban list, ratio, role level, villager switch, auto-start, local users and NPCs (LOOK works too)");
        return;
    }

    if (strcmp(cmd->en, "ADD") == 0)
    {
        // ADD USER <username> [-u] <玩家名或槽位>：添加本地用户（新窗口由
        // 指定玩家控制，无 -u 默认房主）；ADD NPC [名字] on|off：添加 NPC
        if (!room || !ci.isAdmin)
        {
            SendToClientL10n(sock, "ERROR|", "只有房主可以执行该操作", "Only the host can do that");
            return;
        }

        if (room->gameStarted || room->gameEnded)
        {
            SendToClientL10n(sock, "ERROR|", "游戏已在进行中，不能添加", "Cannot add during a game");
            return;
        }

        vector<string> toks = SplitTokens(argStr);

        if (toks.empty())
        {
            SendToClientL10n(sock, "ROOM_MSG|",
                "ADD 用法：ADD USER <用户名> [-u] <玩家名或槽位>（添加本地用户，无 -u 默认给房主）；ADD NPC [NPC名] on|off（on=在线 AI，off=离线逻辑）",
                "ADD usage: ADD USER <name> [-u] <player or slot> (add a local user, default owner is the host); ADD NPC [name] on|off (on=online AI, off=offline logic)");
            return;
        }

        string sub = toks[0];

        for (char& c : sub) c = (char)toupper((unsigned char)c);

        if (sub == "USER")
        {
            if (toks.size() < 2)
            {
                SendToClientL10n(sock, "ERROR|", "用法：ADD USER <用户名> [-u] <玩家名或槽位>", "Usage: ADD USER <name> [-u] <player or slot>");
                return;
            }

            // username 必须过与 NAME 相同的校验（白名单/长度/禁 IP 形似/唯一）
            string rawName = toks[1];

            if (!IsValidNameChars(rawName))
            {
                SendToClientL10n(sock, "ERROR|", "用户名只能包含中英文、数字与下划线",
                    "User name may only contain letters, digits, CJK chars and underscore");
                return;
            }

            string username = SanitizeName(rawName);

            if (CountUtf8Chars(username) < 2)
            {
                SendToClientL10n(sock, "ERROR|", "用户名至少需要 2 个字符", "User name needs at least 2 characters");
                return;
            }

            if (LooksLikeIpName(rawName))
            {
                SendToClientL10n(sock, "ERROR|", "用户名不能是 IP 格式", "User name cannot be an IP address");
                return;
            }

            if (NameTaken(username, sock))
            {
                SendToClientL10n(sock, "ERROR|", "用户名已被占用，请换一个", "User name already taken, try another");
                return;
            }

            // 解析 -u 与目标玩家（槽号或名字）；无 -u 默认房主（槽 0）
            string targetArg;
            bool hasU = false;

            for (size_t i = 2; i < toks.size(); ++i)
            {
                if (_stricmp(toks[i].c_str(), "-u") == 0)
                {
                    hasU = true;
                }
                else if (targetArg.empty())
                {
                    targetArg = toks[i];
                }
            }

            int ownerSlot = 0;

            if (hasU)
            {
                if (targetArg.empty())
                {
                    SendToClientL10n(sock, "ERROR|", "用法：ADD USER <用户名> [-u] <玩家名或槽位>", "Usage: ADD USER <name> [-u] <player or slot>");
                    return;
                }

                ownerSlot = ResolveSlotOrName(room, targetArg, INVALID_SOCKET);

                if (ownerSlot < 0)
                {
                    SendToClientL10n(sock, "ERROR|",
                        "目标玩家不存在：%s（不是房内玩家的编号或名字），请重新输入",
                        "Target not found: %s (not a slot number or a name in this room). Try again.",
                        targetArg.c_str());
                    return;
                }

                if (room->slots[ownerSlot].isNpc)
                {
                    SendToClientL10n(sock, "ERROR|", "不能把本地用户分配给 NPC", "Cannot assign a local user to an NPC");
                    return;
                }
            }

            if (room->playerCount >= MAX_PLAYERS)
            {
                SendToClientL10n(sock, "ERROR|", "房间已满", "Room is full");
                return;
            }

            // 拉起 Client 自动模式窗口：连接大厅改名入房，由控制者操纵
            DWORD pid = 0;

            if (!SpawnClientWindow(username, room->port, pid))
            {
                SendToClientL10n(sock, "ERROR|", "无法启动本地用户窗口", "Failed to start the local user window");
                return;
            }

            LocalUserRec lu;
            lu.name = username;
            lu.ownerSlot = ownerSlot;
            lu.pid = pid;
            room->localUsers.push_back(lu);

            string ownerName = room->slots[ownerSlot].name;
            SendToClientL10n(sock, "ROOM_MSG|",
                "已添加本地用户 %s（控制者：%s，新窗口已启动）",
                "Added local user %s (owner: %s, new window started)",
                username.c_str(), ownerName.c_str());
            SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|",
                "房主添加了本地用户：%s", "Host added a local user: %s", username.c_str());
            return;
        }

        if (sub == "NPC")
        {
            if (toks.size() < 2)
            {
                SendToClientL10n(sock, "ERROR|", "用法：ADD NPC [NPC名] on|off", "Usage: ADD NPC [name] on|off");
                return;
            }

            string mode = toks.back();
            string modeUp = mode;

            for (char& c : modeUp) c = (char)toupper((unsigned char)c);

            if (modeUp != "ON" && modeUp != "OFF")
            {
                SendToClientL10n(sock, "ERROR|", "NPC 模式必须是 on（在线）或 off（离线）", "NPC mode must be on (online) or off (offline)");
                return;
            }

            bool online = (modeUp == "ON");

            // 名字可选：未指定时从内置 100 个英文名表按序取未占用名（§19.7）
            string npcName;

            if (toks.size() >= 3)
            {
                if (toks.size() > 3)
                {
                    SendToClientL10n(sock, "ERROR|", "用法：ADD NPC [NPC名] on|off", "Usage: ADD NPC [name] on|off");
                    return;
                }

                string rawName = toks[1];

                if (!IsValidNameChars(rawName))
                {
                    SendToClientL10n(sock, "ERROR|", "NPC 名只能包含中英文、数字与下划线",
                        "NPC name may only contain letters, digits, CJK chars and underscore");
                    return;
                }

                npcName = SanitizeName(rawName);

                if (CountUtf8Chars(npcName) < 2)
                {
                    SendToClientL10n(sock, "ERROR|", "NPC 名至少需要 2 个字符", "NPC name needs at least 2 characters");
                    return;
                }

                if (LooksLikeIpName(rawName))
                {
                    SendToClientL10n(sock, "ERROR|", "NPC 名不能是 IP 格式", "NPC name cannot be an IP address");
                    return;
                }
            }
            else
            {
                vector<string> used;

                for (auto& kv : g_rooms)
                {
                    for (int i = 0; i < MAX_PLAYERS; ++i)
                    {
                        if (SlotOccupied(kv.second->slots[i]) && !kv.second->slots[i].name.empty())
                        {
                            used.push_back(kv.second->slots[i].name);
                        }
                    }
                }

                npcName = NpcNextFreeName(used);

                if (npcName.empty())
                {
                    SendToClientL10n(sock, "ERROR|", "内置 NPC 名字已用完，请指定名字", "Built-in NPC names exhausted; specify a name");
                    return;
                }
            }

            if (NameTaken(npcName, sock))
            {
                SendToClientL10n(sock, "ERROR|", "NPC 名已被占用，请换一个", "NPC name already taken, try another");
                return;
            }

            if (room->playerCount >= MAX_PLAYERS)
            {
                SendToClientL10n(sock, "ERROR|", "房间已满", "Room is full");
                return;
            }

            // 找空槽（无 socket 且非 NPC）
            int slot = -1;

            for (int i = 0; i < MAX_PLAYERS; ++i)
            {
                if (!SlotOccupied(room->slots[i])) { slot = i; break; }
            }

            if (slot < 0)
            {
                SendToClientL10n(sock, "ERROR|", "房间已满", "Room is full");
                return;
            }

            room->slots[slot].isNpc = true;
            room->slots[slot].npcOnline = online;
            room->slots[slot].name = npcName;
            room->slots[slot].ip = "npc";
            room->slots[slot].lang = Lang::Zh;
            room->playerCount++;

            SendToClientL10n(sock, "ROOM_MSG|",
                "已添加%s NPC：%s", "Added %s NPC: %s",
                online ? "在线" : "离线", npcName.c_str());
            SendToAllL10n(room, INVALID_SOCKET, "ROOM_MSG|",
                "房主添加了%s NPC：%s", "Host added an %s NPC: %s",
                online ? "在线" : "离线", npcName.c_str());
            return;
        }

        SendToClientL10n(sock, "ERROR|",
            "ADD 用法：ADD USER <用户名> [-u] <玩家名或槽位> | ADD NPC [NPC名] on|off",
            "ADD usage: ADD USER <name> [-u] <player or slot> | ADD NPC [name] on|off");
        return;
    }

    if (strcmp(cmd->en, "EXIT") == 0)
    {
        if (room)
        {
            RemovePlayerFromRoom(sock);
            SendToClientL10n(sock, "LEFT_ROOM|", "你已离开房间", "You left the room");
        }
        return;
    }
}

// ============ 连接处理 ============

void HandleClientInner(SOCKET sock)
{
    // 握手阶段：带超时的 select + 半包拼接（30 秒无数据即断开），
    // 修掉"只连接不发数据"的客户端永久占用线程（参考 reference/demon）。
    // 收到任意字节都刷新 lastSeen，握手阶段同样计入心跳。
    string helloData;
    char helloBuf[1024];
    ULONGLONG lastSeen = GetTickCount64();

    while (helloData.find('\n') == string::npos)
    {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval tv = { 30, 0 };

        if (select(0, &readSet, NULL, NULL, &tv) <= 0)
        {
            closesocket(sock);
            return;
        }

        int hr = recv(sock, helloBuf, sizeof(helloBuf) - 1, 0);

        if (hr <= 0)
        {
            closesocket(sock);
            return;
        }

        lastSeen = GetTickCount64();
        helloBuf[hr] = '\0';
        helloData += helloBuf;

        // 防御：握手行不可能这么长，超限直接断开
        if (helloData.size() > 4096)
        {
            closesocket(sock);
            return;
        }
    }

    size_t nl = helloData.find('\n');
    string handshake = helloData.substr(0, nl);
    string buffer = helloData.substr(nl + 1);

    if (!handshake.empty() && handshake.back() == '\r')
    {
        handshake.pop_back();
    }

    // 客户端用 HELLO|3（协议版本号）；Server.exe 通知沿用 HELLO|START
    if (handshake != "HELLO|3" && handshake != "HELLO|START")
    {
        closesocket(sock);
        return;
    }

    ClientInfo ci;
    ci.sock = sock;
    ci.slot = -1;
    ci.inRoom = false;
    ci.isAdmin = false;
    ci.name = "Player";
    ci.lang = Lang::Zh;

    {
        lock_guard<mutex> lock(g_clientsMutex);
        ci.ip = GetClientIp(sock);
        g_clients[sock] = ci;
    }

    int sndtimeo = 5000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndtimeo, sizeof(sndtimeo));

    // WELCOME 正文留空：客户端按自身语言显示本地欢迎语
    SendToClient(sock, "WELCOME|");

    while (true)
    {
        // 1 秒轮询读就绪：既保持数据驱动低延迟，又让心跳超时判定
        // 不依赖阻塞 recv（半开连接收不到 FIN/RST 也能被识别，§11.3）
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval tv = { 1, 0 };

        int sel = select(0, &readSet, NULL, NULL, &tv);

        if (sel < 0)
        {
            break;
        }

        if (sel == 0)
        {
            // 超过 HEARTBEAT_DEADLINE_SECONDS 未收到任何字节 → 判定失联，
            // 走下方统一断线清理（RemovePlayerFromRoom：房主顶替、空房销毁）
            if (GetTickCount64() - lastSeen > (ULONGLONG)HEARTBEAT_DEADLINE_SECONDS * 1000)
            {
                Log("心跳超时判定失联 sock=" + to_string(sock));
                break;
            }

            continue;
        }

        if (!ReceiveLines(sock, buffer, [sock](const string& line)
        {
            HandleCommand(sock, line);
        }))
        {
            break;
        }

        // ReceiveLines 返回 true 说明本轮确实收到了字节（否则返回 false 断开）
        lastSeen = GetTickCount64();
    }

    // 断线清理（心跳失联与正常断线共用同一收尾）
    {
        lock_guard<mutex> lockRooms(g_roomsMutex);
        lock_guard<mutex> lockClients(g_clientsMutex);

        auto it = g_clients.find(sock);
        if (it != g_clients.end())
        {
            RemovePlayerFromRoom(sock);
        }

        g_clients.erase(sock);
    }

    closesocket(sock);
}

// HandleClient 线程入口：把主体包进 try-catch 防 std::terminate。
// 子线程未捕获异常会让整个进程崩溃（测试曾复现 Start 静默死亡），
// 任何输入处理异常都不应带崩房间管理器，记日志后按断线收尾
void HandleClient(SOCKET sock)
{
    try
    {
        HandleClientInner(sock);
    }
    catch (const std::exception& e)
    {
        Log(string("HandleClient exception: ") + e.what());
    }
    catch (...)
    {
        Log("HandleClient unknown exception");
    }

    closesocket(sock);
}

// ============ 启动等待兜底（§13.2） ============

// 主循环定时调用（调用者须持有 g_roomsMutex）。对 gameStarted 且所有槽位
// sock 均无效（全员已进游戏）的房间计时：持续 GAME_WAIT_SECONDS 未收到
// GAME_ENDED/RELEASE → 判定 Server.exe 启动即死，回滚 gameStarted 防房间
// 永久卡 [游戏中]。任何槽位还有有效 sock（有人留在大厅）即重置计时。
void CheckGameWaitTimeouts()
{
    ULONGLONG now = GetTickCount64();
    vector<string> toRollback;

    for (auto& kv : g_rooms)
    {
        Room* r = kv.second.get();
        if (!r->gameStarted) continue;

        bool allInGame = true;

        for (int i = 0; i < MAX_PLAYERS; ++i)
        {
            if (r->slots[i].sock != INVALID_SOCKET) { allInGame = false; break; }
        }

        // 还没全员进游戏，不满足兜底前提，重置计时等下一次全空
        if (!allInGame)
        {
            r->gameWaitStart = 0;
            continue;
        }

        if (r->gameWaitStart == 0)
        {
            r->gameWaitStart = now;
        }
        else if (now - r->gameWaitStart >= (ULONGLONG)g_gameWaitSeconds * 1000)
        {
            // 兜底只对"Server.exe 进程已死"生效（启动即死/崩溃）：进程仍存活
            // 说明游戏正常进行中（全员进游戏后大厅连接断开是常态，长局可能远超
            // GAME_WAIT_SECONDS），此时强杀会误伤对局致全员断线（2026-08-07 实测）。
            // 进程活着就重置计时继续观察；善后交给 Server 自己的超时/RELEASE 逻辑
            if (GameServerProcessAlive(r))
            {
                r->gameWaitStart = now;
                continue;
            }

            toRollback.push_back(kv.first);
        }
    }

    // 回滚：房间保留、ready 清空，已回房玩家可重新准备开局
    for (const string& rid : toRollback)
    {
        auto rit = g_rooms.find(rid);
        if (rit == g_rooms.end()) continue;

        Room* r = rit->second.get();

        // 回收 Server.exe：启动超时回滚说明它大概率卡死/启动即死，若放任
        // 不管会一直占用游戏端口（§16.3）
        KillGameServer(r);

        r->gameStarted = false;
        r->gameEnded = false;
        r->gameWaitStart = 0;

        for (int i = 0; i < MAX_PLAYERS; ++i)
        {
            if (r->slots[i].sock != INVALID_SOCKET)
            {
                r->slots[i].ready = false;
            }
        }

        Log("游戏服务器启动超时回滚 room=" + rid);
        SendToAllL10n(r, INVALID_SOCKET, "ROOM_MSG|",
            "游戏服务器启动失败，房间已保留，可重新开局",
            "Game server failed to start; room kept, start again");
    }
}

// ============ 空房回收（§16.3） ============

// 主循环定时调用（调用者须持有 g_roomsMutex）。PICK/BAN 踢出最后一名
// 玩家（playerCount 归零）时 EjectPlayerFromRoom 不能就地销毁房间——
// 调用者（BAN 分支）随后仍在使用 room 指针，就地 erase 会悬垂。由这里
// 集中回收：非游戏期的空房 → 先杀残留 Server.exe（防游戏端口占用）再销毁。
// RemovePlayerFromRoom 断线路径的空房销毁保留（双保险）。
void CheckEmptyRoomCleanup()
{
    vector<string> toRemove;

    for (auto& kv : g_rooms)
    {
        Room* r = kv.second.get();

        if (r->playerCount == 0 && !r->gameStarted && !r->gameEnded)
        {
            toRemove.push_back(kv.first);
        }
    }

    for (const string& rid : toRemove)
    {
        auto rit = g_rooms.find(rid);

        if (rit == g_rooms.end()) continue;

        KillGameServer(rit->second.get());
        Log("EMPTY-ROOM cleanup room=" + rid);
        g_rooms.erase(rit);
    }
}

// ============ main ============

int main(int argc, char* argv[])
{
    DisableConsoleQuickEdit();
    SetConsoleUtf8();
    SetConsoleFont();

    LoadGameWaitSeconds();

    // 端口参数：Start.exe [端口]。参数非法直接报错退出（不进入交互，便于
    // 脚本/部署快速暴露配置错误）；无参数时交互输入监听端口。Server 回连
    // 端口始终取实际值（g_listenPort），不再硬编码 8888
    int port = 0;

    if (argc > 1)
    {
        string p = argv[1];

        if (!IsValidPort(p))
        {
            cout << "端口参数非法：" << p << "（必须是 1024-65535 的纯数字）" << endl;
            return 1;
        }

        port = atoi(p.c_str());
    }
    else
    {
        // 无参数：交互输入端口。输入流 EOF 时报错退出（防止 getline 失败
        // 死循环）；首尾空白裁剪不影响"纯数字"校验，空输入也走非法分支
        string line;

        while (true)
        {
            cout << "请输入监听端口（1024-65535）：";
            getline(cin, line);

            if (!cin)
            {
                cout << "输入流已关闭，退出" << endl;
                return 1;
            }

            size_t b = line.find_first_not_of(" \t\r");
            size_t e = line.find_last_not_of(" \t\r");
            string t = (b == string::npos) ? "" : line.substr(b, e - b + 1);

            if (!IsValidPort(t))
            {
                cout << "端口无效：" << t << "（必须是 1024-65535 的纯数字），请重新输入" << endl;
                continue;
            }

            port = atoi(t.c_str());
            break;
        }
    }

    g_listenPort = port;

    signal(SIGINT, [](int) { g_running = false; if (g_listenSock != INVALID_SOCKET) closesocket(g_listenSock); });
    signal(SIGTERM, [](int) { g_running = false; if (g_listenSock != INVALID_SOCKET) closesocket(g_listenSock); });

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        cout << "网络初始化失败" << endl;
        system("pause > nul");
        return 1;
    }

    g_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listenSock == INVALID_SOCKET)
    {
        cout << "创建监听套接字失败" << endl;
        WSACleanup();
        system("pause > nul");
        return 1;
    }

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(g_listenSock, (sockaddr*)&addr, sizeof(addr)) != 0)
    {
        cout << "绑定 " << port << " 端口失败，请确认没有其他房间管理器在运行" << endl;
        closesocket(g_listenSock);
        WSACleanup();
        system("pause > nul");
        return 1;
    }

    if (listen(g_listenSock, 10) != 0)
    {
        cout << "监听失败" << endl;
        closesocket(g_listenSock);
        WSACleanup();
        system("pause > nul");
        return 1;
    }

    Log("房间管理器启动，监听 " + to_string(port));

    while (g_running)
    {
        // 带 1 秒超时的 select：既正常接受新连接，又让主循环定时执行
        // 开局等待兜底检查（防 Server.exe 启动即死卡住房间，§13.2）
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(g_listenSock, &readSet);
        timeval tv = { 1, 0 };

        int sel = select(0, &readSet, NULL, NULL, &tv);

        if (sel < 0)
        {
            // 监听 socket 被信号处理器关闭 → 退出
            if (!g_running) break;
            continue;
        }

        if (sel > 0)
        {
            SOCKET clientSock = accept(g_listenSock, nullptr, nullptr);

            if (clientSock != INVALID_SOCKET)
            {
                thread(HandleClient, clientSock).detach();
            }
        }

        lock_guard<mutex> lock(g_roomsMutex);
        CheckGameWaitTimeouts();
        CheckEmptyRoomCleanup();
    }

    closesocket(g_listenSock);
    WSACleanup();

    // Start 退出时回收全部游戏服务器进程（否则孤儿 Server.exe 继续占用
    // 游戏端口，即使本管理器已退出，§16.3）
    {
        lock_guard<mutex> lock(g_roomsMutex);
        for (auto& kv : g_rooms)
        {
            KillGameServer(kv.second.get());
        }
    }

    Log("房间管理器退出");
    system("pause > nul");
    return 0;
}
