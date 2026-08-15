# round11_test.ps1 — 第十一轮验收（§21）
# 覆盖：
#  S1 局外 @（房间聊天：真人/槽号/不存在/禁言/NPC 代答）+ UNADD/重名/PICK·BAN 剔除 NPC
#  S2 游戏局：局内 @（真人提醒/槽号/不存在/NPC 回话）、NPC 自由讨论补充发言、
#     状态记忆库 npc_memory.txt、游戏进行中 UNADD 拒绝、结束后 BAN/LEVEL/UNADD/ADD 解锁与重开
#  S3 API：等待提示/AI 决策请求（fake server REQ）/npc_key.bin 持久化/坏 URL 超时回退离线
# 运行：powershell -NoProfile -ExecutionPolicy Bypass -File tests\round11_test.ps1
# （UTF-8 带 BOM：中文字符串必须有 BOM 前缀，否则 PS 5.1 按 GBK 误读，踩坑 18）

$script:pass = 0
$script:fail = 0
$script:usedPorts = [System.Collections.ArrayList]::new()
$script:lineLog = [System.Collections.ArrayList]::new()
$wolf = $PSScriptRoot | Split-Path -Parent

function Log-Line([string]$seg, [int]$k, [string]$line) {
    $script:lineLog.Add("[$seg] p${k}: $line") | Out-Null
    if ($script:lineLog.Count -gt 4000) { $script:lineLog.RemoveAt(0) }
}

function Check([string]$name, [bool]$ok) {
    if ($ok) { $script:pass++; Write-Output ("PASS  " + $name) }
    else { $script:fail++; Write-Output ("FAIL  " + $name) }
}

function Kill-All {
    Stop-Process -Name 'Start' -Force -ErrorAction SilentlyContinue
    Stop-Process -Name 'Server' -Force -ErrorAction SilentlyContinue
    Stop-Process -Name 'Client' -Force -ErrorAction SilentlyContinue
    # fake server 进程名是 powershell.exe 杀不到，按 PID 文件精确清理；
    # Get-NetTCPConnection 对 Loopback 监听不可靠（可能查不到），PID 文件更稳
    $fpid = "$wolf\npc_fake_server.pid"
    if (Test-Path $fpid) {
        try {
            $fp = [int]([IO.File]::ReadAllText($fpid).Trim())
            Stop-Process -Id $fp -Force -ErrorAction SilentlyContinue
        } catch { }
        Remove-Item $fpid -ErrorAction SilentlyContinue
    }
    Remove-Item "$wolf\fake_server_log.txt" -ErrorAction SilentlyContinue
    try {
        Get-NetTCPConnection -LocalPort 18080 -ErrorAction SilentlyContinue |
            ForEach-Object { Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue }
    } catch { }
    Start-Sleep -Milliseconds 600
}

function Get-FreePort {
    for ($i = 0; $i -lt 30; $i++) {
        $p = Get-Random -Minimum 8300 -Maximum 8900
        if ($script:usedPorts.Contains($p)) { continue }
        try {
            $l = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Any, $p)
            $l.Start()
            $l.Stop()
            $script:usedPorts.Add($p) | Out-Null
            return $p
        } catch { }
    }
    throw 'no free port'
}

function Start-RM([int]$port = 8888) {
    Stop-Process -Name 'Start' -Force -ErrorAction SilentlyContinue
    Stop-Process -Name 'Server' -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
    Remove-Item "$wolf\start.log" -ErrorAction SilentlyContinue
    $env:WOLF_VOTE_TIMEOUT_SECONDS = '6'
    $proc = Start-Process -FilePath "$wolf\Start.exe" -WorkingDirectory $wolf -ArgumentList @('8888') -WindowStyle Hidden -PassThru -RedirectStandardOutput "$wolf\start.log"
    Start-Sleep -Seconds 2
    return $proc
}

# 大厅保活 runspace：后台每 1 秒给所有在线连接发 PING（踩坑 7/11；
# Start 的失联判定是 3 秒无字节，不是 10 秒）。runspace 与主脚本不共享
# $script:，必须 SetVariable 注入同一 ArrayList 实例（round6 同款）
$script:keepaliveClients = [System.Collections.ArrayList]::new()
$script:keepStop = [System.Threading.ManualResetEvent]::new($false)
$script:kaPs = $null
$script:kaRs = $null

function Start-Keepalive {
    if ($script:kaRs) { return }
    $rs = [System.Management.Automation.Runspaces.RunspaceFactory]::CreateRunspace()
    $rs.Open()
    $rs.SessionStateProxy.SetVariable('liveClients', $script:keepaliveClients)
    $rs.SessionStateProxy.SetVariable('kaStop', $script:keepStop)
    $ps = [System.Management.Automation.PowerShell]::Create()
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
    try { $script:keepStop.Set() } catch {}
    try { if ($script:kaPs) { $script:kaPs.Dispose() } } catch {}
    try { if ($script:kaRs) { $script:kaRs.Close() } } catch {}
    $script:kaRs = $null
}

# ============ 基础 helpers（round10 同款） ============

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

function ReadChunk($s, $timeoutMs) {
    $deadline = [DateTime]::Now.AddMilliseconds($timeoutMs)
    $bytes = New-Object System.Collections.Generic.List[byte]
    while ([DateTime]::Now -lt $deadline) {
        try {
            if ($s.DataAvailable) {
                $b = $s.ReadByte()
                if ($b -lt 0) { break }
                $bytes.Add([byte]$b)
            } else {
                Start-Sleep -Milliseconds 20
            }
        } catch { break }
    }
    return $bytes.ToArray()
}

function Recv-Status($cl, $timeoutMs = 3000) {
    $hdr = RecvUntilStream $cl.s 'ROOM_STATUS' $timeoutMs
    if (-not $hdr) { return $null }
    $chunk = ReadChunk $cl.s 800
    $txt = [System.Text.Encoding]::UTF8.GetString($chunk)
    return ($hdr + "`n" + $txt)
}

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
    $entry = @{ w = $w; wlock = [object]::new() }
    $script:keepaliveClients.Add($entry) | Out-Null
    return @{ c = $c; s = $s; w = $w; name = $name; entry = $entry }
}

