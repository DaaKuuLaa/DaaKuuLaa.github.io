# 狼人杀第四轮验收脚本（测试工具，非游戏实现）
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File tests\round4_test.ps1 *> tests\round4_out.txt
# 覆盖 REQUIREMENTS.md §13.4 验收 1-9：
#   1. 完整链路：4 人建房 → 比例合法 → 全员 READY → START → 收 GAME_PREPARE → 连游戏服 →
#      断开大厅连接 → LIST 仍显示该房间（人数定格 4/12 + [游戏中]）→ GIVEUP 中止本局 →
#      Server 发 GAME_ENDED → 重连大厅回房成功 → 全员回房 → 可再开一局
#   2. 开局失败路径：START 成功但 Server.exe 即死 → WOLF_GAME_WAIT_SECONDS=6 注入 →
#      房间回滚保留（[游戏中] 消失、可再 JOIN）
#   3. 游戏进行中 REJOIN 被拒（REJOIN_FAIL|游戏仍在进行中）→ Server 中止后模拟客户端
#      每 5 秒自动重试 REJOIN（最多 6 次）→ 最终 JOINED| 成功
#   4. 游戏进行中外人 JOIN → 拒绝（ERROR|该房间正在游戏中）
#   5. 全员回房前外人 JOIN → 拒绝（gameEnded 拦截）；全员回房后 JOIN → 允许
#   6. 房主保护：原房主最后回房 → 槽 0 无顶替、收 ADMIN|，先前回房的非房主未收 ADMIN|
#   7. 重复 REJOIN：回房后再断开再回 → 正常、STATUS 不超员
#   8. 夜晚逐阶段开场广播：守卫/狼人/预言家/女巫请睁眼（全员收到）+ 职业专属提示仅限本人。
#      注：验收 8 用 6 人直连局（1 狼 0 中立 4 神 村民开）：神职池固定顺序取前 4 才同时
#      包含预言家/女巫/猎人/守卫；4 人局只有 2 神，覆盖不了"守卫请睁眼"广播
#      （§13.1 按职业是否在场决定广播与否）。
#   9. 攻击用例：垃圾字节/PING-only 连接不影响房间、REJOIN 伪造 pid 回落空槽不崩、
#      被拉黑回房被拒、未知房间/格式错误 clean 拒绝、游戏期间 LIST 正常
# 大厅流程通过裸 socket 直连 Start.exe（8888）；游戏局通过裸 socket 直连 Server.exe。
# 所有连接由后台 runspace 每 1 秒发 PING 保活（StreamWriter 加锁，AGENTS.md 踩坑 7/11）。

$ErrorActionPreference = 'Stop'
$wolf = Split-Path $PSScriptRoot -Parent
$script:pass = 0
$script:fail = 0

function Check($desc, $cond) {
    if ($cond) { $script:pass++; Write-Output ("PASS  " + $desc) }
    else       { $script:fail++; Write-Output ("FAIL  " + $desc) }
}

# ============ 保活 runspace（后台每 1 秒给所有在线连接发 PING） ============
# 长静默窗口（等回滚/等重试）内连接不发字节会被 Start/Server 的 3 秒失联判定
# 误杀（踩坑 7）；后台 runspace 与主线程共用 StreamWriter 必须加锁（踩坑 11）。
$script:liveClients = [System.Collections.ArrayList]::new()
$script:kaStop = New-Object System.Threading.ManualResetEvent($false)
$script:kaPs = $null
$script:kaRs = $null

function Start-Keepalive {
    $rs = [runspacefactory]::CreateRunspace()
    $rs.Open()
    $rs.SessionStateProxy.SetVariable('liveClients', $script:liveClients)
    $rs.SessionStateProxy.SetVariable('kaStop', $script:kaStop)
    $ps = [powershell]::Create()
    $ps.Runspace = $rs
    $null = $ps.AddScript({
        while (-not $kaStop.WaitOne(0)) {
            $kaStop.WaitOne(1000)
            $snap = $null
            try { $snap = $liveClients.ToArray() } catch {}
            foreach ($pc in $snap) {
                try {
                    [System.Threading.Monitor]::Enter($pc.wlock)
                    try { $pc.w.WriteLine('PING') } finally { [System.Threading.Monitor]::Exit($pc.wlock) }
                } catch {}
            }
        }
    })
    $null = $ps.BeginInvoke()
    $script:kaPs = $ps
    $script:kaRs = $rs
}

function Stop-Keepalive {
    try { $script:kaStop.Set() } catch {}
    try { if ($script:kaPs) { $script:kaPs.Dispose() } } catch {}
    try { if ($script:kaRs) { $script:kaRs.Close() } } catch {}
}

# ============ 裸 socket 客户端封装 ============

