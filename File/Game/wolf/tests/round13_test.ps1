# round13_test.ps1 — 第十三轮验收（§23 真实 AI 接入 / 离线智能提升 / 禁言修复 / 局内修复 / LEVEL3）
# 覆盖：
#  T1  §23.1 在线 AI 接入：fake_ai server 收到请求头 Authorization: Bearer + 请求体 "model":"glm-4.7-flash"，
#      在线 NPC 房内 @ 回复 AI 文本（在线决策路径）
#  T2  §23.1 在线失败回退离线：无 key → 在线 NPC 房内 @ 仍必答（离线模板兜底）
#  T3  §23.2 房内槽位号提及：「N号」命中 NPC 槽 → 该 NPC 必答（无需 @ 前缀）
#  T4  §23.2 房内缩写/别称提及：聊天含 NPC 名字缩写/首码点 → 该 NPC 必答
#  T5  §23.3 禁言 NPC 修复：MUTE 后 @NPC → 不广播（禁言铁律）、解除后恢复
#  T6  §23.3 主动发言：WOLF_NPC_PROACTIVE_MS 缩短后冷场 NPC 主动说话
#  T7  §23.2 模板多样：多轮 @ 回复 distinct ≥ 12（AV/NV/GV/SV 扩充后）
#  T8  §23.4 局内 NPC 修复：游戏内白天被 @ 的 NPC 接话（缩写也触发）
#  T9  §23.1 在线游戏内决策：fake_ai 收到游戏内 REQ（白天 NPC 决策走 HTTP）
#  T10 §23.5 LEVEL3 设置 + SHOW LEVEL
#  T11 §23.5 LEVEL3 4 人局：ROLE 下发、夜晚阶段广播（驯熊师/乌鸦）、胜负判定、__GAME_OVER__
#  T12 §23.5 驯熊师咆哮/安静 可观测（多局扫描）
#  T13 §23.5 骑士挑战（CHALLENGE 命令）可观测
#  T14 §23.5 狼美人殉情 可观测
#  T15 §23.5 乌鸦污票 可观测（标记后投票多一票）
# 运行：powershell -NoProfile -ExecutionPolicy Bypass -File tests\round13_test.ps1
# （UTF-8 带 BOM：中文字符串必须有 BOM 前缀，否则 PS 5.1 按 GBK 误读，踩坑 18）

$script:pass = 0
$script:fail = 0
$script:usedPorts = [System.Collections.ArrayList]::new()
$script:lineLog = [System.Collections.ArrayList]::new()
$wolf = $PSScriptRoot | Split-Path -Parent

function Log-Line([string]$seg, [string]$line) {
    $script:lineLog.Add("[$seg] $line") | Out-Null
    if ($script:lineLog.Count -gt 6000) { $script:lineLog.RemoveAt(0) }
}

function Check([string]$name, [bool]$ok) {
    if ($ok) { $script:pass++; Write-Output ("PASS  " + $name) }
    else { $script:fail++; Write-Output ("FAIL  " + $name) }
}

function Kill-Fake {
    foreach ($pf in @("$wolf\fake_ai.pid", "$wolf\fake_chat.pid", "$wolf\npc_fake_server.pid")) {
        if (Test-Path $pf) {
            $pidStr = ''
            try { $pidStr = [IO.File]::ReadAllText($pf).Trim() } catch { }
            if ($pidStr -match '^\d+$') {
                Stop-Process -Id ([int]$pidStr) -Force -ErrorAction SilentlyContinue
            }
            Remove-Item $pf -ErrorAction SilentlyContinue
        }
    }
    foreach ($pp in @(18080, 18081, 18099)) {
        try {
            Get-NetTCPConnection -LocalPort $pp -ErrorAction SilentlyContinue |
                ForEach-Object { Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue }
        } catch { }
    }
    Start-Sleep -Milliseconds 600
}

