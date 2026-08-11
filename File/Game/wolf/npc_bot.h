// npc_bot.h - NPC 决策模块（header-only，Server.cpp include 后直接调用）
//
// 契约：REQUIREMENTS.md §19.7（ADD NPC）。在线 NPC（语言码 npc）走
// NpcOnlineDecide(ctx)，失败返回空串由调用方回退离线；离线 NPC
// （语言码 npc-off）走 NpcOfflineDecide(ctx)。两者都返回动作行：
//   SPEECH|内容 / VOTE|i（0=弃权）/ NIGHT_KILL|i / NIGHT_CHECK|i /
//   NIGHT_SAVE|i / NIGHT_POISON|i（-1=不用毒）/ NIGHT_GUARD|i（-1=不守）/
//   NIGHT_SHOOT|i（-1=不开枪）/ NONE，Server 直接按行消费。
//
// 与 Server.cpp 的配合（见其 NpcGetAction 组装点）：
//   - Server 填 NpcContext 的 selfIndex/roleEn/phase/aliveSlots/names/
//     targets/history/lang 字段；其余增强字段未填时本模块自动从
//     历史文本解析或回退，保证两种调用约定都能编译运行。
//   - 阶段名同时兼容 Server 的下划线写法（night_guard/night_kill/
//     night_check/night_save/night_poison/day_speech/day_vote/lastword/
//     hunter_shot）与文档的连字符写法（night-guard/dying/hunter-shoot 等）。
//
// 设计约定：
//   - 离线决策 = 模板 + 逻辑判断 + 随机数：结果不固定，但必须优先响应
//     历史文本里的明确线索（跳神职/查杀/验人结果），纯随机会被验收判定为
//     不合格（§19.7 明确要求"不是纯随机，必须按职业有逻辑判断"）。
//   - 无全局可变状态：随机数引擎 thread_local，各线程互不干扰；
//     模板表是只读静态数据，线程安全。
//   - 在线决策可失败：超时/非 200/JSON 解析失败一律返回空串，绝不抛异常，
//     由调用方回退离线逻辑。
//   - 环境变量覆盖（测试注入用，见 §19.7）：
//     WOLF_NPC_API_URL（默认官方地址）、WOLF_NPC_API_KEY（有 env 直接用，
//     否则读 DPAPI 加密落盘文件 npc_key.bin，见 NpcResolveKey）、
//     WOLF_NPC_TIMEOUT_SECONDS（默认 10，范围 1-60）、
//     WOLF_NPC_RETRIES（默认 1，范围 0-5）。
//   - 秘钥不写死在源码里：源码/二进制泄露不等于 key 泄露（需求 5.1）。

#ifndef WOLF_NPC_BOT_H
#define WOLF_NPC_BOT_H

#include "common.h"

#include <winhttp.h>
#include <dpapi.h>
#include <random>
#include <cstring>
#include <cctype>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

// ============ 随机数（线程安全） ============

// 每线程独立的引擎：把随机状态锁在线程上，多线程同时决策不会串扰；
// 同时避免每次调用重建引擎（random_device 慢，且频繁重种反而退化）
inline std::mt19937& NpcRng()
{
    static thread_local std::mt19937 rng = [] {
        std::random_device rd;
        std::seed_seq seq{ rd(), rd(), rd() };
        return std::mt19937(seq);
    }();

    return rng;
}

// 闭区间随机整数 [lo, hi]；hi <= lo 时退化为 lo（防御空列表调用）
inline int NpcRandInt(int lo, int hi)
{
    if (hi <= lo) return lo;

    std::uniform_int_distribution<int> dist(lo, hi);

    return dist(NpcRng());
}

// percent 概率（0-100）返回 true
inline bool NpcRandChance(int percent)
{
    return NpcRandInt(1, 100) <= percent;
}

// ============ 名字工具 ============

// 大小写不敏感比较：与 common.h NameEquals 同规则（名字去重/比对一律
// 不敏感，防 Grace/grace 变体绕过），本模块独立保留一份不依赖调用方
inline bool NpcNameEquals(const string& a, const string& b)
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

// 是否纯 ASCII 名字（内置 NPC 名全是英文，判断边界只需按英文字母/数字处理）
inline bool NpcIsAsciiName(const string& name)
{
    for (size_t i = 0; i < name.size(); ++i)
    {
        if ((unsigned char)name[i] > 127) return false;
    }

    return true;
}

// 名字在行中出现的位置是否构成"独立词"：Bob 前后不能是字母/数字/下划线，
// 否则会把 BobC 这类长名字里截出的子串误判成玩家 Bob
inline bool NpcNameBoundaryOk(const string& line, size_t pos, const string& name)
{
    if (pos > 0)
    {
        unsigned char c = (unsigned char)line[pos - 1];
        if (isalpha(c) || isdigit(c) || c == '_') return false;
    }

    size_t end = pos + name.size();
    if (end < line.size())
    {
        unsigned char c = (unsigned char)line[end];
        if (isalpha(c) || isdigit(c) || c == '_') return false;
    }

    return true;
}

// ============ 决策上下文 ============

// 决策上下文：Server 填主要字段（见 Server.cpp NpcGetAction），
// 其余增强字段未填时决策逻辑自动从历史文本解析或回退，两套调用约定兼容
struct NpcContext
{
    // ---- Server 必填字段 ----
    int selfIndex = 0;              // 自己槽位 1..N
    string roleEn;                  // 职业英文名（JOBS[].enName）
    string phase;                   // 阶段：night_guard/night_kill/night_check/
                                    // night_save/night_poison/day_speech/day_vote/
                                    // lastword/hunter_shot（连字符写法也兼容）
    vector<int> aliveSlots;         // 存活玩家槽位（1..N）
    vector<string> names;           // 全部玩家名（下标 i 即槽位 i+1）
    vector<int> targets;            // 可行动目标（槽号；内容由 Server 决定，
                                    // 投票时 0=弃权、夜晚行动不含自己）
    string history;                 // 中文历史文本（验人/死亡/刀人/阶段线索）
    Lang lang = Lang::Zh;           // 发言语言（NPC 固定中文，需求 §19.7）

    // ---- 可选增强字段（未填时自动回退到上面字段或历史解析） ----
    int playerIndex = 0;            // 自己在名单中的位置（未填用 selfIndex）
    string roleEnName;              // 职业英文名（未填用 roleEn）
    bool isDay = false;             // 是否白天（未填按 phase 推断）
    vector<int> alivePlayers;       // 存活玩家索引（未填用 aliveSlots）
    int dayNumber = 0;              // 当前天数（0=未知，从 history "第N夜/天" 解析）
    int lastGuardTarget = 0;        // 守卫上一夜守的对象（0=未知，从 history 解析）

    // ---- 自由讨论上下文（白天聊天专用；夜晚/投票决策不需要时留空即可） ----
    vector<string> chatLog;         // 本白天全部聊天行（名字：内容，含投票广播）
    string atTarget;                // 被 @ 时的完整文本（去@头内容+完整原始行），无=空
    string lastChat;                // 距上次该 NPC 发言后新增的聊天拼接（限 400 字）
};

// 取决策者索引（Server 的 selfIndex 优先）
inline int NpcSelfIndex(const NpcContext& ctx)
{
    return (ctx.selfIndex != 0) ? ctx.selfIndex : ctx.playerIndex;
}

// 取职业英文名（Server 的 roleEn 优先）
inline string NpcRole(const NpcContext& ctx)
{
    return ctx.roleEn.empty() ? ctx.roleEnName : ctx.roleEn;
}

// 取存活玩家列表（Server 的 aliveSlots 优先）
inline vector<int> NpcAliveList(const NpcContext& ctx)
{
    if (!ctx.aliveSlots.empty()) return ctx.aliveSlots;

    return ctx.alivePlayers;
}

// 阶段名归一化：连字符转下划线、文档别名 dying 映射到集成侧 lastword，
// 保证两套约定下的阶段名都能落到同一个分派分支
inline string NpcNormalizePhase(const string& phase)
{
    string p = phase;

    for (size_t i = 0; i < p.size(); ++i)
    {
        if (p[i] == '-') p[i] = '_';
    }

    if (p == "dying") p = "lastword";

    return p;
}

// 当前天数：字段有值直接用；否则从历史 "第N夜/天" 解析（Server 的
// BuildNpcHistory 开头就写"现在是第N夜/天"，文本来源稳定可靠）
inline int NpcDayNumber(const NpcContext& ctx)
{
    if (ctx.dayNumber >= 1) return ctx.dayNumber;

    size_t p = ctx.history.find("第");

    while (p != string::npos)
    {
        size_t d = p + 3; // "第" 是 3 字节 UTF-8

        while (d < ctx.history.size() && isdigit((unsigned char)ctx.history[d])) ++d;

        if (d > p + 3 && d + 3 <= ctx.history.size()
            && (ctx.history.compare(d, 3, "夜") == 0 || ctx.history.compare(d, 3, "天") == 0))
        {
            return atoi(ctx.history.substr(p + 3, d - p - 3).c_str());
        }

        p = ctx.history.find("第", p + 3);
    }

    return 1;
}

