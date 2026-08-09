// Start.cpp - 房间管理器（大厅服务器，监听 8888）
//
// 职责：
//   - 维护房间列表（按玩家自选端口建房）；
//   - 玩家 READY 后启动倒计时并 spawn 每局一个的 Server.exe；
//   - 处理 GAME_ENDED（游戏结束，房间恢复等待状态）与 RELEASE（销毁房间）；
//   - 处理 REJOIN（游戏结束后玩家自动回到原房间，含房主顶替）。
//
// 房间生命周期：
//   CREATE/JOIN 建房 → READY×2 → StartGameServer → 游戏期间玩家的大厅连接关闭
//   （房间保留，槽位悬空）→ 游戏结束 GAME_ENDED → 玩家 REJOIN 回房
//   → 重新 READY 再开一局；若双方都失联，服务器发 RELEASE 销毁房间。
//
// 每个客户端连接由一个 HandleClient 线程服务。
// 首行 HELLO|START 握手带 30 秒超时（半包拼接），防止死连接永久占用线程。
#include "common.h"

void Log(const string& msg)
{
    string s = LogMsg("start.log", msg);
    cout << s << endl;
}

// ============ 数据结构 ============

struct Room
{
    string roomId;            // 6 位大写随机房间号
    string port;              // 游戏端口（也是玩家建房时选择的端口）
    SOCKET client1;           // 玩家1（房主）
    SOCKET client2;           // 玩家2
    bool ready1;
    bool ready2;
    string name1;
    string name2;
    bool gameStarted;         // 游戏已开始
    bool gameStarting;        // 倒计时中
    bool portReserved;        // 端口已预留给本局
    bool gameEnded;           // 游戏已结束，等待玩家回房
    time_t client2BanTime;    // 房主 PICK 踢人后的临时禁入时间
    string client1Ip;
    string client2Ip;
    string gameServerIp;      // 游戏服务器地址（本地固定 127.0.0.1）

    Room()
        : client1(INVALID_SOCKET), client2(INVALID_SOCKET),
          ready1(false), ready2(false), gameStarted(false),
          gameStarting(false), portReserved(false), gameEnded(false),
          client2BanTime(0), gameServerIp("127.0.0.1") {}
};

map<string, shared_ptr<Room>> g_rooms;
mutex g_roomsMutex;
SOCKET g_listenSock = INVALID_SOCKET;
bool g_running = true;

