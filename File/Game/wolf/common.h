// common.h - 狼人杀三个可执行文件（Start/Server/Client）共用的基础工具、
// 职业技能表、命令与协议数据表。
//
// 来源：参考参考项目 reference/demon/common.h 的已验证实现（ReceiveLines、
// SanitizeName、SetConsoleUtf8、DisableConsoleQuickEdit 等），狼人杀按需求
// 扩展了多玩家槽位上限、职业表、双语命令表。
//
// 注意：本工程由人工维护，改动前请先阅读对应 .cpp 文件头的设计说明。
#ifndef WOLF_COMMON_H
#define WOLF_COMMON_H

#define _WIN32_WINNT 0x0600

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <functional>
#include <atomic>
#include <ctime>
#include <windows.h>
#include <iostream>
#include <conio.h>
#include <signal.h>

using namespace std;

#pragma comment(lib, "ws2_32.lib")

// ============ 全局常量 ============

// 房间人数上限（需求 §2.1）
const int MAX_PLAYERS = 12;

// 被 PICK 踢出后的禁入时长（秒）
const int BAN_SECONDS = 10;

// 玩家名字最长字符数（按 UTF-8 码点计；限 10 个字符内，
// 同时配合"名字禁止 IP 格式"规则，避免名字与 IP 拉黑判据混淆）
const int MAX_NAME_CHARS = 10;

// 应用层心跳（保活探测）参数：
// 客户端每 HEARTBEAT_INTERVAL_SECONDS 秒向当前连接发一行 "PING"；
// 服务器对已连接 socket 记录 lastSeen，超过 HEARTBEAT_DEADLINE_SECONDS
// 未收到任何字节即判定失联。能检出拔线/进程被杀但 TCP 未关的半开连接，
// 不依赖 FIN/RST（参考 reference/demon 的 FIONREAD 观测心跳思路升级为真保活）。
// 需求 §19.3：失联等待从 10s 收紧到 3s，PING 频率同步加密到 1s（留 2 倍余量）。
// 注意：NPC 玩家无 socket，不参与失联判定，该常量只约束真实 socket 连接。
const int HEARTBEAT_INTERVAL_SECONDS = 1;
const int HEARTBEAT_DEADLINE_SECONDS = 3;

// 是否保活探针行：协议保留 "PING" 作为纯心跳，Start/Server 收到必须忽略
// （不算聊天/命令），只用于刷新 lastSeen。玩家聊天打出 PING 会被吞掉，
// 属协议保留字的既定代价。
inline bool IsPingLine(const string& s)
{
    return s == "PING";
}

// ============ 语言（中文/英文版） ============

// 三端运行时语言。客户端分 cn/en 两个产物（en 版用 WOLF_EN 条件编译），
// 但 Start/Server 是单一二进制，按每个客户端自己的语言输出文本。
enum class Lang
{
    Zh = 0,
    En
};

// 解析协议 LANG|<code> 里的语言码（"zh"/"en"，大小写不敏感，缺省中文）。
inline Lang ParseLang(const string& s)
{
    return (_stricmp(s.c_str(), "en") == 0) ? Lang::En : Lang::Zh;
}

// 语言码的协议串（Start 按此拼进 Server.exe 命令行尾部）。
inline const char* LangCode(Lang l)
{
    return (l == Lang::En) ? "en" : "zh";
}

// 双语文本取串：两端都提供中文与英文，按接收者语言选一个。
inline const char* Txt(Lang l, const char* zh, const char* en)
{
    return (l == Lang::En) ? en : zh;
}

// 双语带参格式化：printf 风格，%s 必须传 .c_str()。
// 中文/英文两套格式串占位符必须一致；va_start 以 enFmt 为基准定位变参。
// printf 风格格式化，注意 %s 不能直接传 std::string。
inline string FmtLang(Lang l, const char* zhFmt, const char* enFmt, ...)
{
    const char* fmt = (l == Lang::En) ? enFmt : zhFmt;
    char buf[4096];
    va_list args;
    va_start(args, enFmt);
    vsnprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    return string(buf);
}

// ============ 阵营 ============
enum Camp
{
    CAMP_WOLF = 0,   // 狼人阵营
    CAMP_NEUTRAL,    // 中立阵营
    CAMP_GOD,        // 神职阵营
    CAMP_VILLAGER,   // 好人平民（村民）
    CAMP_COUNT
};