// 守卫上一夜守的人：字段有值直接用；否则从历史"守护对象：N号"解析
// （Server 在 night_guard 阶段的 extra 线索里带了这段文本）
inline int NpcGuardLast(const NpcContext& ctx)
{
    if (ctx.lastGuardTarget != 0) return ctx.lastGuardTarget;

    size_t p = ctx.history.find("守护对象");

    if (p == string::npos) return 0;

    // 全角冒号是三字节字符，必须用字符串字面量匹配 UTF-8 序列
    // （多字节字符字面量 '：' 会被 MSVC 截断成单字节，永远匹配不上）
    size_t colon = ctx.history.find("：", p);

    if (colon == string::npos) return 0;

    size_t d = colon + 1;

    // "无（无人）" 一类非数字文本说明上一夜没守人
    if (d >= ctx.history.size() || (unsigned char)ctx.history[d] >= 0x80) return 0;

    size_t start = d;

    while (d < ctx.history.size() && isdigit((unsigned char)ctx.history[d])) ++d;

    if (d == start) return 0;

    return atoi(ctx.history.substr(start, d - start).c_str());
}

// 取玩家名（越界防御：索引非法或名单为空返回空串）
inline string NpcPlayerName(const NpcContext& ctx, int idx)
{
    if (idx >= 1 && idx <= (int)ctx.names.size())
    {
        return ctx.names[idx - 1];
    }

    return "";
}

// 目标是否在可选范围内：决策必须落在 Server 给的可选目标里，
// 越界目标会被 Server 判定非法动作重新询问
inline bool NpcInTargets(const NpcContext& ctx, int idx)
{
    for (size_t i = 0; i < ctx.targets.size(); ++i)
    {
        if (ctx.targets[i] == idx) return true;
    }

    return false;
}

// 按换行切分历史文本（\r 一并剥掉），逐行分析比整块子串搜索更可靠：
// 线索只应来自"同一句话"，跨行的名字+关键词组合是噪声
inline vector<string> NpcSplitLines(const string& s)
{
    vector<string> lines;
    string cur;

    for (size_t i = 0; i < s.size(); ++i)
    {
        char c = s[i];

        if (c == '\n')
        {
            lines.push_back(cur);
            cur.clear();
        }
        else if (c != '\r')
        {
            cur += c;
        }
    }

    if (!cur.empty()) lines.push_back(cur);

    return lines;
}

// ============ 内置 NPC 名单 ============

// 100 个互不重复的英文名：ADD NPC 无名字时按序取第一个未占用名（§19.7）；
// 全部用字母组成，与 NAME 白名单（中英文/数字/下划线）天然兼容，不会被净化截断
inline const vector<string>& NpcBuiltinNames()
{
    static const vector<string> names = {
        "Alice", "Bob", "Carol", "Dave", "Eve", "Frank", "Grace", "Heidi", "Ivan", "Judy",
        "Mallory", "Nancy", "Oscar", "Peggy", "Trent", "Victor", "Wendy", "Alex", "Brianna", "Chris",
        "Dana", "Erin", "Fiona", "Gabe", "Hannah", "Isaac", "Jenna", "Kevin", "Laura", "Mike",
        "Nina", "Oliver", "Paula", "Quinn", "Rachel", "Sam", "Tina", "Uma", "Vance", "Will",
        "Xena", "Yvonne", "Zack", "Aaron", "Belle", "Clyde", "Daisy", "Edgar", "Flora", "Gary",
        "Holly", "Jack", "Kelly", "Leo", "Marge", "Nora", "Owen", "Penny", "Rita", "Scott",
        "Tracy", "Ulysses", "Vera", "Walt", "Yolanda", "Zoe", "Adam", "Beth", "Colin", "Doris",
        "Ethan", "Faye", "Glen", "Helen", "Iris", "Jason", "Katie", "Liam", "Mona", "Neal",
        "Opal", "Perry", "Rose", "Saul", "Terra", "Ursula", "Vincent", "Wanda", "Xavier", "Yves",
        "Zane", "Amber", "Angela", "Bruce", "Claire", "Derek", "Eleanor", "Felix", "Gloria", "Harvey",
    };

    return names;
}

// 内置名单里第一个未占用的名字：占用判定大小写不敏感（与全局名字查重同规则），
// 全被占用返回空串由调用方回退到让玩家显式指定名字
inline string NpcNextFreeName(const vector<string>& used)
{
    const vector<string>& all = NpcBuiltinNames();

    for (size_t i = 0; i < all.size(); ++i)
    {
        bool taken = false;

        for (size_t j = 0; j < used.size(); ++j)
        {
            if (NpcNameEquals(all[i], used[j]))
            {
                taken = true;
                break;
            }
        }

        if (!taken) return all[i];
    }

    return "";
}

// 职业英文名转中文显示名：在线 prompt 与遗言模板要用中文职业名，
// 未识别的职业原样返回（防御未知职业扩展）
inline string NpcRoleZh(const string& en)
{
    static const struct { const char* en; const char* zh; } map[] = {
        { "werewolf",  "狼人" },
        { "whitewolf", "白狼王" },
        { "seer",      "预言家" },
        { "witch",     "女巫" },
        { "hunter",    "猎人" },
        { "guard",     "守卫" },
        { "idiot",     "白痴" },
        { "cupid",     "丘比特" },
        { "thief",     "盗贼" },
        { "villager",  "村民" },
    };

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
    {
        if (en == map[i].en) return map[i].zh;
    }

    return en;
}

// ============ 环境变量与 JSON 工具（在线决策用） ============

// 读环境变量：未设置返回默认值（测试注入用，见 §19.7）
inline string NpcEnvOr(const char* name, const char* dflt)
{
    char buf[1024] = { 0 };
    size_t len = 0;

    if (getenv_s(&len, buf, sizeof(buf), name) == 0 && len > 0) return string(buf);

    return dflt;
}

// 读整数环境变量并夹紧到 [lo, hi]；非数字或未设置回退默认值——
// 注入值不可信，越界/乱值必须按保守默认走，不能把超时设成负数或 0
inline int NpcEnvInt(const char* name, int dflt, int lo, int hi)
{
    string v = NpcEnvOr(name, "");

    if (v.empty()) return dflt;

    for (size_t i = 0; i < v.size(); ++i)
    {
        if (!isdigit((unsigned char)v[i])) return dflt;
    }

    int n = atoi(v.c_str());

    if (n < lo) n = lo;
    if (n > hi) n = hi;

    return n;
}

// JSON 字符串转义：中文按 UTF-8 原样输出，只转义结构字符与控制字符，
// 避免模型 prompt 里的引号/换行把请求体拆坏
inline string NpcJsonEscape(const string& s)
{
    string out;
    out.reserve(s.size() + 16);

    for (size_t i = 0; i < s.size(); ++i)
    {
        unsigned char c = (unsigned char)s[i];

        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20)
        {
            char buf[16];

            sprintf(buf, "\\u%04x", c);
            out += buf;
        }
        else out += (char)c;
    }

    return out;
}

// UTF-8 字符串转宽字符（WinHTTP 的接口只收宽字符串；名字/路径里可能有中文）
inline wstring NpcToWide(const string& s)
{
    if (s.empty()) return L"";

    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);

    if (n <= 0) return L"";

    wstring w;
    w.resize(n);

    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);

    return w;
}

// 阶段名转中文描述（在线 prompt 的"当前阶段"行）
inline string NpcPhaseZh(const string& phase)
{
    string p = NpcNormalizePhase(phase);

    if (p == "night_check") return "夜晚-预言家验人";
    if (p == "night_kill") return "夜晚-狼人刀人";
    if (p == "night_save") return "夜晚-女巫解药";
    if (p == "night_poison") return "夜晚-女巫毒药";
    if (p == "night_guard") return "夜晚-守卫守护";
    if (p == "hunter_shot" || p == "hunter_shoot") return "猎人开枪";
    if (p == "day_vote") return "白天-投票";
    if (p == "day_speech") return "白天-发言";
    if (p == "lastword") return "遗言";

    return p;
}

// ============ 历史文本线索解析 ============

// 一条验人记录：夜数、被验者槽号、结果（狼人/好人/中立）
struct NpcCheckNote
{
    int night;
    int slot;
    string result;
};