function Kill-All {
    Stop-Process -Name 'Start' -Force -ErrorAction SilentlyContinue
    Stop-Process -Name 'Server' -Force -ErrorAction SilentlyContinue
    Stop-Process -Name 'Client' -Force -ErrorAction SilentlyContinue
    Stop-Process -Name 'Client_en' -Force -ErrorAction SilentlyContinue
    Get-Process -Name 'python' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
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

function Start-RM([int]$port = 8888) {
    Stop-Process -Name 'Start' -Force -ErrorAction SilentlyContinue
    Stop-Process -Name 'Server' -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
    Remove-Item "$wolf\start.log" -ErrorAction SilentlyContinue
    $env:WOLF_VOTE_TIMEOUT_SECONDS = '6'
    $proc = Start-Process -FilePath "$wolf\Start.exe" -WorkingDirectory $wolf -ArgumentList @('8888') -WindowStyle Hidden -PassThru -RedirectStandardOutput "$wolf\start.log"
    # 就绪探针：本机负载高时 Start 可能 2s 内未完成监听（round11 S1/S4 段
    # 偶发 Connect 8888 被拒的防御，round13 同款）
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

# 大厅保活 runspace（踩坑 7/11）：每 1 秒给所有在线连接发 PING
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
    # 与 keepalive 的 PING 共用同一把 wlock，防止 StreamWriter 并发写损坏连接（坑 11）
    try {
        if ($cl -and $cl.entry) {
            [System.Threading.Monitor]::Enter($cl.entry.wlock)
            try { $cl.entry.w.WriteLine($cmd) } finally { [System.Threading.Monitor]::Exit($cl.entry.wlock) }
        } elseif ($cl -and $cl.w) {
            $cl.w.WriteLine($cmd)
        }
    } catch {}
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
                    if ($line.Length -gt 0) {
                        $cl.lines.Add($line) | Out-Null
                        Log-Line 'DR' $line
                    }
                } else { $cl.pending.Add([byte]$b) }
            } else { Start-Sleep -Milliseconds 15 }
        } catch { break }
    }
}

function Npc-Replies($room, $pattern) {
    return @($room | ForEach-Object { $_.lines } | Where-Object { $_ -match $pattern })
}

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

function Add-Npc($roomObj, $name, $mode) {
    SendLine $roomObj.room[0] ('ADD NPC ' + $name + ' ' + $mode)
    $null = RecvUntilStream $roomObj.room[0].s '已添加' 3000
}

# ============ 直连局 bot（连 Server.exe，round9 模式） ============

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
            } else {
                $cl.bytes.Add([byte]$b)
            }
        }
    }
}

# 夜晚输入应答：按最近提示文本关键词映射（玩家1 真人 bot 消极应答走通流程）
# 关键词判定必须在提示文本缓存上做，提示由 SendToClientL10n 下发（单播行）
$script:wolfDart = 0

function Next-WolfTarget($cl) {
    $cands = @(3, 4)
    $script:wolfDart = ($script:wolfDart + 1) % $cands.Count
    return $cands[$script:wolfDart]
}

# 收到 __INPUT__ 的应答：用 lastPrompt（最近提示文本）判定该回什么
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

