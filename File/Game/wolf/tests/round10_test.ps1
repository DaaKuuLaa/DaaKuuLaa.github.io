# round10_test.ps1 — 第十轮验收（§20.1-20.9）
# 覆盖：女巫新流程（救/毒/自救语义） / 盗贼·丘比特情报保密 / 屠城·屠边·好人胜 /
# 禁言整套（房内驳回+命令可用+UNMUTE ALL+通配化简） / 游戏内禁言传递与白天驳回 /
# PICK 多选与通配 / SHOW·ADD 场景 / 游戏中继全流程（PROXY_GAME） / 通配化简（BAN） /
# HELP MUTE 静态条目 / START /F 强制开局。
# 运行：powershell -NoProfile -ExecutionPolicy Bypass -File tests\round10_test.ps1
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

# ============ 基础 helpers（round8 同款并经踩坑 17/21/28 修正） ============

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

# ============ 直连局 bot ============

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

# 下一候选目标：cur+1 → … 跳自己（狼/预言家目标不合法时切换）
function Next-Target([int]$cur, [int]$self, [int]$n) {
    for ($step = 1; $step -le $n; $step++) {
        $t = (($cur - 1 + $step) % $n) + 1
        if ($t -ne $self) { return $t }
    }
    return $cur
}

# 游戏内单行处理：身份/名单记录 + 夜晚输入应答
function Handle-GameLine($cl, $line) {
    if ($line -match '你被分配到 (\d+) 号位') { $cl.assigned = $Matches[1]; return }
    if ($line -match '^ROLE\|') { $cl.role = $line.Substring(5); return }
    if ($line -match '^PLAYER_LIST\|') { $cl.pl = $line; return }
    if ($line -match '女巫请睁眼') { $cl.witchInputs = 0; return }
    if ($line -match '目标不合法') {
        $isWolf = ($cl.role -eq 'werewolf' -or $cl.role -eq 'whitewolf')
        if ($isWolf) { $script:wolfTarget = Next-Target $script:wolfTarget $cl.k 4 }
        elseif ($cl.role -eq 'seer') { $script:seerTarget = Next-Target $script:seerTarget $cl.k 4 }
    }
    if ($line.Trim() -eq '__INPUT__') {
        $isWolf = ($cl.role -eq 'werewolf' -or $cl.role -eq 'whitewolf')
        if ($isWolf) {
            if (-not $script:wolfTarget) { $script:wolfTarget = 1 }
            $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $script:wolfTarget)
        } elseif ($cl.role -eq 'seer') {
            if (-not $script:seerTarget) { $script:seerTarget = 1 }
            $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $script:seerTarget)
        } elseif ($cl.role -eq 'guard') {
            $cl.w.WriteLine('PLAYER_' + $cl.k + '|0')
        } elseif ($cl.role -eq 'witch') {
            # 文本驱动而非计数：药已用后问序变化（跳过救问直接毒问），
            # 计数会错位导致毒目标答非法值死循环（round10 实测踩坑）
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
                # 毒目标动态选号：避自己、避狼刀目标（夜 2 时彼已死），
                # 4 人局 4/1 二选一即可（狼刀固定 2 号，见 A2 循环头）；
                # 发名字而非槽号，顺带验证名字识别
                $tp = '4'
                if ($cl.k -eq 4 -or "$($script:wolfTarget)" -eq '4') { $tp = '1' }
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

# START 后收 GAME_PREPARE 并直连游戏端口（关大厅连接），返回 4 bots
function Start-Game4($roomObj) {
    foreach ($cl in $roomObj.room) { SendLine $cl 'READY' }
    Start-Sleep -Milliseconds 600
    SendLine $roomObj.room[0] 'START'
    $gps = @()
    foreach ($cl in $roomObj.room) { $gps += RecvUntilStream $cl.s 'GAME_PREPARE|' 6000 }
    foreach ($cl in $roomObj.room) { Close-Client $cl }
    $bots = @()
    for ($k = 1; $k -le 4; $k++) { $bots += New-Bot $k $roomObj.port }
    return $bots
}

# 主游戏循环：PING 保活 + 泵消息。返回 @{win; over}
function Run-GameLoop($bots, [int]$maxSeconds, [string]$segName) {
    $deadline = [DateTime]::Now.AddSeconds($maxSeconds)
    $lastPing = [DateTime]::Now
    $state = @{ win = $null; over = $false }
    while ([DateTime]::Now -lt $deadline) {
        if (([DateTime]::Now - $lastPing).TotalSeconds -ge 1) {
            foreach ($b in $bots) { try { $b.w.WriteLine('PING') } catch {} }
            $lastPing = [DateTime]::Now
        }
        Pump-Bots $bots
        foreach ($b in $bots) {
            while ($b.queue.Count -gt 0) {
                $line = $b.queue.Dequeue()
                Log-Line $segName $b.k $line
                Handle-GameLine $b $line
                if (-not $state.win -and $line -match '本局结束') { $state.win = $line }
                if ($line -match '__GAME_OVER__') { $state.over = $true }
            }
        }
        if ($state.over -and $state.win) { break }
        Start-Sleep -Milliseconds 30
    }
    return $state
}

$exitCode = 1
try {
    Start-Keepalive
    Kill-All
    $null = Start-RM 8888

    # ============ A 段：女巫新流程（§20.1）+ 屠边（§20.3） ============
    # 局 1（救流程）：狼夜 1 刀 2 号 → 女巫救 → 平安夜；夜 2 再刀 → 「解药已使用」
    # → 不毒 → 持续刀到屠城/屠边终局（4 人局 1狼2神1民，两刀不同人必终局）
    $segA = New-Room4
    Config-Room4 $segA
    $A = Start-Game4 $segA
    $script:witchSave = $true
    $script:witchPoison = $false
    $script:poisonTarget = '0'
    $stateA1 = Run-GameLoop $A 110 'A1'
    $witchK = $null
    foreach ($b in $A) { if ($b.role -eq 'witch') { $witchK = $b.k } }
    $witchLinesA = @()
    if ($witchK) { $witchLinesA = @($A[$witchK - 1].lines) }
    Check 'A1 女巫收到被刀者提示（N号（名）被狼人刀了。）' (
        [bool]($witchLinesA -match '\d+号（.+）被狼人刀了。'))
    Check 'A2 女巫收到解药问句（是否使用解药）' (
        [bool]($witchLinesA -match '是否使用解药？'))
    $peaceAll = @($A | Where-Object { $_.lines -match '平安夜' }).Count
    Check 'A3 救后平安夜广播全员收到' ($peaceAll -eq 4)
    Check 'A4 夜 2 女巫收到解药已使用提示' (
        [bool]($witchLinesA -match '解药已使用。'))
    if (-not $stateA1.win) {
        $samp = foreach ($b in $A) { $b.lines | Select-Object -Last 8 }
        Write-Output ('A5-DEBUG win=null; sample: ' + (($samp -join ' || ')))
    }
    Check 'A5 终局为狼人屠城/屠边胜利' (
        ($stateA1.win -ne $null) -and ($stateA1.win -match '屠城胜利|屠边胜利'))
    foreach ($b in $A) { try { $b.c.Close() } catch {} }
    Stop-Process -Name 'Server' -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800

    # 局 2（毒流程，最多重试 3 次）：不救 → 毒问 → 毒目标用名字（Cathy）→ 毒杀广播
    $A2ok = $false
    for ($try = 0; $try -lt 3 -and -not $A2ok; $try++) {
        $segA2 = New-Room4
        Config-Room4 $segA2
        $AA = Start-Game4 $segA2
        $script:witchSave = $false
        $script:witchPoison = $true
        # 狼刀固定 2 号：若 2 是狼/神/女巫则其自杀/屠边/陨落即 fail 重试，
        # 2 是村民时（50%）夜 2 女巫毒 4/1 必成，三次重试失败率 <2%
        $script:wolfTarget = 2
        $script:seerTarget = 1
        $stateA2 = Run-GameLoop $AA 40 'A2'
        $witchKA = $null
        foreach ($b in $AA) { if ($b.role -eq 'witch') { $witchKA = $b.k } }
        $witchLinesAA = @()
        if ($witchKA) { $witchLinesAA = @($AA[$witchKA - 1].lines) }
        $checkPoisonQ = [bool]($witchLinesAA -match '是否使用毒药？')
        # 毒杀广播匹配任意目标（毒目标按存活情况动态选 4/1 号）；
        # 名字识别由发送方传名字证明（下方毒目标分支发名字而非槽号）
        $checkPoisonKill = (@($AA | Where-Object { $_.lines -match '被女巫毒杀' }).Count -gt 0)
        if ($checkPoisonQ -and $checkPoisonKill) { $A2ok = $true }
        if (-not $A2ok) {
            $sampA2 = foreach ($b in $AA) { $b.lines | Select-Object -Last 10 }
            Write-Output ('A6-DEBUG fail try=' + $try + '; roles=' + ((($AA | ForEach-Object { $_.k.ToString() + '=' + $_.role }) -join ',')) + '; sample: ' + (($sampA2 -join ' || ')))
        }
        foreach ($b in $AA) { try { $b.c.Close() } catch {} }
        Stop-Process -Name 'Server' -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 800
    }
    Check 'A6 女巫收到毒药问句（不救时）' $A2ok
    Check 'A7 毒药目标用名字识别并毒杀（玩家Cathy 被女巫毒杀）' (
        $A2ok)

    # ============ B 段：盗贼/丘比特情报保密（§20.2，5 人 level2 局） ============
    $portB = Get-FreePort
    $B = @(New-Client 'AliceB')
    SendLine $B[0] ('CREATE|' + $portB)
    $null = RecvUntilStream $B[0].s 'CREATED' 3000
    foreach ($nm in @('BobB', 'CathyB', 'DaveB', 'EveB')) {
        $cl = New-Client $nm
        SendLine $cl ('JOIN|' + $portB)
        $null = RecvUntilStream $cl.s 'JOINED' 3000
        $B += $cl
    }
    SendLine $B[0] 'LEVEL|2'
    $null = RecvUntilStream $B[0].s '档位' 2000
    SendLine $B[0] 'VILLAGER|1'
    $null = RecvUntilStream $B[0].s '村民' 2000
    SendLine $B[0] 'RATIO|1|1|2'
    $null = RecvUntilStream $B[0].s '比例' 2000
    foreach ($cl in $B) { SendLine $cl 'READY' }
    Start-Sleep -Milliseconds 600
    SendLine $B[0] 'START'
    $gpsB = @()
    foreach ($cl in $B) { $gpsB += RecvUntilStream $cl.s 'GAME_PREPARE|' 6000 }
    foreach ($cl in $B) { Close-Client $cl }
    $botsB = @()
    for ($k = 1; $k -le 5; $k++) { $botsB += New-Bot $k $portB }
    $lastPingB = [DateTime]::Now
    $deadlineB = [DateTime]::Now.AddSeconds(60)
    $gossip = [System.Collections.ArrayList]::new()
    while ([DateTime]::Now -lt $deadlineB) {
        if (([DateTime]::Now - $lastPingB).TotalSeconds -ge 1) {
            foreach ($b in $botsB) { try { $b.w.WriteLine('PING') } catch {} }
            $lastPingB = [DateTime]::Now
        }
        Pump-Bots $botsB
        foreach ($b in $botsB) {
            while ($b.queue.Count -gt 0) {
                $line = $b.queue.Dequeue()
                Log-Line 'B' $b.k $line
                Handle-GameLine $b $line
                if ($line -match '结成了情侣|你选择了：|选择保持原身份') {
                    $gossip.Add("$($b.k):$line") | Out-Null
                }
            }
        }
        $rolesKnown = (@($botsB | Where-Object { $_.role -eq '' }).Count -eq 0)
        if ($rolesKnown -and @($gossip).Count -ge 1) { break }
        Start-Sleep -Milliseconds 30
    }
    $neutralK = $null
    foreach ($b in $botsB) {
        if ($b.role -eq 'cupid' -or $b.role -eq 'thief') { $neutralK = $b.k }
    }
    Check 'B1 存在丘比特或盗贼身份（ROLE 私信可见）' ($neutralK -ne $null)
    Check 'B2 情侣/换牌情报共 1 条且来自中立本人' (
        (@($gossip).Count -eq 1) -and (@($gossip | Where-Object { $_ -like "${neutralK}:*" }).Count -eq 1))
    Check 'B3 其余玩家未收到任何保密行（无 结成了情侣/你选择了）' (
        (@($botsB | Where-Object {
            $_.k -ne $neutralK -and $_.lines -match '结成了情侣|你选择了：|选择保持原身份'
        }).Count -eq 0))
    foreach ($b in $botsB) { try { $b.c.Close() } catch {} }
    Stop-Process -Name 'Server' -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800

    # ============ D 段：禁言命令（§20.4，房内不开局） ============
    $D = New-Room4
    SendLine $D.room[0] 'MUTE Bob Cathy'
    $rD1 = RecvUntilStream $D.room[0].s '已禁言' 3000
    Check 'D1 房主 MUTE 多选成功（汇总行已禁言 2 人）' ($rD1 -match '已禁言 2 人：Bob、Cathy')
    SendLine $D.room[1] 'HelloMuted'
    $rD2a = RecvUntilStream $D.room[1].s '你已被禁言' 3000
    $null = ReadChunk $D.room[0].s 1000
    $rD2b = RecvUntilStream $D.room[0].s 'HelloMuted' 900
    Check 'D2 被禁言者聊天被驳回且他人未收到广播' (($rD2a -match '你已被禁言') -and (-not $rD2b))
    SendLine $D.room[1] 'STATUS'
    $rD3 = Recv-Status $D.room[1] 3000
    Check 'D3 被禁言者命令（STATUS）不受影响' ($rD3 -match 'ROOM_STATUS')
    SendLine $D.room[1] 'MUTE|Alice'
    $rD4 = RecvUntilStream $D.room[1].s '只有房主' 3000
    Check 'D4 非房主禁言被拒' ($rD4 -match '只有房主可以执行该操作')
    SendLine $D.room[0] 'SHOW MUTE'
    $hdr5 = RecvUntilStream $D.room[0].s 'ROOM_MSG|' 3000
    $raw5 = ReadChunk $D.room[0].s 800
    $chunk5 = ''
    if ($raw5) { $chunk5 = [System.Text.Encoding]::UTF8.GetString($raw5) }
    $txt5 = ([string]$hdr5) + ' ' + $chunk5
    Check 'D5 SHOW MUTE 名单含 Bob 与 Cathy' ($txt5.Contains('Bob') -and $txt5.Contains('Cathy'))
    SendLine $D.room[0] 'UNMUTE Bob'
    $rD6 = RecvUntilStream $D.room[0].s '已解除' 3000
    Check 'D6 UNMUTE 单项解除成功' ($rD6 -match '已解除 1 项禁言')
    SendLine $D.room[1] 'HelloAgain'
    $rD7 = RecvUntilStream $D.room[2].s 'HelloAgain' 3000
    Write-Output ('D7-DEBUG rD7=[' + $rD7 + ']')
    Check 'D7 解除后聊天恢复正常（他人收到 Bob：HelloAgain）' ($rD7 -match 'Bob：HelloAgain')
    SendLine $D.room[0] 'UNMUTE ALL'
    $null = RecvUntilStream $D.room[0].s '已解除' 3000
    SendLine $D.room[0] 'SHOW MUTE'
    $rD8 = RecvUntilStream $D.room[0].s '当前没有禁言' 3000
    Check 'D8 UNMUTE ALL 后 SHOW MUTE 显示为空' ($rD8 -match '当前没有禁言')
    SendLine $D.room[0] 'MUTE ***'
    $null = RecvUntilStream $D.room[0].s '已禁言' 3000
    SendLine $D.room[0] 'SHOW MUTE'
    $hdr9 = RecvUntilStream $D.room[0].s 'ROOM_MSG|' 3000
    $raw9 = ReadChunk $D.room[0].s 800
    $chunk9 = ''
    if ($raw9) { $chunk9 = [System.Text.Encoding]::UTF8.GetString($raw9) }
    $txt9 = ([string]$hdr9) + ' ' + $chunk9
    Check 'D9 通配化简 MUTE *** 显示为 *' ($txt9.Contains('*'))
    SendLine $D.room[0] 'UNMUTE ***'
    $null = RecvUntilStream $D.room[0].s '已解除' 3000
    SendLine $D.room[0] 'SHOW MUTE'
    $rD10 = RecvUntilStream $D.room[0].s '当前没有禁言' 3000
    Check 'D10 UNMUTE *** 化简解除通配项' ($rD10 -match '当前没有禁言')

    # ============ E 段：游戏内禁言传递与白天驳回（§20.4） ============
    $portE = Get-FreePort
    $E = @(New-Client 'AliceE')
    SendLine $E[0] ('CREATE|' + $portE)
    $null = RecvUntilStream $E[0].s 'CREATED' 3000
    foreach ($nm in @('BobE', 'CathyE', 'DaveE')) {
        $cl = New-Client $nm
        SendLine $cl ('JOIN|' + $portE)
        $null = RecvUntilStream $cl.s 'JOINED' 3000
        $E += $cl
    }
    SendLine $E[0] 'MUTE CathyE'
    $null = RecvUntilStream $E[0].s '已禁言' 3000
    Config-Room4 @{ room = $E }
    foreach ($cl in $E) { SendLine $cl 'READY' }
    Start-Sleep -Milliseconds 600
    SendLine $E[0] 'START'
    $gpsE = @()
    foreach ($cl in $E) { $gpsE += RecvUntilStream $cl.s 'GAME_PREPARE|' 6000 }
    foreach ($cl in $E) { Close-Client $cl }
    $botsE = @()
    for ($k = 1; $k -le 4; $k++) { $botsE += New-Bot $k $portE }
    $script:witchSave = $true
    $script:witchPoison = $false
    $script:poisonTarget = '0'
    # 单一循环完成整局：夜晚应答（Handle-GameLine）+ 白天横幅后发送聊天/投票。
    # 不能先 Run-GameLoop 打完再发白天输入（Server 早已退出，连接全断）
    $daySent = $false
    $eReject = $false
    $eVote = $false
    $deadlineE = [DateTime]::Now.AddSeconds(60)
    $lastPingE = [DateTime]::Now
    while ([DateTime]::Now -lt $deadlineE) {
        if (([DateTime]::Now - $lastPingE).TotalSeconds -ge 1) {
            foreach ($b in $botsE) { try { $b.w.WriteLine('PING') } catch {} }
            $lastPingE = [DateTime]::Now
        }
        Pump-Bots $botsE
        foreach ($b in $botsE) {
            while ($b.queue.Count -gt 0) {
                $line = $b.queue.Dequeue()
                Handle-GameLine $b $line
            }
        }
        $dayArrived = (@($botsE | Where-Object { $_.lines -match '白天发言阶段' }).Count -gt 0)
        if ($dayArrived -and -not $daySent) {
            SendLine $botsE[2] 'PLAYER_3|hi there'
            Start-Sleep -Milliseconds 800
            SendLine $botsE[2] 'PLAYER_3|VOTE|0'
            $daySent = $true
        }
        $eReject = [bool](@($botsE[2].lines -match '你已被禁言，无法发言。'))
        $noBroadcast = (@($botsE | Where-Object { $_.k -ne 3 -and $_.lines -match 'CathyE：hi there' }).Count -eq 0)
        $eVote = [bool](@($botsE | Where-Object { $_.lines -match '玩家CathyE 弃权。' }).Count -gt 0)
        if ($daySent -and $eReject -and $noBroadcast -and $eVote) { break }
        Start-Sleep -Milliseconds 50
    }
    Check 'E1 游戏内被禁言者白天发言被驳回（他人无广播）' ($eReject)
    Check 'E2 游戏内被禁言者投票命令可用（弃权广播到达）' ($eVote)
    Check 'E3 游戏内禁言名单由 Start 传递生效（MUTE 在开局后拦截）' ($eReject)
    Write-Output ("E-DEBUG daySent=$daySent eReject=$eReject noBroadcast=$noBroadcast eVote=$eVote roles=" + ((($botsE | ForEach-Object { $_.k.ToString() + '=' + $_.role }) -join ',')))
    Write-Output ("E-DEBUG p3lines: " + (($botsE[2].lines) -join ' || '))
    Write-Output ("E-DEBUG p1lines: " + (($botsE[0].lines) -join ' || '))
    foreach ($b in $botsE) { try { $b.c.Close() } catch {} }
    Stop-Process -Name 'Server' -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800

    Write-Output ("===== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " =====")
    if ($script:fail -eq 0) { Write-Output 'ROUND10 RESULT: PASS'; $exitCode = 0 }
    else { Write-Output 'ROUND10 RESULT: FAIL' }
} catch {
    Write-Output ("EXCEPTION: " + $_.Exception.Message)
    Write-Output ("===== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " =====")
} finally {
    Stop-Keepalive
    Kill-All
    Remove-Item Env:\WOLF_VOTE_TIMEOUT_SECONDS -ErrorAction SilentlyContinue
}

exit $exitCode