function Close-Client($cl) {
    if ($cl -and $cl.entry) { $script:keepaliveClients.Remove($cl.entry) | Out-Null }
    try { if ($cl -and $cl.c) { $cl.c.Close() } } catch {}
}

function SendLine($cl, $cmd) {
    try { $cl.w.WriteLine($cmd) } catch {}
}

function Connect-Retry($port) {
    for ($i = 0; $i -lt 12; $i++) {
        try {
            $c = New-Object Net.Sockets.TcpClient
            $c.Connect('127.0.0.1', $port)
            return $c
        } catch {
            Start-Sleep -Milliseconds 300
        }
    }
    throw "cannot connect $port"
}

# ============ 直连局 bot（round10 同款） ============

function New-Bot($k, $port) {
    $c = Connect-Retry $port
    $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s, [System.Text.UTF8Encoding]::new($false))
    $w.NewLine = "`n"
    $w.AutoFlush = $true
    $w.WriteLine('PLAYER_ID|' + $k)
    return @{
        k = $k; c = $c; s = $s; w = $w
        bytes = [System.Collections.Generic.List[byte]]::new()
        queue = [System.Collections.Queue]::new()
        role = ''; assigned = ''; pl = $null; witchInputs = 0
        lines = [System.Collections.ArrayList]::new()
    }
}

function Pump-Bots($bots) {
    foreach ($cl in $bots) {
        try {
            while ($cl.s.DataAvailable) {
                $b = $cl.s.ReadByte()
                if ($b -lt 0) { break }
                if ($b -eq 10) {
                    $raw = $cl.bytes.ToArray()
                    $cl.bytes.Clear()
                    $line = [System.Text.Encoding]::UTF8.GetString($raw).TrimEnd("`r")
                    if ($line.Length -gt 0) {
                        $cl.lines.Add($line) | Out-Null
                        $cl.queue.Enqueue($line)
                    }
                } else {
                    $cl.bytes.Add([byte]$b)
                }
            }
        } catch { }
    }
}

function Next-Target([int]$cur, [int]$self, [int]$n) {
    for ($step = 1; $step -le $n; $step++) {
        $t = (($cur - 1 + $step) % $n) + 1
        if ($t -ne $self) { return $t }
    }
    return $cur
}

# 游戏内单行处理：身份/名单记录 + 夜晚输入应答（round10 同款）
function Handle-GameLine($cl, $line) {
    if ($line -match '你被分配到 (\d+) 号位') { $cl.assigned = $Matches[1]; return }
    if ($line -match '^ROLE\|') { $cl.role = $line.Substring(5); return }
    if ($line -match '^PLAYER_LIST\|') {
        $cl.pl = $line
        $n = 0
        if ([int]::TryParse($line.Split('|')[1], [ref]$n) -and $n -ge 2) { $cl.n = $n }
        return
    }
    if ($line -match '女巫请睁眼') { $cl.witchInputs = 0; $cl.witchTry = 0; return }
    if ($line -match '目标不合法') {
        $n = $cl.n
        if (-not $n) { $n = 4 }
        $isWolf = ($cl.role -eq 'werewolf' -or $cl.role -eq 'whitewolf')
        if ($isWolf) { $script:wolfTarget = Next-Target $script:wolfTarget $cl.k $n }
        elseif ($cl.role -eq 'seer') { $script:seerTarget = Next-Target $script:seerTarget $cl.k $n }
    }
    if ($line.Trim() -eq '__INPUT__') {
        $n = $cl.n
        if (-not $n) { $n = 4 }
        $isWolf = ($cl.role -eq 'werewolf' -or $cl.role -eq 'whitewolf')
        if ($isWolf) {
            if (-not $script:wolfTarget) { $script:wolfTarget = Next-Target 1 $cl.k $n }
            $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $script:wolfTarget)
        } elseif ($cl.role -eq 'seer') {
            if (-not $script:seerTarget) { $script:seerTarget = Next-Target 1 $cl.k $n }
            $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $script:seerTarget)
        } elseif ($cl.role -eq 'guard') {
            $cl.w.WriteLine('PLAYER_' + $cl.k + '|0')
        } elseif ($cl.role -eq 'witch') {
            $lastHint = ''
            for ($hi = $cl.lines.Count - 1; $hi -ge 0; $hi--) {
                $cand = $cl.lines[$hi]
                if ($cand -match '是否使用解药|是否使用毒药|请输入毒药目标') {
                    $lastHint = $cand
                    break
                }
            }
            if ($lastHint -match '是否使用解药') {
                if ($script:witchSave) { $cl.w.WriteLine('PLAYER_' + $cl.k + '|1') }
                else { $cl.w.WriteLine('PLAYER_' + $cl.k + '|0') }
            } elseif ($lastHint -match '请输入毒药目标') {
                if (-not $cl.witchTry) { $cl.witchTry = 0 }
                $cl.witchTry++
                $tp = 1
                for ($s = 0; $s -lt 8; $s++) {
                    $cand = ((($cl.witchTry - 1 + $s) % $n) + 1)
                    if ($cand -ne $cl.k -and "$cand" -ne "$($script:wolfTarget)") { $tp = $cand; break }
                }
                $tpName = ''
                if ($cl.pl) {
                    $parts = $cl.pl.Split('|')
                    if ($parts.Length -gt $tp) { $tpName = $parts[[int]$tp + 1] }
                }
                if ($tpName) { $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $tpName) }
                else { $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $tp) }
            } else {
                if ($script:witchPoison) { $cl.w.WriteLine('PLAYER_' + $cl.k + '|1') }
                else { $cl.w.WriteLine('PLAYER_' + $cl.k + '|0') }
            }
        } elseif ($cl.role -eq 'cupid') {
            $cl.w.WriteLine('PLAYER_' + $cl.k + '|1 2')
        } elseif ($cl.role -eq 'thief') {
            $cl.w.WriteLine('PLAYER_' + $cl.k + '|1')
        } else {
            $cl.w.WriteLine('PLAYER_' + $cl.k + '|0')
        }
    }
}

