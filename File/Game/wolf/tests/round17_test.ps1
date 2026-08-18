# round17_test.ps1 - 第十七轮验收（§26：AI 隐瞒/误导，不总是如实交底）：
#   A 段：房内 @ 身份问句（你是预言家吗）→ 60% 概率走 HIDE 池（隐瞒不交底），
#         8 次采样 HIDE 特征 >=3，且全程无"我是预言家"直接承认
#   B 段：普通问句（不敏感）不受影响：8 次采样 HIDE 特征 <=2（对照，防全走 HIDE）
#   C 段：在线房内 @ 身份问句 → fake_ai 收到 REQ，系统提示词含"隐瞒/误导"约束
#   D 段：源码级断言（游戏内系统提示词 NpcBuildSystemPrompt 含身份保密约束）
# 运行：powershell -NoProfile -ExecutionPolicy Bypass -File tests\round17_test.ps1
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
    return @{ c = $c; s = $s; w = $w; name = $name }
}

function Close-Client($cl) {
    try { if ($cl -and $cl.c) { $cl.c.Close() } } catch {}
}

function SendLine($cl, $cmd) {
    try { $cl.w.WriteLine($cmd) } catch {}
}

$pass = 0
$fail = 0
function Check($name, $ok) {
    if ($ok) { $script:pass++; Write-Output ("PASS  " + $name) }
    else { $script:fail++; Write-Output ("FAIL  " + $name) }
}

$hidePattern = '先不急着谈|你猜|不透露|放一放|不好玩了|见分晓|懂的人都懂|不表态'