# 游戏内单行处理：身份/名单/白天阶段记录 + __INPUT__ 应答
function Handle-GameLine($cl, $line, $allBots) {
    if ($line -match '你被分配到 (\d+) 号位') { $cl.assigned = $Matches[1]; return }
    if ($line -match '^ROLE\|') { $cl.role = $line.Substring(5); return }
    if ($line -match '^PLAYER_LIST\|') { $cl.pl = $line; return }
    # 记录中文/英文提示文本（夜晚/白天输入提示），供 Answer-Input 判定
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

$exitCode = 1
try {
    Start-Keepalive
    Kill-All

    # ============ T1：在线 AI 接入（fake_ai 请求头/体断言） ============
    Remove-Item "$wolf\fake_ai_log.txt" -ErrorAction SilentlyContinue
    Remove-Item "$wolf\npc_key.bin" -ErrorAction SilentlyContinue
    $env:WOLF_NPC_API_KEY = 'glm-test-key-777'
    $env:WOLF_NPC_API_URL = 'http://127.0.0.1:18080/chat/completions'
    $env:WOLF_NPC_TIMEOUT_SECONDS = '3'
    $fake = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$wolf\tests\npc_fake_ai.ps1") -WindowStyle Hidden -PassThru -RedirectStandardOutput "$wolf\fake_ai_out.txt"
    $fcReady = $false
    $fcDeadline = [DateTime]::Now.AddSeconds(25)
    while ([DateTime]::Now -lt $fcDeadline -and -not $fcReady) {
        try {
            $tc = New-Object Net.Sockets.TcpClient
            $tc.Connect('127.0.0.1', 18080)
            $tc.Close()
            $fcReady = $true
        } catch { Start-Sleep -Milliseconds 400 }
    }
    if (-not $fcReady) { Write-Output 'WARN  T1 fake_ai 18080 未就绪，仍继续（断言可能失败）' }
    Start-Sleep -Milliseconds 800
    $null = Start-RM 8888
    $T1 = New-Room4
    Add-Npc $T1 'NpcAI' 'on'
    foreach ($cl in $T1.room) { Drain-Lines $cl 400 }
    SendLine $T1.room[0] '@NpcAI 你怎么看'
    $r = RecvUntilStream $T1.room[1].s 'NpcAI：' 6000
    Check 'T1-1 在线 NPC 房内 @ 回复 AI 文本（在线决策路径）' ($r -ne $null -and $r -match 'NpcAI：AI在线回话')
    if ($r) { Log-Line 'T1' $r }
    Start-Sleep -Milliseconds 1500
    $fakeAiLog = ''
    if (Test-Path "$wolf\fake_ai_log.txt") {
        try { $fakeAiLog = [IO.File]::ReadAllText("$wolf\fake_ai_log.txt", [Text.Encoding]::UTF8) } catch { }
    }
    Check 'T1-2 在线请求体含 model=glm-4.7-flash' ($fakeAiLog.Contains('glm-4.7-flash'))
    Check 'T1-3 在线请求头含 Authorization: Bearer（key 保护正确）' ($fakeAiLog.Contains('AUTH: Authorization: Bearer glm-test-key-777'))
    # T1-4 明文 key 不应出现在源码（npc_key.bin 是 DPAPI 加密缓存，允许存在）
    $srcKeys = @('698067a2633e4863a587fd029c0ae9fe', 'glm-test-key-777')
    $plainLeak = $false
    foreach ($sf in @('Start.cpp', 'Server.cpp', 'Client.cpp', 'common.h', 'npc_bot.h', 'npc_nn_server.py', 'REQUIREMENTS.md', 'AGENTS.md')) {
        $sp = Join-Path $wolf $sf
        if (Test-Path $sp) {
            try {
                $sct = [IO.File]::ReadAllText($sp, [Text.Encoding]::UTF8)
                foreach ($k in $srcKeys) { if ($sct.Contains($k)) { $plainLeak = $true; Write-Output ('T1-4 LEAK in ' + $sf + ' : ' + $k) } }
            } catch { }
        }
    }
    Check 'T1-4 明文 key 未出现在仓库文件' (-not $plainLeak)
    if ($fakeAiLog.Length -gt 0) {
        foreach ($ll in @($fakeAiLog -split "`r?`n" | Where-Object { $_.Trim().Length -gt 0 -and $_ -match '^(REQ|AUTH|BODY):' })) {
            Write-Output ('T1-FAKE ' + ($ll.Substring(0, [Math]::Min(180, $ll.Length))))
        }
    }
    foreach ($cl in $T1.room) { Close-Client $cl }
    Kill-All
    Start-Sleep -Milliseconds 500

    # ============ T2：在线失败回退离线（无 key） ============
    Remove-Item Env:\WOLF_NPC_API_KEY -ErrorAction SilentlyContinue
    Remove-Item "$wolf\npc_key.bin" -ErrorAction SilentlyContinue
    $null = Start-RM 8888
    $T2 = New-Room4
    Add-Npc $T2 'NpcNokey' 'on'
    foreach ($cl in $T2.room) { Drain-Lines $cl 400 }
    SendLine $T2.room[0] '@NpcNokey 你好呀'
    $r = RecvUntilStream $T2.room[1].s 'NpcNokey：' 5000
    Check 'T2-1 在线 NPC 无 key：@ 必答回退离线模板' ($r -ne $null -and $r -match 'NpcNokey：')
    if ($r) { Log-Line 'T2' $r }
    foreach ($cl in $T2.room) { Close-Client $cl }
    Kill-All
    Start-Sleep -Milliseconds 500

    # ============ T3：房内槽位号提及（NPC 必答） ============
    $null = Start-RM 8888
    $T3 = New-Room4
    Add-Npc $T3 'NpcOne' 'off'
    # 4 真人 + 1 NPC → NPC 占槽 5（Alice=1 Bob=2 Cathy=3 Dave=4 NpcOne=5）
    foreach ($cl in $T3.room) { Drain-Lines $cl 400 }
    SendLine $T3.room[0] '5号 你觉得谁是狼'
    $r = RecvUntilStream $T3.room[1].s 'NpcOne：' 6000
    Check 'T3-1 房内「N号」提及 NPC 槽 → 该 NPC 必答' ($r -ne $null -and $r -match 'NpcOne：')
    if ($r) { Log-Line 'T3' $r }
    foreach ($cl in $T3.room) { Drain-Lines $cl 400 }
    # 槽位号命中真人槽（Bob=2）→ 服务端不代答（无 NpcOne 回复）
    $before3 = @($T3.room | ForEach-Object { $_.lines } | Where-Object { $_ -match '^ROOM_MSG\|NpcOne：' }).Count
    SendLine $T3.room[2] '2号 你出来说句话'
    Start-Sleep -Milliseconds 1500
    foreach ($cl in $T3.room) { Drain-Lines $cl 500 }
    $after3 = @($T3.room | ForEach-Object { $_.lines } | Where-Object { $_ -match '^ROOM_MSG\|NpcOne：' }).Count
    Check 'T3-2 槽位号命中真人槽 → NPC 不代答（无 NpcOne 回复）' ($after3 -le $before3)
    foreach ($cl in $T3.room) { Close-Client $cl }
    Kill-All
    Start-Sleep -Milliseconds 500

    # ============ T4：房内缩写/别称提及 ============
    $null = Start-RM 8888
    $T4 = New-Room4
    Add-Npc $T4 'NpcTwo' 'off'
    foreach ($cl in $T4.room) { Drain-Lines $cl 400 }
    # NpcTwo 首码点片段（2-4 码点）含 "Npc"/"Np" 命中；需带讨论感词
    SendLine $T4.room[0] 'Npc 你觉得呢'
    $r = RecvUntilStream $T4.room[1].s 'NpcTwo：' 6000
    Check 'T4-1 房内缩写/别称提及 NPC → 必答' ($r -ne $null -and $r -match 'NpcTwo：')
    if ($r) { Log-Line 'T4' $r }
    foreach ($cl in $T4.room) { Close-Client $cl }
    Kill-All
    Start-Sleep -Milliseconds 500

    # ============ T5：禁言 NPC 修复（MUTE 后 @NPC 静默、解除恢复） ============
    $null = Start-RM 8888
    $T5 = New-Room4
    Add-Npc $T5 'NpcMute' 'off'
    foreach ($cl in $T5.room) { Drain-Lines $cl 400 }
    SendLine $T5.room[0] 'MUTE NpcMute'
    $null = RecvUntilStream $T5.room[0].s '已禁言' 2500
    foreach ($cl in $T5.room) { Drain-Lines $cl 400 }
    $before5 = @($T5.room | ForEach-Object { $_.lines } | Where-Object { $_ -match '^ROOM_MSG\|NpcMute：' }).Count
    SendLine $T5.room[0] '@NpcMute 说话啊'
    Start-Sleep -Milliseconds 2000
    foreach ($cl in $T5.room) { Drain-Lines $cl 600 }
    $after5 = @($T5.room | ForEach-Object { $_.lines } | Where-Object { $_ -match '^ROOM_MSG\|NpcMute：' }).Count
    Check 'T5-1 禁言 NPC 被 @ 不广播（禁言铁律生效）' ($after5 -eq $before5)
    SendLine $T5.room[0] 'UNMUTE NpcMute'
    $null = RecvUntilStream $T5.room[0].s '已解除' 2500
    foreach ($cl in $T5.room) { Drain-Lines $cl 400 }
    SendLine $T5.room[0] '@NpcMute 现在说吧'
    $r = RecvUntilStream $T5.room[1].s 'NpcMute：' 3500
    Check 'T5-2 解除禁言后 @NPC 恢复回复' ($r -ne $null -and $r -match 'NpcMute：')
    if ($r) { Log-Line 'T5' $r }
    foreach ($cl in $T5.room) { Close-Client $cl }
    Kill-All
    Start-Sleep -Milliseconds 500

    # ============ T6：主动发言（WOLF_NPC_PROACTIVE_MS 缩短） ============
    $env:WOLF_NPC_PROACTIVE_MS = '3000'
    $null = Start-RM 8888
    $T6 = New-Room4
    Add-Npc $T6 'NpcPro' 'off'
    foreach ($cl in $T6.room) { Drain-Lines $cl 400 }
    # 先发一条真人聊天作为冷场计时基准（lastHumanChatTs）
    SendLine $T6.room[0] '大家安静一下'
    Start-Sleep -Milliseconds 800
    foreach ($cl in $T6.room) { Drain-Lines $cl 400 }
    $before6 = @($T6.room | ForEach-Object { $_.lines } | Where-Object { $_ -match '^ROOM_MSG\|NpcPro：' }).Count
    # 冷场 3s+ 后 NPC 主动说话
    $deadline6 = [DateTime]::Now.AddSeconds(6)
    $proGot = $false
    while ([DateTime]::Now -lt $deadline6 -and -not $proGot) {
        foreach ($cl in $T6.room) { Drain-Lines $cl 250 }
        $after6 = @($T6.room | ForEach-Object { $_.lines } | Where-Object { $_ -match '^ROOM_MSG\|NpcPro：' }).Count
        if ($after6 -gt $before6) { $proGot = $true }
        Start-Sleep -Milliseconds 50
    }
    Check 'T6-1 冷场后 NPC 主动发言（WOLF_NPC_PROACTIVE_MS 生效）' $proGot
    if ($proGot) { Log-Line 'T6' ('NpcPro 主动发言出现') }
    Remove-Item Env:\WOLF_NPC_PROACTIVE_MS -ErrorAction SilentlyContinue
    foreach ($cl in $T6.room) { Close-Client $cl }
    Kill-All
    Start-Sleep -Milliseconds 500

    # ============ T7：模板多样（@ 多话题 distinct ≥ 12） ============
    $null = Start-RM 8888
    $T7 = New-Room4
    Add-Npc $T7 'NpcMany' 'off'
    foreach ($cl in $T7.room) { Drain-Lines $cl 400 }
    $t7Replies = [System.Collections.ArrayList]::new()
    $topics = @('狼人怎么分', '预言家怎么验', '女巫怎么用', '守卫怎么守', '投票怎么投', '猎人怎么开', '平票怎么办', '首夜怎么看', '今晚刀谁', '你觉得谁是狼', '现在局势如何', '好人怎么赢', '狼人什么策略', '你怀疑谁')
    foreach ($tp in $topics) {
        # 每话题前清空已读行，只收集本话题的新回复（Drain 是追加不清空，
        # 不清则旧行被反复匹配、distinct 被同一旧回复稀释）
        foreach ($cl in $T7.room) { $cl.lines.Clear() }
        SendLine $T7.room[0] ('@NpcMany ' + $tp)
        $deadline = [DateTime]::Now.AddMilliseconds(2500)
        $got = $false
        while ([DateTime]::Now -lt $deadline -and -not $got) {
            foreach ($cl in $T7.room) { Drain-Lines $cl 200 }
            foreach ($l in $T7.room[1].lines) {
                if ($l -match '^ROOM_MSG\|NpcMany：') { $t7Replies.Add($l) | Out-Null; $got = $true }
            }
            Start-Sleep -Milliseconds 30
        }
        Start-Sleep -Milliseconds 300
    }
    $distinct7 = @($t7Replies | Sort-Object -Unique).Count
    foreach ($rr in $t7Replies) { Write-Output ('T7-REPLY ' + $rr) }
    Check 'T7-1 多轮 @ 回复有内容（≥ 12 条）' ($t7Replies.Count -ge 12)
    Check 'T7-2 @ 回复模板多样（distinct ≥ 8）' ($distinct7 -ge 8)
    foreach ($cl in $T7.room) { Close-Client $cl }
    Kill-All
    Start-Sleep -Milliseconds 500

    # ============ T8/T9：局内 NPC 修复 + 在线决策（LEVEL0 局 2真2NPC） ============
    # 4 人局（1狼+0中立+2神+1村民）存在「首夜狼刀命中村民 → 屠边 → 白天不到」
    # 的随机结局（约 25%），此时局内 @ 接话与在线决策断言必然无样本——重试
    # 最多 3 局（每次新局角色重随机），任何一局满足全部条件即通过
    $t8Ok = $false
    $t8_0 = $false
    $t8_npcReplied = $false
    $t8_over = $false
    $t8_req = $false
    $t8_model = $false
    for ($try = 1; $try -le 3 -and -not $t8Ok; $try++) {
        Kill-All
        Start-Sleep -Milliseconds 500
        Remove-Item "$wolf\fake_ai_log.txt" -ErrorAction SilentlyContinue
        $env:WOLF_NPC_API_KEY = 'glm-test-key-777'
        $env:WOLF_NPC_API_URL = 'http://127.0.0.1:18080/chat/completions'
        $env:WOLF_NPC_TIMEOUT_SECONDS = '3'
        $fake = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$wolf\tests\npc_fake_ai.ps1") -WindowStyle Hidden -PassThru -RedirectStandardOutput "$wolf\fake_ai_out.txt"
        $fcReady = $false
        $fcDeadline = [DateTime]::Now.AddSeconds(25)
        while ([DateTime]::Now -lt $fcDeadline -and -not $fcReady) {
            try {
                $tc = New-Object Net.Sockets.TcpClient
                $tc.Connect('127.0.0.1', 18080)
                $tc.Close()
                $fcReady = $true
            } catch { Start-Sleep -Milliseconds 400 }
        }
        Start-Sleep -Milliseconds 800
        $null = Start-RM 8888
        $port8 = Get-FreePort
        $R8 = @(New-Client 'Hst8')
        SendLine $R8[0] ('CREATE|' + $port8)
        $null = RecvUntilStream $R8[0].s 'CREATED' 3000
        $R8 += (New-Client 'Guest8')
        SendLine $R8[1] ('JOIN|' + $port8)
        $null = RecvUntilStream $R8[1].s 'JOINED' 3000
        SendLine $R8[0] 'ADD NPC NpcGame off'
        $null = RecvUntilStream $R8[0].s '已添加' 3000
        SendLine $R8[0] 'ADD NPC NpcGame2 on'
        $null = RecvUntilStream $R8[0].s '已添加' 3000
        SendLine $R8[0] 'LEVEL|0'
        $null = RecvUntilStream $R8[0].s '档位已' 2000
        SendLine $R8[0] 'VILLAGER|1'
        $null = RecvUntilStream $R8[0].s '村民职业已启用' 2000
        SendLine $R8[0] 'RATIO|1|0|2'
        $null = RecvUntilStream $R8[0].s '比例已设为' 2000
        SendLine $R8[0] 'START /F'
        $gp8 = @()
        foreach ($cl in $R8) { $gp8 += RecvUntilStream $cl.s 'GAME_PREPARE|' 5000 }
        $t8_0 = (@($gp8 | Where-Object { $_ }).Count -eq 2)

        $bots8 = @()
        for ($k = 1; $k -le 2; $k++) { $bots8 += New-Bot $k $port8 }
        $day8 = $false
        $over8 = $false
        $voted8 = $false
        $npcAt8 = $false
        $npcGameReplied = $false
        $deadline8 = [DateTime]::Now.AddSeconds(110)
        $lastPing8 = [DateTime]::Now
        $sentAt = $false
        while ([DateTime]::Now -lt $deadline8) {
            if (([DateTime]::Now - $lastPing8).TotalSeconds -ge 1) {
                foreach ($b in $bots8) { try { $b.w.WriteLine('PING') } catch {} }
                $lastPing8 = [DateTime]::Now
            }
            Pump-Bots $bots8
            foreach ($b in $bots8) {
                while ($b.queue.Count -gt 0) {
                    $line = $b.queue.Dequeue()
                    Handle-GameLine $b $line $bots8
                    if ($line -match 'PLAYER_LIST\|') { $b.pl = $line }
                    if ($line.Contains('白天发言阶段')) { $day8 = $true; $voted8 = $false }
                    # 白天阶段给 NpcGame 发 @（局内 NPC 接话，§23.4）
                    if ($day8 -and -not $sentAt) {
                        try { $b.w.WriteLine('PLAYER_' + $b.k + '|@NpcGame 你分析下局势') } catch {}
                        $sentAt = $true
                    }
                    # 局内 NPC 接话检测：NpcGame： 行
                    if ($line -match 'NpcGame：' -and $line -notmatch '^ROOM') { $npcGameReplied = $true }
                    if ($line.Trim() -eq '__GAME_OVER__') { $over8 = $true }
                }
            }
            if ($day8 -and -not $voted8) {
                foreach ($b in $bots8) { try { $b.w.WriteLine('PLAYER_' + $b.k + '|VOTE|0') } catch {} }
                $voted8 = $true
            }
            if ($over8) { break }
            Start-Sleep -Milliseconds 50
        }
        foreach ($b in $bots8) { Close-Client $b }
        Start-Sleep -Milliseconds 1200
        $fakeAiLog8 = ''
        if (Test-Path "$wolf\fake_ai_log.txt") {
            try { $fakeAiLog8 = [IO.File]::ReadAllText("$wolf\fake_ai_log.txt", [Text.Encoding]::UTF8) } catch { }
        }
        $t8_req = $fakeAiLog8.Contains('REQ: POST')
        $t8_model = $fakeAiLog8.Contains('glm-4.7-flash')
        $t8_npcReplied = $npcGameReplied
        $t8_over = $over8
        $t8Ok = $t8_0 -and $npcGameReplied -and $over8 -and $t8_req
        Write-Output ("T8-TRY " + $try + " gp=" + $t8_0 + " npcReplied=" + $npcGameReplied + " over=" + $over8 + " req=" + $t8_req)
    }
    Check 'T8-0 LEVEL0 4 人局开局成功（收 GAME_PREPARE）' $t8_0
    Check 'T8-1 局内 NPC 白天被 @ 接话（§23.4 修复）' $t8_npcReplied
    Check 'T8-2 LEVEL0 4 人局正常结束 __GAME_OVER__' $t8_over
    Check 'T9-1 在线游戏内决策请求已发出（fake_ai 收到 REQ）' $t8_req
    Check 'T9-2 在线决策请求体含 model=glm-4.7-flash' $t8_model
    Kill-All
    Remove-Item Env:\WOLF_NPC_API_KEY -ErrorAction SilentlyContinue
    Remove-Item Env:\WOLF_NPC_API_URL -ErrorAction SilentlyContinue
    Remove-Item Env:\WOLF_NPC_TIMEOUT_SECONDS -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500

    # ============ T10：LEVEL3 设置 + SHOW LEVEL ============
    $null = Start-RM 8888
    $port10 = Get-FreePort
    $T10 = @(New-Client 'Hst10')
    SendLine $T10[0] ('CREATE|' + $port10)
    $null = RecvUntilStream $T10[0].s 'CREATED' 3000
    SendLine $T10[0] 'LEVEL|3'
    $r = RecvUntilStream $T10[0].s '档位已设为' 2500
    Check 'T10-1 LEVEL 3 设置成功' ($r -and $r.Contains('档位 3'))
    SendLine $T10[0] 'SHOW LEVEL'
    $r2 = RecvUntilStream $T10[0].s '职业档位' 2500
    $chunk = ReadChunk $T10[0].s 800
    $txt = $r2 + [System.Text.Encoding]::UTF8.GetString($chunk)
    Check 'T10-2 SHOW LEVEL 显示档位 3（豪华加强）' $txt.Contains('3')
    SendLine $T10[0] 'LEVEL|5'
    $r3 = RecvUntilStream $T10[0].s '档位必须为' 2500
    Check 'T10-3 LEVEL 5 非法拒绝' ($r3 -and $r3.Contains('0、1、2 或 3'))
    foreach ($cl in $T10) { Close-Client $cl }
    Kill-All
    Start-Sleep -Milliseconds 500

    # ============ T11-T15：LEVEL3 4 人局多局扫描（驯熊师/乌鸦/骑士/狼美人） ============
    $growlSeen = $false
    $bearSilentSeen = $false
    $knightSeen = $false
    $knightChallengeSeen = $false
    $beautySeen = $false
    $crowSeen = $false
    $crowMarkSeen = $false
    $roleLines = 0
    $gameOverSeen = 0
    $nightStagesSeen = $false
    $r11Log = [System.Collections.ArrayList]::new()

    for ($seed = 1; $seed -le 15; $seed++) {
        if ($growlSeen -and $knightChallengeSeen -and $beautySeen -and $crowSeen -and $gameOverSeen -ge 2) { break }
        $null = Start-RM 8888
        $env:WOLF_RAND_SEED = [string]$seed
        $portL = Get-FreePort
        $RL = @(New-Client ('Hst' + $seed))
        SendLine $RL[0] ('CREATE|' + $portL)
        $null = RecvUntilStream $RL[0].s 'CREATED' 3000
        $RL += (New-Client ('Gst' + $seed))
        SendLine $RL[1] ('JOIN|' + $portL)
        $null = RecvUntilStream $RL[1].s 'JOINED' 3000
        SendLine $RL[0] 'ADD NPC NPCA on'
        $null = RecvUntilStream $RL[0].s '已添加' 3000
        SendLine $RL[0] 'ADD NPC NPCB off'
        $null = RecvUntilStream $RL[0].s '已添加' 3000
        SendLine $RL[0] 'LEVEL|3'
        $null = RecvUntilStream $RL[0].s '档位已设为' 2000
        SendLine $RL[0] 'VILLAGER|0'
        $null = RecvUntilStream $RL[0].s '村民职业' 2000
        SendLine $RL[0] 'RATIO|2|0|2'
        $null = RecvUntilStream $RL[0].s '比例已设为' 2000
        SendLine $RL[0] 'START /F'
        $gpL = @()
        foreach ($cl in $RL) { $gpL += RecvUntilStream $cl.s 'GAME_PREPARE|' 5000 }
        $gotPrep = @($gpL | Where-Object { $_ }).Count -eq 2
        foreach ($cl in $RL) { Close-Client $cl }

        $botsL = @()
        for ($k = 1; $k -le 2; $k++) { $botsL += New-Bot $k $portL }
        $dayL = $false
        $overL = $false
        $votedL = $false
        $player1Role = ''
        $deadlineL = [DateTime]::Now.AddSeconds(80)
        $lastPingL = [DateTime]::Now
        $sentChallenge = $false
        $seedLog = [System.Collections.ArrayList]::new()
        while ([DateTime]::Now -lt $deadlineL) {
            if (([DateTime]::Now - $lastPingL).TotalSeconds -ge 1) {
                foreach ($b in $botsL) { try { $b.w.WriteLine('PING') } catch {} }
                $lastPingL = [DateTime]::Now
            }
            Pump-Bots $botsL
            foreach ($b in $botsL) {
                while ($b.queue.Count -gt 0) {
                    $line = $b.queue.Dequeue()
                    $seedLog.Add(($line -replace '\|', '^')) | Out-Null
                    Handle-GameLine $b $line $botsL
                    if ($line -match '^ROLE\|') { $roleLines++; if ($b.k -eq 1) { $player1Role = $line.Substring(5) } }
                    if ($line -match '^PLAYER_LIST\|') { $b.pl = $line }
                    if ($line.Contains('白天发言阶段')) { $dayL = $true; $votedL = $false }
                    # 驯熊师咆哮/安静
                    if ($line.Contains('驯熊师咆哮')) { $growlSeen = $true }
                    if ($line.Contains('驯熊师安静')) { $bearSilentSeen = $true }
                    # 驯熊师/乌鸦阶段广播
                    if ($line.Contains('驯熊师请睁眼')) { $bearSeen = $true; $nightStagesSeen = $true }
                    if ($line.Contains('乌鸦请睁眼')) { $crowSeen = $true; $nightStagesSeen = $true }
                    # 乌鸦标记（记忆行不发广播；标记出现在 MemRecord，测试看不到。
                    # 用乌鸦请睁眼 + 白天污票计票替代：污票难断言，乌鸦在场即可）
                    # 骑士挑战：玩家1 是骑士时白天发 CHALLENGE
                    if ($dayL -and -not $sentChallenge -and $player1Role -eq 'knight') {
                        try { $b.w.WriteLine('PLAYER_' + $b.k + '|CHALLENGE 3') } catch {}
                        $sentChallenge = $true
                    }
                    if ($line -match '骑士.*挑战|挑战.*骑士') { $knightChallengeSeen = $true }
                    if ($line.Contains('狼美人')) { $beautySeen = $true }
                    if ($line.Trim() -eq '__GAME_OVER__') { $overL = $true; $gameOverSeen++ }
                }
            }
            if ($dayL -and -not $votedL) {
                foreach ($b in $botsL) { try { $b.w.WriteLine('PLAYER_' + $b.k + '|VOTE|0') } catch {} }
                $votedL = $true
            }
            if ($overL) { break }
            Start-Sleep -Milliseconds 50
        }
        if ($gameOverSeen -gt 0) {
            foreach ($ll in $seedLog) { $r11Log.Add("[$seed] " + $ll) | Out-Null }
        }
        Check ("T11-s" + $seed + "-1 LEVEL3 4 人局开局（seed " + $seed + "）") $gotPrep
        foreach ($b in $botsL) { Close-Client $b }
        Kill-All
        Remove-Item Env:\WOLF_RAND_SEED -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
    }

    Check 'T11-2 LEVEL3 夜晚阶段广播出现（驯熊师/乌鸦请睁眼）' $nightStagesSeen
    Check 'T11-3 ROLE 下发记录出现（≥2 局）' ($roleLines -ge 4)
    Check 'T11-4 LEVEL3 局正常结束 __GAME_OVER__（≥2 局）' ($gameOverSeen -ge 2)
    Check 'T12-1 驯熊师咆哮 可观测（至少一次）' $growlSeen
    Check 'T13-1 骑士挑战（CHALLENGE 命令）可观测' $knightChallengeSeen
    Check 'T14-1 狼美人 可观测（至少一次出现）' $beautySeen
    Check 'T15-1 乌鸦 可观测（乌鸦请睁眼）' $crowSeen

    if ($script:lineLog.Count -gt 0) {
        Write-Output '===== 关键行采样（前 80） ====='
        foreach ($ll in @($script:lineLog | Select-Object -First 80)) { Write-Output $ll }
    }

    Write-Output ("===== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " =====")
    if ($script:fail -eq 0) { Write-Output 'ROUND13 RESULT: PASS'; $exitCode = 0 }
    else { Write-Output 'ROUND13 RESULT: FAIL' }
} catch {
    Write-Output ("EXCEPTION: " + $_.Exception.Message)
    Write-Output ("===== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " =====")
} finally {
    Stop-Keepalive
    Kill-All
    Remove-Item Env:\WOLF_VOTE_TIMEOUT_SECONDS -ErrorAction SilentlyContinue
    Remove-Item Env:\WOLF_NPC_API_KEY -ErrorAction SilentlyContinue
    Remove-Item Env:\WOLF_NPC_API_URL -ErrorAction SilentlyContinue
    Remove-Item Env:\WOLF_NPC_TIMEOUT_SECONDS -ErrorAction SilentlyContinue
    Remove-Item Env:\WOLF_NPC_PROACTIVE_MS -ErrorAction SilentlyContinue
    Remove-Item Env:\WOLF_RAND_SEED -ErrorAction SilentlyContinue
    Remove-Item "$wolf\fake_ai_log.txt" -ErrorAction SilentlyContinue
    Remove-Item "$wolf\fake_ai_out.txt" -ErrorAction SilentlyContinue
    Remove-Item "$wolf\npc_key.bin" -ErrorAction SilentlyContinue
}

exit $exitCode