// 从"你的验人记录"解析全部验人条目。Server 拼接记录时只用句号分隔、
// 不换行，多条记录可能挤在同一行，必须逐条循环找"查验"而不是整行一次
inline vector<NpcCheckNote> NpcParseCheckNotes(const vector<string>& lines)
{
    vector<NpcCheckNote> out;

    for (size_t li = 0; li < lines.size(); ++li)
    {
        const string& ln = lines[li];
        size_t p = 0;

        while ((p = ln.find("查验", p)) != string::npos)
        {
            // 夜数：往前找"第N夜"，要求"夜"字紧贴"查验"（格式"第N夜查验"）
            size_t dpos = ln.rfind("第", p);

            if (dpos == string::npos || dpos + 3 > p)
            {
                p += 6;
                continue;
            }

            size_t nd = dpos + 3;
            size_t n0 = nd;

            while (nd < p && isdigit((unsigned char)ln[nd])) ++nd;

            if (nd == n0 || nd + 3 != p || ln.compare(nd, 3, "夜") != 0)
            {
                p += 6;
                continue;
            }

            int night = atoi(ln.substr(n0, nd - n0).c_str());

            // 槽号："查验"后紧跟数字
            size_t sd = p + 6;
            size_t s0 = sd;

            while (sd < ln.size() && isdigit((unsigned char)ln[sd])) ++sd;

            if (sd == s0)
            {
                p += 6;
                continue;
            }

            int slot = atoi(ln.substr(s0, sd - s0).c_str());

            if (slot < 1)
            {
                p += 6;
                continue;
            }

            // 名字："号"之后到全角冒号；结果：冒号之后到句号
            size_t nstart = sd + 3;

            if (nstart + 1 >= ln.size())
            {
                p += 6;
                continue;
            }

            size_t colon = ln.find("：", nstart);

            if (colon == string::npos)
            {
                p += 6;
                continue;
            }

            string name = ln.substr(nstart, colon - nstart);
            size_t rstart = colon + 3;
            size_t rend = ln.find("。", rstart);

            if (rend == string::npos) rend = ln.size();

            NpcCheckNote n;

            n.night = night;
            n.slot = slot;
            n.result = ln.substr(rstart, rend - rstart);

            out.push_back(n);
            p += 6;
        }
    }

    return out;
}

// 数组是否含某元素（验过目标去重、目标池过滤共用）
inline bool NpcContains(const vector<int>& v, int x)
{
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (v[i] == x) return true;
    }

    return false;
}

// ============ UTF-8 码点工具（别称匹配用） ============

// 取 s 中第 i 字节处一个码点的字节数（1-4）；i 越界或非法序列返回 0
inline size_t NpcCpLenAt(const string& s, size_t i)
{
    if (i >= s.size()) return 0;

    unsigned char c = (unsigned char)s[i];

    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;

    return 0;
}

// 字符串的码点个数（多字节字符按码点算，2 码点门限必须用码点而非字节数）
inline size_t NpcCpCount(const string& s)
{
    size_t n = 0;

    for (size_t i = 0; i < s.size(); )
    {
        size_t len = NpcCpLenAt(s, i);

        if (len == 0)
        {
            ++i;
            continue;
        }

        ++n;
        i += len;
    }

    return n;
}

// 名字的保守近似匹配（宁漏勿误：错认别称会冤枉好人）：
// 1) 槽位写法「N号」「N 号」：直呼槽号是最直白的指名，直接命中；
// 2) 首码点相同 + 目标名 ≥2 码点 + 文中提及片段 2-4 码点 + 同句含
//    发言/投票/阵营等社会性词，才判为别名；
// 3) 单码点名字（如"A"）不纳入，任何 ASCII 单字母都能当首码点，误伤太高
inline bool NpcMatchNickname(const string& line, const string& name, int slot)
{
    if (name.empty()) return false;

    // 槽位写法：数字 + 「号」（中间可有一个 ASCII 空格）
    string num = to_string(slot);

    for (size_t p = 0; p + num.size() <= line.size(); ++p)
    {
        bool same = true;

        for (size_t i = 0; i < num.size(); ++i)
        {
            if (line[p + i] != num[i])
            {
                same = false;
                break;
            }
        }

        if (!same) continue;

        // 数字前不能还是数字：「13号」不能被「3号」误中
        if (p > 0 && isdigit((unsigned char)line[p - 1])) continue;

        size_t after = p + num.size();

        if (after < line.size() && line[after] == ' ') ++after;

        if (after + 3 <= line.size() && line.compare(after, 3, "号") == 0) return true;
    }

    if (NpcCpCount(name) < 2) return false;

    // 同句必须有"讨论感"词，纯名字串（如名单行）不做别名猜测
    static const char* const kws[] = {
        "发言", "说", "觉得", "认为", "投票", "票", "狼", "好人",
        "身份", "怀疑", "验", "查", "死", "刀",
    };
    bool kwHit = false;

    for (size_t k = 0; k < sizeof(kws) / sizeof(kws[0]); ++k)
    {
        if (line.find(kws[k]) != string::npos)
        {
            kwHit = true;
            break;
        }
    }

    if (!kwHit) return false;

    // 首码点比对：取目标名第一个码点，逐码点位置试取 2-4 码点片段
    size_t firstLen = NpcCpLenAt(name, 0);

    if (firstLen == 0 || firstLen >= name.size()) return false;

    const string firstCp = name.substr(0, firstLen);

    for (size_t i = 0; i < line.size(); )
    {
        size_t len = NpcCpLenAt(line, i);

        if (len == 0)
        {
            ++i;
            continue;
        }

        if (line.compare(i, len, firstCp) != 0)
        {
            i += len;
            continue;
        }

        // 从该起点向后延伸 2-4 个码点都算候选片段（2 码点即名字本身时跳过）
        size_t j = i;
        int cps = 0;

        while (j < line.size() && cps < 4)
        {
            size_t l2 = NpcCpLenAt(line, j);

            if (l2 == 0) break;

            ++cps;
            j += l2;

            // 片段不得就是全名：全名命中走精确匹配，别在近似路径重复计数
            if (cps >= 2 && line.compare(i, j - i, name) != 0) return true;
        }

        i += len;
    }

    return false;
}

// 在历史行里找"关键词 + 玩家名同现"的线索目标：同一行出现任一关键词、
// 且出现某个玩家名（独立词边界）即命中。逐行分析防跨行拼凑出假线索。
// 精确全名之外补一个保守近似（槽位号/首码点别名），命中同样记入候选
inline void NpcFindNameTargets(const NpcContext& ctx, const vector<string>& lines,
                               const char** kws, size_t kwCount, vector<int>& out)
{
    for (size_t li = 0; li < lines.size(); ++li)
    {
        const string& line = lines[li];
        bool kwHit = false;

        for (size_t k = 0; k < kwCount; ++k)
        {
            if (line.find(kws[k]) != string::npos)
            {
                kwHit = true;
                break;
            }
        }

        if (!kwHit) continue;

        for (int s = 1; s <= (int)ctx.names.size(); ++s)
        {
            if (s == NpcSelfIndex(ctx)) continue;

            const string& nm = ctx.names[s - 1];

            if (nm.empty()) continue;

            // 精确全名命中后不再跑近似路径：同名重复计数没有信息量
            bool exactHit = false;

            size_t p = line.find(nm);

            while (p != string::npos)
            {
                if (NpcNameBoundaryOk(line, p, nm))
                {
                    out.push_back(s);
                    exactHit = true;
                }

                p = line.find(nm, p + nm.size());
            }

            if (!exactHit && NpcMatchNickname(line, nm, s)) out.push_back(s);
        }
    }
}

// 明跳预言家报出的狼：行里同时有"我是预言家"与"狼人"，取"验"字之后
// 第一个出现的玩家名（跳预言家的人名在"验"字之前，天然被排除）
inline int NpcClaimedWolf(const NpcContext& ctx, const vector<string>& lines)
{
    for (size_t li = 0; li < lines.size(); ++li)
    {
        const string& ln = lines[li];

        if (ln.find("我是预言家") == string::npos) continue;
        if (ln.find("狼人") == string::npos) continue;

        size_t yan = ln.find("验");

        if (yan == string::npos) continue;

        for (int s = 1; s <= (int)ctx.names.size(); ++s)
        {
            const string& nm = ctx.names[s - 1];

            if (nm.empty()) continue;

            size_t p = ln.find(nm, yan);

            while (p != string::npos)
            {
                if (p >= yan && NpcNameBoundaryOk(ln, p, nm)) return s;

                p = ln.find(nm, p + nm.size());
            }
        }
    }

    return 0;
}