// 职业定义表（下标即职业 ID；enName 为协议与英文命令用的小写串，zhName 为中文显示/中文命令）
struct JobDef
{
    int        id;
    const char* enName;
    const char* zhName;
    int        camp;      // CAMP_*
    const char* detail;   // 职业详细介绍（中文；开局展示 / HELP <职业> 共用）
    const char* detailEn; // 职业详细介绍（英文）
};

// 职业名与详细描述全表（detail 中英文成对，客户端按自身语言显示）
const JobDef JOBS[] =
{
    { 0, "werewolf",   "狼人",  CAMP_WOLF,   "狼人阵营。每晚与狼群商量后刀杀一名玩家。狼人中至少一名存活即可继续行刀；白狼王参与夜晚击杀。",
                                          "Wolf camp. Each night the pack chooses a player to kill. The pack can keep hunting while any wolf is alive; the White Wolf King joins the night kill." },
    { 1, "whitewolf",  "白狼王", CAMP_WOLF,   "狼人阵营。夜晚与狼人一同击杀；白天可以自爆带走一名玩家（立即进入夜晚）。",
                                          "Wolf camp. Kills with the wolves at night; during the day can self-detonate and take one player down (night falls immediately)." },
    { 2, "seer",       "预言家", CAMP_GOD,    "神职阵营。每晚可验一名玩家的阵营（狼人/中立/好人），结果仅自己可见。",
                                          "God camp. Each night checks one player's camp (wolf/neutral/good); only you see the result." },
    { 3, "witch",      "女巫",   CAMP_GOD,    "神职阵营。拥有一瓶解药与一瓶毒药：解药可救回当夜被狼刀者（首夜可自救），毒药可毒杀一名玩家；两药各限一次，同一夜不可双用。",
                                          "God camp. Has one antidote (saves the night's wolf victim; may save yourself on night one) and one poison (kills one player); each used once, not both the same night." },
    { 4, "hunter",     "猎人",   CAMP_GOD,    "神职阵营。被放逐/被狼刀/被自爆身亡时都可开枪带走一名玩家（全局限一枪）；被毒死时不能开枪。",
                                          "God camp. When exiled/killed by wolves/suicide-bombed you shoot one player (one shot per game); cannot shoot if poisoned." },
    { 5, "guard",      "守卫",   CAMP_GOD,    "神职阵营。每晚可守护一名玩家免遭狼刀，不可连续两晚守护同一人；若被守者同时被女巫解药救无效（守救冲突判死）。",
                                          "God camp. Each night protects one player from the wolf kill; never the same player two nights in a row; if also saved by the witch the guard conflict kills them." },
    { 6, "idiot",      "白痴",   CAMP_GOD,    "神职阵营。被放逐时可翻牌免死（仅一次），翻牌后失去投票权但继续存活。",
                                          "God camp. When exiled you may flip your card and survive (once); after flipping you lose voting rights but stay alive." },
    { 7, "cupid",      "丘比特", CAMP_NEUTRAL,"中立阵营。开局为两名玩家（可含自己）结成情侣；情侣一方死亡另一方殉情。若情侣为一狼一好人，则情侣组成第三方阵营。",
                                          "Neutral camp. At start, pairs two players (you may include yourself) as lovers; if one dies the other follows. A wolf+good pair forms a third camp." },
    { 8, "thief",      "盗贼",   CAMP_NEUTRAL,"中立阵营。开局身份池中额外放入两份身份卡，盗贼从中二选一（可含狼）后按所选身份行动。",
                                          "Neutral camp. At start, two extra identity cards are added to the pool; the thief picks one (a wolf may be hidden in it) and acts as that identity." },
    { 9, "villager",   "村民",   CAMP_VILLAGER,"好人平民。无特殊技能，白天参与发言与投票放逐。",
                                          "Good side. No special skill; talks and votes to exile during the day." },
};

const int JOB_COUNT = sizeof(JOBS) / sizeof(JOBS[0]);

// 按职业名（英文或中文）查找职业。返回 nullptr 表示未找到。
const JobDef* FindJob(const string& name)
{
    for (int i = 0; i < JOB_COUNT; ++i)
    {
        if (JOBS[i].enName == name || JOBS[i].zhName == name)
        {
            return &JOBS[i];
        }
    }
    return nullptr;
}