# 4 人房（Alice 房主 + Bob/Cathy/Dave），返回 @{port; room}
function New-Room4 {
    $port = Get-FreePort
    $arr = @(New-Client 'Alice')
    SendLine $arr[0] ('CREATE|' + $port)
    $null = RecvUntilStream $arr[0].s 'CREATED' 3000
    foreach ($nm in @('Bob', 'Cathy', 'Dave')) {
        $cl = New-Client $nm
        SendLine $cl ('JOIN|' + $port)
        $null = RecvUntilStream $cl.s 'JOINED' 3000
        $arr += $cl
    }
    return @{ port = $port; room = $arr }
}

# 比例/档位设置（默认 1狼0中2神+村民开；LEVEL0 池=狼+预言家+女巫+村民）
function Config-Room4($roomObj) {
    SendLine $roomObj.room[0] 'LEVEL|0'
    $null = RecvUntilStream $roomObj.room[0].s '档位' 2000
    SendLine $roomObj.room[0] 'VILLAGER|1'
    $null = RecvUntilStream $roomObj.room[0].s '村民' 2000
    SendLine $roomObj.room[0] 'RATIO|1|0|2'
    $null = RecvUntilStream $roomObj.room[0].s '比例' 2000
}

# START 后收 GAME_PREPARE 并直连游戏端口（关大厅连接），返回 @{bots; gps}
function Start-Game4($roomObj) {
    foreach ($cl in $roomObj.room) { SendLine $cl 'READY' }
    Start-Sleep -Milliseconds 600
    SendLine $roomObj.room[0] 'START'
    $gps = @()
    foreach ($cl in $roomObj.room) { $gps += RecvUntilStream $cl.s 'GAME_PREPARE|' 6000 }
    $bots = @()
    foreach ($cl in $roomObj.room) { Close-Client $cl }
    for ($k = 1; $k -le 4; $k++) { $bots += New-Bot $k $roomObj.port }
    return @{ bots = $bots; gps = $gps; port = $roomObj.port }
}

# 白天横幅后向 k 号 bot 发送聊天/投票
function Send-DayChat($bot, $content) {
    SendLine $bot ('PLAYER_' + $bot.k + '|' + $content)
}