// 汇总当前可用的"查杀证据"目标，按可信度排序：
// 预言家自己验出的狼 > 明跳预言家报出的狼 > 查杀/怀疑行里点名的目标
inline int NpcPickSuspect(const NpcContext& ctx, const vector<string>& lines, int self)
{
    if (NpcRole(ctx) == "seer")
    {
        vector<NpcCheckNote> notes = NpcParseCheckNotes(lines);

        for (size_t i = notes.size(); i-- > 0; )
        {
            if (notes[i].result.find("狼人") != string::npos
                && notes[i].slot != self && NpcInTargets(ctx, notes[i].slot))
            {
                return notes[i].slot;
            }
        }
    }

    int claimed = NpcClaimedWolf(ctx, lines);

    if (claimed != 0 && claimed != self && NpcInTargets(ctx, claimed)) return claimed;

    static const char* susKws[] = { "查杀", "可疑", "怀疑" };
    vector<int> sus;

    NpcFindNameTargets(ctx, lines, susKws, 3, sus);

    for (size_t i = 0; i < sus.size(); ++i)
    {
        if (sus[i] != self && NpcInTargets(ctx, sus[i])) return sus[i];
    }

    return 0;
}

// ============ 权重随机与话题统计（自由讨论用） ============

// 简单占位替换：模板新增 {top} 等占位不扩展 NpcFill 的参数量，
// 独立替换函数避免多参数时的次序错位
inline string NpcReplacePh(const string& tmpl, const char* ph, const string& v)
{
    string s = tmpl;
    size_t p;

    while ((p = s.find(ph)) != string::npos) s.replace(p, strlen(ph), v);

    return s;
}

// 权重随机选一个：总权重内均匀随机，权重越高的候选被选中的概率越大。
// 空表返回 0（调用方保证不空时才有悬念）
inline int NpcWeightedPick(const vector<pair<int, int>>& cand)
{
    if (cand.empty()) return 0;

    int total = 0;

    for (size_t i = 0; i < cand.size(); ++i) total += cand[i].second;

    if (total <= 0) return cand[0].first;

    int r = NpcRandInt(0, total - 1);

    for (size_t i = 0; i < cand.size(); ++i)
    {
        r -= cand[i].second;

        if (r < 0) return cand[i].first;
    }

    return cand.back().first;
}

// 在聊天行里数某名字被独立提及的次数（「名字：内容」行内可能有多次出现）
inline int NpcMentionCount(const NpcContext& ctx, int slot)
{
    if (slot < 1 || slot > (int)ctx.names.size()) return 0;

    const string& nm = ctx.names[slot - 1];

    if (nm.empty()) return 0;

    int c = 0;

    for (size_t li = 0; li < ctx.chatLog.size(); ++li)
    {
        const string& ln = ctx.chatLog[li];
        size_t p = ln.find(nm);

        while (p != string::npos)
        {
            if (NpcNameBoundaryOk(ln, p, nm)) ++c;

            p = ln.find(nm, p + nm.size());
        }
    }

    return c;
}

// 聊天里是否被 @ 过：@ 后第一个 token（到空格/全角冒号）等于名字或槽号
inline bool NpcAtMentioned(const NpcContext& ctx, int slot)
{
    if (slot < 1 || slot > (int)ctx.names.size()) return false;

    const string& nm = ctx.names[slot - 1];

    for (size_t li = 0; li < ctx.chatLog.size(); ++li)
    {
        const string& ln = ctx.chatLog[li];
        size_t p = 0;

        while ((p = ln.find('@', p)) != string::npos)
        {
            size_t q = p + 1;

            while (q < ln.size() && ln[q] != ' ' && ln[q] != '：'
                   && (unsigned char)ln[q] < 0x80)
            {
                ++q;
            }

            string tok = ln.substr(p + 1, q - p - 1);

            if (!tok.empty())
            {
                bool digits = true;

                for (size_t i = 0; i < tok.size(); ++i)
                {
                    if (!isdigit((unsigned char)tok[i]))
                    {
                        digits = false;
                        break;
                    }
                }

                if (digits && atoi(tok.c_str()) == slot) return true;

                if (!nm.empty() && NpcNameEquals(tok, nm)) return true;
            }

            p = q;
        }
    }

    return false;
}

// 投票/点名的权重表：base 2 起步（无证据也有候选，绝不空表），再叠加：
// 自己验出狼 +10；明跳预言家报狼 +6；当天聊天被点名/被 @ +4；
// 聊天提及次数 ×2 且上限 +6。证据型高权重但不是必选——保留一定的
// "不跟票"行为，不会让所有 NPC 决策完全同步
inline vector<pair<int, int>> NpcVoteWeights(const NpcContext& ctx,
                                             const vector<string>& lines, int self)
{
    vector<pair<int, int>> w;

    auto addW = [&](int slot, int add)
    {
        if (slot == self) return;

        // 只允许投给 Server 给的可选目标：聊到死人也不投（Server 会判非法弃权）
        if (!NpcInTargets(ctx, slot)) return;

        for (size_t i = 0; i < w.size(); ++i)
        {
            if (w[i].first == slot)
            {
                w[i].second += add;
                return;
            }
        }

        w.push_back(pair<int, int>(slot, add));
    };

    for (size_t i = 0; i < ctx.targets.size(); ++i)
    {
        if (ctx.targets[i] != self) w.push_back(pair<int, int>(ctx.targets[i], 2));
    }

    // 预言家自己验出的狼：最高可信度
    if (NpcRole(ctx) == "seer")
    {
        vector<NpcCheckNote> notes = NpcParseCheckNotes(lines);

        for (size_t i = 0; i < notes.size(); ++i)
        {
            if (notes[i].result.find("狼人") != string::npos) addW(notes[i].slot, 10);
        }
    }

    // 明跳预言家报出的狼：公开信息，次高可信度
    int claimed = NpcClaimedWolf(ctx, lines);

    if (claimed != 0) addW(claimed, 6);

    // 被点名的怀疑对象（查杀/可疑/怀疑行的名字）
    static const char* susKws[] = { "查杀", "可疑", "怀疑" };
    vector<int> sus;

    NpcFindNameTargets(ctx, lines, susKws, 3, sus);

    for (size_t i = 0; i < sus.size(); ++i) addW(sus[i], 4);

    // 被 @ 与聊天提及热度：群众讨论本身就是线索分
    for (int s = 1; s <= (int)ctx.names.size(); ++s)
    {
        if (s == self || ctx.names[s - 1].empty()) continue;

        if (NpcAtMentioned(ctx, s)) addW(s, 4);

        int m = NpcMentionCount(ctx, s);

        if (m > 0) addW(s, min(m * 2, 6));
    }

    return w;
}

// 点一个加权随机怀疑对象（供发言模板点名；投票用同一张表保证口径一致）。
// 返回槽号，0 表示无候选（模板走无目标分支）
inline int NpcSuspectPick(const NpcContext& ctx, const vector<string>& lines, int self)
{
    vector<pair<int, int>> w = NpcVoteWeights(ctx, lines, self);

    return NpcWeightedPick(w);
}

// 聊天高频话题词：统计角色词（狼/预言家/女巫/守卫/猎人/票/死/刀）与
// 玩家名的出现次数，返回最高频者供「我觉得{词}那边有问题」类模板；
// 无聊天返回空串。需要 ctx 才能把玩家名也纳入统计
inline string NpcMentionTopic(const NpcContext& ctx)
{
    static const char* const words[] = {
        "狼", "预言家", "女巫", "守卫", "猎人", "票", "死", "刀",
    };
    const size_t W = sizeof(words) / sizeof(words[0]);

    // 角色词在前、玩家名在后共用一张计数表：角色词不套词边界
    // （「狼人」应同时命中「狼」），玩家名必须独立词边界
    vector<string> labels;
    vector<bool> needBound;

    for (size_t k = 0; k < W; ++k)
    {
        labels.push_back(words[k]);
        needBound.push_back(false);
    }

    for (size_t s = 1; s <= (size_t)ctx.names.size(); ++s)
    {
        if ((int)s == NpcSelfIndex(ctx))
        {
            labels.push_back("");
            needBound.push_back(true);
        }
        else
        {
            labels.push_back(ctx.names[s - 1]);
            needBound.push_back(true);
        }
    }

    vector<int> cnt(labels.size(), 0);

    for (size_t li = 0; li < ctx.chatLog.size(); ++li)
    {
        const string& ln = ctx.chatLog[li];

        for (size_t k = 0; k < labels.size(); ++k)
        {
            if (labels[k].empty()) continue;

            size_t p = 0;

            while ((p = ln.find(labels[k], p)) != string::npos)
            {
                if (!needBound[k] || NpcNameBoundaryOk(ln, p, labels[k])) ++cnt[k];

                p += labels[k].size();
            }
        }
    }

    int bestIdx = -1;

    for (size_t k = 0; k < cnt.size(); ++k)
    {
        if (cnt[k] > 0 && (bestIdx < 0 || cnt[k] > cnt[bestIdx])) bestIdx = (int)k;
    }

    if (bestIdx < 0) return "";

    return labels[bestIdx];
}