// ============ 档位/村民开关对应的职业集合 ============
// 档位：0 基础 / 1 经典 / 2 豪华（需求 §3.2）
// 返回该档位下启用的非村民职业 ID 列表。
bool GetJobsForLevel(int level, vector<int>& out)
{
    out.clear();
    if (level < 0 || level > 2) return false;
    // 档位 0 基础：狼人、预言家、女巫、猎人
    out.push_back(0); // werewolf
    out.push_back(2); // seer
    out.push_back(3); // witch
    out.push_back(4); // hunter
    if (level >= 1)
    {
        out.push_back(5); // guard
        out.push_back(6); // idiot
    }
    if (level >= 2)
    {
        out.push_back(1); // whitewolf
        out.push_back(7); // cupid
        out.push_back(8); // thief
    }
    return true;
}

// ============ 命令表 ============
// 语言分 中/英 两层：英文命令、中文命令、英文短别名同一条写入表项。
// alias 为英文短别名（VOTE→"V" 等，无别名为空串）；desc/descEn 为双语文案。
struct CommandEntry
{
    const char* en;     // 英文命令（如 "RATIO"）
    const char* zh;     // 中文命令（如 "比例"）
    const char* alias;  // 英文短别名（如 VOTE→"V"；空串表示无短别名）
    const char* args;   // 参数说明（“无参数”用空串）
    const char* desc;   // 中文功能描述
    const char* descEn; // 英文功能描述
    int group;          // 命令分组（HELP 展示用）：通用/大厅/房间/游戏
};

// 命令分组：HELP 时按此分类排序输出
enum CmdGroup
{
    CMD_GROUP_COMMON = 0,   // 通用（任何状态可用）
    CMD_GROUP_LOBBY,        // 大厅
    CMD_GROUP_ROOM,         // 房间
    CMD_GROUP_GAME,         // 游戏
};

