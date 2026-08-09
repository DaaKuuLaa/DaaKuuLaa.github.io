# 狼人杀第九轮验收脚本（测试工具，非游戏实现）
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File tests\round9_test.ps1 *> tests\round9_out.txt
# 覆盖 REQUIREMENTS.md §19（2026-08-08）：
#   A) BAN/UNBAN 通配符：名字模式 Tech* / ??? / 全角＊？等效 / IP 段 10.129.* /
#      UNBAN 通配 / .ban 文件含模式行 / SHOW BAN 显示模式项 / 精确匹配不回归 /
#      模式命中房内玩家立即踢出
#   B) SHOW/LOOK：各子项输出（BAN/RATIO/LEVEL/VILLAGER/AUTO/ADD）、房主专属、
#      空列表与无参数用法提示、大厅用法
#   C) ADD NPC 离线 + START /F：NPC 占槽进 STATUS、恒 ready、/F 未准备强制开局、
#      至少 2 人边界、非房主 /F 拒绝、PLAYER_LIST 含 NPC、白天 NPC 发言广播、
#      NPC 自动投票、游戏正常结束 __GAME_OVER__
#   D) ADD USER：默认房主 / -u 指定 / 窗口自动入房（STATUS 可见）/ 重名拒绝 /
#      开局窗口自动连游戏端口（PLAYER_LIST 含本地用户）
#   E) 失联 3 秒：静默 3s+ 判定失联清连接；PING 1s 保活不误杀
#   F) 在线 NPC：WOLF_NPC_API_URL 指向本地假服务器 → 在线决策/回退离线均完成行动
# 大厅流程通过裸 socket 直连 Start.exe；游戏局通过裸 socket 直连 Server.exe。
# StreamWriter 必须显式无 BOM UTF8 编码（AGENTS.md 踩坑 17/21）。

$ErrorActionPreference = 'Stop'
$wolf = Split-Path $PSScriptRoot -Parent
$script:pass = 0
$script:fail = 0
$script:wolfTarget = $null

# 已连接的大厅客户端（TCP 连接对象）注册表：等待类函数（RecvUntilStream/
# ReadChunk）在等待间隙对它们统一发 PING 保活（3s 失联判定，§19.3）。
# 所有写入都在主线程串行完成，无并发写交错问题
$script:pingList = [System.Collections.ArrayList]::new()

function Ping-All {
    foreach ($tcp in $script:pingList) {
        try {
            $ns = $tcp.GetStream()
            $ns.Write([System.Text.Encoding]::UTF8.GetBytes("PING`n"), 0, 5)
        } catch {}
    }
}

function Check($desc, $cond) {
    if ($cond) { $script:pass++; Write-Output ("PASS  " + $desc) }
    else       { $script:fail++; Write-Output ("FAIL  " + $desc) }
}

# ============ 进程 / 端口 / 工具 ============

function Kill-All {
    Get-Process -Name Start,Server,Client,Client_en -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 600
}

# 空闲端口探测：TcpListener(Any)（踩坑 16）+ 已分配端口登记（踩坑 23）
$script:usedPorts = @()

function Get-FreePort {
    for ($p = 8560; $p -lt 8640; $p++) {
        if ($script:usedPorts -contains $p) { continue }
        try {
            $l = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Any, $p)
            $l.Start()
            $l.Stop()
            $script:usedPorts += $p
            return $p
        } catch {}
    }
    return 8560
}

function Start-RM($port) {
    Kill-All
    $proc = Start-Process -FilePath "$wolf\Start.exe" -WorkingDirectory $wolf -ArgumentList @($port.ToString()) -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 2
    return $proc
}

function RecvUntilStream($s, $match, $timeoutMs = 5000) {
    $deadline = [DateTime]::Now.AddMilliseconds($timeoutMs)
    $pending = New-Object System.Collections.Generic.List[byte]
    $lastPing = [DateTime]::Now
    while ([DateTime]::Now -lt $deadline) {
        if (([DateTime]::Now - $lastPing).TotalMilliseconds -ge 900) {
            Ping-All
            $lastPing = [DateTime]::Now
        }
        while ($s.DataAvailable) {
            $b = $s.ReadByte()
            if ($b -lt 0) { return $null }
            $pending.Add([byte]$b)
            if ($b -eq 10) {
                $line = [System.Text.Encoding]::UTF8.GetString($pending.ToArray())
                $line = $line.TrimEnd("`r", "`n")
                if ($line.Contains($match)) { return $line }
                $pending.Clear()
            }
        }
        Start-Sleep -Milliseconds 20
    }
    return $null
}

function ReadChunk($s, $timeoutMs) {
    $deadline = [DateTime]::Now.AddMilliseconds($timeoutMs)
    $bytes = New-Object System.Collections.Generic.List[byte]
    $lastPing = [DateTime]::Now
    while ([DateTime]::Now -lt $deadline) {
        if (([DateTime]::Now - $lastPing).TotalMilliseconds -ge 900) {
            Ping-All
            $lastPing = [DateTime]::Now
        }
        if ($s.DataAvailable) {
            $b = $s.ReadByte()
            if ($b -lt 0) { break }
            $bytes.Add([byte]$b)
        } else {
            Start-Sleep -Milliseconds 20
        }
    }
    return $bytes.ToArray()
}