$exitCode = 1
try {
    Kill-All
    Remove-Item "$wolf\npc_nlp.log" -ErrorAction SilentlyContinue
    Remove-Item Env:WOLF_NPC_API_KEY -ErrorAction SilentlyContinue
    Remove-Item Env:WOLF_NPC_API_URL -ErrorAction SilentlyContinue

    # ============ A 段：房内 @ 身份问句 → HIDE 池（隐瞒） ============
    $null = Start-RM 8888
    $portA = Get-FreePort
    $ha = New-Client 'HideHost'
    SendLine $ha ('CREATE|' + $portA)
    $null = RecvUntilStream $ha.s 'CREATED' 3000
    $ga = New-Client 'HideGuest'
    SendLine $ga ('JOIN|' + $portA)
    $null = RecvUntilStream $ga.s 'JOINED' 3000
    SendLine $ha 'ADD NPC Terse off'
    $null = RecvUntilStream $ha.s '已添加' 3000
    Start-Sleep -Milliseconds 400

    $hideCnt = 0
    $admitCnt = 0
    $gotAny = 0
    for ($i = 0; $i -lt 8; $i++) {
        SendLine $ha '@Terse 你是预言家吗'
        $r = RecvUntilStream $ga.s 'Terse：' 5000
        if ($null -ne $r) {
            $gotAny++
            if ($r -match $hidePattern) { $hideCnt++ }
            if ($r -match '我是预言家') { $admitCnt++ }
            Write-Output ('A-REPLY' + $i + ' ' + $r.Substring(0, [Math]::Min(60, $r.Length)))
        }
        Start-Sleep -Milliseconds 300
    }
    Check 'A1 身份问句 8 次采样 HIDE 隐瞒回复 >=3（60% 概率不交底）' ($hideCnt -ge 3)
    Check 'A2 全程无"我是预言家"直接承认（不总是如实说出）' ($admitCnt -eq 0)
    Check 'A3 8 次 @ 全部有回复（必答红线不破）' ($gotAny -ge 8)
    Close-Client $ha
    Close-Client $ga
    Kill-All
    Start-Sleep -Milliseconds 500

    # ============ B 段：普通问句对照（不敏感不受影响） ============
    $null = Start-RM 8888
    $portB = Get-FreePort
    $hb = New-Client 'PlainHost'
    SendLine $hb ('CREATE|' + $portB)
    $null = RecvUntilStream $hb.s 'CREATED' 3000
    $gb = New-Client 'PlainGuest'
    SendLine $gb ('JOIN|' + $portB)
    $null = RecvUntilStream $gb.s 'JOINED' 3000
    SendLine $hb 'ADD NPC Chatty off'
    $null = RecvUntilStream $hb.s '已添加' 3000
    Start-Sleep -Milliseconds 400

    $hideCntB = 0
    for ($i = 0; $i -lt 8; $i++) {
        SendLine $hb '@Chatty 你觉得今天天气怎么样'
        $r = RecvUntilStream $gb.s 'Chatty：' 5000
        if ($null -ne $r -and $r -match $hidePattern) { $hideCntB++ }
        Start-Sleep -Milliseconds 300
    }
    Check 'B1 普通问句 8 次采样 HIDE 误伤 <=2（只对底牌敏感词隐瞒）' ($hideCntB -le 2)
    Close-Client $hb
    Close-Client $gb
    Kill-All
    Start-Sleep -Milliseconds 500

    # ============ C 段：在线房内 @ 身份问句 → 系统提示词含保密约束 ============
    Remove-Item "$wolf\fake_ai_log.txt" -ErrorAction SilentlyContinue
    Remove-Item "$wolf\npc_key.bin" -ErrorAction SilentlyContinue
    # 按 pid 清残留 fake_ai（120s 寿命外的旧进程可能占 18080 干扰断言）
    $fakePidF = "$wolf\npc_fake_ai.pid"
    if (Test-Path $fakePidF) {
        try {
            $fp = [int]([IO.File]::ReadAllText($fakePidF))
            Stop-Process -Id $fp -Force -ErrorAction SilentlyContinue
        } catch { }
        Remove-Item $fakePidF -ErrorAction SilentlyContinue
    }
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
    if (-not $fcReady) { Write-Output 'WARN  C 段 fake_ai 18080 未就绪，仍继续（断言可能失败）' }
    Start-Sleep -Milliseconds 800
    $null = Start-RM 8888
    $portC = Get-FreePort
    $hc = New-Client 'AiHost'
    SendLine $hc ('CREATE|' + $portC)
    $null = RecvUntilStream $hc.s 'CREATED' 3000
    $gc = New-Client 'AiGuest'
    SendLine $gc ('JOIN|' + $portC)
    $null = RecvUntilStream $gc.s 'JOINED' 3000
    SendLine $hc 'ADD NPC NpcAI on'
    $null = RecvUntilStream $hc.s '已添加' 3000
    Start-Sleep -Milliseconds 400
    SendLine $hc '@NpcAI 你是预言家吗'
    # 在线路径最坏 8s（3s 超时 + 2s 重试 + 3s，坑 37），窗口按最坏给足
    $r = RecvUntilStream $gc.s 'NpcAI：' 12000
    Check 'C1 在线 NPC 房内 @ 身份问句有回复' ($null -ne $r)
    if ($r) { Write-Output ('C-REPLY ' + $r.Substring(0, [Math]::Min(60, $r.Length))) }
    Start-Sleep -Milliseconds 1500
    $fakeAiLog = ''
    if (Test-Path "$wolf\fake_ai_log.txt") {
        try { $fakeAiLog = [IO.File]::ReadAllText("$wolf\fake_ai_log.txt", [Text.Encoding]::UTF8) } catch { }
    }
    Check 'C2 在线请求体系统提示词含身份保密约束（隐瞒/误导）' ($fakeAiLog.Contains('隐瞒') -or $fakeAiLog.Contains('误导'))
    Close-Client $hc
    Close-Client $gc
    Kill-All
    Start-Sleep -Milliseconds 500
    Remove-Item Env:WOLF_NPC_API_KEY -ErrorAction SilentlyContinue
    Remove-Item Env:WOLF_NPC_API_URL -ErrorAction SilentlyContinue
    Remove-Item Env:WOLF_NPC_TIMEOUT_SECONDS -ErrorAction SilentlyContinue
    Remove-Item "$wolf\npc_key.bin" -ErrorAction SilentlyContinue

    # ============ D 段：源码级断言（游戏内系统提示词保密约束） ============
    $srcD = ''
    try { $srcD = [IO.File]::ReadAllText("$wolf\npc_bot.h", [Text.Encoding]::UTF8) } catch { }
    Check 'D1 游戏内系统提示词含"不要直接报出自己的身份"约束' ($srcD.Contains('不要直接报出自己的身份'))
    Check 'D2 游戏内系统提示词含"故意误导"约束' ($srcD.Contains('故意误导'))

    $exitCode = 0
}
catch {
    Write-Output ("EXCEPTION: " + $_.Exception.Message)
}
finally {
    Kill-All
}

Write-Output ("===== 结果: PASS=" + $pass + " FAIL=" + $fail + " =====")
if ($fail -eq 0 -and $exitCode -eq 0) { Write-Output 'ROUND17 RESULT: PASS'; exit 0 }
Write-Output 'ROUND17 RESULT: FAIL'
exit 1