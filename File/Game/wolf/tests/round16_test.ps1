# round16_test.ps1 - 第十六轮验收（§25）：
#   A 段：SHIT 彩蛋房内（名字/槽号/*/非法目标/回执/💩 三行）+ SHOW NPCKEY 状态行
#   B 段：游戏内白天 SHIT（4 人局直连，2真2NPC，3 局重试抗首夜屠边）
#   C 段：Python NLP 服务（Start 自动拉起 + @NPC 回复经 NLP + 请求日志落地）
#   D 段：无 NLP 服务时回退 C++ 模板（必答不崩）
# 运行：powershell -NoProfile -ExecutionPolicy Bypass -File tests\round16_test.ps1
# 注意：B 段直连局每 2-3s 后台 PING 保活（Start-Keepalive）
$ErrorActionPreference = 'Stop'
$wolf = $PSScriptRoot | Split-Path -Parent
Set-Location $wolf

$script:usedPorts = [System.Collections.Generic.HashSet[int]]::new()

function Start-RM([int]$port = 8888) {
    Stop-Process -Name 'Start' -Force -ErrorAction SilentlyContinue
    Stop-Process -Name 'Server' -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
    Remove-Item "$wolf\start.log" -ErrorAction SilentlyContinue
    $proc = Start-Process -FilePath "$wolf\Start.exe" -WorkingDirectory $wolf -ArgumentList @('8888') -WindowStyle Hidden -PassThru -RedirectStandardOutput "$wolf\start.log"
    $ready = $false
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 500
        if ($proc.HasExited) { break }
        try {
            $t = New-Object Net.Sockets.TcpClient
            $t.Connect('127.0.0.1', $port)
            $t.Close()
            $ready = $true
            break
        } catch { }
    }
    if (-not $ready) { Write-Output 'WARN: Start-RM 就绪探针超时，Start 未监听' }
    return $proc
}

function Kill-Fake {
    try {
        Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*npc_nlp_server*' } |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    } catch { }
    Start-Sleep -Milliseconds 600
}

function Kill-All {
    Stop-Process -Name 'Start' -Force -ErrorAction SilentlyContinue
    Stop-Process -Name 'Server' -Force -ErrorAction SilentlyContinue
    Stop-Process -Name 'Client' -Force -ErrorAction SilentlyContinue
    Kill-Fake
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
    return @{ c = $c; s = $s; w = $w; name = $name; entry = $entry; lines = [System.Collections.ArrayList]::new(); pending = [System.Collections.Generic.List[byte]]::new() }
}

function Close-Client($cl) {
    if ($cl -and $cl.entry) { $script:keepaliveClients.Remove($cl.entry) | Out-Null }
    try { if ($cl -and $cl.c) { $cl.c.Close() } } catch {}
}

function SendLine($cl, $cmd) {
    try { $cl.w.WriteLine($cmd) } catch {}
}

function Drain-Lines($cl, $timeoutMs) {
    $deadline = [DateTime]::Now.AddMilliseconds($timeoutMs)
    while ([DateTime]::Now -lt $deadline) {
        try {
            if ($cl.s.DataAvailable) {
                $b = $cl.s.ReadByte()
                if ($b -lt 0) { break }
                if ($b -eq 10) {
                    $raw = @($cl.pending.ToArray())
                    $cl.pending.Clear()
                    $line = [System.Text.Encoding]::UTF8.GetString($raw).TrimEnd("`r")
                    if ($line.Length -gt 0) { $cl.lines.Add($line) | Out-Null }
                } else { $cl.pending.Add([byte]$b) }
            } else { Start-Sleep -Milliseconds 15 }
        } catch { break }
    }
}

# ===== 游戏直连 bot（round13 T8 段同款） =====
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
        lastPrompt = ''; lastPromptSet = ''
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
            } else { $cl.bytes.Add([byte]$b) }
        }
    }
}

$script:wolfDart = 0

function Next-WolfTarget($cl) {
    $cands = @(3, 4)
    $script:wolfDart = ($script:wolfDart + 1) % $cands.Count
    return $cands[$script:wolfDart]
}