const CommandEntry COMMANDS[] = {
    // ---- 通用 ----
    { "HELP",    "帮助",     "",    "[ALL|职业名]", "显示命令帮助；HELP ALL 查看全部职业列表", "Show command help; HELP ALL lists all roles", CMD_GROUP_COMMON },
    { "NAME",    "名字",     "",    "<新名字>",     "改名（全服唯一，重名会被拒绝；名字不能是 IP 格式）", "Rename (unique server-wide; IP-like names are rejected)", CMD_GROUP_COMMON },
    { "EXIT",    "退出",     "",    "",             "离开房间 / 游戏 / 退出程序", "Leave room / game / quit", CMD_GROUP_COMMON },
    // ---- 大厅 ----
    { "LIST",    "列表",     "",    "",             "查看房间列表", "List rooms", CMD_GROUP_LOBBY },
    { "CREATE",  "建房",     "CR",  "<端口>",       "创建房间（端口 1024-65535）", "Create a room (port 1024-65535)", CMD_GROUP_LOBBY },
    { "JOIN",    "加入",     "",    "<端口>",       "加入指定房间", "Join a room", CMD_GROUP_LOBBY },
    // ---- 房间（标【房主】的仅房主可用） ----
    { "READY",   "准备",     "",    "",             "准备 / 取消准备", "Ready / not ready", CMD_GROUP_ROOM },
    { "STATUS",  "状态",     "ST",  "",             "查看本房玩家与准备状态", "Show room members and ready status", CMD_GROUP_ROOM },
    { "START",   "开始",     "",    "",             "【房主】全员准备后开始游戏", "[Host] Start the game when all ready", CMD_GROUP_ROOM },
    { "AUTO",    "自动",     "",    "",             "【房主】切换“全员准备自动开局”开关", "[Host] Toggle auto-start when all ready", CMD_GROUP_ROOM },
    { "TRANSFER","转移",     "TF",  "<槽号或名字>", "【房主】把房主转给指定玩家", "[Host] Transfer host to a player", CMD_GROUP_ROOM },
    { "PICK",    "踢",       "",    "<槽号或名字>", "【房主】踢出指定玩家（禁入 10 秒）", "[Host] Kick a player (blocked for 10s)", CMD_GROUP_ROOM },
    { "BAN",     "拉黑",     "",    "<名字/IP 或文件>", "【房主】拉黑玩家或 IP（空格分隔批量，.ban 文件导入）", "[Host] Ban players or IPs (space-separated batch, .ban file import)", CMD_GROUP_ROOM },
    { "UNBAN",   "取消拉黑", "",    "<名字/IP 或文件>", "【房主】取消拉黑（批量或 .ban 文件导入）", "[Host] Unban names or IPs (batch or .ban file import)", CMD_GROUP_ROOM },
    { "MUTE",    "禁言",     "",    "<槽号/名字/通配/ALL>", "【房主】禁言玩家（空格分隔多项；被禁言者的聊天不会广播）", "[Host] Mute players (space-separated; muted chat is not broadcast)", CMD_GROUP_ROOM },
    { "UNMUTE",  "解禁",     "",    "<名字/通配/ALL>",       "【房主】解除禁言（空格分隔多项；ALL 清空全部禁言）", "[Host] Unmute players (space-separated; ALL clears all)", CMD_GROUP_ROOM },
    { "IP",      "查IP",     "",    "<玩家名>",       "【房主】查询房间内玩家 IP（游戏期保留槽位也可查）", "[Host] Show a player's IP (retained slots queryable)", CMD_GROUP_ROOM },
    { "LG",      "日志",     "",    "",               "【房主】查看房间玩家进出记录", "[Host] Show room entry/exit log", CMD_GROUP_ROOM },
    { "LEVEL",   "档位",     "",    "<0|1|2>",      "【房主】设置职业档位（0 基础 / 1 经典 / 2 豪华）", "[Host] Set role level (0 basic / 1 classic / 2 deluxe)", CMD_GROUP_ROOM },
    { "VILLAGER","村民",     "VG",  "<0|1>",        "【房主】开关村民职业（1=启用，默认关闭）", "[Host] Toggle villager role (1=on, default off)", CMD_GROUP_ROOM },
    { "RATIO",   "比例",     "",    "<狼> <中立> <神>", "【房主】设置三阵营人数（真实人数，非法不设置）", "[Host] Set wolf/neutral/god counts (real numbers)", CMD_GROUP_ROOM },
    { "CONFIRM", "同意",     "CF",  "<1|0>",        "【房主】对自动配置结果确认（1 同意 / 0 拒绝）", "[Host] Confirm auto config (1 agree / 0 reject)", CMD_GROUP_ROOM },
    { "SHOW",    "查看",     "LOOK","<BAN|RATIO|LEVEL|VILLAGER|AUTO|ADD>", "查看黑名单/比例/配置/本地用户与 NPC（LOOK 等效）", "Show ban list / ratio / config / local users and NPCs (LOOK alias)", CMD_GROUP_ROOM },
    { "ADD",     "添加",     "",    "<USER <名字> [-u] <玩家> | NPC [名字] on|off>", "【房主】添加本地用户（新窗口，由指定玩家控制）或 NPC（on=在线 AI，off=离线逻辑）", "[Host] Add a local user (new window, controlled by a player) or an NPC (on=online AI, off=offline logic)", CMD_GROUP_ROOM },
    // ---- 游戏 ----
    { "VOTE",    "投票",     "V",   "<编号>",       "白天投票放逐（编号=槽位，0 弃权）", "Vote to exile (number=slot, 0 abstain)", CMD_GROUP_GAME },
    { "BOMB",    "自爆",     "B",   "<编号>",       "白狼王白天自爆（带走一名玩家进夜晚）", "White Wolf King bomb (take one player into night)", CMD_GROUP_GAME },
};
const int COMMAND_COUNT = sizeof(COMMANDS) / sizeof(COMMANDS[0]);

// 按命令字（英文全名/英文短别名/中文别名）查找命令。返回指针或 nullptr。
// allowZh=false（英文版客户端）时不匹配中文别名，杜绝中文命令在英文版生效。
const CommandEntry* FindCommand(const string& word, bool allowZh = true)
{
    for (int i = 0; i < COMMAND_COUNT; ++i)
    {
        // 英文命令与短别名忽略大小写；中文命令精确匹配（英文版关闭）
        if (_stricmp(word.c_str(), COMMANDS[i].en) == 0
            || (_stricmp(word.c_str(), COMMANDS[i].alias) == 0)
            || (allowZh && word == COMMANDS[i].zh))
        {
            return &COMMANDS[i];
        }
    }
    return nullptr;
}

// ============ 控制台工具（来自参考/demon/common.h） ============