// ============ 发言模板（每类 ≥5 变体随机） ============

// 模板占位符填充：{n}=槽号 {name}=名字 {res}=验人结果 {role}=职业名。
// 用替换而不是 sprintf 变体，避免各模板占位符顺序不一致时参数错位
inline string NpcFill(const string& tmpl, const string& n, const string& name,
                      const string& res, const string& role)
{
    string s = tmpl;
    size_t p;

    while ((p = s.find("{n}")) != string::npos) s.replace(p, 3, n);
    while ((p = s.find("{name}")) != string::npos) s.replace(p, 6, name);
    while ((p = s.find("{res}")) != string::npos) s.replace(p, 5, res);
    while ((p = s.find("{role}")) != string::npos) s.replace(p, 6, role);

    return s;
}

// 验人结果词规范化：原始标签转发言用词（好人→金水），
// 未识别结果报金水而不是乱报狼人，避免编造查杀冤枉好人
inline string NpcResultWord(const string& raw)
{
    if (raw.find("狼人") != string::npos) return "狼人";
    if (raw.find("好人") != string::npos) return "金水";
    if (raw.find("中立") != string::npos) return "中立";

    return "金水";
}

// 预言家报验人模板（5 变体随机）
inline string NpcSeerReport(const string& n, const string& name, const string& res)
{
    static const char* const V[] = {
        "我是预言家，昨晚验了{n}号{name}，是{res}。",
        "昨晚查验{n}号{name}，结果为{res}，请大家记住。",
        "我是预言家。昨晚验了{name}（{n}号），{res}。",
        "报个验人：{n}号{name}是{res}。",
        "昨晚验{n}号{name}，{res}。信我就票型跟上。",
    };

    return NpcFill(V[NpcRandInt(0, 4)], n, name, res, "");
}

// 首日发言模板（5 变体随机；第一天没信息，只表水不带节奏）。
// topic 是聊天高频词：首日就出现话题词说明有人在带节奏，点一句留给后续观察
inline string NpcFirstDaySpeech(const string& selfName, const string& topic = "")
{
    static const char* const V[] = {
        "大家好，我是{name}。第一天先听发言，暂不投票。",
        "第一天信息少，先表水：我是好人，大家别乱票。",
        "首日不急着出人，等有身份的人发言后再判断。",
        "我是好人阵营，建议今天认真听发言，别盲投。",
        "第一天先观察，谁的发言明显带节奏，我重点留意。",
    };
    static const char* const TV[] = {
        "我是{name}。第一天先听发言，我注意到{tp}的话题，暂不投票。",
        "第一天信息少，先表水：我是好人。有人提{tp}，我先记一笔。",
        "首日不急着出人，{tp}这事等有身份的人表态后再判断。",
        "我是好人阵营，今天认真听发言。{tp}我这边先观察。",
        "第一天先观察，{tp}的风向我会重点留意。",
    };

    if (topic.empty()) return NpcFill(V[NpcRandInt(0, 4)], "", selfName, "", "");

    string s = NpcReplacePh(TV[NpcRandInt(0, 4)], "{tp}", topic);

    return NpcReplacePh(s, "{name}", selfName);
}

// 表水/怀疑模板（5 变体随机，点名怀疑对象；无目标时用通用表水）。
// topic 非空且无点名目标时：聊"话题本身"替代点名，避免永远没人带话题
inline string NpcSuspectSpeech(const string& name, const string& topic = "")
{
    static const char* const V[] = {
        "我觉得{name}发言不对劲，逻辑前后矛盾。",
        "我怀疑{name}，位置和节奏都像狼在带。",
        "{name}这轮发言很可疑，大家盯一下他的票型。",
        "我暂时怀疑{name}，还想再观察观察。",
        "听了这么久，{name}的嫌疑最大。",
    };
    static const char* const TV[] = {
        "我觉得{tp}那边有问题，大家盯一下。",
        "最近{tp}的讨论有点多，我怀疑这里藏着狼。",
        "我重点关注{tp}，投票前大家再想想。",
        "提到{tp}我就多留个心眼，先记一笔。",
        "{tp}的风向太整齐了，我怕有人带节奏。",
    };
    static const char* const GV[] = {
        "这轮发言我没什么头绪，先站好人边。",
        "我还在分析，暂时不点名，大家谨慎投票。",
        "今天节奏有点乱，希望有人带头梳理一下。",
        "我先表水：我是好人，听听大家的意见。",
        "这轮先听别人的发言，我晚点给结论。",
    };

    if (!name.empty()) return NpcFill(V[NpcRandInt(0, 4)], "", name, "", "");

    if (!topic.empty()) return NpcReplacePh(TV[NpcRandInt(0, 4)], "{tp}", topic);

    return GV[NpcRandInt(0, 4)];
}

// 被 @ 时的回应模板（5 变体随机；直接答而不复读对方全文——
// atTarget 里含「去头内容+完整原始行」，整段塞进发言会显得复读机）
inline string NpcAtReply()
{
    static const char* const V[] = {
        "收到，我来说说对这个点的看法。",
        "既然有人点名我，我就着这个话题回应两句。",
        "被@了，我表个态：先观察局势，不急着下结论。",
        "好的，我针对这个点补充下意见，供大家参考。",
        "回应一下刚提到我的发言：谨慎为上，再看票型。",
    };

    return V[NpcRandInt(0, 4)];
}

// 投票宣言模板（5 变体随机）
inline string NpcVoteSpeech(const string& name)
{
    static const char* const V[] = {
        "我投{name}。",
        "这一票我给{name}。",
        "今天先出{name}，我投他。",
        "我的票投给{name}。",
        "票{name}，理由前面已经说过了。",
    };

    return NpcFill(V[NpcRandInt(0, 4)], "", name, "", "");
}

// 遗言模板（5 变体随机，报身份+怀疑；无目标时用通用遗言）
inline string NpcLastwordSpeech(const string& roleZh, const string& name)
{
    static const char* const V[] = {
        "我是{role}，我怀疑{name}，大家帮我出他。",
        "{role}被投出去了，我怀疑{name}是狼，票他。",
        "最后说一句：我是{role}，{name}最可疑。",
        "我是{role}。如果{name}还活着，请一定盯住他。",
        "{role}遗言：{name}有狼面，下一轮把他出了。",
    };
    static const char* const GV[] = {
        "我是{role}，各位保重，好人加油。",
        "{role}先走一步，大家认真分析，别让狼赢。",
        "我是{role}，没来得及抓到狼，靠大家了。",
        "我是{role}，祝好人阵营顺利。",
        "{role}走了，大家一定小心。",
    };

    if (name.empty()) return NpcFill(GV[NpcRandInt(0, 4)], "", "", "", roleZh);

    return NpcFill(V[NpcRandInt(0, 4)], "", name, "", roleZh);
}

// 随机点一个存活目标当"怀疑对象"：无证据时表水/遗言不能永远不点名，
// 完全随机反而制造发言多样性（目标池由 Server 保证合法）
inline string NpcRandomSuspectName(const NpcContext& ctx, int self)
{
    vector<int> pool;

    for (size_t i = 0; i < ctx.targets.size(); ++i)
    {
        if (ctx.targets[i] != self) pool.push_back(ctx.targets[i]);
    }

    if (pool.empty()) return "";

    return NpcPlayerName(ctx, pool[NpcRandInt(0, (int)pool.size() - 1)]);
}

// ============ 离线决策（按职业逻辑 + 模板 + 随机） ============