function Answer-Input($cl) {
    $p = $cl.lastPrompt
    if ($p -match '狼人|刀杀') { return [string](Next-WolfTarget $cl) }
    if ($p -match '乌鸦|标记') { return '1' }
    if ($p -match '预言家|查验') { return '1' }
    if ($p -match '丘比特|两个槽位') { return '1 2' }
    if ($p -match '解药') { return '0' }
    if ($p -match '毒药') { return '0' }
    if ($p -match '守卫|守护') { return '0' }
    if ($p -match '猎人|开枪') { return '0' }
    if ($p -match '殉情|带走') { return '0' }
    if ($p -match '盗贼|选择') { return '0' }
    return '0'
}

function Handle-GameLine($cl, $line, $allBots) {
    if ($line -match '你被分配到 (\d+) 号位') { $cl.assigned = $Matches[1]; return }
    if ($line -match '^ROLE\|') { $cl.role = $line.Substring(5); return }
    if ($line -match '^PLAYER_LIST\|') { $cl.pl = $line; return }
    if ($line -match '你是|请输入|是否使用|请睁眼') {
        if ($line -notmatch '^__') { $cl.lastPrompt = $line; $cl.lastPromptSet = [DateTime]::Now }
        return
    }
    if ($line.Trim() -eq '__INPUT__') {
        $ans = Answer-Input $cl
        try { $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $ans) } catch {}
        return
    }
}

$pass = 0
$fail = 0
function Check($name, $ok) {
    if ($ok) { $script:pass++; Write-Output ("PASS  " + $name) }
    else { $script:fail++; Write-Output ("FAIL  " + $name) }
}