function Write-Locked($cl, $cmd) {
    [System.Threading.Monitor]::Enter($cl.wlock)
    try { $cl.w.WriteLine($cmd) } finally { [System.Threading.Monitor]::Exit($cl.wlock) }
}

function SendLine($cl, $cmd) { Write-Locked $cl $cmd }

function RecvUntilStream($s, $match, $timeoutMs = 5000) {
    $deadline = [DateTime]::Now.AddMilliseconds($timeoutMs)
    $pending = New-Object System.Collections.Generic.List[byte]
    while ([DateTime]::Now -lt $deadline) {
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

function RecvUntil($cl, $match, $timeoutMs = 5000) {
    return RecvUntilStream $cl.s $match $timeoutMs
}

# STATUS 多行回复：头行换行已被 RecvUntilStream 消费，剩余行补读后拼回换行
# （§16.5 竖排表：ROOM_STATUS|ID | NAME | ST + 每玩家一行）
function Recv-Status($cl, $timeoutMs = 3000) {
    $hdr = RecvUntilStream $cl.s 'ROOM_STATUS' $timeoutMs
    if (-not $hdr) { return '' }
    $chunk = ReadChunk $cl.s 800
    $rows = [System.Text.Encoding]::UTF8.GetString($chunk)
    return ($hdr + "`n" + $rows)
}

function ReadChunk($s, $timeoutMs) {
    $deadline = [DateTime]::Now.AddMilliseconds($timeoutMs)
    $bytes = New-Object System.Collections.Generic.List[byte]
    while ([DateTime]::Now -lt $deadline) {
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

function RecvAll($cl, $timeoutMs = 500) {
    $arr = ReadChunk $cl.s $timeoutMs
    return [System.Text.Encoding]::UTF8.GetString($arr)
}

# 大厅客户端：HELLO/NAME 握手完成后再注册进保活列表，保证首行不被 PING 打断
function New-Client($name) {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', 8888)
    $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s)
    $w.NewLine = "`n"
    $w.AutoFlush = $true
    $w.WriteLine('HELLO|3')
    $null = RecvUntilStream $s 'WELCOME' 3000
    $w.WriteLine('NAME|' + $name)
    $null = RecvUntilStream $s 'NAME_SET' 3000
    $cl = @{ c = $c; s = $s; w = $w; wlock = [object]::new() }
    $null = $script:liveClients.Add($cl)
    return $cl
}

function Close-Client($cl) {
    try { if ($cl -and $script:liveClients.Contains($cl)) { $null = $script:liveClients.Remove($cl) } } catch {}
    try { if ($cl -and $cl.c) { $cl.c.Close() } } catch {}
}

# 游戏机器人：先发 PLAYER_ID|k 首行再注册保活（Server 接受线程只认首行为身份行，
# PING 先到会被当成非法身份拒绝连接）
function New-Bot($k, $port) {
    $c = Connect-Retry $port
    $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s)
    $w.NewLine = "`n"
    $w.AutoFlush = $true
    $w.WriteLine('PLAYER_ID|' + $k)
    $bot = @{
        k = $k; c = $c; s = $s; w = $w; wlock = [object]::new(); closed = $false
        role = ''; witchInputs = 0
        bytes = [System.Collections.Generic.List[byte]]::new()
        queue = [System.Collections.Queue]::new()
        bc = @{ guard = $false; wolf = $false; seer = $false; witch = $false }
        pt = @{ guard = $false; wolf = $false; seer = $false; witch = $false }
    }
    $null = $script:liveClients.Add($bot)
    return $bot
}

function Close-Bot($bot) {
    $bot.closed = $true
    try { if ($script:liveClients.Contains($bot)) { $null = $script:liveClients.Remove($bot) } } catch {}
    try { $bot.c.Close() } catch {}
}

# ============ 进程 / 端口 / 连接工具 ============

function Kill-All {
    Get-Process -Name Start,Server,Client,Client_en -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 600
}

function Start-RM {
    Kill-All
    $proc = Start-Process -FilePath "$wolf\Start.exe" -WorkingDirectory $wolf -ArgumentList @('8888') -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 2
    return $proc
}

# 空闲端口探测（1024-65535；Start 的房间端口即游戏服务器端口，一房一端口）
function Get-FreePort {
    for ($p = 7510; $p -lt 7580; $p++) {
        try {
            $l = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Any, $p)
            $l.Start()
            $l.Stop()
            return $p
        } catch {}
    }
    return 7510
}

# 带重试的连接：Server.exe 刚拉起时监听可能还没就绪
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

# 中止本局：原 socket 已断开后，新连接发 GIVEUP|pid 让服务端立即放弃重连等待
function Send-GiveUp($port, $pidStr) {
    $c = Connect-Retry $port
    $w = New-Object IO.StreamWriter($c.GetStream())
    $w.NewLine = "`n"
    $w.AutoFlush = $true
    $w.WriteLine('GIVEUP|' + $pidStr)
    $c.Close()
}

function Is-WolfRole($role) {
    return ($role -eq 'werewolf') -or ($role -eq 'whitewolf')
}

function Get-WolfTarget($bots) {
    foreach ($b in $bots) {
        if (-not $b.closed -and -not (Is-WolfRole $b.role)) { return $b.k }
    }
    return 0
}

function Get-SeerTarget($bots, $selfK) {
    foreach ($b in $bots) {
        if (-not $b.closed -and $b.k -ne $selfK) { return $b.k }
    }
    return 0
}

# ============ 夜晚广播测试局（6 人直连，§13.4 验收 8） ============
# 机器人按角色应答 __INPUT__（守卫/狼/预言家/女巫），收齐四段开场广播与
# 四类专属提示后由外层断言；本函数只负责把夜晚推进到天亮点。
# Server.exe 参数序（与 round3_test 已验证用法一致）：
#   port 名字... 127.0.0.1 8888 roomId 狼 中立 神 档位 村民
function Run-NightGame($port) {
    # 尾部必须带 N 个语言码（§12.1）：总数 = 9+2N 个参数（含 exe 名后 9+2N-1 个），
    # 缺了语言码 Server 会把 (argc-10)/2 算错（6 人局传 15 个会被当成 3 人局，bot 4-6 全被拒）
    $srvArgs = @($port, 'N1', 'N2', 'N3', 'N4', 'N5', 'N6',
        '127.0.0.1', '8888', 'R4NIGHT', '1', '0', '4', '1', '1',
        'zh', 'zh', 'zh', 'zh', 'zh', 'zh')
    $srvProc = Start-Process -FilePath "$wolf\Server.exe" -WorkingDirectory $wolf -ArgumentList $srvArgs -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 2

    $bots = @()
    for ($i = 1; $i -le 6; $i++) { $bots += New-Bot $i $port }

    $dawn = $false
    $deadline = [DateTime]::Now.AddSeconds(60)
    $done = $false

    while (-not $done -and [DateTime]::Now -lt $deadline) {
        if ($srvProc.HasExited) { break }

        foreach ($b in $bots) { Pump-Bot $b }

        foreach ($b in $bots) {
            while ($b.queue.Count -gt 0) {
                $line = $b.queue.Dequeue()

                if ($line.Contains('ROLE|')) { $b.role = $line.Substring(5) }
                if ($line.Contains('守卫请睁眼')) { $b.bc.guard = $true }
                if ($line.Contains('狼人请睁眼')) { $b.bc.wolf = $true }
                if ($line.Contains('预言家请睁眼')) { $b.bc.seer = $true }
                if ($line.Contains('女巫请睁眼')) { $b.bc.witch = $true }
                if ($line.Contains('你是守卫')) { $b.pt.guard = $true }
                if ($line.Contains('你是狼人')) { $b.pt.wolf = $true }
                if ($line.Contains('你是预言家')) { $b.pt.seer = $true }
                if ($line.Contains('你是女巫')) { $b.pt.witch = $true }
                if ($line.Contains('天亮了')) { $dawn = $true }

                if ($line.Trim() -eq '__INPUT__' -and -not $b.closed) {
                    if (Is-WolfRole $b.role) {
                        $t = Get-WolfTarget $bots
                        if ($t -gt 0) { SendLine $b ('PLAYER_' + $b.k + '|' + $t) }
                    } elseif ($b.role -eq 'guard') {
                        # 守第一个非己玩家，与狼刀目标无关即可推进
                        $gt = Get-SeerTarget $bots $b.k
                        if ($gt -gt 0) { SendLine $b ('PLAYER_' + $b.k + '|' + $gt) }
                    } elseif ($b.role -eq 'seer') {
                        $t = Get-SeerTarget $bots $b.k
                        if ($t -gt 0) { SendLine $b ('PLAYER_' + $b.k + '|' + $t) }
                    } elseif ($b.role -eq 'witch') {
                        # 解药救狼刀目标（可自救），毒药弃用
                        if ($b.witchInputs -eq 0) {
                            $t = Get-WolfTarget $bots
                            if ($t -gt 0) { SendLine $b ('PLAYER_' + $b.k + '|' + $t) }
                        } else {
                            SendLine $b ('PLAYER_' + $b.k + '|0')
                        }
                        $b.witchInputs++
                    } else {
                        SendLine $b ('PLAYER_' + $b.k + '|0')
                    }
                }
            }
        }

        # 完成条件：全员收到四段开场广播 + 专属提示总数恰为 4 + 各职业本人收到 + 夜晚走通
        $allBc = $true
        foreach ($b in $bots) {
            if (-not ($b.bc.guard -and $b.bc.wolf -and $b.bc.seer -and $b.bc.witch)) { $allBc = $false; break }
        }
        $totPt = 0
        foreach ($b in $bots) {
            foreach ($k in @('guard', 'wolf', 'seer', 'witch')) { if ($b.pt.$k) { $totPt++ } }
        }
        $ownersOk = $true
        foreach ($b in $bots) {
            if ($b.role -eq 'guard' -and -not $b.pt.guard) { $ownersOk = $false }
            if ($b.role -eq 'werewolf' -and -not $b.pt.wolf) { $ownersOk = $false }
            if ($b.role -eq 'seer' -and -not $b.pt.seer) { $ownersOk = $false }
            if ($b.role -eq 'witch' -and -not $b.pt.witch) { $ownersOk = $false }
        }
        if ($allBc -and $totPt -eq 4 -and $ownersOk -and $dawn) { $done = $true }

        Start-Sleep -Milliseconds 30
    }

    foreach ($b in $bots) { Close-Bot $b }
    # 必须记录在 Stop-Process 之前：杀进程后 HasExited 恒 True，无法再判定崩溃
    $exitedBeforeKill = $srvProc.HasExited
    Stop-Process -Id $srvProc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300

    return @{ bots = $bots; dawn = $dawn; done = $done; crashed = $exitedBeforeKill }
}

# ============ 机器人泵（单线程轮询式收包，参考 server_test8 模式） ============
function Pump-Bot($bot) {
    if ($bot.closed) { return }
    try {
        while ($bot.s.DataAvailable) {
            $b = $bot.s.ReadByte()
            if ($b -lt 0) { $bot.closed = $true; break }
            if ($b -eq 10) {
                $raw = $bot.bytes.ToArray()
                $bot.bytes.Clear()
                $line = [System.Text.Encoding]::UTF8.GetString($raw)
                $line = $line.TrimEnd("`r")
                if ($line.Length -gt 0) { $bot.queue.Enqueue($line) }
            } else {
                $bot.bytes.Add([byte]$b)
            }
        }
    } catch { $bot.closed = $true }
}

# ============ 验收 1/3/4/5/6/7：完整链路 ============
Start-Keepalive
$rm1 = Start-RM
$portA = Get-FreePort

$A = New-Client 'Alice'
SendLine $A ('CREATE|' + $portA)
$r = RecvUntil $A 'CREATED' 3000
Check 'A1 房主建房成功' ($r -match 'CREATED')

$B = New-Client 'Bob'
SendLine $B ('JOIN|' + $portA)
$r = RecvUntil $B 'JOINED' 3000
Check 'A2 Bob 入房' ($r -match 'JOINED')
$C = New-Client 'Cathy'
SendLine $C ('JOIN|' + $portA)
$r = RecvUntil $C 'JOINED' 3000
Check 'A3 Cathy 入房' ($r -match 'JOINED')
$D = New-Client 'Dave'
SendLine $D ('JOIN|' + $portA)
$r = RecvUntil $D 'JOINED' 3000
Check 'A4 Dave 入房' ($r -match 'JOINED')

SendLine $A 'VILLAGER|1'
$r = RecvUntil $A '村民职业已启用' 2000
Check 'A5 开启村民职业' ($r -match '村民职业已启用')
SendLine $A 'RATIO|1|0|2'
$r = RecvUntil $A '比例已设为' 2000
Check 'A6 比例 1 狼 0 中立 2 神设置成功' ($r -match '比例已设为')

foreach ($cl in @($A, $B, $C, $D)) { SendLine $cl 'READY' }
Start-Sleep -Seconds 1
SendLine $A 'START'
$gpA = RecvUntil $A 'GAME_PREPARE|' 6000
Check 'A7 房主收到 GAME_PREPARE（开局成功）' ($gpA -match 'GAME_PREPARE\|')
$roomId = $gpA.Split('|')[2]
$gport = $gpA.Split('|')[1]
$allGp = $true
foreach ($cl in @($B, $C, $D)) {
    $r = RecvUntil $cl 'GAME_PREPARE|' 6000
    if (-not ($r -match 'GAME_PREPARE\|')) { $allGp = $false }
}
Check 'A8 其余 3 名玩家均收到 GAME_PREPARE' $allGp

$botA = @()
for ($i = 1; $i -le 4; $i++) { $botA += New-Bot $i $gport }
foreach ($bt in $botA) {
    $r = RecvUntilStream $bt.s 'ROLE|' 10000
    if ($r) { $bt.role = $r.Substring(5) }
}
Check 'A9 4 个机器人直连游戏服并收到身份' (($botA | Where-Object { $_.role }).Count -eq 4)

foreach ($cl in @($A, $B, $C, $D)) { Close-Client $cl }
Start-Sleep -Milliseconds 500

$L1 = New-Client 'Lisa'
SendLine $L1 'LIST'
$r = RecvUntil $L1 'ROOMS_LIST' 3000
Check 'A10 断开大厅后房间仍在列表（人数定格 4/12 + [游戏中]）' (
    $r -and ($r -match ([regex]::Escape($gport) + '\s+4/12')) -and $r.Contains('[游戏中]'))

$L2 = New-Client 'Larry'
SendLine $L2 ('JOIN|' + $gport)
$r = RecvUntil $L2 'ERROR' 3000
Check 'A11 游戏进行中外人 JOIN 被拒（该房间正在游戏中）' ($r -match '该房间正在游戏中')

$rej = New-Client 'Bob'
SendLine $rej ('REJOIN|' + $roomId + '|2')
$r = RecvUntil $rej 'REJOIN_FAIL' 3000
Check 'A12 游戏进行中 REJOIN 被拒（游戏仍在进行中）' ($r -match '游戏仍在进行中')
Close-Client $rej

# 中止本局：机器人 1 断线触发服务端重连等待，新连接发 GIVEUP|1 立即中止
# （WaitForReconnect 轮询 giveUp 标志，1-2 秒内生效）
Close-Bot $botA[0]
Start-Sleep -Seconds 2
Send-GiveUp $gport '1'
Start-Sleep -Seconds 5

# 模拟客户端自动重试逻辑：每 5 秒重发 REJOIN（最多 6 次），直到 JOINED
$retry = New-Client 'Bob'
$joined = $false
for ($i = 0; $i -lt 6; $i++) {
    SendLine $retry ('GAME_ENDED|' + $roomId)
    SendLine $retry ('REJOIN|' + $roomId + '|2')
    $rj = RecvUntil $retry 'JOINED' 3000
    if ($rj) { $joined = $true; break }
    Start-Sleep -Seconds 5
}
Check 'A13 中止后模拟客户端自动重试回房成功（JOINED）' $joined

SendLine $L2 ('JOIN|' + $gport)
$r = RecvUntil $L2 'ERROR' 3000
Check 'A14 全员回房前外人 JOIN 被拒（gameEnded 拦截）' ($r -match '该房间正在游戏中')

$C2 = New-Client 'Cathy'
SendLine $C2 ('GAME_ENDED|' + $roomId)
SendLine $C2 ('REJOIN|' + $roomId + '|3')
$r = RecvUntil $C2 'JOINED' 3000
Check 'A15 Cathy 回房成功' ($r -match 'JOINED')
$D2 = New-Client 'Dave'
SendLine $D2 ('GAME_ENDED|' + $roomId)
SendLine $D2 ('REJOIN|' + $roomId + '|4')
$r = RecvUntil $D2 'JOINED' 3000
Check 'A16 Dave 回房成功' ($r -match 'JOINED')

# 原房主最后回房：hostPid=1 强制槽 0，isAdmin 恢复，ADMIN| 只发给他
$A2 = New-Client 'Alice'
SendLine $A2 ('GAME_ENDED|' + $roomId)
SendLine $A2 ('REJOIN|' + $roomId + '|1')
$r = RecvUntil $A2 'JOINED' 3000
$ad = RecvUntil $A2 'ADMIN' 2000
Check 'A17 原房主最后回房成功并恢复房主（ADMIN）' (($r -match 'JOINED') -and ($ad -match 'ADMIN'))

$noAdmin = $true
foreach ($cl in @($retry, $C2, $D2)) {
    $drain = RecvAll $cl 500
    if ($drain.Contains('ADMIN')) { $noAdmin = $false }
}
Check 'A18 先前回房的非房主未收到 ADMIN（槽 0 无顶替）' $noAdmin

SendLine $L2 ('JOIN|' + $gport)
$r = RecvUntil $L2 'JOINED' 3000
Check 'A19 全员回房后外人 JOIN 成功' ($r -match 'JOINED')
Close-Client $L2
Start-Sleep -Milliseconds 500

# 重复 REJOIN：回房后再断开再回，正常且 STATUS 不超员
Close-Client $retry
Start-Sleep -Milliseconds 400
$retry2 = New-Client 'Bob'
SendLine $retry2 ('GAME_ENDED|' + $roomId)
SendLine $retry2 ('REJOIN|' + $roomId + '|2')
$r = RecvUntil $retry2 'JOINED' 3000
Check 'A20 重复 REJOIN（断开再回）成功' ($r -match 'JOINED')
SendLine $A2 'STATUS'
$st = Recv-Status $A2 3000
# 竖排表：表头 + 4 个玩家数据行（§16.5）
$stRows = @($st -split "`n" | Where-Object { $_ -match '^\s*\d+\s+\| ' })
Check 'A21 回房后 STATUS 4 人不超员' (
    $st -and ($stRows.Count -ge 4) -and $st.Contains('Alice') -and $st.Contains('Bob') -and $st.Contains('Cathy') -and $st.Contains('Dave'))

# 可再开一局：GAME_ENDED 清 ready 时槽位 sock 全无效、未清到，直接再 START
$ok2 = $false
if ($st) {
    foreach ($pair in @(@('Alice', $A2), @('Bob', $retry2), @('Cathy', $C2), @('Dave', $D2))) {
        if ($st -match ('\|' + '\s*' + [regex]::Escape($pair[0]) + '\s*\|')) { SendLine $pair[1] 'READY' }
    }
    Start-Sleep -Milliseconds 800
    SendLine $A2 'START'
    $gp2 = RecvUntil $A2 'GAME_PREPARE|' 6000
    $ok2 = $gp2 -match 'GAME_PREPARE\|'
    foreach ($cl in @($retry2, $C2, $D2)) {
        $r = RecvUntil $cl 'GAME_PREPARE|' 6000
        if (-not ($r -match 'GAME_PREPARE\|')) { $ok2 = $false }
    }
}
Check 'A22 全员回房后可再开一局（再次收到 GAME_PREPARE）' $ok2

foreach ($cl in @($A2, $retry2, $C2, $D2, $L1)) { Close-Client $cl }
foreach ($bt in $botA) { Close-Bot $bt }
Get-Process -Name Server -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 600

# ============ 验收 2：开局失败路径（Server 即死 → WOLF_GAME_WAIT_SECONDS 兜底回滚） ============
# 兜底计时在 Start.exe 启动时读取环境变量，必须重启 Start 注入 6 秒窗口
Stop-Process -Id $rm1.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
$env:WOLF_GAME_WAIT_SECONDS = '6'
$rm2 = Start-RM
$portB = Get-FreePort

$B1 = New-Client 'Amy'
SendLine $B1 ('CREATE|' + $portB)
$r = RecvUntil $B1 'CREATED' 3000
Check 'B1 建房成功（回滚局）' ($r -match 'CREATED')
$B2 = New-Client 'Ben'
SendLine $B2 ('JOIN|' + $portB)
$null = RecvUntil $B2 'JOINED' 3000
$B3 = New-Client 'Cara'
SendLine $B3 ('JOIN|' + $portB)
$null = RecvUntil $B3 'JOINED' 3000
$B4 = New-Client 'Dan'
SendLine $B4 ('JOIN|' + $portB)
$null = RecvUntil $B4 'JOINED' 3000

SendLine $B1 'VILLAGER|1'
$null = RecvUntil $B1 '村民职业已启用' 2000
SendLine $B1 'RATIO|1|0|2'
$null = RecvUntil $B1 '比例已设为' 2000
foreach ($cl in @($B1, $B2, $B3, $B4)) { SendLine $cl 'READY' }
Start-Sleep -Seconds 1
SendLine $B1 'START'
$r = RecvUntil $B1 'GAME_PREPARE|' 6000
Check 'B2 START 成功（GAME_PREPARE 收到）' ($r -match 'GAME_PREPARE\|')

# Server 进程即死且玩家全部离开大厅：房间不销毁，等 6 秒兜底回滚 gameStarted
Get-Process -Name Server -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
foreach ($cl in @($B1, $B2, $B3, $B4)) { Close-Client $cl }
Start-Sleep -Seconds 9

$W = New-Client 'Wanda'
SendLine $W 'LIST'
$r = RecvUntil $W 'ROOMS_LIST' 3000
Check 'B3 Server 即死后房间保留并回滚（LIST 无 [游戏中] 标记）' (
    $r -and ($r -match ([regex]::Escape($portB) + '\s+4/12')) -and (-not $r.Contains('[游戏中]')))

$X = New-Client 'Xavier'
SendLine $X ('JOIN|' + $portB)
$r = RecvUntil $X 'JOINED' 3000
Check 'B4 回滚后玩家可重新加入房间' ($r -match 'JOINED')
foreach ($cl in @($W, $X)) { Close-Client $cl }

Remove-Item Env:\WOLF_GAME_WAIT_SECONDS -ErrorAction SilentlyContinue
Stop-Process -Id $rm2.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
$rm3 = Start-RM

# ============ 验收 8：夜晚阶段开场广播（6 人直连局） ============
$portC = Get-FreePort
$ng = Run-NightGame $portC
$botsN = $ng.bots
$rolesN = @()
foreach ($xb in $botsN) { if ($xb.role) { $rolesN += $xb.role } }
$gb = $botsN | Where-Object { $_.role -eq 'guard' }
$wb = $botsN | Where-Object { $_.role -eq 'werewolf' }
$sb = $botsN | Where-Object { $_.role -eq 'seer' }
$wi = $botsN | Where-Object { $_.role -eq 'witch' }
Check 'C1 6 机器人身份齐（守卫/狼/预言家/女巫都在场）' (
    ($rolesN.Count -eq 6) -and ($rolesN -contains 'guard') -and ($rolesN -contains 'werewolf') -and ($rolesN -contains 'seer') -and ($rolesN -contains 'witch'))
Check 'C2 全员收到「守卫请睁眼。」广播' (($botsN | Where-Object { $_.bc.guard }).Count -eq 6)
Check 'C3 全员收到「狼人请睁眼。」广播' (($botsN | Where-Object { $_.bc.wolf }).Count -eq 6)
Check 'C4 全员收到「预言家请睁眼。」广播' (($botsN | Where-Object { $_.bc.seer }).Count -eq 6)
Check 'C5 全员收到「女巫请睁眼。」广播' (($botsN | Where-Object { $_.bc.witch }).Count -eq 6)
Check 'C6 「你是守卫…」专属提示只有守卫本人收到' (@($botsN | Where-Object { $_.pt.guard }).Count -eq 1 -and $gb.pt.guard)
Check 'C7 「你是狼人…」专属提示只有狼人本人收到' (@($botsN | Where-Object { $_.pt.wolf }).Count -eq 1 -and $wb.pt.wolf)
Check 'C8 「你是预言家…」专属提示只有预言家本人收到' (@($botsN | Where-Object { $_.pt.seer }).Count -eq 1 -and $sb.pt.seer)
Check 'C9 「你是女巫…」专属提示只有女巫本人收到' (@($botsN | Where-Object { $_.pt.witch }).Count -eq 1 -and $wi.pt.witch)
Check 'C10 夜晚广播不阻塞流程（天亮了到达）' $ng.dawn
Check 'C11 夜晚局进程未崩溃' (-not $ng.crashed)

# ============ 验收 9：攻击用例 ============
$portD = Get-FreePort
$D1 = New-Client 'Dora'
SendLine $D1 ('CREATE|' + $portD)
$r = RecvUntil $D1 'CREATED' 3000
Check 'D1 建房成功（攻击局）' ($r -match 'CREATED')
$D2 = New-Client 'Duke'
SendLine $D2 ('JOIN|' + $portD)
$r = RecvUntil $D2 'JOINED' 3000
Check 'D2 Duke 入房' ($r -match 'JOINED')
$D3 = New-Client 'Dana'
SendLine $D3 ('JOIN|' + $portD)
$null = RecvUntil $D3 'JOINED' 3000
$D4 = New-Client 'Dean'
SendLine $D4 ('JOIN|' + $portD)
$null = RecvUntil $D4 'JOINED' 3000

# 开局前先拉黑 Evil（游戏期 BAN 被拒，验收 9 的"被拉黑者回房被拒"需提前入单）
SendLine $D1 'BAN|Evil'
$r = RecvUntil $D1 '已拉黑' 2000
Check 'D3 开局前拉黑 Evil（BAN 入单）' ($r -match '已拉黑')
SendLine $D1 'VILLAGER|1'
$null = RecvUntil $D1 '村民职业已启用' 2000
SendLine $D1 'RATIO|1|0|2'
$null = RecvUntil $D1 '比例已设为' 2000
foreach ($cl in @($D1, $D2, $D3, $D4)) { SendLine $cl 'READY' }
Start-Sleep -Seconds 1
SendLine $D1 'START'
$gpD = RecvUntil $D1 'GAME_PREPARE|' 6000
Check 'D4 攻击局开局成功（GAME_PREPARE）' ($gpD -match 'GAME_PREPARE\|')
$roomD = $gpD.Split('|')[2]

$botD = @()
for ($i = 1; $i -le 4; $i++) { $botD += New-Bot $i $portD }
foreach ($bt in $botD) {
    $r = RecvUntilStream $bt.s 'ROLE|' 10000
    if ($r) { $bt.role = $r.Substring(5) }
}
Check 'D5 机器人直连游戏服成功' (($botD | Where-Object { $_.role }).Count -eq 4)

foreach ($cl in @($D1, $D2, $D3, $D4)) { Close-Client $cl }
Start-Sleep -Milliseconds 500

# 垃圾字节连接：乱码行 + 原始字节 + 心跳，不应影响房间存在与 LIST
$g = New-Client 'Garb'
SendLine $g '%%%%%***'
try { $g.s.Write([byte[]]@(0xFF, 0xFE, 0x00, 0x41, 0x0A), 0, 5) } catch {}
SendLine $g 'GARBAGE|<>"'
SendLine $g 'PING'
Start-Sleep -Seconds 3
Close-Client $g

$V = New-Client 'Vera'
SendLine $V 'LIST'
$r = RecvUntil $V 'ROOMS_LIST' 3000
Check 'D6 垃圾字节连接后房间仍在（LIST 4/12 + [游戏中]）' (
    $r -and ($r -match ([regex]::Escape($portD) + '\s+4/12')) -and $r.Contains('[游戏中]'))

# PING-only 连接 6 秒：纯心跳行不应影响房间
$p = New-Client 'Pina'
Start-Sleep -Seconds 6
Close-Client $p
SendLine $V 'LIST'
$r = RecvUntil $V 'ROOMS_LIST' 3000
Check 'D7 PING-only 连接不影响房间（游戏期间 LIST 正常）' (
    $r -and ($r -match ([regex]::Escape($portD) + '\s+4/12')) -and $r.Contains('[游戏中]'))

# 中止本局（机器人 1 断线 + GIVEUP）
Close-Bot $botD[0]
Start-Sleep -Seconds 2
Send-GiveUp $portD '1'
Start-Sleep -Seconds 5

# 伪造 pid 99（无此槽位）：回落空槽、不崩
$i1 = New-Client 'Intruder'
SendLine $i1 ('GAME_ENDED|' + $roomD)
SendLine $i1 ('REJOIN|' + $roomD + '|99')
$r = RecvUntil $i1 'JOINED' 3000
Check 'D8 伪造 pid 99 回房回落空槽（JOINED、不崩）' ($r -match 'JOINED')

# 伪造房主 pid 1：走 hostPid 匹配路径占槽 0、不崩
$s1 = New-Client 'Sneak'
SendLine $s1 ('GAME_ENDED|' + $roomD)
SendLine $s1 ('REJOIN|' + $roomD + '|1')
$r = RecvUntil $s1 'JOINED' 3000
$ad = RecvUntil $s1 'ADMIN' 2000
Check 'D9 伪造房主 pid 1 占槽 0（JOINED+ADMIN、不崩）' (($r -match 'JOINED') -and ($ad -match 'ADMIN'))
Close-Client $s1
Start-Sleep -Milliseconds 400

# 真实房主 pid 1：伪造者断开后槽 0 sock 已清，回收成功
$D5 = New-Client 'Dora'
SendLine $D5 ('GAME_ENDED|' + $roomD)
SendLine $D5 ('REJOIN|' + $roomD + '|1')
$r = RecvUntil $D5 'JOINED' 3000
$ad = RecvUntil $D5 'ADMIN' 2000
Check 'D10 真实房主回收槽 0（JOINED+ADMIN）' (($r -match 'JOINED') -and ($ad -match 'ADMIN'))

# 伪造撞槽 pid 2（原槽 1 已被 Intruder 占）：回落其他空槽、不崩
$D6 = New-Client 'Duke'
SendLine $D6 ('GAME_ENDED|' + $roomD)
SendLine $D6 ('REJOIN|' + $roomD + '|2')
$r = RecvUntil $D6 'JOINED' 3000
Check 'D11 伪造撞槽 pid 2 回落其他空槽（JOINED、不崩）' ($r -match 'JOINED')

# 被拉黑者回房被拒（拉黑检查在槽位分配前，与 JOIN 同规则）
$e1 = New-Client 'Evil'
SendLine $e1 ('GAME_ENDED|' + $roomD)
SendLine $e1 ('REJOIN|' + $roomD + '|3')
$r = RecvUntil $e1 'REJOIN_FAIL' 3000
Check 'D12 被拉黑者回房被拒（REJOIN_FAIL 含拉黑）' ($r -match '拉黑')

# 未知房间 / 格式错误 clean 拒绝
$x1 = New-Client 'Xeno'
SendLine $x1 'REJOIN|NOSUCHROOMXYZ|1'
$r = RecvUntil $x1 'REJOIN_FAIL' 3000
Check 'D13 REJOIN 未知房间 clean 拒绝（房间不存在）' ($r -match '房间不存在')
$y1 = New-Client 'Yuri'
SendLine $y1 ('REJOIN|' + $roomD)
$r = RecvUntil $y1 'REJOIN_FAIL' 3000
Check 'D14 REJOIN 格式错误 clean 拒绝（格式错误）' ($r -match '格式错误')

# 攻击后房间与 LIST 仍正常
SendLine $V 'LIST'
$r = RecvUntil $V 'ROOMS_LIST' 3000
Check 'D15 攻击后 LIST 仍正常（房间在列、不崩）' ($r -match ([regex]::Escape($portD) + '\s+4/12'))

foreach ($cl in @($D5, $D6, $i1, $e1, $x1, $y1, $V)) { Close-Client $cl }
foreach ($bt in $botD) { Close-Bot $bt }
Get-Process -Name Server -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

# ============ 收尾 ============
Stop-Keepalive
foreach ($cl in $script:liveClients.ToArray()) { Close-Client $cl }
Stop-Process -Id $rm3.Id -Force -ErrorAction SilentlyContinue
Kill-All
Remove-Item Env:\WOLF_GAME_WAIT_SECONDS -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Write-Output ('ROUND4  PASS=' + $script:pass + '  FAIL=' + $script:fail)
if ($script:fail -gt 0) {
    Write-Output 'ROUND4  RESULT: FAIL'
    exit 1
}
Write-Output 'ROUND4  RESULT: ALL PASS'
exit 0