// 预言家验人：先验"疑似目标（查杀/可疑/狼人线索）里没验过的"，
// 再验完全没验过的，最后兜底随机（已验目标重验毫无信息量，优先排除）
inline string NpcNightCheck(const NpcContext& ctx, const vector<string>& lines, int self)
{
    vector<NpcCheckNote> notes = NpcParseCheckNotes(lines);
    vector<int> checked;

    for (size_t i = 0; i < notes.size(); ++i) checked.push_back(notes[i].slot);

    static const char* susKws[] = { "查杀", "可疑", "怀疑", "狼人" };
    vector<int> sus;

    NpcFindNameTargets(ctx, lines, susKws, 4, sus);

    vector<int> pick;

    for (size_t i = 0; i < sus.size(); ++i)
    {
        if (sus[i] != self && NpcInTargets(ctx, sus[i]) && !NpcContains(checked, sus[i]))
        {
            pick.push_back(sus[i]);
        }
    }

    if (!pick.empty())
    {
        return "NIGHT_CHECK|" + to_string(pick[NpcRandInt(0, (int)pick.size() - 1)]);
    }

    pick.clear();

    for (size_t i = 0; i < ctx.targets.size(); ++i)
    {
        if (ctx.targets[i] != self && !NpcContains(checked, ctx.targets[i]))
        {
            pick.push_back(ctx.targets[i]);
        }
    }

    if (pick.empty())
    {
        for (size_t i = 0; i < ctx.targets.size(); ++i)
        {
            if (ctx.targets[i] != self) pick.push_back(ctx.targets[i]);
        }
    }

    if (pick.empty()) return "NONE";

    return "NIGHT_CHECK|" + to_string(pick[NpcRandInt(0, (int)pick.size() - 1)]);
}

// 狼人刀人：优先刀历史里暴露身份线索的神职（预言家>女巫>守卫>猎人），
// 无线索就在目标池随机（Server 保证不含狼队友与死者）
inline string NpcNightKill(const NpcContext& ctx, const vector<string>& lines, int self)
{
    static const char* roleKws[][1] = {
        { "预言家" },
        { "女巫" },
        { "守卫" },
        { "猎人" },
    };

    for (int i = 0; i < 4; ++i)
    {
        vector<int> cand;

        NpcFindNameTargets(ctx, lines, roleKws[i], 1, cand);

        vector<int> valid;

        for (size_t j = 0; j < cand.size(); ++j)
        {
            if (cand[j] != self && NpcInTargets(ctx, cand[j])) valid.push_back(cand[j]);
        }

        if (!valid.empty())
        {
            return "NIGHT_KILL|" + to_string(valid[NpcRandInt(0, (int)valid.size() - 1)]);
        }
    }

    if (ctx.targets.empty()) return "NONE";

    return "NIGHT_KILL|" + to_string(ctx.targets[NpcRandInt(0, (int)ctx.targets.size() - 1)]);
}

// 女巫解药：从"线索"里读当夜被刀者；首夜 70% 概率救（可自救），
// 后续夜 25%——药只有一瓶，后夜省着用；无人被刀则不用药
inline string NpcNightSave(const NpcContext& ctx, const vector<string>& lines, int self)
{
    (void)self;

    int victim = 0;

    for (size_t li = 0; li < lines.size(); ++li)
    {
        const string& ln = lines[li];
        size_t p = ln.find("被狼人击杀");

        if (p == string::npos) continue;

        if (ln.find("无人") != string::npos) return "NIGHT_SAVE|-1";

        size_t num = ln.find_first_of("0123456789", p);

        if (num != string::npos)
        {
            victim = atoi(ln.c_str() + num);
            break;
        }
    }

    if (victim == 0) return "NIGHT_SAVE|-1";

    int chance = (NpcDayNumber(ctx) <= 1) ? 70 : 25;

    if (!NpcRandChance(chance)) return "NIGHT_SAVE|-1";

    if (!NpcInTargets(ctx, victim)) return "NIGHT_SAVE|-1";

    return "NIGHT_SAVE|" + to_string(victim);
}

// 女巫毒药：只有明确查杀线索才低概率（30%）下毒，否则不用——
// 毒错人比不毒损失更大，没有把握不动药
inline string NpcNightPoison(const NpcContext& ctx, const vector<string>& lines, int self)
{
    (void)self;

    static const char* kws[] = { "查杀" };
    vector<int> cand;

    NpcFindNameTargets(ctx, lines, kws, 1, cand);

    vector<int> valid;

    for (size_t j = 0; j < cand.size(); ++j)
    {
        if (cand[j] != self && NpcInTargets(ctx, cand[j])) valid.push_back(cand[j]);
    }

    if (valid.empty()) return "NIGHT_POISON|-1";

    if (!NpcRandChance(30)) return "NIGHT_POISON|-1";

    return "NIGHT_POISON|" + to_string(valid[NpcRandInt(0, (int)valid.size() - 1)]);
}

// 守卫：不能连续两夜守同一人（规则硬约束），优先守线索里暴露的预言家，
// 其次随机（含守自己），10% 空守——全随机会让多守卫 NPC 行为完全同步
inline string NpcNightGuard(const NpcContext& ctx, const vector<string>& lines, int self)
{
    (void)self;

    int last = NpcGuardLast(ctx);

    static const char* seerKw[] = { "预言家" };
    vector<int> cand;

    NpcFindNameTargets(ctx, lines, seerKw, 1, cand);

    vector<int> valid;

    for (size_t j = 0; j < cand.size(); ++j)
    {
        if (cand[j] != last && NpcInTargets(ctx, cand[j])) valid.push_back(cand[j]);
    }

    if (!valid.empty())
    {
        return "NIGHT_GUARD|" + to_string(valid[NpcRandInt(0, (int)valid.size() - 1)]);
    }

    vector<int> pool;

    for (size_t j = 0; j < ctx.targets.size(); ++j)
    {
        if (ctx.targets[j] != last) pool.push_back(ctx.targets[j]);
    }

    if (pool.empty()) return "NIGHT_GUARD|-1";

    if (NpcRandChance(10)) return "NIGHT_GUARD|-1";

    return "NIGHT_GUARD|" + to_string(pool[NpcRandInt(0, (int)pool.size() - 1)]);
}

// 猎人开枪：有明确查杀线索就打它；否则 20% 随机开、80% 不开——
// 随机开枪可能带走好人，宁可按兵不动等线索
inline string NpcHunterShot(const NpcContext& ctx, const vector<string>& lines, int self)
{
    static const char* kws[] = { "查杀" };
    vector<int> cand;

    NpcFindNameTargets(ctx, lines, kws, 1, cand);

    vector<int> valid;

    for (size_t j = 0; j < cand.size(); ++j)
    {
        if (cand[j] != self && NpcInTargets(ctx, cand[j])) valid.push_back(cand[j]);
    }

    if (!valid.empty())
    {
        return "NIGHT_SHOOT|" + to_string(valid[NpcRandInt(0, (int)valid.size() - 1)]);
    }

    if (!NpcRandChance(20)) return "NIGHT_SHOOT|-1";

    if (ctx.targets.empty()) return "NIGHT_SHOOT|-1";

    return "NIGHT_SHOOT|" + to_string(ctx.targets[NpcRandInt(0, (int)ctx.targets.size() - 1)]);
}

// 白天投票：证据与聊天线索折算成权重表随机（权重越高越可能被投），
// base 2 保证必有候选、证据 +10/+6 让清狼方向大概率但不必然被投，
// 保留 15% 弃权——全随机或从不弃权都显得不自然
inline string NpcDayVote(const NpcContext& ctx, const vector<string>& lines, int self)
{
    vector<pair<int, int>> w = NpcVoteWeights(ctx, lines, self);

    if (w.empty()) return "VOTE|0";

    if (NpcRandChance(15)) return "VOTE|0";

    return "VOTE|" + to_string(NpcWeightedPick(w));
}

// 白天发言：被 @ 优先回应（自由讨论的接话机制）；预言家有验人记录就报验人
// （优先报验出的狼，否则报最近一次）；有查杀证据直接投票宣言；都没有就
// 权重随机点名怀疑对象（含 base 2 必有候选），首日套首日模板，可带话题词
inline string NpcDaySpeech(const NpcContext& ctx, const vector<string>& lines, int self)
{
    if (!ctx.atTarget.empty()) return "SPEECH|" + NpcAtReply();

    if (NpcRole(ctx) == "seer")
    {
        vector<NpcCheckNote> notes = NpcParseCheckNotes(lines);

        if (!notes.empty())
        {
            size_t k = notes.size() - 1;

            for (size_t i = notes.size(); i-- > 0; )
            {
                if (notes[i].result.find("狼人") != string::npos)
                {
                    k = i;
                    break;
                }
            }

            return "SPEECH|" + NpcSeerReport(to_string(notes[k].slot),
                NpcPlayerName(ctx, notes[k].slot), NpcResultWord(notes[k].result));
        }
    }

    int sus = NpcPickSuspect(ctx, lines, self);

    if (sus != 0)
    {
        return "SPEECH|" + NpcVoteSpeech(NpcPlayerName(ctx, sus));
    }

    // 无明确证据：权重随机点名（与投票同表），无候选退到话题词/通用表水
    int ws = NpcSuspectPick(ctx, lines, self);
    string wname = (ws != 0) ? NpcPlayerName(ctx, ws) : "";
    string topic = NpcMentionTopic(ctx);

    if (NpcDayNumber(ctx) <= 1)
    {
        return "SPEECH|" + NpcFirstDaySpeech(NpcPlayerName(ctx, self), topic);
    }

    return "SPEECH|" + NpcSuspectSpeech(wname, topic);
}