// 关闭控制台"快速编辑"模式（防止鼠标框选阻塞程序）。
// 关键：ENABLE_QUICK_EDIT_MODE 只有在同时设置了 ENABLE_EXTENDED_FLAGS 时
// 才会被 SetConsoleMode 真正改变——不设该标志，系统会静默忽略这次修改。
void DisableConsoleQuickEdit()
{
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;

    if (!GetConsoleMode(hStdin, &mode)) return;

    mode &= ~ENABLE_QUICK_EDIT_MODE;
    mode &= ~ENABLE_MOUSE_INPUT;
    mode &= ~ENABLE_INSERT_MODE;
    mode |= ENABLE_EXTENDED_FLAGS;
    SetConsoleMode(hStdin, mode);
}

// 控制台切换为 UTF-8 编码（源码为 UTF-8，/utf-8 编译后字符串字面量为 UTF-8）。
void SetConsoleUtf8()
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

// 设置控制台字体：优先切等宽"新宋体"，失败则恢复原字体（中文显示不变形）。
void SetConsoleFont()
{
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_FONT_INFOEX fontInfo;
    CONSOLE_FONT_INFOEX check;

    fontInfo.cbSize = sizeof(CONSOLE_FONT_INFOEX);
    GetCurrentConsoleFontEx(hConsole, FALSE, &fontInfo);
    CONSOLE_FONT_INFOEX original = fontInfo;

    wcscpy_s(fontInfo.FaceName, L"NSimSun");
    SetCurrentConsoleFontEx(hConsole, FALSE, &fontInfo);

    check.cbSize = sizeof(CONSOLE_FONT_INFOEX);
    GetCurrentConsoleFontEx(hConsole, FALSE, &check);
    if (wcscmp(check.FaceName, L"NSimSun") != 0)
    {
        SetCurrentConsoleFontEx(hConsole, FALSE, &original);
    }
}

// 显示/隐藏光标
void ShowCursor(bool visible)
{
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO cursorInfo;

    GetConsoleCursorInfo(hConsole, &cursorInfo);
    cursorInfo.bVisible = visible;
    SetConsoleCursorInfo(hConsole, &cursorInfo);
}

// 清屏：直接用控制台 API 填充空白，不派生子进程（不用 system("cls")）。
void ClearScreen()
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;

    if (!GetConsoleScreenBufferInfo(hOut, &info)) return;

    COORD topLeft = { 0, 0 };
    DWORD written = 0;

    DWORD cells = (DWORD)info.dwSize.X * info.dwSize.Y;
    FillConsoleOutputCharacterW(hOut, L' ', cells, topLeft, &written);
    SetConsoleCursorPosition(hOut, topLeft);
}

// ============ 日志 ============

// 向 logFile 追加一行带时间戳的日志，返回与日志同内容的字符串（供控制台输出）。
string LogMsg(const char* logFile, const string& msg)
{
    time_t now = time(nullptr);
    char buf[64];

    ctime_s(buf, sizeof(buf), &now);
    buf[strlen(buf) - 1] = '\0';

    string logmsg = "[" + string(buf) + "] " + msg;

    ofstream logfile(logFile, ios::app);
    if (logfile.is_open())
    {
        logfile << logmsg << endl;
        logfile.flush();
    }

    return logmsg;
}

// ============ 输入清理（安全） ============

// 统计 UTF-8 字符串的字符数（按码点计：续字节 0x80-0xBF 不计数）。
// 汉字一字计 1 字符，与"限 10 个字符内"的人话直觉一致（非按字节）。
int CountUtf8Chars(const string& s)
{
    int n = 0;

    for (unsigned char c : s)
    {
        if ((c & 0xC0) != 0x80) ++n;
    }

    return n;
}

// 按码点截断 UTF-8 字符串到最多 maxChars 个字符（保持码点完整，不劈断汉字）。
string TruncateUtf8Chars(const string& s, int maxChars)
{
    if (CountUtf8Chars(s) <= maxChars) return s;

    string out;
    int n = 0;
    size_t i = 0;

    while (i < s.size() && n < maxChars)
    {
        unsigned char c = (unsigned char)s[i];
        size_t len;

        if (c >= 0xF0)      len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        else                len = 1;

        // 防御：尾部残缺序列不能越界拷贝，交给 SanitizeName 的尾修复兜底
        if (i + len > s.size()) len = s.size() - i;

        out += s.substr(i, len);
        i += len;
        ++n;
    }

    return out;
}

