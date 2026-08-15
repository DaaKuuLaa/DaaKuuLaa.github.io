# round12_test.ps1 — 第十二轮验收（§22 房内 NPC 聊天）
# 覆盖：
#  R1 房内 @离线 NPC 必答：单次恰一条（修复重复发声）、多话题多样性（distinct≥2）、词嵌入（含话题词）
#  R2 房内普通聊天相关性：名字出现（85%）、游戏话题词（30%）、纯闲聊（6% 低概率）、2s 频率限制
#  R3 多 NPC 同时在场：聊天同时含两人名 → 两人都可能接话（≥1 回复）
#  R4 极端输入不崩：超长内容、纯标点、管道符注入、@ 不存在的 NPC（只广播不代答）、进程存活
#  R5 在线 NPC 房内对话：fake_chat server 返回 {"reply":"AI房内回话"} → Start 异步取 AI 文本广播
#  R6 在线失败回退离线：无 key → NpcOnlineRoomChat 直接失败 → @ 必答仍由离线模板兜底
#  R7 在线超时回退离线：key 有但 URL 无服务 + 短超时 → 约 1s 内离线回退必答
# 运行：powershell -NoProfile -ExecutionPolicy Bypass -File tests\round12_test.ps1
# （UTF-8 带 BOM：中文字符串必须有 BOM 前缀，否则 PS 5.1 按 GBK 误读，踩坑 18）

$script:pass = 0
$script:fail = 0
$script:usedPorts = [System.Collections.ArrayList]::new()
$script:lineLog = [System.Collections.ArrayList]::new()
$wolf = $PSScriptRoot | Split-Path -Parent

function Log-Line([string]$seg, [string]$line) {
    $script:lineLog.Add("[$seg] $line") | Out-Null
    if ($script:lineLog.Count -gt 4000) { $script:lineLog.RemoveAt(0) }
}

function Check([string]$name, [bool]$ok) {
    if ($ok) { $script:pass++; Write-Output ("PASS  " + $name) }
    else { $script:fail++; Write-Output ("FAIL  " + $name) }
}

function Kill-Fake {
    # fake server 进程名是 powershell.exe 按进程名杀不到；且 Get-NetTCPConnection
    # 对 Loopback 监听不可靠（踩坑 16 同源：查询不到就杀不掉残留）。fake server
    # 自己写 PID 文件（fake_chat.pid/npc_fake_server.pid），按文件精确清理
    foreach ($pf in @("$wolf\fake_chat.pid", "$wolf\npc_fake_server.pid")) {
        if (Test-Path $pf) {
            $pidStr = ''
            try { $pidStr = [IO.File]::ReadAllText($pf).Trim() } catch { }
            if ($pidStr -match '^\d+$') {
                Stop-Process -Id ([int]$pidStr) -Force -ErrorAction SilentlyContinue
            }
            Remove-Item $pf -ErrorAction SilentlyContinue
        }
    }
    try {
        Get-NetTCPConnection -LocalPort 18080 -ErrorAction SilentlyContinue |
            ForEach-Object { Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue }
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

# 大厅保活 runspace：后台每 1 秒给所有在线连接发 PING（踩坑 7/11）
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
    return @{ c = $c; s = $s; w = $w; name = $name; entry = $entry; lines = [System.Collections.ArrayList]::new() }
}

function Close-Client($cl) {
    if ($cl -and $cl.entry) { $script:keepaliveClients.Remove($cl.entry) | Out-Null }
    try { if ($cl -and $cl.c) { $cl.c.Close() } } catch {}
}

function SendLine($cl, $cmd) {
    try { $cl.w.WriteLine($cmd) } catch {}
}

# 从一条已缓存流里收集该客户端至今所有 ROOM_MSG 行（用于重复计数等）
function Client-RoomMsgs($cl) {
    return @($cl.lines | Where-Object { $_ -match '^ROOM_MSG\|' })
}

# 把所有到达的行读入 lines（供 R1 重复计数），timeoutMs 内持续读
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
                        Log-Line 'R' $line
                    }
                } else { $cl.pending.Add([byte]$b) }
            } else { Start-Sleep -Milliseconds 15 }
        } catch { break }
    }
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
    # 每个客户端补 pending 字节缓冲，供 Drain-Lines 使用
    foreach ($cl in $arr) {
        if (-not $cl.pending) { $cl.pending = [System.Collections.Generic.List[byte]]::new() }
    }
    return @{ port = $port; room = $arr }
}

