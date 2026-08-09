// common.h - 三个可执行文件共用的基础工具（日志、控制台、网络行缓冲、输入清理）
//
// 注意：本工程由人工维护，改动前请先阅读对应 .cpp 文件头的设计说明。
#ifndef DEMON_COMMON_H
#define DEMON_COMMON_H

// _WIN32_WINNT 必须位于任何 Windows 头之前
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
#include <conio.h>
#include <signal.h>

using namespace std;

#pragma comment(lib, "ws2_32.lib")

// ============ 控制台工具 ============

// 关闭控制台"快速编辑"模式（防止鼠标框选阻塞程序）。
// 关键：ENABLE_QUICK_EDIT_MODE 只有在同时设置了 ENABLE_EXTENDED_FLAGS 时
// 才会被 SetConsoleMode 真正改变——不设该标志，系统会静默忽略这次修改，
// 快速编辑依然生效（鼠标框选/选择模式会冻结控制台输入，表现为"输入后无反应"）。
// 三个可执行文件的 main() 都在启动时调用本函数（共享同一个控制台的
// Start/Server 只需 Start 调用一次）。
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
// 不设置的话，在 GBK(936) 控制台上中文会显示为乱码。
void SetConsoleUtf8()
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

// 设置控制台字体（保证中文显示正常）。
// Consolas 没有中文字形，中文会显示为方块；优先切到等宽的"新宋体"，
// 若系统没有该字体（非中文系统）则校验失败并恢复原字体。
void SetConsoleFont()
{
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_FONT_INFOEX fontInfo;
    CONSOLE_FONT_INFOEX check;

    fontInfo.cbSize = sizeof(CONSOLE_FONT_INFOEX);
    GetCurrentConsoleFontEx(hConsole, FALSE, &fontInfo);

    CONSOLE_FONT_INFOEX original = fontInfo;

    // 只改字体名与字号，其余属性沿用当前设置
    wcscpy_s(fontInfo.FaceName, L"NSimSun");
    SetCurrentConsoleFontEx(hConsole, FALSE, &fontInfo);

    // 校验字体是否真的生效（字体不存在时 conhost 会静默回退）
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

// 清屏：直接用控制台 API 填充空白，不派生子进程。
// 旧实现 system("cls") 会拉起 cmd.exe，在自动化注入/多控制台场景下
// cmd.exe 可能因控制台输出状态问题长时间阻塞（游戏主线程卡死），
// 且每次清屏都多一次进程创建开销（2026-08-02）。
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

// 向 logFile 追加一行带时间戳的日志，返回格式化后的内容（供控制台同步输出）
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

// 清理玩家名：去掉引号/竖线/换行/控制字符，限长 20，空名给默认值。
// 目的：防止名字注入 CreateProcessW 命令行（含 "）或伪造协议字段（| 换行）。
string SanitizeName(string name)
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
    if (out.length() > 20) out = out.substr(0, 20);

    // 修正 20 字节截断切在多字节 UTF-8 字符中间的情况：剥离不完整的尾部序列。
    // 注意：完整汉字以续字节（0x80-0xBF）结尾，不能像旧代码那样无条件删续字节，
    // 否则任何以汉字结尾的名字（如"张三丰"）最后一位都会变成乱码。
    while (!out.empty())
    {
        size_t n = out.size();
        unsigned char last = (unsigned char)out[n - 1];

        // 纯 ASCII 结尾：完整，无需处理
        if (last < 0x80) break;

        // 前导字节直接结尾（截断切在字符开头）：删掉
        if (last >= 0xC0)
        {
            out.pop_back();
            continue;
        }

        // 以续字节结尾：向前找到序列起始，判断续字节数量是否完整
        size_t start = n;
        size_t cont = 0;

        while (start > 0 && (unsigned char)out[start - 1] >= 0x80 && (unsigned char)out[start - 1] <= 0xBF)
        {
            --start;
            ++cont;
        }

        // 全是续字节（异常数据）：清空
        if (start == 0)
        {
            out.clear();
            break;
        }

        unsigned char lead = (unsigned char)out[start - 1];
        size_t needed;

        if (lead >= 0xF0) needed = 4;       // 4 字节字符
        else if (lead >= 0xE0) needed = 3;  // 3 字节字符（汉字）
        else if (lead >= 0xC0) needed = 2;  // 2 字节字符
        else break;                          // 前导非法（前面是 ASCII 或异常）：保留

        if (cont + 1 == needed) break;       // 续字节数量完整 → 序列完整

        // 不完整：删除整个不完整序列（含前导字节）
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

// 从套接字读取数据并按行切分（兼容 \r\n），每行交给 handler。
// 返回 false 表示连接已断开/出错；buffer 保留半行数据供下次拼接。
//
// 安全：单行最大长度限制 16KB，防止恶意端发无换行数据导致 buffer 无限膨胀
// （内存耗尽 DoS）。
//
// 注意：必须先处理缓冲中已有的完整行，再阻塞 recv。否则对端把多行一次
// 发完并等待响应时（回房 REJOIN：HELLO/NAME/GAME_ENDED/REJOIN 合并在一个
// 包；Server.exe 的通知连接）会先卡死在 recv，缓冲里的命令永远不被处理
// ——"回房死锁"根因（2026-08-02 修复）。
bool ReceiveLines(SOCKET sock, string& buffer, const function<void(const string&)>& handler)
{
    // 缓冲中已有完整行 → 先消化，不碰 recv（修复回房死锁）
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
        return false; // 超长行 → 视作连接异常，断开
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

#endif // DEMON_COMMON_H