// 遗言：报自己身份（中文职业名）+ 怀疑目标（有证据用证据，没有随机点名）
inline string NpcLastword(const NpcContext& ctx, const vector<string>& lines, int self)
{
    string nm;
    int sus = NpcPickSuspect(ctx, lines, self);

    if (sus == 0) nm = NpcRandomSuspectName(ctx, self);
    else nm = NpcPlayerName(ctx, sus);

    return "SPEECH|" + NpcLastwordSpeech(NpcRoleZh(NpcRole(ctx)), nm);
}

// 离线决策总入口：按阶段分派。返回动作行或 NONE；
// 未识别阶段返回 NONE（防御 Server 未来新增阶段）
inline string NpcOfflineDecide(const NpcContext& ctx)
{
    string phase = NpcNormalizePhase(ctx.phase);
    int self = NpcSelfIndex(ctx);
    vector<string> lines = NpcSplitLines(ctx.history);

    if (phase == "night_check") return NpcNightCheck(ctx, lines, self);
    if (phase == "night_kill") return NpcNightKill(ctx, lines, self);
    if (phase == "night_save") return NpcNightSave(ctx, lines, self);
    if (phase == "night_poison") return NpcNightPoison(ctx, lines, self);
    if (phase == "night_guard") return NpcNightGuard(ctx, lines, self);
    if (phase == "hunter_shot" || phase == "hunter_shoot") return NpcHunterShot(ctx, lines, self);
    if (phase == "day_vote") return NpcDayVote(ctx, lines, self);
    if (phase == "day_speech") return NpcDaySpeech(ctx, lines, self);
    if (phase == "lastword") return NpcLastword(ctx, lines, self);

    return "NONE";
}

// ============ 在线决策（WinHTTP 调 GLM API，失败回退离线） ============

// 一次 HTTP 调用的结果：ok=拿到 200 响应体；retryable=值得重试
// （连接失败/超时/模型繁忙），请求本身被拒（400/401）重试也白费
struct NpcHttpResult
{
    bool ok;
    bool retryable;
    string body;
};

// 从 JSON 文本的冒号位置读取字符串值（处理 \" \\ \n \r \t \uXXXX 转义）。
// 模型回复外层 JSON 会把动作串再包一层转义，必须先解出 content 原文再取 action
inline string NpcJsonReadString(const string& s, size_t colonPos)
{
    size_t q = colonPos + 1;

    while (q < s.size() && (s[q] == ' ' || s[q] == '\t')) ++q;

    if (q >= s.size() || s[q] != '"') return "";

    ++q;

    string val;

    while (q < s.size())
    {
        unsigned char c = (unsigned char)s[q];

        if (c == '"') return val;

        if (c == '\\')
        {
            if (q + 1 >= s.size()) return "";

            char e = s[q + 1];

            if (e == '"') val += '"';
            else if (e == '\\') val += '\\';
            else if (e == 'n') val += '\n';
            else if (e == 'r') val += '\r';
            else if (e == 't') val += '\t';
            else if (e == 'u' && q + 6 < s.size())
            {
                // \uXXXX 转回 UTF-8（中文在 BMP 内，三字节编码足够）
                unsigned int code = 0;
                bool okHex = true;

                for (int i = 0; i < 4; ++i)
                {
                    char h = s[q + 2 + i];

                    if (h >= '0' && h <= '9') code = code * 16 + (unsigned int)(h - '0');
                    else if (h >= 'a' && h <= 'f') code = code * 16 + (unsigned int)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code = code * 16 + (unsigned int)(h - 'A' + 10);
                    else
                    {
                        okHex = false;
                        break;
                    }
                }

                if (okHex && code < 0x80) val += (char)code;
                else if (okHex && code < 0x800)
                {
                    val += (char)(0xC0 | (code >> 6));
                    val += (char)(0x80 | (code & 0x3F));
                }
                else if (okHex)
                {
                    val += (char)(0xE0 | (code >> 12));
                    val += (char)(0x80 | ((code >> 6) & 0x3F));
                    val += (char)(0x80 | (code & 0x3F));
                }

                q += 6;
                continue;
            }
            else val += e;

            q += 2;
            continue;
        }

        val += (char)c;
        ++q;
    }

    return "";
}

// 从模型响应里提取动作行：优先取 content 字段解转义后的正文，
// 再在正文里找 "action" 键；本地假服务器直接返回 {"action":...} 时
// 走正文兜底路径，两种形态都覆盖
inline string NpcExtractAction(const string& resp)
{
    string inner;

    size_t k = resp.find("\"content\"");

    while (k != string::npos)
    {
        size_t colon = resp.find(':', k + 9);

        if (colon != string::npos)
        {
            inner = NpcJsonReadString(resp, colon);

            if (!inner.empty()) break;
        }

        k = resp.find("\"content\"", k + 9);
    }

    if (inner.empty()) inner = resp;

    size_t p = inner.find("\"action\"");

    while (p != string::npos)
    {
        size_t colon = inner.find(':', p + 8);

        if (colon != string::npos)
        {
            string val = NpcJsonReadString(inner, colon);

            if (!val.empty()) return val;
        }

        p = inner.find("\"action\"", p + 8);
    }

    return "";
}

// 同步 POST 一次：拆 URL（host:port/path，默认 https 443 / http 80）、
// 设全套超时（解析/连接/发送/接收同一时限）、200 才读体。异常路径
// 必须把所有句柄关干净，句柄泄漏会让 Server 长跑后句柄耗尽
inline NpcHttpResult NpcHttpOnce(const string& url, const string& reqHeaders,
                                 const string& body, int timeoutSec)
{
    NpcHttpResult r;

    r.ok = false;
    r.retryable = false;

    size_t schemeEnd = url.find("://");

    if (schemeEnd == string::npos) return r;

    string scheme = url.substr(0, schemeEnd);
    string rest = url.substr(schemeEnd + 3);
    bool https = (scheme == "https");
    size_t slash = rest.find('/');
    string hostport = (slash == string::npos) ? rest : rest.substr(0, slash);
    string path = (slash == string::npos) ? "/" : rest.substr(slash);
    size_t colon = hostport.find(':');
    string host = (colon == string::npos) ? hostport : hostport.substr(0, colon);
    int port = https ? 443 : 80;

    if (colon != string::npos) port = atoi(hostport.c_str() + colon + 1);

    if (host.empty() || port <= 0 || port > 65535) return r;

    HINTERNET hSession = WinHttpOpen(L"wolf-npc/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession) return r;

    HINTERNET hConnect = WinHttpConnect(hSession, NpcToWide(host).c_str(),
                                        (INTERNET_PORT)port, 0);

    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return r;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", NpcToWide(path).c_str(),
                                            NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            https ? WINHTTP_FLAG_SECURE : 0);

    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return r;
    }

    DWORD ms = (DWORD)timeoutSec * 1000;

    WinHttpSetTimeouts(hRequest, ms, ms, ms, ms);

    wstring wHeaders = NpcToWide(reqHeaders);

    BOOL sent = WinHttpSendRequest(hRequest, wHeaders.c_str(), (DWORD)wHeaders.size(),
                                   (LPVOID)body.c_str(), (DWORD)body.size(),
                                   (DWORD)body.size(), 0);

    BOOL gotResponse = FALSE;

    if (sent) gotResponse = WinHttpReceiveResponse(hRequest, NULL);

    DWORD status = 0;

    if (gotResponse)
    {
        DWORD sz = sizeof(status);

        if (!WinHttpQueryHeaders(hRequest,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                 WINHTTP_NO_HEADER_INDEX))
        {
            status = 0;
        }
    }

    if (gotResponse && status == 200)
    {
        DWORD avail = 0;
        char buf[8192];

        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0)
        {
            DWORD read = 0;

            if (!WinHttpReadData(hRequest, buf,
                                 avail < (DWORD)sizeof(buf) ? avail : (DWORD)sizeof(buf),
                                 &read) || read == 0)
            {
                break;
            }

            r.body.append(buf, read);
        }

        r.ok = true;
    }
    else if (!sent || !gotResponse)
    {
        // 连不上/超时是网络层失败，值得稍后重试
        r.retryable = true;
    }
    else if (status == 429 || status == 500 || status == 502 || status == 503 || status == 504)
    {
        // 模型繁忙/网关抖动，官方文档建议退避重试
        r.retryable = true;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return r;
}