// 判断字符串是否为点分十进制 IPv4 地址（如 192.168.1.1）。
// 复用点：BAN/UNBAN 参数识别"按 IP 还是按名字操作"；NAME 校验驳回此格式。
// 拒绝前导零（如 192.168.001.1）：inet_ntoa 从不产生前导零，可避免
// 同值不同写的 IP 绕过拉黑（199.9.1.1 / 199.09.1.1）。
bool IsIpAddress(const string& s)
{
    if (s.empty() || s.size() > 15) return false;

    int dots = 0;
    string cur;

    for (size_t i = 0; i < s.size(); ++i)
    {
        char c = s[i];

        if (c == '.')
        {
            ++dots;
            if (cur.empty()) return false;
            cur.clear();
        }
        else if (c >= '0' && c <= '9')
        {
            cur += c;
            if (cur.size() > 3) return false;
        }
        else
        {
            return false;
        }
    }

    if (dots != 3 || cur.empty()) return false;

    // 复核每段：数值 0-255 且无前导零
    size_t start = 0;

    for (int i = 0; i < 4; ++i)
    {
        size_t end = s.find('.', start);
        if (end == string::npos) end = s.size();

        string part = s.substr(start, end - start);
        if (part.empty() || part.size() > 3) return false;
        if (atoi(part.c_str()) > 255) return false;
        if (part.size() > 1 && part[0] == '0') return false;

        start = end + 1;
    }

    return true;
}

// 只做名字字符过滤（去引号/竖线/换行/控制字符、空名补 Player），不截断长度。
// 供"名字是否 IP 格式"这类截断前判定使用：IP 是 7-15 字符，先截断
// 会把 11 位的真实 IP 切成长度 10 的非 IP 串（如 192.168.1.），漏检掉。
string StripInvalidChars(const string& name)
{
    string out;

    for (char c : name)
    {
        if (c == '"' || c == '|' || c == '\n' || c == '\r' || (unsigned char)c < 32)
        {
            continue;
        }
        out += c;
    }

    if (out.empty()) out = "Player";

    return out;
}

// 名字是否构成禁止的 IP 格式（在长度截断前判定）：
// 名字拉黑按名字、IP 拉黑按 IP，玩家若拿 IP 当名字则绕过 IP 黑名单，
// 故形如 IP 的名字一律驳回。客户端/服务端 NAME 校验共用。
bool LooksLikeIpName(const string& raw)
{
    string x = StripInvalidChars(raw);

    if (IsIpAddress(x)) return true;

    // 前导零写法（199.09.1.1）不是合法 IPv4，但仍是点分数字形似串，
    // 混在名字里同样会与 IP 混淆，必须一并驳回。规则放宽为：
    // 2-4 段、每段 1-3 位数字、总长不超过 IP 上限 15 即判形似。
    int dots = 0;
    string seg;

    for (char c : x)
    {
        if (c == '.')
        {
            if (seg.empty() || seg.size() > 3) return false;
            seg.clear();
            ++dots;
        }
        else if (c >= '0' && c <= '9')
        {
            seg += c;
        }
        else
        {
            return false;
        }
    }

    if (seg.empty() || seg.size() > 3) return false;

    return dots >= 1 && dots <= 3 && x.size() <= 15;
}

// 名字字符白名单校验（需求 §14.6）：逐码点检查，仅允许 ASCII 字母/数字、
// 下划线 _ 与汉字（CJK 统一表意 0x4E00-0x9FFF）。空格、标点、emoji、
// 全角变体等一律拒绝。必须在 SanitizeName（净化+截断）之前对原始输入调用：
// 否则 "a b" 这类输入会被净化成合法名 "ab" 而绕过拒绝（净化容忍旧行为
// 已被新规则取代）。
bool IsValidNameChars(const string& s)
{
    size_t i = 0;

    while (i < s.size())
    {
        unsigned char c = (unsigned char)s[i];

        if (c < 0x80)
        {
            // ASCII：字母、数字、下划线
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_'))
            {
                return false;
            }

            ++i;
        }
        else if (c >= 0xE0 && c < 0xF0)
        {
            // 3 字节 UTF-8：解码出码点核对汉字区间；残缺序列一律拒绝
            if (i + 2 >= s.size()) return false;

            unsigned char c1 = (unsigned char)s[i + 1];
            unsigned char c2 = (unsigned char)s[i + 2];

            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return false;

            unsigned int cp = ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);

            if (cp < 0x4E00 || cp > 0x9FFF) return false;

            i += 3;
        }
        else
        {
            // 2 字节/4 字节（如 emoji）及其余序列：不在白名单
            return false;
        }
    }

    return true;
}