struct ClientInfo
{
    SOCKET sock;
    string roomId;
    int playerId;
    bool inRoom;
    string name;
    string ip;
    bool isAdmin;
    bool inGame;
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

void SendToClient(SOCKET sock, const string& msg)
{
    if (sock == INVALID_SOCKET) return;

    string out = msg + "\n";

    int total = 0;

    while (total < (int)out.length())
    {
        int sent = send(sock, out.c_str() + total, (int)out.length() - total, 0);

        if (sent <= 0)
        {
            // 发送失败（对端不读/断线）→ 关闭连接，让 HandleClient 的
            // recv 循环感知断线并走清理流程（移除房间、释放客户端状态）。
            // 配合 HandleClient 里的 SO_SNDTIMEO：阻塞 send 超过 5 秒会以
            // WSAETIMEDOUT 失败，不会永久卡住调用方（调用方有时持锁）
            closesocket(sock);
            return;
        }

        total += sent;
    }
}

void SendToRoomUnsafe(const string& roomId, const string& msg, SOCKET exclude = INVALID_SOCKET)
{
    auto it = g_rooms.find(roomId);

    if (it == g_rooms.end()) return;

    auto& room = *it->second;

    if (room.client1 != INVALID_SOCKET && room.client1 != exclude) SendToClient(room.client1, msg);
    if (room.client2 != INVALID_SOCKET && room.client2 != exclude) SendToClient(room.client2, msg);
}

void UpdateClientAdmin(SOCKET sock, int playerId)
{
    lock_guard<mutex> lock(g_clientsMutex);
    auto it = g_clients.find(sock);

    if (it != g_clients.end())
    {
        it->second.playerId = playerId;
        it->second.isAdmin = (playerId == 1);
    }
}

// 把玩家从房间移除（玩家主动离开、被踢或断线）。
// 房主离开且游戏未开始时，把 2 号位提升为新房主。
// 房间空了且游戏未进行时销毁房间。
void RemovePlayerFromRoom(const string& roomId, int playerId, bool isAdmin)
{
    lock_guard<mutex> lock(g_roomsMutex);
    auto it = g_rooms.find(roomId);

    if (it == g_rooms.end()) return;

    auto& room = *it->second;
    bool inGame = room.gameStarted || room.gameStarting;

    // 倒计时进行中（gameStarting && !gameStarted）玩家离开 → 取消倒计时，
    // 否则房间要等 countdown 线程结束才恢复可用，且双方都退出时会短暂锁死端口。
    if (room.gameStarting && !room.gameStarted)
    {
        room.gameStarting = false;
        room.portReserved = false;
        inGame = false;
    }

    if (playerId == 1)
    {
        room.client1 = INVALID_SOCKET;
        room.ready1 = false;

        if (!inGame)
        {
            room.name1 = "";
            room.client1Ip = "";
        }
    }
    else
    {
        room.client2 = INVALID_SOCKET;
        room.ready2 = false;

        if (!inGame)
        {
            room.name2 = "";
            room.client2Ip = "";
        }
    }

    // 房主离开且房间未开游戏 → 2 号位顶替为房主
    if (!inGame && isAdmin && room.client2 != INVALID_SOCKET)
    {
        SOCKET newAdminSock = room.client2;

        room.client1 = room.client2;
        room.client2 = INVALID_SOCKET;
        room.name1 = room.name2;
        room.name2 = "";
        room.ready1 = room.ready2;
        room.ready2 = false;
        room.client1Ip = room.client2Ip;
        room.client2Ip = "";

        UpdateClientAdmin(newAdminSock, 1);
        SendToClient(newAdminSock, "ADMIN|You are now the room admin");
        SendToClient(newAdminSock, "ROOM_EVENT|" + room.name1 + " is now the admin.");
    }

    // 房间空了且游戏未进行 → 销毁
    if (!inGame && room.client1 == INVALID_SOCKET && room.client2 == INVALID_SOCKET)
    {
        g_rooms.erase(it);
        Log("Room " + roomId + " destroyed (empty)");
    }
}

// ============ 启动游戏服务器 ============

// 先 spawn Server.exe，成功后再发送 GAME_PREPARE 给双方（见函数体内的说明）。
// Server.exe 由本进程创建，参数：<gamePort> "<name1>" "<name2>" <startIp> <startPort> <roomId>。
// 必须在发送 GAME_PREPARE 之前就把 gameStarted 置为 true：客户端收到
// GAME_PREPARE 会立即关闭大厅连接，若此时房间仍是 gameStarted=false，
// RemovePlayerFromRoom 会判定"倒计时中退出"并销毁空房间（房间丢失 BUG）。
void StartGameServer(shared_ptr<Room> roomPtr)
{
    try
    {
        if (!roomPtr) return;

        auto& room = *roomPtr;

        // 从房间拷贝启动所需的信息（避免在锁外访问房间字段）
        bool canStart = false;
        string port;
        string roomId;
        string gameServerIp;
        string name1;
        string name2;
        SOCKET client1 = INVALID_SOCKET;
        SOCKET client2 = INVALID_SOCKET;

        {
            lock_guard<mutex> lock(g_roomsMutex);

            canStart = (room.client1 != INVALID_SOCKET && room.client2 != INVALID_SOCKET
                && room.ready1 && room.ready2 && !room.gameStarted);

            if (!canStart)
            {
                room.gameStarting = false;
                return;
            }

            room.portReserved = true;
            room.gameStarting = false;
            room.gameStarted = true;
            room.gameEnded = false;

            port = room.port;
            roomId = room.roomId;
            gameServerIp = room.gameServerIp;
            name1 = room.name1;
            name2 = room.name2;
            client1 = room.client1;
            client2 = room.client2;
        }

        string msg1_1 = "GAME_PREPARE|" + port + "|" + roomId + "|" + gameServerIp + "|1";
        string msg1_2 = "GAME_PREPARE|" + port + "|" + roomId + "|" + gameServerIp + "|2";

        // 先 spawn Server.exe，成功后再发送 GAME_PREPARE：
        // 旧代码先发通知、sleep 500ms 才 spawn，客户端收到 GAME_PREPARE
        // 立即去连游戏端口，连上的是"还没开始监听"的服务器 → 连接失败、
        // 客户端直接回房。现在 spawn 优先 + 客户端侧连接重试兜底（2026-08-03）。
        string startIp = "127.0.0.1";
        int startPort = 8888;
        string cmdLineUtf8 = "Server.exe " + port + " \"" + name1 + "\" \"" + name2 + "\" "
                           + startIp + " " + to_string(startPort) + " " + roomId;

        Log("Starting game server: " + cmdLineUtf8);

        // Convert UTF-8 cmdLine to wide for CreateProcessW (avoids ANSI codepage corruption for CJK names)
        auto Utf8ToWide = [](const string& s) -> wstring
        {
            if (s.empty()) return L"";
            int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
            if (len <= 0) return L"";
            wstring w(len, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], len);
            return w;
        };

        wstring cmdLineWide = Utf8ToWide(cmdLineUtf8);

        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = { 0 };

        if (CreateProcessW(NULL, &cmdLineWide[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            Log("Game server process created successfully");
        }
        else
        {
            // 启动失败：恢复房间状态，让玩家可以重试
            Log("Failed to create game server process, error: " + to_string(GetLastError()));

            {
                lock_guard<mutex> lock(g_roomsMutex);

                room.gameStarted = false;
                room.gameStarting = false;
                room.portReserved = false;
                room.ready1 = false;
                room.ready2 = false;
            }

            SendToClient(client1, "ROOM_EVENT|Failed to start the game server, please try again.");
            SendToClient(client2, "ROOM_EVENT|Failed to start the game server, please try again.");
            return;
        }

        // 服务器进程已创建（监听就绪还需一瞬间，客户端带 5 次重试兜底）
        SendToClient(client1, msg1_1);
        SendToClient(client2, msg1_2);

        Log("Game started in room " + roomId + " on port " + port);
    }
    catch (const exception& e)
    {
        Log(string("Exception in StartGameServer: ") + e.what());
    }
}

// ============ 客户端连接处理 ============

void HandleClient(SOCKET clientSock)
{
    try
    {
        // 首行必须是 HELLO|START（Server.exe 通知也会带这一行）。
        // 不用裸 recv：死连接/只连接不发数据的客户端会让 recv 无限阻塞、
        // 永久占用一个线程。改为带超时的半包拼接（30 秒无数据即断开）。
        string helloData;
        char helloBuf[1024];

        while (helloData.find('\n') == string::npos)
        {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(clientSock, &readSet);
            timeval tv = { 30, 0 };

            if (select(0, &readSet, NULL, NULL, &tv) <= 0)
            {
                closesocket(clientSock);
                return;
            }

            int hr = recv(clientSock, helloBuf, sizeof(helloBuf) - 1, 0);

            if (hr <= 0)
            {
                closesocket(clientSock);
                return;
            }

            helloBuf[hr] = '\0';
            helloData += helloBuf;

            // 防御：握手行不可能这么长（HELLO|START），超限直接断开
            if (helloData.size() > 4096)
            {
                closesocket(clientSock);
                return;
            }
        }

        size_t newlinePos = helloData.find('\n');
        string firstLine = helloData.substr(0, newlinePos);
        string buffer = helloData.substr(newlinePos + 1);

        if (firstLine.find("HELLO|START") != 0)
        {
            closesocket(clientSock);
            return;
        }

        ClientInfo info;
        info.sock = clientSock;
        info.inRoom = false;
        info.playerId = 0;
        info.isAdmin = false;
        info.ip = GetClientIp(clientSock);
        info.name = "Player";
        info.inGame = false;

        {
            lock_guard<mutex> lock(g_clientsMutex);
            g_clients[clientSock] = info;
        }

        SendToClient(clientSock, "WELCOME|Demon Roulette Room Manager");

        // 发送超时：对端停止读取（TCP 接收窗口满）时阻塞 send 会无限等待，
        // 而 SendToClient 的调用方有时持锁（g_roomsMutex/g_clientsMutex），
        // 一个不读数据的客户端会把整个房间管理器拖住（2026-08-03 修复）。
        int sndTimeout = 5000;
        setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));

        // 是否继续服务该连接（EXIT 且不在房间时退出循环）
        bool keepAlive = true;

        // 单条命令处理（返回 false 表示应断开该连接）
        auto handleLine = [&](const string& line) -> bool
        {
            // ---- LIST：列出可加入的房间 ----
            if (line == "LIST")
            {
                lock_guard<mutex> lock(g_roomsMutex);

                string msg = "ROOMS_LIST";

                for (auto& pair : g_rooms)
                {
                    auto& room = *pair.second;

                    // 游戏中的房间：显示但标记 [2/2] [in-game]。双方玩家的大厅连接
                    // 已在 GAME_PREPARE 后关闭，槽位看似为空；游戏一旦开始就恒为
                    // 满员 2/2，不可加入（2026-08-03 修复：旧代码按 socket 数显示
                    // 1/2，误导玩家以为房间里只有一个对手）。
                    if (room.gameStarted)
                    {
                        msg += "|" + room.port + "\t2/2 [in-game]";
                        continue;
                    }

                    // 倒计时中/端口预留的房间不可见；双方都已离线（无人可回）的房间隐藏
                    if (room.gameStarting || room.portReserved) continue;
                    if (room.client1 == INVALID_SOCKET && room.client2 == INVALID_SOCKET) continue;

                    int players = (room.client2 != INVALID_SOCKET) ? 2 : 1;
                    msg += "|" + room.port + "\t" + to_string(players) + "/2";
                }

                if (msg == "ROOMS_LIST") msg = "ROOMS_LIST|EMPTY";

                SendToClient(clientSock, msg);
                return true;
            }

            // ---- GAME_ENDED <roomId>：游戏结束，房间恢复等待状态 ----
            // 发送方可能是 Server.exe（正常结算）或客户端（回房时顺带通知）。
            // 支持 GAME_ENDED|<roomId> 和 GAME_ENDED <roomId> 两种格式（向后兼容）。
            if (line.find("GAME_ENDED") == 0)
            {
                string roomId;
                size_t pipePos = line.find('|');
                if (pipePos != string::npos)
                    roomId = line.substr(pipePos + 1);
                else
                {
                    istringstream iss(line);
                    string cmd;
                    iss >> cmd >> roomId;
                }

                if (roomId.empty()) return true;

                lock_guard<mutex> lock(g_roomsMutex);
                auto rit = g_rooms.find(roomId);

                if (rit != g_rooms.end())
                {
                    auto& room = *rit->second;

                    room.gameStarted = false;
                    room.gameStarting = false;
                    room.gameEnded = true;
                    room.portReserved = false;
                    room.ready1 = false;
                    room.ready2 = false;

                    if (room.client1 != INVALID_SOCKET) SendToClient(room.client1, "GAME_ENDED|" + roomId);
                    if (room.client2 != INVALID_SOCKET) SendToClient(room.client2, "GAME_ENDED|" + roomId);

                    Log("Game ended in room " + roomId);
                }

                return true;
            }

            // ---- RELEASE <roomId>：销毁房间（双方玩家都失联时由 Server.exe 发送） ----
            // 支持 RELEASE|<roomId> 和 RELEASE <roomId> 两种格式（向后兼容）。
            if (line.find("RELEASE") == 0)
            {
                string roomId;
                size_t pipePos = line.find('|');
                if (pipePos != string::npos)
                    roomId = line.substr(pipePos + 1);
                else
                {
                    istringstream iss(line);
                    string cmd;
                    iss >> cmd >> roomId;
                }

                if (roomId.empty()) return true;

                lock_guard<mutex> lock(g_roomsMutex);
                auto rit = g_rooms.find(roomId);

                if (rit != g_rooms.end())
                {
                    Log("Room " + roomId + " released (both players lost)");
                    g_rooms.erase(rit);
                }

                return true;
            }

            // ---- REJOIN <roomId> <playerId>：游戏结束后玩家回到原房间 ----
            // playerId 是玩家在本局中的编号（1/2），用于尽量保留原座位/房主身份。
            // 支持 REJOIN|<roomId>|<playerId> 和 REJOIN <roomId> <playerId> 两种格式（向后兼容）。
            if (line.find("REJOIN") == 0)
            {
                string roomId, pidStr;
                size_t firstPipe = line.find('|');
                if (firstPipe != string::npos)
                {
                    size_t secondPipe = line.find('|', firstPipe + 1);
                    if (secondPipe != string::npos)
                    {
                        roomId = line.substr(firstPipe + 1, secondPipe - firstPipe - 1);
                        pidStr = line.substr(secondPipe + 1);
                    }
                }
                else
                {
                    istringstream iss(line);
                    string cmd;
                    iss >> cmd >> roomId >> pidStr;
                }

                if (roomId.empty())
                {
                    SendToClient(clientSock, "REJOIN_FAIL|Missing room ID");
                    return true;
                }

                int pid = atoi(pidStr.c_str());
                if (pid != 1 && pid != 2) pid = 0; // 未指定 → 自动分配

                lock_guard<mutex> lock(g_roomsMutex);

                auto rit = g_rooms.find(roomId);

                if (rit == g_rooms.end())
                {
                    SendToClient(clientSock, "REJOIN_FAIL|Room no longer exists (may have been released)");
                    return true;
                }

                auto& room = *rit->second;

                // 已经在该房间（重复 REJOIN）→ 直接确认
                if (room.client1 == clientSock || room.client2 == clientSock)
                {
                    bool isAdmin = (room.client1 == clientSock);

                    SendToClient(clientSock, "JOINED|" + roomId);
                    if (isAdmin) SendToClient(clientSock, "ADMIN|You are the room admin");
                    return true;
                }

                if (room.gameStarted || room.gameStarting)
                {
                    SendToClient(clientSock, "REJOIN_FAIL|Game still in progress");
                    return true;
                }

                // 分配座位：优先请求的座位，其次空位；1 号位空着 → 顶替为房主
                SOCKET* target = nullptr;
                int targetPid = 0;
                bool becomesAdmin = false;

                if (pid == 1 && room.client1 == INVALID_SOCKET)
                {
                    target = &room.client1;
                    targetPid = 1;
                    becomesAdmin = true;
                }
                else if (pid == 2 && room.client2 == INVALID_SOCKET)
                {
                    target = &room.client2;
                    targetPid = 2;
                }
                else if (room.client1 == INVALID_SOCKET)
                {
                    target = &room.client1;
                    targetPid = 1;
                    becomesAdmin = true;
                }
                else if (room.client2 == INVALID_SOCKET)
                {
                    target = &room.client2;
                    targetPid = 2;
                }
                else
                {
                    SendToClient(clientSock, "REJOIN_FAIL|Room is full");
                    return true;
                }

                *target = clientSock;

                if (targetPid == 1) room.client1Ip = info.ip;
                else room.client2Ip = info.ip;

                // 回到房间 → 房间重新开放等待状态
                room.gameEnded = false;
                room.gameStarted = false;
                room.gameStarting = false;
                room.portReserved = false;
                room.ready1 = false;
                room.ready2 = false;

                // 用客户端当前名字填充座位（空则沿用房间里的旧名字）
                string curName;

                {
                    lock_guard<mutex> lk(g_clientsMutex);
                    auto it = g_clients.find(clientSock);
                    if (it != g_clients.end()) curName = it->second.name;
                }

                if (targetPid == 1)
                {
                    if (!curName.empty()) room.name1 = curName;
                }
                else
                {
                    if (!curName.empty()) room.name2 = curName;
                }

                {
                    lock_guard<mutex> lk(g_clientsMutex);
                    auto it = g_clients.find(clientSock);

                    if (it != g_clients.end())
                    {
                        it->second.roomId = roomId;
                        it->second.inRoom = true;
                        it->second.playerId = targetPid;
                        it->second.isAdmin = becomesAdmin;
                    }
                }

                info.inRoom = true;
                info.roomId = roomId;
                info.playerId = targetPid;
                info.isAdmin = becomesAdmin;

                SendToClient(clientSock, "JOINED|" + roomId);

                if (becomesAdmin)
                {
                    SendToClient(clientSock, "ADMIN|You are the room admin");
                }

                string backName = (targetPid == 1) ? room.name1 : room.name2;
                SendToRoomUnsafe(roomId, "ROOM_EVENT|" + backName + " returned to the room.");

                Log("Player " + info.ip + " rejoined room " + roomId + " as Player " + to_string(targetPid));
                return true;
            }

            // ---- EXIT：离开房间或退出大厅 ----
            if (line == "EXIT")
            {
                string roomId;
                int playerId = 0;
                bool isAdmin = false;
                bool wasInRoom = false;

                {
                    lock_guard<mutex> lock(g_clientsMutex);
                    auto it = g_clients.find(clientSock);

                    if (it != g_clients.end() && it->second.inRoom)
                    {
                        roomId = it->second.roomId;
                        playerId = it->second.playerId;
                        isAdmin = it->second.isAdmin;
                        wasInRoom = true;
                    }
                }

                if (wasInRoom)
                {
                    string name = info.name;

                    if (!roomId.empty()) RemovePlayerFromRoom(roomId, playerId, isAdmin);

                    SendToRoomUnsafe(roomId, "ROOM_EVENT|" + name + " left the room.");

                    {
                        lock_guard<mutex> lock(g_clientsMutex);
                        auto it = g_clients.find(clientSock);

                        if (it != g_clients.end())
                        {
                            it->second.inRoom = false;
                            it->second.roomId = "";
                            it->second.playerId = 0;
                            it->second.isAdmin = false;
                        }
                    }

                    SendToClient(clientSock, "LEFT_ROOM|You left the room.");
                }
                else
                {
                    // 不在任何房间 → 断开连接
                    return false;
                }

                return true;
            }

            // ---- STATUS：查看当前房间的准备状态 ----
            if (line == "STATUS")
            {
                string roomId;

                {
                    lock_guard<mutex> lock(g_clientsMutex);
                    auto it = g_clients.find(clientSock);

                    if (it == g_clients.end() || !it->second.inRoom)
                    {
                        SendToClient(clientSock, "ERROR:Not in a room");
                        return true;
                    }

                    roomId = it->second.roomId;
                }

                lock_guard<mutex> lock(g_roomsMutex);
                auto rit = g_rooms.find(roomId);

                if (rit == g_rooms.end()) return true;

                auto& room = *rit->second;

                SendToClient(clientSock, "ROOM_STATUS|" + room.name1 + "|" + (room.ready1 ? "1" : "0")
                    + "|" + room.name2 + "|" + (room.ready2 ? "1" : "0"));
                return true;
            }

            // ---- READY：准备（双方就绪后开始倒计时并启动游戏） ----
            if (line == "READY")
            {
                string roomId;
                int playerId;

                {
                    lock_guard<mutex> lock(g_clientsMutex);
                    auto it = g_clients.find(clientSock);

                    if (it == g_clients.end() || !it->second.inRoom)
                    {
                        SendToClient(clientSock, "ERROR:Not in a room");
                        return true;
                    }

                    roomId = it->second.roomId;
                    playerId = it->second.playerId;
                }

                lock_guard<mutex> lock(g_roomsMutex);
                auto rit = g_rooms.find(roomId);

                if (rit == g_rooms.end())
                {
                    SendToClient(clientSock, "ERROR:Room not found");
                    return true;
                }

                auto& room = *rit->second;

                if (room.gameStarted || room.gameStarting)
                {
                    SendToClient(clientSock, "ERROR:Game already in progress");
                    return true;
                }

                if (playerId == 1)
                {
                    room.ready1 = !room.ready1;
                    SendToClient(clientSock, "READY_STATUS|" + to_string(room.ready1));
                }
                else
                {
                    room.ready2 = !room.ready2;
                    SendToClient(clientSock, "READY_STATUS|" + to_string(room.ready2));
                }

                string name = (playerId == 1) ? room.name1 : room.name2;
                bool nowReady = (playerId == 1) ? room.ready1 : room.ready2;
                string event = name + (nowReady ? " is ready." : " is not ready.");
                SendToRoomUnsafe(roomId, "ROOM_EVENT|" + event);

                // 观测点：记录准备状态（自动化测试据此确认双方就绪）
                Log("Player " + name + " READY in room " + roomId + " now "
                    + (nowReady ? "ready" : "not-ready")
                    + " (" + to_string(room.ready1) + "/" + to_string(room.ready2) + ")");

                // 双方都就绪 → 3 秒倒计时后启动游戏
                if (room.ready1 && room.ready2 && !room.gameStarting)
                {
                    room.gameStarting = true;
                    shared_ptr<Room> roomPtr = rit->second;

                    thread countdown([roomId, roomPtr]()
                    {
                        auto& rm = *roomPtr;

                        {
                            lock_guard<mutex> lock(g_roomsMutex);
                            SendToRoomUnsafe(roomId, "ROOM_EVENT|All players ready. Game starts in 3 seconds.");
                            SendToRoomUnsafe(roomId, "ROOM_EVENT|");
                        }

                        for (int i = 2; i >= 1; --i)
                        {
                            this_thread::sleep_for(chrono::seconds(1));

                            bool starting;

                            {
                                lock_guard<mutex> lock(g_roomsMutex);
                                starting = rm.gameStarting;
                            }

                            if (!starting) return;

                            {
                                lock_guard<mutex> lock(g_roomsMutex);
                                SendToRoomUnsafe(roomId, "ROOM_EVENT|Game starts in " + to_string(i)
                                    + " second" + (i > 1 ? "s" : "") + ".");
                            }
                        }

                        this_thread::sleep_for(chrono::seconds(1));

                        {
                            lock_guard<mutex> lock(g_roomsMutex);

                            if (!rm.gameStarting) return;

                            SendToRoomUnsafe(roomId, "ROOM_EVENT|Game started.");
                        }

                        StartGameServer(roomPtr);
                    });

                    countdown.detach();
                }

                return true;
            }

            // ---- PICK：房主踢人（仅 2 号位可被踢，踢后 10 秒内禁入） ----
            if (line == "PICK")
            {
                string roomId;

                {
                    lock_guard<mutex> lock(g_clientsMutex);
                    auto it = g_clients.find(clientSock);

                    if (it == g_clients.end() || !it->second.inRoom || !it->second.isAdmin)
                    {
                        SendToClient(clientSock, "ERROR:Only admin can kick");
                        return true;
                    }

                    roomId = it->second.roomId;
                }

                lock_guard<mutex> lock(g_roomsMutex);
                auto rit = g_rooms.find(roomId);

                if (rit == g_rooms.end()) return true;

                auto& room = *rit->second;

                if (room.client2 == INVALID_SOCKET)
                {
                    SendToClient(clientSock, "ERROR:No player 2 to kick");
                    return true;
                }

                if (room.gameStarted || room.gameStarting)
                {
                    SendToClient(clientSock, "ERROR:Game in progress");
                    return true;
                }

                SOCKET kickedSock = room.client2;
                room.client2BanTime = time(nullptr) + 10;

                SendToClient(kickedSock, "KICKED|You have been kicked by admin");

                // 直接内联移除玩家2（本函数已持有 g_roomsMutex，不能调用
                // RemovePlayerFromRoom 再次加锁，否则自死锁）
                room.client2 = INVALID_SOCKET;
                room.ready2 = false;
                room.name2 = "";
                room.client2Ip = "";

                closesocket(kickedSock);

                {
                    lock_guard<mutex> lk(g_clientsMutex);
                    auto cit = g_clients.find(kickedSock);

                    if (cit != g_clients.end())
                    {
                        cit->second.inRoom = false;
                        cit->second.roomId = "";
                        cit->second.playerId = 0;
                    }
                }

                SendToClient(clientSock, "KICK_SUCCESS|Player 2 kicked");
                return true;
            }

            // ---- CREATE <port>：建房（成为房主/玩家1） ----
            if (line.find("CREATE") == 0)
            {
                istringstream iss(line);
                string cmd;
                string port;
                iss >> cmd >> port;

                if (port.empty())
                {
                    SendToClient(clientSock, "ERROR:Port required");
                    return true;
                }

                if (port.front() == '<' && port.back() == '>')
                {
                    port = port.substr(1, port.length() - 2);
                }

                if (!IsValidPort(port))
                {
                    SendToClient(clientSock, "ERROR:Invalid port (1024-65535)");
                    return true;
                }

                {
                    lock_guard<mutex> lock(g_clientsMutex);

                    if (g_clients[clientSock].inRoom)
                    {
                        SendToClient(clientSock, "ERROR:Already in a room");
                        return true;
                    }
                }

                lock_guard<mutex> lock(g_roomsMutex);

                // 端口不能与任何现有房间重复（含游戏中/已结束的房间）：
                // 旧代码跳过 gameStarted 的房间，游戏结束后的房间可被重复建房，
                // 两个房间共用同一端口 → 后启动的 Server.exe bind 失败(10048)，
                // 玩家连上的还是别人的服务器（2026-08-03 修复）。
                bool portUsed = false;

                for (auto& p : g_rooms)
                {
                    if (p.second->port == port)
                    {
                        portUsed = true;
                        break;
                    }
                }

                if (portUsed)
                {
                    SendToClient(clientSock, "ERROR:Port already in use");
                    return true;
                }

                // 生成不重复的房间号
                string roomId;

                do
                {
                    roomId.clear();
                    for (int i = 0; i < 6; ++i) roomId += 'A' + (rand() % 26);
                } while (g_rooms.find(roomId) != g_rooms.end());

                auto room = make_shared<Room>();
                room->roomId = roomId;
                room->port = port;
                room->client1 = clientSock;
                room->client2 = INVALID_SOCKET;
                room->ready1 = false;
                room->ready2 = false;
                room->gameStarted = false;
                room->gameStarting = false;
                room->portReserved = false;
                room->gameEnded = false;
                room->client1Ip = info.ip;
                room->gameServerIp = "127.0.0.1";

                string currentName;

                {
                    lock_guard<mutex> lk(g_clientsMutex);
                    auto it = g_clients.find(clientSock);
                    if (it != g_clients.end()) currentName = it->second.name;
                }

                room->name1 = currentName;
                g_rooms[roomId] = room;

                {
                    lock_guard<mutex> lk(g_clientsMutex);
                    g_clients[clientSock].roomId = roomId;
                    g_clients[clientSock].inRoom = true;
                    g_clients[clientSock].playerId = 1;
                    g_clients[clientSock].isAdmin = true;
                }

                info.inRoom = true;
                info.roomId = roomId;
                info.playerId = 1;
                info.isAdmin = true;

                SendToClient(clientSock, "CREATED|" + roomId + "|" + port);
                SendToClient(clientSock, "ADMIN|You are the room admin");
                SendToClient(clientSock, "ROOM_EVENT|" + currentName + " created the room.");
                Log("Room " + roomId + " created on port " + port + " by " + info.ip);
                return true;
            }

            // ---- JOIN <port>：加入房间（成为玩家2） ----
            if (line.find("JOIN") == 0)
            {
                istringstream iss(line);
                string cmd;
                string port;
                iss >> cmd >> port;

                if (port.empty())
                {
                    SendToClient(clientSock, "ERROR:Port required");
                    return true;
                }

                if (port.front() == '<' && port.back() == '>')
                {
                    port = port.substr(1, port.length() - 2);
                }

                if (!IsValidPort(port))
                {
                    SendToClient(clientSock, "ERROR:Invalid port (1024-65535)");
                    return true;
                }

                {
                    lock_guard<mutex> lock(g_clientsMutex);

                    if (g_clients[clientSock].inRoom)
                    {
                        SendToClient(clientSock, "ERROR:Already in a room");
                        return true;
                    }
                }

                lock_guard<mutex> lock(g_roomsMutex);
                string foundId;
                shared_ptr<Room> foundRoom;

                for (auto& p : g_rooms)
                {
                    if (p.second->port == port && !p.second->gameStarted && !p.second->portReserved)
                    {
                        foundId = p.first;
                        foundRoom = p.second;
                        break;
                    }
                }

                if (!foundRoom)
                {
                    SendToClient(clientSock, "ERROR:Room not found with port " + port);
                    return true;
                }

                auto& room = *foundRoom;

                // 游戏刚结束的房间不允许新玩家直接 JOIN，需原玩家 REJOIN
                if (room.gameEnded)
                {
                    SendToClient(clientSock, "ERROR:Game ended, players will return via rejoin");
                    return true;
                }

                if (room.client2BanTime > time(nullptr))
                {
                    SendToClient(clientSock, "ERROR:Banned");
                    return true;
                }

                if (room.client2 != INVALID_SOCKET)
                {
                    SendToClient(clientSock, "ERROR:Room full");
                    return true;
                }

                string currentName;

                {
                    lock_guard<mutex> lk(g_clientsMutex);
                    auto it = g_clients.find(clientSock);
                    if (it != g_clients.end()) currentName = it->second.name;
                }

                room.client2 = clientSock;
                room.client2Ip = info.ip;
                room.name2 = currentName;

                {
                    lock_guard<mutex> lk(g_clientsMutex);
                    g_clients[clientSock].roomId = foundId;
                    g_clients[clientSock].inRoom = true;
                    g_clients[clientSock].playerId = 2;
                    g_clients[clientSock].isAdmin = false;
                }

                info.inRoom = true;
                info.roomId = foundId;
                info.playerId = 2;
                info.isAdmin = false;

                SendToClient(clientSock, "JOINED|" + foundId);
                SendToClient(room.client1, "ROOM_EVENT|" + currentName + " joined the room.");
                SendToClient(clientSock, "ROOM_EVENT|" + currentName + " joined the room.");
                Log("Player " + info.ip + " joined room " + foundId + " (port " + port + ")");
                return true;
            }

            // ---- NAME <name>：改名 ----
            // 支持 NAME|<name> 和 NAME <name> 两种格式（向后兼容）。
            if (line.find("NAME") == 0)
            {
                string name;
                size_t pipePos = line.find('|');
                if (pipePos != string::npos)
                    name = line.substr(pipePos + 1);
                else
                {
                    istringstream iss(line);
                    string cmd;
                    iss >> cmd;
                    getline(iss, name);
                    if (!name.empty() && name[0] == ' ') name = name.substr(1);
                }

                name = SanitizeName(name);

                {
                    lock_guard<mutex> lock(g_clientsMutex);
                    auto it = g_clients.find(clientSock);

                    if (it != g_clients.end())
                    {
                        it->second.name = name;
                        info.name = name;
                    }
                }

                if (info.inRoom)
                {
                    lock_guard<mutex> lock(g_roomsMutex);
                    auto rit = g_rooms.find(info.roomId);

                    if (rit != g_rooms.end())
                    {
                        if (info.playerId == 1) rit->second->name1 = name;
                        else rit->second->name2 = name;
                    }
                }

                SendToClient(clientSock, "NAME_SET|" + name);
                return true;
            }

            // ---- 兜底：房间内视为聊天，否则报错 ----
            if (info.inRoom && !info.roomId.empty())
            {
                SendToRoomUnsafe(info.roomId, "ROOM_EVENT|" + info.name + ": " + SanitizeChat(line));
            }
            else
            {
                SendToClient(clientSock, "ERROR:Unknown command");
            }

            return true;
        };

        // 主读取循环：按行处理命令；断线则退出
        while (g_running && keepAlive)
        {
            if (!ReceiveLines(clientSock, buffer, [&](const string& line)
            {
                keepAlive = handleLine(line);
            }))
            {
                Log("Client " + info.ip + " disconnected");
                break;
            }
        }

        // 清理：从房间移除并释放连接
        string roomId;
        int playerId = 0;
        bool isAdmin = false;

        {
            lock_guard<mutex> lock(g_clientsMutex);
            auto it = g_clients.find(clientSock);

            if (it != g_clients.end())
            {
                roomId = it->second.roomId;
                playerId = it->second.playerId;
                isAdmin = it->second.isAdmin;
                g_clients.erase(it);
            }
        }

        if (!roomId.empty()) RemovePlayerFromRoom(roomId, playerId, isAdmin);

        closesocket(clientSock);
        Log("Client " + info.ip + " cleaned up");
    }
    catch (const exception& e)
    {
        Log(string("Exception: ") + e.what());
        closesocket(clientSock);
    }
}