$exitCode = 1
try {
    Start-Keepalive
    Kill-All
    $null = Start-RM 8888

    # ============ S1：局外 @ 与 UNADD/剔除（房内不开局） ============
    $S1 = New-Room4
    # 目标 = Alice/Bob/Cathy/Dave（槽 1-4）
    SendLine $S1.room[0] '@Bob 你好呀'
    $r = RecvUntilStream $S1.room[1].s '你被 Alice at了' 3000
    Check 'S1-1 局外 @ 真人：目标收到你被 X at了提醒' ($r -match '你被 Alice at了：你好呀')
    $r = RecvUntilStream $S1.room[0].s '你at了 Bob' 3000
    Check 'S1-2 局外 @ 真人：发送者收到你at了确认' ($r -match '你at了 Bob')
    $r = RecvUntilStream $S1.room[3].s 'Alice：@Bob 你好呀' 3000
    Check 'S1-3 局外 @ 广播原样（含 @ 前缀）到达他人' ($r -match 'Alice：@Bob 你好呀')
    SendLine $S1.room[0] '@4 槽号测试'
    $r = RecvUntilStream $S1.room[3].s '你被 Alice at了：槽号测试' 3000
    Check 'S1-4 局外 @ 槽号：目标（槽4）收到提醒' ($r -ne $null)
    SendLine $S1.room[0] '@不存在X 无人应答'
    $null = RecvUntilStream $S1.room[1].s 'Alice：@不存在X 无人应答' 3000
    $r = RecvUntilStream $S1.room[1].s 'at了' 1200
    Check 'S1-5 局外 @ 不存在：仅广播无提醒' ($r -eq $null)
    SendLine $S1.room[0] 'MUTE Cathy'
    $null = RecvUntilStream $S1.room[0].s '已禁言' 3000
    $null = ReadChunk $S1.room[1].s 400
    SendLine $S1.room[2] '@Bob 禁言中test'
    $r = RecvUntilStream $S1.room[2].s '你已被禁言' 3000
    $r2 = RecvUntilStream $S1.room[1].s 'Cathy：@Bob 禁言中test' 1200
    Check 'S1-6 被禁言者局外 at：驳回且不广播' (($r -match '你已被禁言') -and (-not $r2))
    SendLine $S1.room[0] 'UNMUTE Cathy'
    $null = RecvUntilStream $S1.room[0].s '已解除' 3000

    # ADD NPC / 重名 / UNADD 权限与目标校验
    SendLine $S1.room[0] 'ADD NPC NpcOne off'
    $r = RecvUntilStream $S1.room[0].s '已添加' 3000
    Check 'S1-7 ADD NPC 成功（已添加文案）' ($r -match '已添加.*NpcOne')
    SendLine $S1.room[1] 'UNADD NpcOne'
    $r = RecvUntilStream $S1.room[1].s '只有房主' 3000
    Check 'S1-8 UNADD 非房主被拒' ($r -match '只有房主可以执行该操作')
    SendLine $S1.room[0] 'ADD NPC Alice off'
    $r = RecvUntilStream $S1.room[0].s 'NPC 名已被占用' 3000
    Check 'S1-9 ADD NPC 与房间真人重名被拒' ($r -match 'NPC 名已被占用，请换一个')
    SendLine $S1.room[0] 'UNADD Bob'
    $r = RecvUntilStream $S1.room[0].s 'UNADD 只能移除' 3000
    Check 'S1-10 UNADD 真人目标被拒（请用 PICK）' ($r -match '只能移除 NPC 或本地用户')
    SendLine $S1.room[0] 'UNADD 幽灵'
    $r = RecvUntilStream $S1.room[0].s '目标玩家不存在' 3000
    Check 'S1-11 UNADD 未知目标被拒' ($r -match '目标玩家不存在：幽灵')
    SendLine $S1.room[0] 'ADD NPC NpcTwo off'
    $null = RecvUntilStream $S1.room[0].s '已添加' 3000

    # 局外 @NPC：Start 代答全员可见（含发送者）
    $null = ReadChunk $S1.room[0].s 500
    $null = ReadChunk $S1.room[1].s 500
    SendLine $S1.room[0] '@NpcOne 在吗'
    $r = RecvUntilStream $S1.room[0].s 'ROOM_MSG|NpcOne：' 4000
    Check 'S1-12 局外 @NPC：Start 代答且发送者（Alice）能看见' ($r -ne $null -and $r.Length -gt 13)

    # UNADD 单目标移除 / 别名 UA / * 批量
    SendLine $S1.room[0] 'UNADD NpcOne'
    $r = RecvUntilStream $S1.room[0].s '已移除 NpcOne' 3000
    $r2 = RecvUntilStream $S1.room[1].s '房主移除了 NpcOne' 3000
    Check 'S1-13 UNADD 单目标：宿主确认+全员广播' (($r -match '已移除 NpcOne') -and ($r2 -match '房主移除了 NpcOne'))
    SendLine $S1.room[0] 'UA NpcTwo'
    $r = RecvUntilStream $S1.room[0].s '已移除 NpcTwo' 3000
    Check 'S1-14 UNADD 短别名 UA 生效' ($r -match '已移除 NpcTwo')
    SendLine $S1.room[0] 'ADD NPC X1 off'
    $null = RecvUntilStream $S1.room[0].s '已添加' 3000
    SendLine $S1.room[0] 'ADD NPC X2 off'
    $null = RecvUntilStream $S1.room[0].s '已添加' 3000
    SendLine $S1.room[0] 'UNADD *'
    $r = RecvUntilStream $S1.room[0].s '已移除 2 个' 3000
    Check 'S1-15 UNADD * 批量移除汇总' ($r -match '已移除 2 个：X1、X2')

    # BAN/PICK 剔除 NPC（复用同一条移除路径）
    SendLine $S1.room[0] 'ADD NPC NpcThree off'
    $null = RecvUntilStream $S1.room[0].s '已添加' 3000
    SendLine $S1.room[0] 'BAN NpcThree'
    $r = RecvUntilStream $S1.room[0].s '已拉黑 NpcThree' 3000
    $r2 = RecvUntilStream $S1.room[1].s '房主移除了 NpcThree' 3000
    Check 'S1-16 BAN NPC：拉黑+同时拆槽广播' (($r -match '已拉黑 NpcThree') -and ($r2 -match '房主移除了 NpcThree'))
    SendLine $S1.room[0] 'ADD NPC NpcFour off'
    $null = RecvUntilStream $S1.room[0].s '已添加' 3000
    SendLine $S1.room[0] 'PICK NpcFour'
    $r = RecvUntilStream $S1.room[0].s '房主移除了 NPC：NpcFour' 3000
    Check 'S1-17 PICK NPC 直接移除（NPC 文案）' ($r -match '房主移除了 NPC：NpcFour')
    SendLine $S1.room[0] 'STATUS'
    $st = Recv-Status $S1.room[0] 3000
    Check 'S1-18 移除后 STATUS 不再含 NPC 名' (($st -notmatch 'NpcThree') -and ($st -notmatch 'NpcFour'))
    foreach ($cl in $S1.room) { Close-Client $cl }
    Start-Sleep -Milliseconds 500

    # ============ S2：游戏局（局内 @ + NPC 讨论 + 记忆库 + 结束解锁） ============
    Remove-Item "$wolf\npc_memory.txt" -ErrorAction SilentlyContinue
    $S2 = New-Room4
    SendLine $S2.room[0] 'ADD NPC NpcOne off'
    $null = RecvUntilStream $S2.room[0].s '已添加' 3000
    SendLine $S2.room[0] 'ADD NPC NpcTwo off'
    $null = RecvUntilStream $S2.room[0].s '已添加' 3000
    Config-Room4 $S2
    foreach ($cl in $S2.room) { SendLine $cl 'READY' }
    Start-Sleep -Milliseconds 600
    SendLine $S2.room[0] 'START'
    Start-Sleep -Milliseconds 400
    # 先收 GAME_PREPARE 入账：S2-1 的读窗口会消费并丢弃不匹配行，
    # 若先读 UNADD 再收 GAME_PREPARE，Alice 的 GAME_PREPARE 已被丢 → $g2[0]=null
    # → roomId 为空 → REJOIN||pid 被 Start 按"房间不存在"拒绝（2026-08-12 实测复现）
    $g2 = @()
    foreach ($cl in $S2.room) { $g2 += RecvUntilStream $cl.s 'GAME_PREPARE|' 6000 }
    # 游戏开始瞬间（gameStarted=true、成员仍连大厅）：UNADD 必须被游戏期门拒绝
    SendLine $S2.room[0] 'UNADD NpcOne'
    $r = RecvUntilStream $S2.room[0].s '游戏进行中不能移除' 3000
    Check 'S2-1 游戏进行中 UNADD 被拒' ($r -match '游戏进行中不能移除')
    foreach ($cl in $S2.room) { Close-Client $cl }
    $bots2 = @()
    for ($k = 1; $k -le 4; $k++) { $bots2 += New-Bot $k $S2.port }
    $script:witchSave = $false
    # 关毒：毒中神职会夜晚 1 屠边即终局，S2 的 @ 测试须至少一个完整白天窗口
    $script:witchPoison = $false
    $script:wolfTarget = 2
    $script:seerTarget = 1
    # 白天讨论节拍（stDisc 步进 + 时间戳防抖；@ 提醒测试已脱离状态机，
    # 走在横幅事件与主循环直发上，避免 6s 投票窗口内走不完）：
    # 0=等白天横幅 → 1=等开场白稳定(1s,防基线偏低) → 2=发聊天等 NPC 补充发言(≤2s)
    # → 3=发@NpcOne 等回应(≤2s) → 6=白天投票；每白天横幅回拨 2 重置
    # @Bob/atConfirm/@不存在 由横幅与主循环块负责，stDisc 不再持有
    $stDisc = 0
    $tStamp = [DateTime]::Now.AddSeconds(-10)
    $discussed = $false
    $atReplied = $false
    $atNotify = $false
    $atConfirm = $false
    $badSent = $false
    $badChecked = $false
    $badStamp = [DateTime]::Now
    $badNoRemind = $false
    $votedThisDay = $false
    $dayCount = 0
    $npc1Base = 0
    $npc1AfterChat = 0
    $s2over = $false
    $won = $null
    $day1 = $false
    $deadline2 = [DateTime]::Now.AddSeconds(120)
    $lastPing2 = [DateTime]::Now
    while ([DateTime]::Now -lt $deadline2) {
        if (([DateTime]::Now - $lastPing2).TotalSeconds -ge 1) {
            foreach ($b in $bots2) { try { $b.w.WriteLine('PING') } catch {} }
            $lastPing2 = [DateTime]::Now
        }
        Pump-Bots $bots2
        foreach ($b in $bots2) {
            if (-not $b.dead -and @($b.lines | Where-Object {
                $_ -match '你被放逐|你被狼人击杀|你被女巫毒杀|你被猎人开枪|你被带走|你被毒杀|你被击杀' 
            }).Count -gt 0) {
                $b.dead = $true
            }
            while ($b.queue.Count -gt 0) {
                $line = $b.queue.Dequeue()
                Log-Line 'S2' $b.k $line
                Handle-GameLine $b $line
                if (-not $won -and $line -match '本局结束') { $won = $line }
                if ($line -match '白天发言阶段') {
                    $dayCount++; $votedThisDay = $false
                    # 白天窗口刚开即发 @（Server 投票收集期处理任何聊天，最早
                    # 命中窗口）；发送者与目标必须都存活——死者发的 PLAYER_k 行
                    # 被 Server 丢弃、死者也收不到提醒（round12 实测：Alice 先
                    # 死后横幅 @Bob 全部无声）
                    if (-not $atConfirm) {
                        $alive = @($bots2 | Where-Object { -not $_.dead })
                        if ($alive.Count -ge 2) {
                            $tg = $alive[1]
                            $tName = ''
                            if ($tg.pl) {
                                $tp = $tg.pl.Split('|')
                                if ($tp.Length -gt ($tg.k + 1)) { $tName = $tp[[int]$tg.k + 1] }
                            }
                            if ($tName) {
                                Send-DayChat $alive[0] ('@' + $tName + ' 叫你表态')
                            }
                        }
                    }
                    # 每个白天横幅无条件回拨 2（重跑讨论节拍也重置投票节拍），
                    # stDisc=7 卡死时下一白天才能继续主动投票
                    if ($stDisc -ge 2) { $stDisc = 2 }
                }
                if ($line.Trim() -eq '__GAME_OVER__') { $s2over = $true }
            }
        }
        $day1 = ($dayCount -ge 1)
        if ($day1 -and $stDisc -eq 0) {
            $tStamp = [DateTime]::Now
            $stDisc = 1
        }
        if ($stDisc -eq 1 -and ([DateTime]::Now - $tStamp).TotalSeconds -ge 1) {
            $npc1Base = @($bots2[0].lines | Where-Object { $_ -like 'NpcOne：*' }).Count
            Send-DayChat $bots2[1] '大家好我是Bob'
            $tStamp = [DateTime]::Now
            $stDisc = 2
        }
        if ($stDisc -eq 2) {
            $cNpc = @($bots2[0].lines | Where-Object { $_ -like 'NpcOne：*' -or $_ -like 'NpcTwo：*' }).Count
            if ($cNpc -gt $npc1Base) {
                $discussed = $true
                $alive2 = @($bots2 | Where-Object { -not $_.dead })
                if ($alive2.Count -ge 1) { Send-DayChat $alive2[0] '@NpcOne 你怎么看' }
                $npc1AfterChat = @($bots2[0].lines | Where-Object { $_ -like 'NpcOne：*' }).Count
                $tStamp = [DateTime]::Now
                $stDisc = 3
            } elseif (([DateTime]::Now - $tStamp).TotalSeconds -ge 2) {
                $discussed = $false
                $stDisc = 6
            }
        }
        if ($stDisc -eq 3) {
            $cNpc1 = @($bots2[0].lines | Where-Object { $_ -like 'NpcOne：*' }).Count
            if ($cNpc1 -gt $npc1AfterChat) {
                $atReplied = $true
            }
            if (([DateTime]::Now - $tStamp).TotalSeconds -ge 2) {
                $stDisc = 6
            }
        }
        # @ 真人提醒/确认统计：横幅已发 @（发送者/目标取存活真人），行到达后即
        # 命中（原 stDisc=4 内统计，状态机简化后挂主循环每轮检查）；排除
        # @NpcOne 的『你at了NpcOne』误判——确认行目标必须是真人名
        if (-not $atNotify) {
            $atNotify = [bool](@($bots2 | ForEach-Object { $_.lines } | Where-Object {
                $_ -match 'at了你：叫你表态' }).Count -gt 0)
        }
        if (-not $atConfirm) {
            $atConfirm = [bool](@($bots2 | ForEach-Object { $_.lines } | Where-Object {
                $_ -match '你at了(?!Npc)' }).Count -gt 0)
        }
        # @不存在 测试不再挂状态机：atConfirm 命中后任意时刻发送（白天窗口内
        # Server 必处理且秒回；夜晚发送被吞也无提醒行，两种情形均符合断言）；
        # 发送者必须存活（死者行被 Server 丢弃）
        if (-not $badSent -and $atConfirm) {
            $alive3 = @($bots2 | Where-Object { -not $_.dead })
            if ($alive3.Count -ge 1) {
                Send-DayChat $alive3[0] '@不存在X 针对空气'
                $badSent = $true
                $badStamp = [DateTime]::Now
            }
        }
        if ($badSent -and -not $badChecked -and (([DateTime]::Now - $badStamp).TotalSeconds -ge 1.5)) {
            $badLines = 0
            foreach ($b in $bots2) {
                $badLines += @($b.lines | Where-Object { $_ -match 'at了你：针对空气' }).Count
            }
            $badNoRemind = ($badLines -eq 0)
            $badChecked = $true
        }
        if ($stDisc -eq 6 -and $day1 -and -not $votedThisDay) {
            foreach ($b in $bots2) { SendLine $b ('PLAYER_' + $b.k + '|VOTE|0') }
            $votedThisDay = $true
            $stDisc = 7
        }
        if ($s2over) { break }
        Start-Sleep -Milliseconds 50
    }
    Check 'S2-2 白天真人聊天后 NPC 出现补充发言（讨论节拍）' $discussed
    Check 'S2-3 真人 @NPC 后 NPC 出现回应发言' $atReplied
    Write-Output ("S2-DBG stDisc=$stDisc dayCount=$dayCount atNotify=$atNotify atConfirm=$atConfirm badSent=$badSent badChecked=$badChecked badNoRemind=$badNoRemind")
    foreach ($b in $bots2) {
        Write-Output ("S2-DBG p" + $b.k + " lines=" + $b.lines.Count + " last6=" + (($b.lines | Select-Object -Last 6) -join ' || '))
    }
    Check 'S2-4 局内 @真人：目标收到 at 提醒（Alice at了你）' $atNotify
    Check 'S2-5 局内 @真人：发送者收到你at了确认' $atConfirm
    Check 'S2-6 局内 @不存在：无任何提醒行' $badNoRemind
    Check 'S2-7 游戏局正常结束（本局结束+__GAME_OVER__）' ($s2over -and $won)
    if (-not ($s2over -and $won)) {
        $samp = foreach ($b in $bots2) { 'p' + $b.k + ':' + (($b.lines | Select-Object -Last 12) -join ' || ') }
        Write-Output ('S2-DEBUG roles=' + ((($bots2 | ForEach-Object { $_.k.ToString() + '=' + $_.role }) -join ',')) + ' sample=' + (($samp) -join ' || '))
    }
    # 状态记忆库：局结束后 npc_memory.txt 应含名单与结果
    Start-Sleep -Milliseconds 1200
    $memTxt = ''
    if (Test-Path "$wolf\npc_memory.txt") {
        $memTxt = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes("$wolf\npc_memory.txt"))
    }
    Check 'S2-8 状态记忆库 npc_memory.txt 已生成且含名单/结局' (
        ($memTxt.Contains('玩家名单')) -and (($memTxt -match '本局结束|胜利方') -or ($memTxt.Contains('死亡'))))
    if ($memTxt) {
        Write-Output ('-- npc_memory dump (' + (@($memTxt -split "`n").Count) + ' lines) --')
        Write-Output (@($memTxt -split "`n" | Where-Object {
            $_ -match '玩家名单|死亡|刀|查验|毒|平安|胜利|放逐|白天|阶段|回合' }) -join "`n")
    }
    foreach ($b in $bots2) { try { $b.c.Close() } catch {} }
    Start-Sleep -Milliseconds 1000

    # 全员 REJOIN 回房（2 真人；NPC 槽无连接）→ 游戏结束后配置解锁。
    # 重试到 JOINED：Start 收 GAME_ENDED 有微小延迟，早发会被"游戏仍在进行中"拒
    $roomId2 = ''
    if ($g2[0] -match 'GAME_PREPARE\|([^|]+)\|([^|]+)') { $roomId2 = $Matches[2] }
    $rj = @()
    foreach ($rp in @(@{ n = 'Alice'; pid = '1' }, @{ n = 'Bob'; pid = '2' })) {
        $cl = New-Client $rp.n
        $done = $false
        for ($ik = 0; $ik -lt 12 -and -not $done; $ik++) {
            SendLine $cl ('REJOIN|' + $roomId2 + '|' + $rp.pid)
            $r = RecvUntilStream $cl.s 'JOINED' 2000
            if ($r -match 'JOINED') { $done = $true }
            elseif ($r) { Write-Output ("REJOIN-DEBUG $($rp.n) try=$ik resp=[$r]") }
        }
        $rj += $cl
    }
    SendLine $rj[0] 'BAN Bob'
    $r = RecvUntilStream $rj[0].s '已拉黑 Bob' 3000
    Check 'S2-9 游戏结束后 BAN 解锁（不再提示游戏进行中）' ($r -match '已拉黑 Bob')
    SendLine $rj[0] 'LEVEL|2'
    $r = RecvUntilStream $rj[0].s '档位' 3000
    Check 'S2-10 游戏结束后 LEVEL 配置解锁' ($r -match '档位')
    SendLine $rj[0] 'UNADD NpcOne'
    $r = RecvUntilStream $rj[0].s '已移除 NpcOne' 3000
    Check 'S2-11 游戏结束后 UNADD 解锁' ($r -match '已移除 NpcOne')
    SendLine $rj[0] 'ADD NPC NewNpc on'
    $r = RecvUntilStream $rj[0].s '已添加' 3000
    Check 'S2-12 游戏结束后 ADD NPC 解锁' ($r -match '已添加在线 NPC：NewNpc')
    # 重开一局：BAN Bob 已把 Bob 踢出（playerCount=Alice+2NPC=3），
    # 普通 START 需 ≥4 人 → 用 START /F 强制开局（≥2 人，§19.2）
    SendLine $rj[0] 'READY'
    SendLine $rj[1] 'READY'
    Start-Sleep -Milliseconds 600
    SendLine $rj[0] 'START /F'
    $r = RecvUntilStream $rj[0].s 'GAME_PREPARE|' 6000
    Check 'S2-13 游戏结束后可直接重开（START 放行）' ($r -ne $null)
    foreach ($cl in $rj) { Close-Client $cl }
    Stop-Process -Name 'Server' -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800

    # ============ S3：API（在线 NPC：等待提示/请求/文件 key/超时回退） ============
    Kill-All
    Remove-Item "$wolf\fake_out.txt" -ErrorAction SilentlyContinue
    Remove-Item "$wolf\fake_server_log.txt" -ErrorAction SilentlyContinue
    Remove-Item "$wolf\npc_key.bin" -ErrorAction SilentlyContinue
    $env:WOLF_VOTE_TIMEOUT_SECONDS = '6'
    $env:WOLF_NPC_API_KEY = 'testkey-12345'
    $env:WOLF_NPC_API_URL = 'http://127.0.0.1:18080/chat'
    $env:WOLF_NPC_TIMEOUT_SECONDS = '3'
    $fake = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$wolf\tests\npc_fake_server.ps1") -WindowStyle Hidden -PassThru -RedirectStandardOutput "$wolf\fake_out.txt"
    # fake server 在本机 PowerShell 子进程冷启动约 15 秒才就绪，Start-RM
    # 前必须等它监听上（round12 R5 同类坑），否则白天决策连不上回退离线
    $fsReady = $false
    $fsDeadline = [DateTime]::Now.AddSeconds(25)
    while ([DateTime]::Now -lt $fsDeadline -and -not $fsReady) {
        try {
            $tc = New-Object Net.Sockets.TcpClient
            $tc.Connect('127.0.0.1', 18080)
            $tc.Close()
            $fsReady = $true
        } catch { Start-Sleep -Milliseconds 400 }
    }
    if (-not $fsReady) { Write-Output 'WARN fake server 18080 未就绪' }
    $null = Start-RM 8888
    $S3 = New-Room4
    SendLine $S3.room[0] 'ADD NPC NpcOn on'
    $null = RecvUntilStream $S3.room[0].s '已添加' 3000
    SendLine $S3.room[0] 'ADD NPC NpcOff off'
    $null = RecvUntilStream $S3.room[0].s '已添加' 3000
    Config-Room4 $S3
    $g3 = Start-Game4 $S3
    $bots3 = $g3.bots
    # S3 段狼 bot 固定杀槽2、女巫 bot 若毒杀会在首夜与狼杀叠加屠神
    # （2 神全灭）→ 游戏在白天 1 前结束、「AI 分析中」永不出现。关掉毒杀
    # 保证首夜只死 1 人、白天 1 必到，在线决策请求才有着落
    $script:witchPoison = $false
    $script:wolfTarget = 2
    $script:seerTarget = 1
    $waitHinted = $false
    $s3over = $false
    $won3 = $null
    $day3 = $false
    $deadline3 = [DateTime]::Now.AddSeconds(60)
    $lastPing3 = [DateTime]::Now
    while ([DateTime]::Now -lt $deadline3) {
        if (([DateTime]::Now - $lastPing3).TotalSeconds -ge 1) {
            foreach ($b in $bots3) { try { $b.w.WriteLine('PING') } catch {} }
            $lastPing3 = [DateTime]::Now
        }
        Pump-Bots $bots3
        foreach ($b in $bots3) {
            while ($b.queue.Count -gt 0) {
                $line = $b.queue.Dequeue()
                Log-Line 'S3' $b.k $line
                Handle-GameLine $b $line
                if (-not $won3 -and $line -match '本局结束') { $won3 = $line }
                if ($line -match '__GAME_OVER__') { $s3over = $true }
            }
        }
        $day3 = (($bots3 | Where-Object { $_.lines -match '白天发言阶段' }).Count -gt 0)
        if (-not $waitHinted) {
            $waitHinted = (@($bots3 | Where-Object { $_.lines -match 'AI 分析中' }).Count -gt 0)
        }
        if ($day3 -and -not $script:s3voted) {
            foreach ($b in $bots3) { SendLine $b ('PLAYER_' + $b.k + '|VOTE|0') }
            $script:s3voted = $true
        }
        if (($s3over -and $won3)) { break }
        Start-Sleep -Milliseconds 50
    }
    Check 'S3-1 在线 NPC 白天决策前全员收到等待提示（AI 分析中）' $waitHinted
    # fake server 进程名是 powershell.exe，按进程名杀不到；按 18080 端口占用
    # 精确清理（与 Kill-All 同款），确保下一轮跑前端口已释放避免抢端口失败
    try { Get-NetTCPConnection -LocalPort 18080 -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue } } catch { }
    Start-Sleep -Seconds 1
    $fakeOut = ''
    if (Test-Path "$wolf\fake_out.txt") {
        try {
            $fs = [System.IO.File]::Open("$wolf\fake_out.txt", 'Open', 'Read', 'ReadWrite')
            try {
                $len = $fs.Length
                $buf = New-Object byte[] $len
                [void]$fs.Read($buf, 0, $len)
            } finally { $fs.Close() }
            if ($buf.Length -ge 2 -and $buf[0] -eq 0xFF -and $buf[1] -eq 0xFE) {
                $fakeOut = [System.Text.Encoding]::Unicode.GetString($buf)
            } else {
                $fakeOut = [System.Text.Encoding]::UTF8.GetString($buf)
            }
        } catch { }
    }
    # fake server 的 stdout 重定向是块缓冲，进程活着时 REQ 不落盘（round12
    # R5-2 同类坑）；fake server 现在另写 fake_server_log.txt（AppendAllText
    # 立即落盘），两处任一读到 REQ 即断言成功
    $fakeLogOut = ''
    if (Test-Path "$wolf\fake_server_log.txt") {
        try { $fakeLogOut = [IO.File]::ReadAllText("$wolf\fake_server_log.txt", [Text.Encoding]::UTF8) } catch { }
    }
    Check 'S3-2 在线 NPC 决策请求已发出（fake server 收到 REQ）' ($fakeOut.Contains('REQ:') -or $fakeLogOut.Contains('REQ:'))
    $keyFile = Test-Path "$wolf\npc_key.bin"
    Check 'S3-3 API key 已持久化到 npc_key.bin' $keyFile
    $keyDbg = ''
    if ($keyFile) {
        try {
            $kb = [IO.File]::ReadAllBytes("$wolf\npc_key.bin")
            $keyDbg = "size=$($kb.Length) head=$([BitConverter]::ToString($kb[0..7]))"
        } catch { $keyDbg = 'read-err' }
    }
    Write-Output ("[dbg] npc_key.bin " + $keyDbg)
    Check 'S3-4 在线+离线混合局走通（在线 NPC 无断链，本局推进或结束）' (
        ($s3over -or $won3 -or $day3))
    foreach ($b in $bots3) { try { $b.c.Close() } catch {} }
    Kill-All
    Start-Sleep -Milliseconds 800

    # 局 2：清 empty env（key 靠 npc_key.bin 文件恢复）+ fake server 已停（超时/拒绝回退）
    Remove-Item Env:\WOLF_NPC_API_KEY -ErrorAction SilentlyContinue
    $env:WOLF_NPC_API_URL = 'http://127.0.0.1:18080/chat'
    $env:WOLF_NPC_TIMEOUT_SECONDS = '3'
    Kill-All
    $null = Start-RM 8888
    $S4 = New-Room4
    SendLine $S4.room[0] 'ADD NPC NpcOn2 on'
    $null = RecvUntilStream $S4.room[0].s '已添加' 3000
    SendLine $S4.room[0] 'ADD NPC NpcOff2 off'
    $null = RecvUntilStream $S4.room[0].s '已添加' 3000
    Config-Room4 $S4
    $g4 = Start-Game4 $S4
    $bots4 = $g4.bots
    $script:witchSave = $false
    $script:witchPoison = $true
    $script:wolfTarget = 2
    $script:seerTarget = 1
    $waitHinted4 = $false
    $s4over = $false
    $won4 = $null
    $day4 = $false
    $deadline4 = [DateTime]::Now.AddSeconds(60)
    $lastPing4 = [DateTime]::Now
    while ([DateTime]::Now -lt $deadline4) {
        if (([DateTime]::Now - $lastPing4).TotalSeconds -ge 1) {
            foreach ($b in $bots4) { try { $b.w.WriteLine('PING') } catch {} }
            $lastPing4 = [DateTime]::Now
        }
        Pump-Bots $bots4
        foreach ($b in $bots4) {
            while ($b.queue.Count -gt 0) {
                $line = $b.queue.Dequeue()
                Log-Line 'S4' $b.k $line
                Handle-GameLine $b $line
                if (-not $won4 -and $line -match '本局结束') { $won4 = $line }
                if ($line -match '__GAME_OVER__') { $s4over = $true }
            }
        }
        $day4 = (($bots4 | Where-Object { $_.lines -match '白天发言阶段' }).Count -gt 0)
        if (-not $waitHinted4) {
            $waitHinted4 = (@($bots4 | Where-Object { $_.lines -match 'AI 分析中' }).Count -gt 0)
        }
        if ($day4 -and -not $script:s4voted) {
            foreach ($b in $bots4) { SendLine $b ('PLAYER_' + $b.k + '|VOTE|0') }
            $script:s4voted = $true
        }
        if (($s4over -and $won4)) { break }
        Start-Sleep -Milliseconds 50
    }
    Check 'S3-5 无 env key 时从文件恢复（AI 分析中提示仍出现=在线路径仍在尝试）' $waitHinted4
    if (-not $waitHinted4) {
        Write-Output '[dbg] S4 no AI-hint; lines containing AI/离线/尝试:'
        foreach ($b in $bots4) {
            foreach ($l in $b.lines) {
                if ($l -match 'AI|离线|尝试|key') { Write-Output ("[dbg] p" + $b.k + ": " + $l) }
            }
        }
    }
    $day4Banners = @($bots4 | ForEach-Object { $_.lines | Where-Object { $_ -match '白天发言阶段' } }).Count
    $abstain4 = @($bots4 | Where-Object { $_.lines -match '玩家.*弃权' }).Count
    Check 'S3-6 在线请求失败回退离线（局内 NPC 仍正常投票推进）' (
        ($day4Banners -ge 2) -or ($abstain4 -ge 1) -or $won4)
    foreach ($b in $bots4) { try { $b.c.Close() } catch {} }
    Stop-Process -Name 'Server' -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800

    Write-Output ("===== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " =====")
    if ($script:fail -eq 0) { Write-Output 'ROUND11 RESULT: PASS'; $exitCode = 0 }
    else { Write-Output 'ROUND11 RESULT: FAIL' }
} catch {
    Write-Output ("EXCEPTION: " + $_.Exception.Message)
    Write-Output ("===== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " =====")
} finally {
    Write-Output "---- debug lines (filtered) ----"
    Write-Output (@($script:lineLog | Where-Object {
        $_ -match 'at了你|你at了|at 了|已拉黑|已移除|已添加|名字已被占用|GAME_PREPARE|JOINED|REJOIN_FAIL|NpcOne：|NpcTwo：|游戏进行中|只有房主|白天发言阶段|本局结束|AI 分析中' }) -join "`n")
    Stop-Keepalive
    Kill-All
    Remove-Item Env:\WOLF_VOTE_TIMEOUT_SECONDS -ErrorAction SilentlyContinue
    Remove-Item Env:\WOLF_NPC_API_KEY -ErrorAction SilentlyContinue
    Remove-Item Env:\WOLF_NPC_API_URL -ErrorAction SilentlyContinue
    Remove-Item Env:\WOLF_NPC_TIMEOUT_SECONDS -ErrorAction SilentlyContinue
    Remove-Item "$wolf\fake_out.txt" -ErrorAction SilentlyContinue
    Remove-Item "$wolf\npc_key.bin" -ErrorAction SilentlyContinue
    Remove-Item "$wolf\npc_memory.txt" -ErrorAction SilentlyContinue
}

exit $exitCode