$exitCode = 1
try {
    Start-Keepalive
    Kill-All
    Remove-Item "$wolf\npc_nlp.log" -ErrorAction SilentlyContinue
    Remove-Item Env:WOLF_NPC_API_KEY -ErrorAction SilentlyContinue
    Remove-Item Env:WOLF_NPC_API_URL -ErrorAction SilentlyContinue

    # ============ A 段：SHIT 房内 + SHOW NPCKEY ============
    $null = Start-RM 8888
    Start-Sleep -Milliseconds 600
    $portA = Get-FreePort
    $h = New-Client 'ShitHost'
    SendLine $h ('CREATE|' + $portA)
    $null = RecvUntilStream $h.s 'CREATED' 3000
    Start-Sleep -Milliseconds 400

    SendLine $h 'SHOW NPCKEY'
    $l = RecvUntilStream $h.s 'AI key' 4000
    Check 'A1 SHOW NPCKEY 输出状态行（未配置）' ($null -ne $l)

    SendLine $h 'SHIT 999'
    $l = RecvUntilStream $h.s 'ERROR' 4000
    Check 'A2 SHIT 非法槽号 clean 提示' ($null -ne $l)

    SendLine $h 'SHIT NobodyHere'
    $l = RecvUntilStream $h.s 'ERROR' 4000
    Check 'A3 SHIT 不存在名字 clean 提示' ($null -ne $l)

    $p2 = New-Client 'ShitGuest'
    SendLine $p2 ('JOIN|' + $portA)
    $null = RecvUntilStream $p2.s 'JOINED' 3000
    SendLine $h 'ADD NPC CoolBot off'
    $null = RecvUntilStream $h.s '已添加' 3000
    Start-Sleep -Milliseconds 400

    SendLine $h 'SHIT *'
    $l = RecvUntilStream $h.s '已发给' 4000
    Check 'A4 SHIT * 发送方收生效回执' ($null -ne $l)
    SendLine $h 'SHIT *'
    $l = RecvUntilStream $p2.s '💩' 4000
    Check 'A5 SHIT * 目标收到 💩' ($null -ne $l)

    SendLine $h 'SHIT ShitGuest'
    $l = RecvUntilStream $p2.s '💩' 4000
    Check 'A6 SHIT 名字 目标收到 💩' ($null -ne $l)

    SendLine $h 'SHIT 2'
    $l = RecvUntilStream $p2.s '💩' 4000
    Check 'A7 SHIT 槽号 目标收到 💩' ($null -ne $l)

    # 💩 三行检查（A5-A7 累计）
    Drain-Lines $p2 300
    $shitLines = @($p2.lines | Where-Object { $_ -match '💩' })
    Check 'A8 💩 共三行/次（居中递进）' (@($shitLines).Count -ge 3)

    Close-Client $h
    Close-Client $p2
    Kill-All

    # ============ B 段：游戏内白天 SHIT（4 人局直连，3 局重试） ============
    $b_ok = $false
    $b_shit2 = $false
    $b_shitAll = $false
    $b_shitBad = $false
    $b_over = $false
    for ($try = 1; $try -le 3 -and -not $b_ok; $try++) {
        Kill-All
        Start-Sleep -Milliseconds 500
        $null = Start-RM 8888
        $portB = Get-FreePort
        $RB = @(New-Client 'BotHost')
        SendLine $RB[0] ('CREATE|' + $portB)
        $null = RecvUntilStream $RB[0].s 'CREATED' 3000
        $RB += (New-Client 'BotGuest')
        SendLine $RB[1] ('JOIN|' + $portB)
        $null = RecvUntilStream $RB[1].s 'JOINED' 3000
        SendLine $RB[0] 'ADD NPC NpcGame off'
        $null = RecvUntilStream $RB[0].s '已添加' 3000
        SendLine $RB[0] 'ADD NPC NpcGame2 off'
        $null = RecvUntilStream $RB[0].s '已添加' 3000
        SendLine $RB[0] 'LEVEL|0'
        $null = RecvUntilStream $RB[0].s '档位已' 2000
        SendLine $RB[0] 'VILLAGER|1'
        $null = RecvUntilStream $RB[0].s '村民职业已启用' 2000
        SendLine $RB[0] 'RATIO|1|0|2'
        $null = RecvUntilStream $RB[0].s '比例已设为' 2000
        SendLine $RB[0] 'START /F'
        $gpB = @()
        foreach ($cl in $RB) { $gpB += RecvUntilStream $cl.s 'GAME_PREPARE|' 5000 }
        $gotPrep = (@($gpB | Where-Object { $_ }).Count -eq 2)

        $botsB = @()
        for ($k = 1; $k -le 2; $k++) { $botsB += New-Bot $k $portB }
        $dayHandledB = $false
        $hit2 = $false
        $hitAll = $false
        $hitBad = $false
        $overB = $false
        $deadlineB = [DateTime]::Now.AddSeconds(110)
        while ([DateTime]::Now -lt $deadlineB) {
            Pump-Bots $botsB
            foreach ($cl in $botsB) {
                while ($cl.queue.Count -gt 0) {
                    $line = $cl.queue.Dequeue()
                    if ($line -match 'SHIT|💩|目标') { Write-Output ("B-DBG [" + $cl.k + "] " + $line) }
                    Handle-GameLine $cl $line $botsB
                    if ($line -match '白天发言阶段') { $dayB = $true }
                    if ($line -match '💩 已发给：BotHost') { $hit2 = $true }
                    if ($line -match '💩 已发给：全体（3 人）') { $hitAll = $true }
                    if ($line -match 'SHIT 目标不存在') { $hitBad = $true }
                    if ($line -match '__GAME_OVER__') { $overB = $true }
                }
            }
            if ($dayB -and -not $dayHandledB) {
                $dayHandledB = $true
                # 白天投票窗口开启后同一批测 SHIT 三连（窗口内串行处理）：
                # bot2 槽号与全体、bot1 非法目标；随后两 bot 弃权投票推进对局
                try { $botsB[1].w.WriteLine('PLAYER_2|SHIT 1') } catch {}
                try { $botsB[0].w.WriteLine('PLAYER_1|SHIT 99') } catch {}
                try { $botsB[1].w.WriteLine('PLAYER_2|SHIT *') } catch {}
                foreach ($cl in $botsB) {
                    try { $cl.w.WriteLine('PLAYER_' + $cl.k + '|VOTE|0') } catch {}
                }
            }
            if ($overB) { break }
            Start-Sleep -Milliseconds 30
        }
        $b_shit2 = $b_shit2 -or $hit2
        $b_shitAll = $b_shitAll -or $hitAll
        $b_shitBad = $b_shitBad -or $hitBad
        $b_over = $b_over -or $overB
        $b_ok = $gotPrep -and $b_shit2 -and $b_shitAll -and $b_shitBad -and $b_over
        Write-Output ("B-TRY " + $try + " prep=" + $gotPrep + " shit2=" + $hit2 + " all=" + $hitAll + " bad=" + $hitBad + " over=" + $overB)
        foreach ($cl in $RB) { Close-Client $cl }
        foreach ($cl in $botsB) { try { $cl.c.Close() } catch {} }
    }
    Check 'B1 4 人局开局成功（收 GAME_PREPARE）' $b_ok
    Check 'B2 游戏内 SHIT 槽号 目标收到 💩 + 回执' $b_shit2
    Check 'B3 游戏内 SHIT * 全体回执' $b_shitAll
    Check 'B4 游戏内 SHIT 非法目标 clean 提示' $b_shitBad
    Check 'B5 对局正常结束 __GAME_OVER__' $b_over
    Kill-All

    # ============ C 段：Python NLP 服务（自动拉起 + 生效） ============
    Remove-Item "$wolf\npc_nlp.log" -ErrorAction SilentlyContinue
    Kill-Fake
    $null = Start-RM 8888
    Start-Sleep -Milliseconds 1200
    $logC = Get-Content "$wolf\start.log" -Encoding UTF8 -ErrorAction SilentlyContinue
    Check 'C1 Start 自动拉起 NLP 服务（start.log 有 spawn 记录）' (@($logC | Where-Object { $_ -match 'NLP server spawn' }).Count -ge 1)

    $portC = Get-FreePort
    $hc = New-Client 'NlpHost'
    SendLine $hc ('CREATE|' + $portC)
    $null = RecvUntilStream $hc.s 'CREATED' 3000
    $gc = New-Client 'NlpGuest'
    SendLine $gc ('JOIN|' + $portC)
    $null = RecvUntilStream $gc.s 'JOINED' 3000
    SendLine $hc 'ADD NPC Terse off'
    $null = RecvUntilStream $hc.s '已添加' 3000
    Start-Sleep -Milliseconds 400

    SendLine $hc '@Terse 你觉得谁是狼？'
    $r = RecvUntilStream $gc.s 'Terse：' 5000
    Check 'C2 @NPC 回复到达（NLP 或 C++ 回退皆可）' ($null -ne $r)
    Start-Sleep -Milliseconds 400
    $nlpLogC = ''
    if (Test-Path "$wolf\npc_nlp.log") { $nlpLogC = [IO.File]::ReadAllText("$wolf\npc_nlp.log", [Text.Encoding]::UTF8) }
    Check 'C3 NLP 服务实际收到请求（npc_nlp.log 有记录）' ($nlpLogC.Contains('你觉得谁是狼'))
    Close-Client $hc
    Close-Client $gc
    Kill-All

    # ============ D 段：无 NLP 服务回退 C++ 模板 ============
    Kill-Fake
    $null = Start-RM 8888
    $portD = Get-FreePort
    $hd = New-Client 'FallHost'
    SendLine $hd ('CREATE|' + $portD)
    $null = RecvUntilStream $hd.s 'CREATED' 3000
    SendLine $hd 'ADD NPC CoolBot off'
    $null = RecvUntilStream $hd.s '已添加' 3000
    Start-Sleep -Milliseconds 400
    SendLine $hd '@CoolBot 你好呀'
    $r = RecvUntilStream $hd.s 'CoolBot：' 5000
    Check 'D1 无 NLP 服务时 @NPC 仍必答（C++ 模板回退）' ($null -ne $r)
    $stillAliveD = $false
    Get-Process Start -ErrorAction SilentlyContinue | ForEach-Object { $stillAliveD = $true }
    Check 'D2 Start 进程存活（回退路径不崩）' $stillAliveD
    Close-Client $hd
    Kill-All

    $exitCode = 0
}
catch {
    Write-Output ("EXCEPTION: " + $_.Exception.Message)
}
finally {
    Stop-Keepalive
    Kill-All
}

Write-Output ("===== 结果: PASS=" + $pass + " FAIL=" + $fail + " =====")
if ($fail -eq 0 -and $exitCode -eq 0) { Write-Output 'ROUND16 RESULT: PASS'; exit 0 }
else { Write-Output 'ROUND16 RESULT: FAIL'; exit 1 }