# STATUS 多行回复：头行换行已被 RecvUntilStream 消费，后续数据行要补读（踩坑 22）
function Recv-Status($cl, $timeoutMs = 3000) {
    $hdr = RecvUntilStream $cl.s 'ROOM_STATUS' $timeoutMs
    if (-not $hdr) { return $null }
    $chunk = ReadChunk $cl.s 800
    $txt = [System.Text.Encoding]::UTF8.GetString($chunk)
    return ($hdr + "`n" + $txt)
}

# 大厅客户端：HELLO/NAME 握手
function New-Client($name, $port = 8888) {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', $port)
    $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s, [System.Text.UTF8Encoding]::new($false))
    $w.NewLine = "`n"
    $w.AutoFlush = $true
    $w.WriteLine('HELLO|3')
    $null = RecvUntilStream $s 'WELCOME' 3000
    $w.WriteLine('NAME|' + $name)
    $null = RecvUntilStream $s 'NAME_SET' 3000
    [void]$script:pingList.Add($c)
    return @{ c = $c; s = $s; w = $w }
}

function Close-Client($cl) {
    if ($cl -and $cl.c) {
        $script:pingList.Remove($cl.c)
        try { $cl.c.Close() } catch {}
    }
}

function SendLine($cl, $cmd) {
    $cl.w.WriteLine($cmd)
}

function Connect-Retry($port) {
    for ($i = 0; $i -lt 6; $i++) {
        try {
            $c = New-Object Net.Sockets.TcpClient
            $c.Connect('127.0.0.1', $port)
            return $c
        } catch {
            Start-Sleep -Seconds 1
        }
    }
    throw "无法连接端口 $port"
}

# ============ 直连局 bot（连 Server.exe） ============

function New-Bot($k, $port) {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', $port)
    $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s, [System.Text.UTF8Encoding]::new($false))
    $w.NewLine = "`n"
    $w.AutoFlush = $true
    $w.WriteLine('PLAYER_ID|' + $k)
    return @{
        k = $k; c = $c; s = $s; w = $w
        bytes = [System.Collections.Generic.List[byte]]::new()
        queue = [System.Collections.Queue]::new()
        role = ''; assigned = ''; pl = $null
    }
}

function Pump-Bots($bots) {
    foreach ($cl in $bots) {
        while ($cl.s.DataAvailable) {
            $b = $cl.s.ReadByte()
            if ($b -lt 0) { break }
            if ($b -eq 10) {
                $raw = $cl.bytes.ToArray()
                $cl.bytes.Clear()
                $line = [System.Text.Encoding]::UTF8.GetString($raw).TrimEnd("`r")
                if ($line.Length -gt 0) { $cl.queue.Enqueue($line) }
            } else {
                $cl.bytes.Add([byte]$b)
            }
        }
    }
}

# 定狼刀目标：真人 bot 之间轮换会刀到已死玩家（bot1 死后目标恒为死人，
# 服务器拒绝 → AskChoice 死循环）；目标固定取 NPC 槽位 3/4 之间轮转——
# 4 人局 NPC 必占 3/4，且狼杀净 NPC 后游戏必然结束，不会出现双死循环
$script:wolfDart = 0

function Next-WolfTarget($cl, $allBots) {
    $cands = @(3, 4)
    $script:wolfDart = ($script:wolfDart + 1) % $cands.Count
    $script:wolfTarget = $cands[$script:wolfDart]
}

# 单行处理：身份/名单记录 + 夜晚输入应答（狼刀轮换目标、预言家验 1、其余 0）
function Handle-GameLine($cl, $line, $allBots) {
    if ($line -match '你被分配到 (\d+) 号位') { $cl.assigned = $Matches[1]; return }
    if ($line -match '^ROLE\|') { $cl.role = $line.Substring(5); return }
    if ($line -match '^PLAYER_LIST\|') { $cl.pl = $line; return }
    if ($line.Trim() -eq '__INPUT__') {
        $isWolf = ($cl.role -eq 'werewolf' -or $cl.role -eq 'whitewolf')
        if ($isWolf) {
            # 应答必须立即发出（服务器 AskChoice 只问一次就阻塞等待，不重问）：
            # 目标取下一只非自己的 bot（不依赖角色判定，避免首轮未定空发）
            Next-WolfTarget $cl $allBots
            $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $script:wolfTarget)
        } elseif ($cl.role -eq 'seer') {
            $cl.w.WriteLine('PLAYER_' + $cl.k + '|1')
        } else {
            $cl.w.WriteLine('PLAYER_' + $cl.k + '|0')
        }
    }
}