// 名字比较：大小写不敏感（只折叠 ASCII，汉字字节不受影响）。
// 重名判定与拉黑名单共用：若按大小写精确串比对，Grace/grace 之类的变体
// 会绕过重名与拉黑，统一按不敏感比较杜绝该旁路。
bool NameEquals(const string& a, const string& b)
{
    if (a.size() != b.size()) return false;

    for (size_t i = 0; i < a.size(); ++i)
    {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];

        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;

        if (ca != cb) return false;
    }

    return true;
}

// 去掉首尾空白（空格/Tab）。NAME|Grace␣ 的尾随空格会生成非预期的名字变体，
// 让拉黑名单的精确匹配落空；在命令解析阶段统一裁剪。
string TrimWhitespace(const string& s)
{
    size_t b = 0;
    size_t e = s.size();

    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;

    return s.substr(b, e - b);
}

// 清理玩家名：去除引号/竖线/换行/控制字符、限长（按码点计数）、修复截断的 UTF-8 码点。
string SanitizeName(string name)
{
    string out = StripInvalidChars(name);

    // 名字限 MAX_NAME_CHARS 个字符（按码点截断，汉字一字一字符，不劈半字）
    if (CountUtf8Chars(out) > MAX_NAME_CHARS)
    {
        out = TruncateUtf8Chars(out, MAX_NAME_CHARS);
    }

    // 修复尾不完整 UTF-8 序列（防汉字截断破坏最后一位）
    while (!out.empty())
    {
        size_t n = out.size();
        unsigned char last = (unsigned char)out[n - 1];

        if (last < 0x80) break;

        if (last >= 0xC0)
        {
            out.pop_back();
            continue;
        }

        size_t start = n;
        size_t cont = 0;
        while (start > 0 && (unsigned char)out[start - 1] >= 0x80 && (unsigned char)out[start - 1] <= 0xBF)
        {
            --start;
            ++cont;
        }

        if (start == 0)
        {
            out.clear();
            break;
        }

        unsigned char lead = (unsigned char)out[start - 1];
        size_t needed;
        if (lead >= 0xF0) needed = 4;
        else if (lead >= 0xE0) needed = 3;
        else if (lead >= 0xC0) needed = 2;
        else break;

        if (cont + 1 == needed) break;

        out.erase(start - 1);
        break;
    }
    return out;
}

// 聊天/命令清理：把换行符替换为空格，防止伪造协议消息。
string SanitizeChat(const string& s)
{
    string out;
    for (char c : s)
    {
        out += (c == '\r' || c == '\n') ? ' ' : c;
    }
    return out;
}

// 端口校验：只允许 1024-65535 的纯数字端口。
bool IsValidPort(const string& portStr)
{
    if (portStr.empty()) return false;
    for (char c : portStr)
    {
        if (!isdigit((unsigned char)c)) return false;
    }
    int p = atoi(portStr.c_str());
    return p >= 1024 && p <= 65535;
}

// ============ 网络工具 ============

// 从套接字按行读取（兼容 \r\n），每行交给 handler。
// 安全：先消化缓冲中已有完整行再 recv（防回房死锁）；
// 单行 16KB 上限；buffer 保留半行数据供下次拼接。
bool ReceiveLines(SOCKET sock, string& buffer, const function<void(const string&)>& handler)
{
    while (buffer.find('\n') != string::npos)
    {
        size_t pos = buffer.find('\n');
        string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) handler(line);
    }

    char data[4096];
    int bytes = recv(sock, data, sizeof(data) - 1, 0);

    if (bytes <= 0) return false;

    data[bytes] = '\0';
    buffer += data;

    if (buffer.size() > 16 * 1024)
    {
        buffer.clear();
        return false;
    }

    size_t pos;
    while ((pos = buffer.find('\n')) != string::npos)
    {
        string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) handler(line);
    }

    return true;
}

// 分割一行命令到 tokens（空格/Tab 分隔），自动跳过空 token。
vector<string> SplitTokens(const string& line)
{
    vector<string> tokens;
    string cur;
    for (char c : line)
    {
        if (c == ' ' || c == '\t')
        {
            if (!cur.empty())
            {
                tokens.push_back(cur);
                cur.clear();
            }
        }
        else
        {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

#endif // WOLF_COMMON_H