// 系统提示词：讲清身份、规则、动作行格式与约束（只输出一行 JSON、
// 只能选可选目标）。规则写不完整模型就会乱编动作，这是在线决策的上限所在
inline string NpcBuildSystemPrompt(const NpcContext& ctx)
{
    string s = "你是狼人杀游戏的" + NpcRoleZh(NpcRole(ctx)) + "玩家，正在参与一局狼人杀。";
    s += "游戏规则：夜晚按职业行动——狼人选择一名玩家击杀（NIGHT_KILL）；";
    s += "预言家查验一名玩家的阵营（NIGHT_CHECK，结果为狼人/好人/中立）；";
    s += "女巫可用解药救当夜被狼刀者（NIGHT_SAVE，首夜可自救，-1=不用解药）";
    s += "或用毒药毒杀一名玩家（NIGHT_POISON，-1=不用毒）；";
    s += "守卫每晚守护一名玩家免遭狼刀（NIGHT_GUARD），不可连续两晚守同一人，-1=不守；";
    s += "猎人被放逐或被狼刀死亡时可开枪带走一名玩家（NIGHT_SHOOT，-1=不开枪）。";
    s += "白天存活玩家先发言讨论（SPEECH），再投票放逐一名玩家（VOTE，0=弃权）。";
    s += "白天被其他玩家@点名时，SPEECH 应优先回应对方的问题或观点。";
    s += "输出要求：只输出严格一行 JSON：{\"action\":\"动作行\"}，不要输出任何其他文字。";
    s += "动作行只能是：SPEECH|发言内容（中文）/ VOTE|目标编号 / NIGHT_KILL|目标编号 / ";
    s += "NIGHT_CHECK|目标编号 / NIGHT_SAVE|目标编号 / NIGHT_POISON|目标编号 / ";
    s += "NIGHT_GUARD|目标编号 / NIGHT_SHOOT|目标编号 / NONE（无可行动目标时）。";
    s += "目标编号只能从「可选目标」中选择，不得编造；SPEECH 内容中引用玩家用名字而非编号。";

    return s;
}

// 用户内容：把结构化上下文（身份/阶段/存活/名单/可选目标/历史）渲染成中文文本，
// 模型只依赖这里的信息决策，信息缺了它就只能瞎编
inline string NpcBuildUserText(const NpcContext& ctx)
{
    int self = NpcSelfIndex(ctx);

    string s = "当前信息：\n身份：" + NpcRoleZh(NpcRole(ctx));
    s += "\n名字：" + NpcPlayerName(ctx, self) + "（槽" + to_string(self) + "）";
    s += "\n当前阶段：" + NpcPhaseZh(ctx.phase);
    s += "\n存活玩家：";

    vector<int> alive = NpcAliveList(ctx);

    for (size_t i = 0; i < alive.size(); ++i)
    {
        s += to_string(alive[i]) + "号" + NpcPlayerName(ctx, alive[i]) + "；";
    }

    s += "\n可选目标：";

    for (size_t i = 0; i < ctx.targets.size(); ++i)
    {
        s += to_string(ctx.targets[i]);

        if (i + 1 < ctx.targets.size()) s += "、";
    }

    s += "\n历史信息：\n" + ctx.history;

    // 自由讨论上下文：被 @ 是全天最明确的提问，优先给模型；
    // lastChat 覆盖之后新到的聊天，chatLog 只作摘要兜底
    if (!ctx.atTarget.empty()) s += "\n有人@了你：" + ctx.atTarget;

    if (!ctx.lastChat.empty()) s += "\n你上次发言后的新聊天：\n" + ctx.lastChat;

    if (!ctx.chatLog.empty() && ctx.lastChat.empty())
    {
        s += "\n当天聊天（近 8 条）：";

        size_t from = (ctx.chatLog.size() > 8) ? ctx.chatLog.size() - 8 : 0;

        for (size_t i = from; i < ctx.chatLog.size(); ++i) s += "\n" + ctx.chatLog[i];
    }

    s += "\n请根据以上信息输出你的动作（一行 JSON）。";

    return s;
}

// ============ API key 保护（需求 5.1） ============
// key 来源优先级：env WOLF_NPC_API_KEY（测试注入用）> DPAPI 加密文件
// npc_key.bin（进程工作目录即项目根）> 无 key。env 有 key 时顺手加密落盘，
// 之后无 env 的启动也能读回同一把 key；解析结果 static 缓存只做一次 IO。
// CryptProtectData 默认熵绑定当前用户，文件拷到别的用户/机器读不回，正好当作防泄露层

// 加密 key 落盘：失败忽略（只读目录/权限不足都不能让在线决策崩掉）
inline bool NpcKeySave(const string& key)
{
    DATA_BLOB in;

    in.pbData = (BYTE*)key.data();
    in.cbData = (DWORD)key.size();

    DATA_BLOB out = { NULL, 0 };

    if (!CryptProtectData(&in, L"wolf-npc", NULL, NULL, NULL, 0, &out)) return false;

    HANDLE h = CreateFileA("npc_key.bin", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);

    bool ok = false;

    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;

        ok = WriteFile(h, out.pbData, out.cbData, &written, NULL) && written == out.cbData;

        CloseHandle(h);
    }

    LocalFree(out.pbData);

    return ok;
}

// 读回加密 key：文件不存在/太小/解密失败一律返回空串（走无 key 提示路径）
inline string NpcKeyLoad()
{
    HANDLE h = CreateFileA("npc_key.bin", GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (h == INVALID_HANDLE_VALUE) return "";

    DWORD sz = GetFileSize(h, NULL);
    string buf;

    if (sz > 0 && sz < 4096)
    {
        buf.resize(sz);
        DWORD read = 0;

        if (ReadFile(h, &buf[0], sz, &read, NULL) && read == sz)
        {
            DATA_BLOB in;

            in.pbData = (BYTE*)&buf[0];
            in.cbData = sz;

            DATA_BLOB out = { NULL, 0 };

            if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &out))
            {
                string k((char*)out.pbData, out.cbData);

                LocalFree(out.pbData);
                CloseHandle(h);

                return k;
            }
        }
    }

    CloseHandle(h);

    return "";
}

// key 解析总入口（static 缓存；env > 加密文件 > 空）。返回空串由调用方
// 提示「未配置 AI key，在线 NPC 回退离线决策」并直接走离线逻辑
inline string NpcResolveKey()
{
    static string cached;
    static bool resolved = false;

    if (resolved) return cached;

    resolved = true;

    string k = NpcEnvOr("WOLF_NPC_API_KEY", "");

    if (!k.empty())
    {
        // env 是一次性注入来源，落盘后后续启动不再依赖外部环境
        NpcKeySave(k);
        cached = k;

        return cached;
    }

    cached = NpcKeyLoad();

    return cached;
}

// 是否有可用 key（Server 在调在线决策前先问，避免无 key 白等一轮网络超时）
inline bool NpcKeyAvailable()
{
    return !NpcResolveKey().empty();
}

// 在线决策总入口：读环境变量覆盖（URL/KEY/超时/重试次数），组装请求体，
// 重试退避 2s/4s；任何一步失败都返回空串由调用方回退离线逻辑。
// 阻塞时长上限 = (重试次数+1) × 超时 + 退避，必须同步但不可无限卡死
inline string NpcOnlineDecide(const NpcContext& ctx)
{
    string url = NpcEnvOr("WOLF_NPC_API_URL",
                          "https://open.bigmodel.cn/api/paas/v4/chat/completions");

    // key 不写死在源码：env > DPAPI 文件，都没有就回退离线（调用方已提示）
    string key = NpcResolveKey();

    if (key.empty()) return "";

    int timeoutSec = NpcEnvInt("WOLF_NPC_TIMEOUT_SECONDS", 10, 1, 60);
    int retries = NpcEnvInt("WOLF_NPC_RETRIES", 1, 0, 5);

    string body = "{\"model\":\"glm-4.7-flash\",\"messages\":[{\"role\":\"system\",\"content\":\""
        + NpcJsonEscape(NpcBuildSystemPrompt(ctx)) + "\"},{\"role\":\"user\",\"content\":\""
        + NpcJsonEscape(NpcBuildUserText(ctx))
        + "\"}],\"temperature\":0.7}";
    string headers = "Content-Type: application/json\r\nAuthorization: Bearer " + key;

    for (int attempt = 0; attempt <= retries; ++attempt)
    {
        if (attempt > 0) Sleep((attempt == 1 ? 2 : 4) * 1000);

        NpcHttpResult r = NpcHttpOnce(url, headers, body, timeoutSec);

        if (r.ok)
        {
            return NpcExtractAction(r.body);
        }

        if (!r.retryable) break;
    }

    return "";
}

#endif // WOLF_NPC_BOT_H