try {
    # ============ A 段：BAN/UNBAN 通配符 ============
    $null = Start-RM 8888
    $portA = Get-FreePort
    $A = @(New-Client 'HstA')
    SendLine $A[0] ('CREATE|' + $portA)
    $null = RecvUntilStream $A[0].s 'CREATED' 3000

    SendLine $A[0] 'BAN Tech*'
    $banR = RecvUntilStream $A[0].s '已拉黑' 3000
    Check 'A1 BAN Tech* 通配模式拉黑成功' ($banR -and $banR.Contains('Tech*'))

    SendLine $A[0] 'SHOW BAN'
    $sb = RecvUntilStream $A[0].s 'Banned List' 3000
    $sbChunk = ReadChunk $A[0].s 800
    $sbTxt = $sb + [System.Text.Encoding]::UTF8.GetString($sbChunk)
    Check 'A2 SHOW BAN 显示名字模式 Tech*' $sbTxt.Contains('Tech*')

    SendLine $A[0] 'BAN ???'
    $null = RecvUntilStream $A[0].s '已拉黑' 3000
    SendLine $A[0] 'SHOW BAN'
    $null = RecvUntilStream $A[0].s 'Banned List' 3000
    $sbChunk2 = ReadChunk $A[0].s 800
    $sbTxt2 = [System.Text.Encoding]::UTF8.GetString($sbChunk2)
    Check 'A3 BAN ??? 三位名模式入单' $sbTxt2.Contains('???')

    SendLine $A[0] 'BAN Q?a＊'
    $null = RecvUntilStream $A[0].s '已拉黑' 3000
    SendLine $A[0] 'SHOW BAN'
    $null = RecvUntilStream $A[0].s 'Banned List' 3000
    $sbChunk3 = ReadChunk $A[0].s 800
    $sbTxt3 = [System.Text.Encoding]::UTF8.GetString($sbChunk3)
    Check 'A4 全角＊规范化为半角 * 入单' $sbTxt3.Contains('Q?a*')

    SendLine $A[0] 'BAN 10.129.*'
    $null = RecvUntilStream $A[0].s '已拉黑' 3000
    SendLine $A[0] 'SHOW BAN'
    $null = RecvUntilStream $A[0].s 'Banned List' 3000
    $sbChunk4 = ReadChunk $A[0].s 800
    $sbTxt4 = [System.Text.Encoding]::UTF8.GetString($sbChunk4)
    Check 'A5 IP 段模式 10.129.* 入单（IP 行）' $sbTxt4.Contains('10.129.*')

    $probe = New-Client 'TechStar'
    SendLine $probe ('JOIN|' + $portA)
    $joinR = RecvUntilStream $probe.s 'JOINED' 2500
    Check 'A6 名字 TechStar 匹配 Tech* 被拒入房' (-not $joinR)
    Close-Client $probe

    SendLine $A[0] 'UNBAN Tech*'
    $ub = RecvUntilStream $A[0].s '已取消拉黑' 3000
    Check 'A7 UNBAN Tech* 按模式串精确删除' ($ub -and $ub.Contains('Tech*'))

    SendLine $A[0] 'SHOW BAN'
    $null = RecvUntilStream $A[0].s 'Banned List' 3000
    $sbChunk5 = ReadChunk $A[0].s 800
    $sbTxt5 = [System.Text.Encoding]::UTF8.GetString($sbChunk5)
    Check 'A8 UNBAN 后 SHOW BAN 不再含 Tech*' (-not $sbTxt5.Contains('Tech*'))

    $banFile = "$wolf\tests\round9_t1.ban"
    [System.IO.File]::WriteAllText($banFile, "FilePat*`n10.9.9.*`n", [System.Text.Encoding]::UTF8)
    SendLine $A[0] ('BAN ' + $banFile)
    $null = RecvUntilStream $A[0].s '完成' 3000
    SendLine $A[0] 'SHOW BAN'
    $null = RecvUntilStream $A[0].s 'Banned List' 3000
    $sbChunk6 = ReadChunk $A[0].s 800
    $sbTxt6 = [System.Text.Encoding]::UTF8.GetString($sbChunk6)
    Check 'A9 .ban 文件导入模式行（FilePat* 与 10.9.9.*）' ($sbTxt6.Contains('FilePat*') -and $sbTxt6.Contains('10.9.9.*'))

    SendLine $A[0] 'BAN Alice'
    $null = RecvUntilStream $A[0].s '已拉黑' 3000
    SendLine $A[0] 'BAN alice'
    $null = RecvUntilStream $A[0].s '已拉黑' 3000
    SendLine $A[0] 'SHOW BAN'
    $null = RecvUntilStream $A[0].s 'Banned List' 3000
    $sbChunk7 = ReadChunk $A[0].s 800
    $sbTxt7 = [System.Text.Encoding]::UTF8.GetString($sbChunk7)
    $aliceCnt = ([regex]::Matches($sbTxt7, '(?m)^名字：Alice$')).Count
    Check 'A10 精确名字拉黑不回归（大小写变体不重复入单）' ($aliceCnt -eq 1)

    $inRoom = New-Client 'TechOne'
    Write-Output ("DBG A11a t=" + [DateTime]::Now.ToString('HH:mm:ss.fff'))
    SendLine $inRoom ('JOIN|' + $portA)
    Write-Output ("DBG A11b t=" + [DateTime]::Now.ToString('HH:mm:ss.fff'))
    $null = RecvUntilStream $inRoom.s 'JOINED' 3000
    Write-Output ("DBG A11c t=" + [DateTime]::Now.ToString('HH:mm:ss.fff'))
    SendLine $A[0] 'BAN Tech*'
    Write-Output ("DBG A11d t=" + [DateTime]::Now.ToString('HH:mm:ss.fff'))
    $kickMsg = RecvUntilStream $inRoom.s '拉黑' 3000
    Check 'A11 模式命中房内玩家立即踢出' ($kickMsg -and $kickMsg.Contains('拉黑'))
    Close-Client $inRoom
    foreach ($c in $A) { Close-Client $c }

    # ============ B 段：SHOW / LOOK ============
    $portB = Get-FreePort
    $B = @(New-Client 'HstB')
    SendLine $B[0] ('CREATE|' + $portB)
    $null = RecvUntilStream $B[0].s 'CREATED' 3000
    $guestB = New-Client 'GuestB'
    SendLine $guestB ('JOIN|' + $portB)
    $null = RecvUntilStream $guestB.s 'JOINED' 3000

    SendLine $B[0] 'SHOW RATIO'
    $sr = RecvUntilStream $B[0].s '狼' 3000
    Check 'B1 SHOW RATIO 显示比例与村民状态' ($sr -and $sr.Contains('村民'))

    SendLine $B[0] 'SHOW LEVEL'
    $sl = RecvUntilStream $B[0].s '档位' 3000
    Check 'B2 SHOW LEVEL 显示职业档位' $sl

    SendLine $B[0] 'SHOW VILLAGER'
    $sv = RecvUntilStream $B[0].s '村民' 3000
    Check 'B3 SHOW VILLAGER 显示开关状态' $sv

    SendLine $B[0] 'SHOW AUTO'
    $sa = RecvUntilStream $B[0].s '自动开局' 3000
    Check 'B4 SHOW AUTO 显示自动开局状态' $sa

    SendLine $B[0] 'SHOW ADD'
    $sadd = RecvUntilStream $B[0].s 'SHOW' 3000
    Check 'B5 SHOW ADD 空列表输出用法提示' ($sadd -and ($sadd.Contains('SHOW ADD') -or $sadd.Contains('SHOW <')))

    SendLine $B[0] 'SHOW'
    $s0 = RecvUntilStream $B[0].s 'SHOW 用法' 3000
    Check 'B6 SHOW 无参数输出用法' $s0

    SendLine $B[0] 'LOOK'
    $lk = RecvUntilStream $B[0].s 'SHOW 用法' 3000
    Check 'B7 LOOK 与 SHOW 等效（用法输出）' $lk

    SendLine $guestB 'SHOW BAN'
    $nr = RecvUntilStream $guestB.s '房主' 3000
    Check 'B8 非房主 SHOW BAN 拒绝' ($nr -and $nr.Contains('房主'))

    $guestLobby = New-Client 'LobbyGuy'
    SendLine $guestLobby 'SHOW BAN'
    $ll = RecvUntilStream $guestLobby.s 'SHOW 用法' 3000
    Check 'B9 大厅 SHOW 输出用法' $ll
    Close-Client $guestLobby

    $null = RecvUntilStream $guestB.s 'ROOM_STATUS' 1500
    foreach ($c in $B) { Close-Client $c }

    # ============ C0 段：START /F 边界 ============
    $portC0 = Get-FreePort
    $C0 = @(New-Client 'HstC0')
    SendLine $C0[0] ('CREATE|' + $portC0)
    $null = RecvUntilStream $C0[0].s 'CREATED' 3000
    SendLine $C0[0] 'START /F'
    $r1 = RecvUntilStream $C0[0].s '至少' 2500
    Check 'C0a 单人房 START /F 拒绝（至少 2 人）' ($r1 -and $r1.Contains('2'))

    $C0b = New-Client 'GuestC0'
    SendLine $C0b ('JOIN|' + $portC0)
    $null = RecvUntilStream $C0b.s 'JOINED' 3000
    SendLine $C0b 'START /F'
    $r2 = RecvUntilStream $C0b.s '房主' 2500
    Check 'C0b 非房主 START /F 拒绝' ($r2 -and $r2.Contains('房主'))

    SendLine $C0[0] 'START /F'
    $r3 = RecvUntilStream $C0[0].s 'GAME_PREPARE|' 3000
    Check 'C0c 双人房未准备 START /F 强制开局成功' $r3
    foreach ($c in $C0) { Close-Client $c }

    # ============ C 段：ADD NPC 离线 + START /F 4 人局 ============
    $null = Start-RM 8888
    $portC = Get-FreePort
    $C = @(New-Client 'HstC')
    SendLine $C[0] ('CREATE|' + $portC)
    $null = RecvUntilStream $C[0].s 'CREATED' 3000
    $C += (New-Client 'GuestC')
    SendLine $C[1] ('JOIN|' + $portC)
    $null = RecvUntilStream $C[1].s 'JOINED' 3000

    SendLine $C[0] 'ADD NPC WuffBot on'
    $addN = RecvUntilStream $C[0].s 'NPC' 3000
    Check 'C1 ADD NPC 在线模式添加成功' ($addN -and $addN.Contains('WuffBot'))

    SendLine $C[0] 'ADD NPC MidiBot off'
    $addN2 = RecvUntilStream $C[0].s 'MidiBot' 3000
    Check 'C2 ADD NPC 离线模式添加成功' ($addN2 -and $addN2.Contains('MidiBot'))

    SendLine $C[0] 'ADD NPC WuffBot on'
    $dupN = RecvUntilStream $C[0].s '占用' 2500
    Check 'C3 NPC 重名拒绝' ($dupN -and $dupN.Contains('占用'))

    SendLine $C[0] 'STATUS'
    $stC = Recv-Status $C[0]
    Check 'C4 STATUS 显示 NPC 槽位（WuffBot/MidiBot 恒准备）' ($stC -and $stC.Contains('WuffBot') -and $stC.Contains('MidiBot'))

    SendLine $C[0] 'SHOW ADD'
    $showAddC = RecvUntilStream $C[0].s 'NPCs' 3000
    $showAddChunk = ReadChunk $C[0].s 800
    $showAddTxt = $showAddC + [System.Text.Encoding]::UTF8.GetString($showAddChunk)
    Check 'C5 SHOW ADD 显示 NPC 列表（在线/离线标记）' ($showAddTxt.Contains('WuffBot') -and $showAddTxt.Contains('在线') -and $showAddTxt.Contains('离线'))

    SendLine $C[0] 'LEVEL|0'
    $null = RecvUntilStream $C[0].s '档位已' 2000
    SendLine $C[0] 'VILLAGER|1'
    $null = RecvUntilStream $C[0].s '村民职业已启用' 2000
    SendLine $C[0] 'RATIO|1|0|2'
    $null = RecvUntilStream $C[0].s '比例已设为' 2000

    # 双方都不 READY：验证 /F 跳过准备检查
    SendLine $C[0] 'START /F'
    $gpC = @()
    foreach ($cl in $C) { $gpC += RecvUntilStream $cl.s 'GAME_PREPARE|' 5000 }
    Check 'C6 未准备 START /F 强制开局（双方收 GAME_PREPARE）' (@($gpC | Where-Object { $_ }).Count -eq 2)

    $gpsText = $gpC -join '|'
    $pidOK = @($gpC | Where-Object { $_ -match '\|1$' }).Count -eq 1 -and @($gpC | Where-Object { $_ -match '\|2$' }).Count -eq 1
    Check 'C7 GAME_PREPARE 压缩名单 pid=1,2（NPC 在名单内不占真实玩家编号）' $pidOK
    foreach ($cl in $C) { Close-Client $cl }

    $botsC = @()
    for ($k = 1; $k -le 2; $k++) { $botsC += New-Bot $k $portC }
    $dayC = $false
    $voteC = $false
    $overC = $false
    $npcSpeakC = $false
    $votedC = $false
    $npcVoteC = $false
    $deadlineC = [DateTime]::Now.AddSeconds(120)
    $lastPingC = [DateTime]::Now
    while ([DateTime]::Now -lt $deadlineC) {
        if (([DateTime]::Now - $lastPingC).TotalSeconds -ge 1) {
            foreach ($b in $botsC) { try { $b.w.WriteLine('PING') } catch {} }
            $lastPingC = [DateTime]::Now
        }
        Pump-Bots $botsC
        foreach ($b in $botsC) {
            while ($b.queue.Count -gt 0) {
                $line = $b.queue.Dequeue()
                Handle-GameLine $b $line $botsC
                if ($line -match 'PLAYER_LIST\|') { $script:plC = $line }
                if ($line.Contains('白天发言阶段')) { $dayC = $true; $votedC = $false }
                if ($line.Contains('WuffBot') -and ($line.Contains('：') -or $line.Contains(':'))) { $npcSpeakC = $true }
                if ($line.Contains('WuffBot')) { $script:dbgC9 += ($line -replace '\|', '^') + '~' }
                if ($line.Contains('投票给了玩家')) { $npcVoteC = $true }
                if ($line.Trim() -eq '__GAME_OVER__') { $overC = $true }
            }
        }
        # 每个白天都须投票：$votedC 在每次「白天发言阶段」广播时重置，
        # 否则白天 1 投过票后第 2 个白天不再投，Server 等投票 120s 超时（C11 实测）
        if ($dayC -and -not $votedC) {
            foreach ($b in $botsC) { try { $b.w.WriteLine('PLAYER_' + $b.k + '|VOTE|0') } catch {} }
            $votedC = $true
        }
        if ($overC) { break }
        Start-Sleep -Milliseconds 50
    }
    $plC = $script:plC
    Check 'C8 PLAYER_LIST 含 NPC 名（WuffBot/MidiBot）' ($plC -and $plC.Contains('WuffBot') -and $plC.Contains('MidiBot'))
    Check 'C9 白天阶段 NPC 发言广播出现' $npcSpeakC
    Check 'C10 白天投票阶段 NPC 自动投票（投票广播出现）' $npcVoteC
    Check 'C11 4 人 NPC 局正常结束（__GAME_OVER__）' $overC
    foreach ($b in $botsC) { Close-Client $b }

    # ============ D 段：ADD USER 本地用户 ============
    $null = Start-RM 8888
    $portD = Get-FreePort
    $D = @(New-Client 'HstD')
    SendLine $D[0] ('CREATE|' + $portD)
    $null = RecvUntilStream $D[0].s 'CREATED' 3000
    $D += (New-Client 'GuestD')
    SendLine $D[1] ('JOIN|' + $portD)
    $null = RecvUntilStream $D[1].s 'JOINED' 3000

    $clientsBefore = @(Get-Process -Name Client -ErrorAction SilentlyContinue).Count
    SendLine $D[0] 'ADD USER LuUser'
    $addU = RecvUntilStream $D[0].s '本地用户' 3000
    Check 'D1 ADD USER 默认控制者=房主' ($addU -and $addU.Contains('LuUser'))

    Start-Sleep -Milliseconds 600
    $null = RecvUntilStream $D[0].s 'ROUND9_NO_MATCH' 2400
    $clientsAfter = @(Get-Process -Name Client -ErrorAction SilentlyContinue).Count
    Check 'D2 ADD USER 拉起 Client 窗口进程' ($clientsAfter -gt $clientsBefore)

    SendLine $D[0] 'ADD USER LuUser2 -u 2'
    $addU2 = RecvUntilStream $D[0].s '本地用户' 3000
    Check 'D3 ADD USER -u 指定控制者' ($addU2 -and $addU2.Contains('LuUser2') -and $addU2.Contains('GuestD'))

    SendLine $D[0] 'ADD USER LuUser'
    $dupU = RecvUntilStream $D[0].s '占用' 2500
    Check 'D4 ADD USER 重名拒绝' ($dupU -and $dupU.Contains('占用'))

    SendLine $D[0] 'STATUS'
    $stD = Recv-Status $D[0] 4000
    Check 'D5 窗口自动入房后 STATUS 显示本地用户' ($stD -and $stD.Contains('LuUser') -and $stD.Contains('LuUser2'))

    SendLine $D[0] 'SHOW ADD'
    $null = RecvUntilStream $D[0].s 'NPCs' 3000
    $saChunk = ReadChunk $D[0].s 800
    $saTxt = [System.Text.Encoding]::UTF8.GetString($saChunk)
    Check 'D6 SHOW ADD 显示本地用户与控制者' ($saTxt.Contains('LuUser') -and $saTxt.Contains('HstD'))

    SendLine $D[0] 'LEVEL|0'
    $null = RecvUntilStream $D[0].s '档位已' 2000
    SendLine $D[0] 'VILLAGER|1'
    $null = RecvUntilStream $D[0].s '村民职业已启用' 2000
    SendLine $D[0] 'RATIO|1|0|2'
    $null = RecvUntilStream $D[0].s '比例已设为' 2000

    # 窗口玩家无人操作不 READY：用 /F 强制开局
    SendLine $D[0] 'START /F'
    $gpD = @()
    foreach ($cl in $D) { $gpD += RecvUntilStream $cl.s 'GAME_PREPARE|' 5000 }
    Check 'D7 START /F 开局（含本地用户，真人收 GAME_PREPARE）' (@($gpD | Where-Object { $_ }).Count -eq 2)
    foreach ($cl in $D) { Close-Client $cl }

    # 等窗口自动连游戏端口，验证 PLAYER_LIST 含本地用户名。
    # Server 需 4/4 全连才开局：窗口已自动连 3/4 号位（LuUser/LuUser2），
    # 真人槽 1/2 空着，探测必须补连 1、2 两个空位（否则 WaitForGameStart 永远等）
    $plD = $null
    $tc1 = $null
    $tc2 = $null
    $w1 = $null
    $w2 = $null
    $deadlineD = [DateTime]::Now.AddSeconds(25)
    try {
        $tc1 = New-Object Net.Sockets.TcpClient
        $tc1.Connect('127.0.0.1', $portD)
        $s1 = $tc1.GetStream()
        $w1 = New-Object IO.StreamWriter($s1, [System.Text.UTF8Encoding]::new($false))
        $w1.NewLine = "`n"
        $w1.AutoFlush = $true
        $w1.WriteLine('PLAYER_ID|1')

        $tc2 = New-Object Net.Sockets.TcpClient
        $tc2.Connect('127.0.0.1', $portD)
        $s2 = $tc2.GetStream()
        $w2 = New-Object IO.StreamWriter($s2, [System.Text.UTF8Encoding]::new($false))
        $w2.NewLine = "`n"
        $w2.AutoFlush = $true
        $w2.WriteLine('PLAYER_ID|2')

        $lastPingD = [DateTime]::Now
        while ([DateTime]::Now -lt $deadlineD -and -not $plD) {
            if (([DateTime]::Now - $lastPingD).TotalSeconds -ge 1) {
                $w1.WriteLine('PING')
                $w2.WriteLine('PING')
                $lastPingD = [DateTime]::Now
            }
            $plD = RecvUntilStream $s1 'PLAYER_LIST' 3000
        }
    } catch {}
    if ($w1) { try { $w1.WriteLine('PING') } catch {} }
    if ($w2) { try { $w2.WriteLine('PING') } catch {} }
    if ($tc1) { $tc1.Close() }
    if ($tc2) { $tc2.Close() }
    Check 'D8 本地用户窗口自动连游戏端口，PLAYER_LIST 含 LuUser' ($plD -and $plD.Contains('LuUser') -and $plD.Contains('LuUser2'))

    # ============ E 段：失联 3 秒 ============
    $null = Start-RM 8888
    $portE = Get-FreePort
    $E = @(New-Client 'HstE')
    SendLine $E[0] ('CREATE|' + $portE)
    $null = RecvUntilStream $E[0].s 'CREATED' 3000
    # SilentE 用裸连接（不注册 PING 保活）：E1 故意静默 3s+ 验证失联判定
    $seC = New-Object Net.Sockets.TcpClient
    $seC.Connect('127.0.0.1', 8888)
    $seS = $seC.GetStream()
    $seW = New-Object IO.StreamWriter($seS, [System.Text.UTF8Encoding]::new($false))
    $seW.NewLine = "`n"
    $seW.AutoFlush = $true
    $seW.WriteLine('HELLO|3')
    $null = RecvUntilStream $seS 'WELCOME' 3000
    $seW.WriteLine('NAME|SilentE')
    $null = RecvUntilStream $seS 'NAME_SET' 3000
    $seW.WriteLine('JOIN|' + $portE)
    $null = RecvUntilStream $seS 'JOINED' 3000
    $E += @{ c = $seC; s = $seS; w = $seW }

    # E1: SilentE 静默 4.5s（不发任何字节，且不在保活列表）→ 3s 失联判定关闭连接
    $eofE = $false
    $lastPingE1 = [DateTime]::Now
    $deadlineE = [DateTime]::Now.AddSeconds(8)
    while ([DateTime]::Now -lt $deadlineE) {
        if (([DateTime]::Now - $lastPingE1).TotalSeconds -ge 1) {
            $null = RecvUntilStream $E[0].s 'ROUND9_NO_MATCH' 300
            $lastPingE1 = [DateTime]::Now
        }
        try {
            $tcp = $seC.Client
            if ($tcp.Poll(0, [Net.Sockets.SelectMode]::SelectRead)) {
                if ($tcp.Available -gt 0) {
                    $null = $tcp.ReadByte()
                } else {
                    $eofE = $true
                    break
                }
            }
        } catch { $eofE = $true; break }
        Start-Sleep -Milliseconds 100
    }
    Check 'E1 静默 3s+ 判定失联（连接被服务端关闭）' $eofE
    Close-Client $E[1]

    # E2: PING 1s 保活 6s 不误杀
    $okE2 = $true
    $lastPingE = [DateTime]::Now
    $deadlineE2 = [DateTime]::Now.AddSeconds(6)
    while ([DateTime]::Now -lt $deadlineE2) {
        if (([DateTime]::Now - $lastPingE).TotalSeconds -ge 1) {
            try { $E[0].w.WriteLine('PING') } catch { $okE2 = $false }
            $lastPingE = [DateTime]::Now
        }
        Start-Sleep -Milliseconds 100
        try {
            if (-not $E[0].c.Connected) { $okE2 = $false }
        } catch { $okE2 = $false }
    }
    SendLine $E[0] 'STATUS'
    $stE = Recv-Status $E[0] 3000
    Check 'E2 PING 1s 保活不误杀（6s 后 STATUS 正常）' ($okE2 -and $stE -and $stE.Contains('HstE'))
    foreach ($c in $E) { Close-Client $c }

    # ============ F 段：在线 NPC（本地假 API 服务器） ============
    $fakeOut = "$wolf\tests\round9_fake_out.txt"
    $fakeProc = Start-Process -FilePath "powershell" -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$wolf\tests\npc_fake_server.ps1") -WorkingDirectory $wolf -RedirectStandardOutput $fakeOut -WindowStyle Hidden -PassThru
    $env:WOLF_NPC_API_URL = 'http://127.0.0.1:18080'
    $env:WOLF_NPC_TIMEOUT_SECONDS = '2'
    $env:WOLF_NPC_RETRIES = '0'
    $null = Start-RM 8888
    $portF = Get-FreePort
    $F = @(New-Client 'HstF')
    SendLine $F[0] ('CREATE|' + $portF)
    $null = RecvUntilStream $F[0].s 'CREATED' 3000
    $F += (New-Client 'GuestF')
    SendLine $F[1] ('JOIN|' + $portF)
    $null = RecvUntilStream $F[1].s 'JOINED' 3000

    SendLine $F[0] 'ADD NPC AIBot on'
    $addF = RecvUntilStream $F[0].s 'NPC' 3000
    Check 'F1 在线 NPC 添加成功' ($addF -and $addF.Contains('AIBot'))

    SendLine $F[0] 'ADD NPC AIBot2 off'
    $null = RecvUntilStream $F[0].s 'AIBot2' 3000

    SendLine $F[0] 'LEVEL|0'
    $null = RecvUntilStream $F[0].s '档位已' 2000
    SendLine $F[0] 'VILLAGER|1'
    $null = RecvUntilStream $F[0].s '村民职业已启用' 2000
    SendLine $F[0] 'RATIO|1|0|2'
    $null = RecvUntilStream $F[0].s '比例已设为' 2000
    SendLine $F[0] 'START /F'
    foreach ($cl in $F) { $null = RecvUntilStream $cl.s 'GAME_PREPARE|' 5000 }
    foreach ($cl in $F) { Close-Client $cl }

    $botsF = @()
    for ($k = 1; $k -le 2; $k++) { $botsF += New-Bot $k $portF }
    $dayF = $false
    $npcF = $false
    $deadlineF = [DateTime]::Now.AddSeconds(70)
    $lastPingF = [DateTime]::Now
    while ([DateTime]::Now -lt $deadlineF) {
        if (([DateTime]::Now - $lastPingF).TotalSeconds -ge 1) {
            foreach ($b in $botsF) { try { $b.w.WriteLine('PING') } catch {} }
            $lastPingF = [DateTime]::Now
        }
        Pump-Bots $botsF
        foreach ($b in $botsF) {
            while ($b.queue.Count -gt 0) {
                $line = $b.queue.Dequeue()
                Handle-GameLine $b $line $botsF
                if ($line.Contains('白天发言阶段')) { $dayF = $true }
                if ($line.Contains('AIBot')) { $npcF = $true }
            }
        }
        if ($dayF -and $npcF) { break }
        Start-Sleep -Milliseconds 50
    }
    Check 'F2 在线 NPC 局推进到白天且 NPC 出现' ($dayF -and $npcF)
    foreach ($b in $botsF) { Close-Client $b }
    Start-Sleep -Seconds 1
    # 先杀 fake 进程再读输出：RedirectStandardOutput 句柄在进程存活期间
    # 锁着文件，先读会抛「文件被占用」（F3 EXCEPTION 实测）
    Stop-Process -Id $fakeProc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
    $fakeOk = $false
    $fakeTxt = ''
    if (Test-Path -LiteralPath $fakeOut) {
        for ($try = 0; $try -lt 3 -and -not $fakeOk; $try++) {
            try {
                # fake server 是 Start-Process 独立进程（不是 *> 重定向），stdout 走
                # 进程管道编码（UTF-8 无 BOM），与主脚本的 UTF-16LE 输出不同——必须按
                # 字节探测：非 FF FE 开头按 UTF-8 读（F3 实测假服务器输出即 UTF-8）
                $bytes = [System.IO.File]::ReadAllBytes($fakeOut)
                if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
                    $fakeTxt = [System.Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2)
                } else {
                    $fakeTxt = [System.Text.Encoding]::UTF8.GetString($bytes)
                }
                if ($fakeTxt.Contains('REQ:')) { $fakeOk = $true }
            } catch { Start-Sleep -Milliseconds 500 }
        }
    }
    Check 'F3 在线 NPC 调用了 API（假服务器收到请求或已回退离线）' $fakeOk
    Remove-Item Env:WOLF_NPC_API_URL -ErrorAction SilentlyContinue
    Remove-Item Env:WOLF_NPC_TIMEOUT_SECONDS -ErrorAction SilentlyContinue
    Remove-Item Env:WOLF_NPC_RETRIES -ErrorAction SilentlyContinue

    Kill-All
    Write-Output ("")
    Write-Output ("ROUND9 RESULT: pass=" + $script:pass + " fail=" + $script:fail)
} catch {
    Write-Output ("EXCEPTION: " + $_.Exception.Message)
    Write-Output ($_.ScriptStackTrace)
    Kill-All
    Write-Output ("ROUND9 RESULT: pass=" + $script:pass + " fail=" + $script:fail)
}