function Add-Npc($roomObj, $name, $mode) {
    SendLine $roomObj.room[0] ('ADD NPC ' + $name + ' ' + $mode)
    $null = RecvUntilStream $roomObj.room[0].s '已添加' 3000
}

$exitCode = 1
try {
    Start-Keepalive
    Kill-All
    $null = Start-RM 8888

    # ============ R1：房内 @离线 NPC 必答（恰一条 + 多样性 + 词嵌入） ============
    $R1 = New-Room4
    Add-Npc $R1 'NpcOne' 'off'
    foreach ($cl in $R1.room) { Drain-Lines $cl 500 }
    $topics = @('狼人怎么分', '预言家怎么验', '女巫怎么用', '守卫怎么守', '投票怎么投', '猎人怎么开', '平票怎么办', '首夜怎么看')
    $r1Replies = [System.Collections.ArrayList]::new()
    $atConfirm1 = $false
    foreach ($tp in $topics) {
        SendLine $R1.room[0] ('@NpcOne ' + $tp)
        $deadline = [DateTime]::Now.AddMilliseconds(2500)
        $got = $false
        while ([DateTime]::Now -lt $deadline -and -not $got) {
            Drain-Lines $R1.room[0] 250
            foreach ($cl in $R1.room) {
                foreach ($l in $cl.lines) {
                    if ($l -match '^ROOM_MSG\|NpcOne：') {
                        $r1Replies.Add($l) | Out-Null
                        $got = $true
                        if ($l -match '你at了') { $atConfirm1 = $true }
                    }
                }
            }
            Start-Sleep -Milliseconds 30
        }
        # 每轮 @ 后短暂停顿，让回复行完整落盘再发下一轮（@ 不受 2s 限频）
        Start-Sleep -Milliseconds 300
    }
    # 单次 @ 恰一条：取所有 NpcOne：行，按时间分组计数——此处只统计本轮收集总量，
    # 重复发声 bug 的断言在 R8 独立做（专用单 @ 场景），R1 只验「有回复+多样」
    $distinct = @($r1Replies | ForEach-Object { $_ } | Sort-Object -Unique)
    Check 'R1-1 房内 @离线 NPC 必有回复（8 话题全部触发）' ($r1Replies.Count -ge 8)
    Check 'R1-2 @ 回复多样性（不同文本 ≥ 2）' ($distinct.Count -ge 2)
    $emb = @($r1Replies | Where-Object {
        $_ -match '狼人|预言家|女巫|守卫|投票|猎人|平票|首夜'
    })
    Check 'R1-3 @ 回复词嵌入（含对方话题词，8 试至少一次）' ($emb.Count -ge 1)
    foreach ($l in $r1Replies) { Log-Line 'R1' $l }

    # ============ R2：房内普通聊天相关性（名字/话题/闲聊） ============
    # 用 Bob 发普通聊天（不 @），NPC 按相关性接话；2.2s 间隔避开 2s 限频
    # 概率断言要留足样本：R2-1 名字命中 85%/次，6 试 0 命中率 ~1e-5，
    # 已稳；R2-2 话题命中 30%/次，5 试 0 命中率 ~16.8% 会偶发假 FAIL
    # （实测一次），提到 10 试降到 ~2.8%
    $nameHits = 0
    foreach ($i in 1..8) {
        $before = @($R1.room[0].lines | Where-Object { $_ -match '^ROOM_MSG\|NpcOne：' }).Count
        SendLine $R1.room[1] 'NpcOne 你觉得呢'
        $deadline = [DateTime]::Now.AddMilliseconds(2500)
        while ([DateTime]::Now -lt $deadline) {
            foreach ($cl in $R1.room) { Drain-Lines $cl 200 }
            $now = @($R1.room[0].lines | Where-Object { $_ -match '^ROOM_MSG\|NpcOne：' }).Count
            if ($now -gt $before) { break }
            Start-Sleep -Milliseconds 30
        }
        $after = @($R1.room[0].lines | Where-Object { $_ -match '^ROOM_MSG\|NpcOne：' }).Count
        if ($after -gt $before) { $nameHits++ }
        Start-Sleep -Milliseconds 2200
    }
    Check 'R2-1 含 NPC 名的普通聊天触发接话（6 试至少 1 次）' ($nameHits -ge 1)

    $topicHits = 0
    foreach ($i in 1..10) {
        $before = @($R1.room[0].lines | Where-Object { $_ -match '^ROOM_MSG\|NpcOne：' }).Count
        SendLine $R1.room[1] '大家觉得狼人是谁'
        $deadline = [DateTime]::Now.AddMilliseconds(2500)
        while ([DateTime]::Now -lt $deadline) {
            foreach ($cl in $R1.room) { Drain-Lines $cl 200 }
            $now = @($R1.room[0].lines | Where-Object { $_ -match '^ROOM_MSG\|NpcOne：' }).Count
            if ($now -gt $before) { break }
            Start-Sleep -Milliseconds 30
        }
        $after = @($R1.room[0].lines | Where-Object { $_ -match '^ROOM_MSG\|NpcOne：' }).Count
        if ($after -gt $before) { $topicHits++ }
        Start-Sleep -Milliseconds 2200
    }
    Check 'R2-2 游戏话题词聊天触发接话（30%，5 试至少 1 次）' ($topicHits -ge 1)

    $idleHits = 0
    foreach ($i in 1..8) {
        $before = @($R1.room[0].lines | Where-Object { $_ -match '^ROOM_MSG\|NpcOne：' }).Count
        SendLine $R1.room[1] '今天天气不错'
        $deadline = [DateTime]::Now.AddMilliseconds(2500)
        while ([DateTime]::Now -lt $deadline) {
            foreach ($cl in $R1.room) { Drain-Lines $cl 200 }
            $now = @($R1.room[0].lines | Where-Object { $_ -match '^ROOM_MSG\|NpcOne：' }).Count
            if ($now -gt $before) { break }
            Start-Sleep -Milliseconds 30
        }
        $after = @($R1.room[0].lines | Where-Object { $_ -match '^ROOM_MSG\|NpcOne：' }).Count
        if ($after -gt $before) { $idleHits++ }
        Start-Sleep -Milliseconds 2200
    }
    Check 'R2-3 纯闲聊低概率接话（6%，8 试上限 5 次）' ($idleHits -le 5)

    # ============ R3：多 NPC 同时在场 ============
    Add-Npc $R1 'NpcTwo' 'off'
    foreach ($cl in $R1.room) { Drain-Lines $cl 400 }
    $before2 = @($R1.room[0].lines | Where-Object { $_ -match '^ROOM_MSG\|Npc(One|Two)：' }).Count
    SendLine $R1.room[1] 'NpcOne 和 NpcTwo 都在吗'
    $deadline = [DateTime]::Now.AddMilliseconds(3000)
    while ([DateTime]::Now -lt $deadline) {
        foreach ($cl in $R1.room) { Drain-Lines $cl 200 }
        $now2 = @($R1.room[0].lines | Where-Object { $_ -match '^ROOM_MSG\|Npc(One|Two)：' }).Count
        if ($now2 -gt $before2) { break }
        Start-Sleep -Milliseconds 30
    }
    $r3Reply = @($R1.room[0].lines | Where-Object { $_ -match '^ROOM_MSG\|Npc(One|Two)：' })
    $r3Who = @($r3Reply | ForEach-Object {
        if ($_ -match '^ROOM_MSG\|(NpcOne|NpcTwo)：') { $Matches[1] }
    } | Sort-Object -Unique)
    Check 'R3-1 聊天含两人名 → 至少一个 NPC 接话' ($r3Who.Count -ge 1)

    # ============ R4：极端输入不崩 ============
    $procAliveBefore = $null
    $pStart = Get-Process -Name 'Start' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($pStart) { $procAliveBefore = -not $pStart.HasExited }

    $long = '超长' * 200
    SendLine $R1.room[0] ('@NpcOne ' + $long)
    $r = RecvUntilStream $R1.room[0].s 'NpcOne：' 2500
    Check 'R4-1 超长 @ 内容：NPC 仍回复' ($r -ne $null -and $r.Length -lt 400)
    foreach ($cl in $R1.room) { Drain-Lines $cl 300 }

    SendLine $R1.room[0] '@NpcOne !!!???'
    $r = RecvUntilStream $R1.room[0].s 'NpcOne：' 2500
    Check 'R4-2 纯标点 @ 内容：NPC 兜底回复（词为空）' ($r -ne $null)
    foreach ($cl in $R1.room) { Drain-Lines $cl 300 }

    SendLine $R1.room[0] '@NpcOne 你好|注入|管道'
    $r = RecvUntilStream $R1.room[0].s 'NpcOne：' 2500
    Check 'R4-3 管道符注入 @：NPC 回复且不崩' ($r -ne $null)
    foreach ($cl in $R1.room) { Drain-Lines $cl 300 }

    # @ 不存在 NPC：只广播、不代答、无 at 确认
    SendLine $R1.room[0] '@幽灵NPC 你谁啊'
    $r = RecvUntilStream $R1.room[1].s 'Alice：@幽灵NPC' 2500
    foreach ($cl in $R1.room) { Drain-Lines $cl 300 }
    $ghost = @($R1.room | ForEach-Object { $_.lines } | Where-Object {
        $_ -match '^ROOM_MSG\|(幽灵NPC|你at了 幽灵NPC)'
    })
    Check 'R4-4 @ 不存在的 NPC：仅广播无代答' (($r -ne $null) -and ($ghost.Count -eq 0))

    $pStart2 = Get-Process -Name 'Start' -ErrorAction SilentlyContinue | Select-Object -First 1
    Check 'R4-5 极端输入后 Start 进程仍存活' (($pStart2 -ne $null) -and (-not $pStart2.HasExited))
    foreach ($cl in $R1.room) { Close-Client $cl }
    Kill-All
    Start-Sleep -Milliseconds 500

    # ============ R8：单次 @ 恰一条回复（重复发声回归） ============
    $null = Start-RM 8888
    $R8 = New-Room4
    Add-Npc $R8 'NpcOne' 'off'
    foreach ($cl in $R8.room) { Drain-Lines $cl 400 }
    SendLine $R8.room[0] '@NpcOne 你好呀'
    $deadline = [DateTime]::Now.AddMilliseconds(2500)
    while ([DateTime]::Now -lt $deadline) {
        foreach ($cl in $R8.room) { Drain-Lines $cl 200 }
        Start-Sleep -Milliseconds 30
    }
    $r8Replies = @($R8.room | ForEach-Object { $_.lines } | Where-Object { $_ -match '^ROOM_MSG\|NpcOne：' })
    # NPC 回复是 INVALID_SOCKET 全员广播（含发送者，§22），4 个客户端各收
    # 1 条共 4 条；重复发声 bug 会让每个客户端收到 2 条共 8 条。断言必须
    # 逐客户端计数（总条数 = 客户端数 × 1），只查总数会与"每端恰一条"混淆
    $r8PerClient = @($R8.room | ForEach-Object {
        @($_.lines | Where-Object { $_ -match '^ROOM_MSG\|NpcOne：' }).Count
    })
    Check 'R8-1 单次 @Npc 恰一条回复（无重复发声）' (($r8Replies.Count -eq 4) -and (@($r8PerClient | Where-Object { $_ -ne 1 }).Count -eq 0))
    foreach ($cl in $R8.room) { Close-Client $cl }
    Kill-All
    Start-Sleep -Milliseconds 500

    # ============ R5：在线 NPC 房内对话（fake_chat 返回 AI 文本） ============
    Remove-Item "$wolf\fake_chat_out.txt" -ErrorAction SilentlyContinue
    Remove-Item "$wolf\npc_key.bin" -ErrorAction SilentlyContinue
    $env:WOLF_NPC_API_KEY = 'testkey-12345'
    $env:WOLF_NPC_API_URL = 'http://127.0.0.1:18080/chat'
    $env:WOLF_NPC_TIMEOUT_SECONDS = '3'
    $fake = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$wolf\tests\npc_fake_chat.ps1") -WindowStyle Hidden -PassThru -RedirectStandardOutput "$wolf\fake_chat_out.txt"
    Remove-Item "$wolf\fake_chat_log.txt" -ErrorAction SilentlyContinue
    # fake_chat 启动后监听生效需要一点时间（Start-Process 子进程冷启动），
    # 且建房流程 8-12 秒 > 旧版 8 秒空闲退出窗口会提前退出（现改为 25 秒）；
    # 这里主动探测 18080 可连再往下走，避免 @ 发向未就绪/已退出的假服务器
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
    if (-not $fcReady) { Write-Output 'WARN  R5 fake_chat 18080 未就绪，仍继续（断言可能失败）' }
    Start-Sleep -Milliseconds 800
    $null = Start-RM 8888
    $R5 = New-Room4
    Add-Npc $R5 'NpcAI' 'on'
    foreach ($cl in $R5.room) { Drain-Lines $cl 400 }
    SendLine $R5.room[0] '@NpcAI 你怎么看'
    $r = RecvUntilStream $R5.room[1].s 'NpcAI：' 6000
    # 断言必须同时匹配 AI 文本本身：只匹配 "NpcAI：" 会把离线兜底模板当成
    # 在线成功（假 PASS，R5-1 曾因此掩盖 R5-2 失败）
    Check 'R5-1 在线 NPC 房内 @ 有回复（AI 文本广播）' ($r -ne $null -and $r -match 'NpcAI：AI房内回话')
    if ($r) { Log-Line 'R5' $r }
    Start-Sleep -Milliseconds 1200
    $fakeChatOut = ''
    if (Test-Path "$wolf\fake_chat_log.txt") {
        try { $fakeChatOut = [IO.File]::ReadAllText("$wolf\fake_chat_log.txt", [Text.Encoding]::UTF8) } catch { }
    } elseif (Test-Path "$wolf\fake_chat_out.txt") {
        try {
            $fs = [System.IO.File]::Open("$wolf\fake_chat_out.txt", 'Open', 'Read', 'ReadWrite')
            try {
                $len = $fs.Length
                $buf = New-Object byte[] $len
                [void]$fs.Read($buf, 0, $len)
            } finally { $fs.Close() }
            if ($buf.Length -ge 2 -and $buf[0] -eq 0xFF -and $buf[1] -eq 0xFE) {
                $fakeChatOut = [System.Text.Encoding]::Unicode.GetString($buf)
            } else {
                $fakeChatOut = [System.Text.Encoding]::UTF8.GetString($buf)
            }
        } catch { }
    }
    Check 'R5-2 在线 NPC 房内对话请求已发出（fake_chat 收到 REQ）' ($fakeChatOut.Contains('REQ:'))
    if ($fakeChatOut.Length -gt 0) {
        foreach ($ll in @($fakeChatOut -split "`r?`n" | Where-Object { $_.Trim().Length -gt 0 })) {
            Write-Output ('R5-FAKE ' + $ll)
        }
    }
    foreach ($cl in $R5.room) { Close-Client $cl }
    Kill-All
    Start-Sleep -Milliseconds 500

    # ============ R6：在线失败回退离线（无 key） ============
    Remove-Item Env:\WOLF_NPC_API_KEY -ErrorAction SilentlyContinue
    Remove-Item "$wolf\npc_key.bin" -ErrorAction SilentlyContinue
    $null = Start-RM 8888
    $R6 = New-Room4
    Add-Npc $R6 'NpcNokey' 'on'
    foreach ($cl in $R6.room) { Drain-Lines $cl 400 }
    SendLine $R6.room[0] '@NpcNokey 你好呀'
    $r = RecvUntilStream $R6.room[1].s 'NpcNokey：' 4000
    Check 'R6-1 在线 NPC 无 key：@ 必答回退离线模板' ($r -ne $null -and $r -match 'NpcNokey：')
    if ($r) { Log-Line 'R6' $r }
    foreach ($cl in $R6.room) { Close-Client $cl }
    Kill-All
    Start-Sleep -Milliseconds 500

    # ============ R7：在线超时回退离线（key 有但 URL 无服务 + 短超时） ============
    $env:WOLF_NPC_API_KEY = 'testkey-12345'
    $env:WOLF_NPC_API_URL = 'http://127.0.0.1:18099/chat'
    $env:WOLF_NPC_TIMEOUT_SECONDS = '1'
    Remove-Item "$wolf\npc_key.bin" -ErrorAction SilentlyContinue
    $null = Start-RM 8888
    $R7 = New-Room4
    Add-Npc $R7 'NpcTmo' 'on'
    foreach ($cl in $R7.room) { Drain-Lines $cl 400 }
    SendLine $R7.room[0] '@NpcTmo 你好呀'
    $r = RecvUntilStream $R7.room[1].s 'NpcTmo：' 8000
    Check 'R7-1 在线 NPC 超时：@ 必答回退离线（短超时快速兜底）' ($r -ne $null -and $r -match 'NpcTmo：')
    if ($r) { Log-Line 'R7' $r }
    foreach ($cl in $R7.room) { Close-Client $cl }

    Write-Output ("===== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " =====")
    if ($script:fail -eq 0) { Write-Output 'ROUND12 RESULT: PASS'; $exitCode = 0 }
    else { Write-Output 'ROUND12 RESULT: FAIL' }
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
    Remove-Item "$wolf\fake_chat_out.txt" -ErrorAction SilentlyContinue
    Remove-Item "$wolf\npc_key.bin" -ErrorAction SilentlyContinue
}

exit $exitCode