// ============ 主流程 ============

int main()
{
    DisableConsoleQuickEdit();
    SetConsoleUtf8();
    SetConsoleFont();

    signal(SIGINT, [](int)
    {
        g_running = false;
        if (g_listenSock != INVALID_SOCKET) closesocket(g_listenSock);
    });

    signal(SIGTERM, [](int)
    {
        g_running = false;
        if (g_listenSock != INVALID_SOCKET) closesocket(g_listenSock);
    });

    string ip = "127.0.0.1";
    int port = 8888;

    cout << "=== Demon Roulette Room Manager ===" << endl;
    cout << "Enter listen port (default 8888): ";

    string p;
    getline(cin, p);
    if (!p.empty()) port = atoi(p.c_str());

    cout << "Enter listen IP (default 127.0.0.1): ";

    string ip2;
    getline(cin, ip2);
    if (!ip2.empty()) ip = ip2;

    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        cout << "WSAStartup failed" << endl;
        _getch();
        return 1;
    }

    srand((unsigned)time(0));

    g_listenSock = socket(AF_INET, SOCK_STREAM, 0);

    if (g_listenSock == INVALID_SOCKET)
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
    if (::bind(g_listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        Log("bind() failed on port " + to_string(port) + ", error: " + to_string(WSAGetLastError()));
        closesocket(g_listenSock);
        WSACleanup();
        _getch();
        return 1;
    }

    if (listen(g_listenSock, 10) == SOCKET_ERROR)
    {
        Log("listen() failed, error: " + to_string(WSAGetLastError()));
        closesocket(g_listenSock);
        WSACleanup();
        _getch();
        return 1;
    }

    Log("Room Manager started on " + ip + ":" + to_string(port));
    cout << "[OK] Room Manager is running on " << ip << ":" << port << endl;

    while (g_running)
    {
        sockaddr_in clientAddr;
        int addrLen = sizeof(clientAddr);

        SOCKET clientSock = accept(g_listenSock, (sockaddr*)&clientAddr, &addrLen);

        if (clientSock == INVALID_SOCKET)
        {
            if (!g_running) break;
            Sleep(100);
            continue;
        }

        Log("New client connected from " + string(inet_ntoa(clientAddr.sin_addr)));
        thread(HandleClient, clientSock).detach();
    }

    closesocket(g_listenSock);
    WSACleanup();
    Log("Room Manager stopped");
    cout << "\n[ Pause ]\n";
    system("pause > nul");
    return 0;
}
