# 狼人杀第八轮验收脚本（测试工具，非游戏实现）
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File tests\round8_test.ps1 *> tests\round8_out.txt
# 覆盖 2026-08-07 用户报告的 PLIST 双根因修复 + 目标不合法提示 + Start 交互输入监听端口（§18）：
#   A) Start 交互输入监听端口：无参数启动 + stdin 喂端口 → 监听生效（含先非法后合法的重输流程）
#   B) 紧凑 4 人局 PLIST 对齐：GAME_PREPARE pid / Server 欢迎语 / PLAYER_LIST 行 / 白天投票广播
#      名字四者全部一致；白天 VOTE 非法目标与白狼王 BOMB 非法目标的具体原因提示（§18.3）
#   C) 槽位空洞局（5 人房去 1 人）PLIST 对齐：压缩名单序 pid、PLAYER_LIST、欢迎语、白天广播
#   D) PICK/TRANSFER 目标不合法：输出具体原因 + 请重新输入（§18.3）
#   E) 空洞局回滚后 REJOIN 按 gamePid 回原槽位（STATUS 槽位对齐，§18.2）
# 大厅流程通过裸 socket 直连 Start.exe；游戏局通过裸 socket 直连 Server.exe。
# StreamWriter 必须显式无 BOM UTF8 编码（AGENTS.md 踩坑 17/21）。

$ErrorActionPreference = 'Stop'
$wolf = Split-Path $PSScriptRoot -Parent
$script:pass = 0
$script:fail = 0
$script:wolfTarget = $null
$script:dayHits = @()
$script:lineLog = @()

function Log-Line($tag, $k, $line) {
    if ($script:lineLog.Count -lt 400) { $script:lineLog += ($tag + $k + ": " + $line) }
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
    for ($p = 8490; $p -lt 8560; $p++) {
        if ($script:usedPorts -contains $p) { continue }
        try {
            $l = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Any, $p)
            $l.Start()
            $l.Stop()
            $script:usedPorts += $p
            return $p
        } catch {}
    }
    return 8490
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

# STATUS 多行回复：头行（ROOM_STATUS|）的换行已被 RecvUntilStream 消费，
# 后续数据行要补读（踩坑 22）
function Recv-Status($cl, $timeoutMs = 3000) {
    $hdr = RecvUntilStream $cl.s 'ROOM_STATUS' $timeoutMs
    if (-not $hdr) { return $null }
    $chunk = ReadChunk $cl.s 800
    $txt = [System.Text.Encoding]::UTF8.GetString($chunk)
    return ($hdr + "`n" + $txt)
}

# 大厅客户端：HELLO/NAME 握手（本轮大厅窗口短，不注册保活 runspace，
# 但服务端 10s 失联判定对 3s 内的命令窗口无影响）
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
    return @{ c = $c; s = $s; w = $w }
}

function Close-Client($cl) {
    try { if ($cl -and $cl.c) { $cl.c.Close() } } catch {}
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
        role = ''; assigned = ''; pl = $null; witchInputs = 0
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

# 单行处理：身份/名单记录 + 夜晚输入应答（狼刀第一个非狼、预言家验 1、
# 女巫首夜救被刀者否则弃药、其余 0）
function Handle-GameLine($cl, $line) {
    if ($line -match '你被分配到 (\d+) 号位') { $cl.assigned = $Matches[1]; return }
    if ($line -match '^ROLE\|') { $cl.role = $line.Substring(5); return }
    if ($line -match '^PLAYER_LIST\|') { $cl.pl = $line; return }
    if ($line.Trim() -eq '__INPUT__') {
        $isWolf = ($cl.role -eq 'werewolf' -or $cl.role -eq 'whitewolf')
        if ($isWolf) {
            if (-not $script:wolfTarget) { $script:wolfTarget = 1 }
            $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $script:wolfTarget)
        } elseif ($cl.role -eq 'seer') {
            $cl.w.WriteLine('PLAYER_' + $cl.k + '|1')
        } elseif ($cl.role -eq 'witch') {
            if ($cl.witchInputs -eq 0 -and $script:wolfTarget) {
                $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $script:wolfTarget)
            } else {
                $cl.w.WriteLine('PLAYER_' + $cl.k + '|0')
            }
            $cl.witchInputs++
        } else {
            $cl.w.WriteLine('PLAYER_' + $cl.k + '|0')
        }
    }
}

# 定狼刀目标：身份齐后取第一个非狼存活者（脚本全知身份）
function Ensure-WolfTarget($bots) {
    if ($script:wolfTarget) { return }
    $rolesKnown = (@($bots | Where-Object { $_.role -eq '' }).Count -eq 0)
    if (-not $rolesKnown) { return }
    foreach ($b in $bots) {
        if ($b.role -ne 'werewolf' -and $b.role -ne 'whitewolf') { $script:wolfTarget = $b.k; return }
    }
    $script:wolfTarget = 1
}

try {
    # ============ A 段：Start 无参数交互输入监听端口 ============
    Kill-All
    $stdinA = "$wolf\tests\round8_stdin_a.txt"
    [System.IO.File]::WriteAllText($stdinA, "8890`n", [System.Text.Encoding]::ASCII)
    $procA = Start-Process -FilePath "$wolf\Start.exe" -WorkingDirectory $wolf -WindowStyle Hidden -RedirectStandardInput $stdinA -PassThru
    Start-Sleep -Seconds 2
    $probeOkA = $false
    try { $probeA = New-Client 'Probe' 8890; $probeOkA = $true; Close-Client $probeA } catch {}
    Check 'A1 无参数启动时交互输入端口生效（8890 监听成功）' $probeOkA
    Stop-Process -Id $procA.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 600

    $stdinB = "$wolf\tests\round8_stdin_b.txt"
    [System.IO.File]::WriteAllText($stdinB, "80`n8891`n", [System.Text.Encoding]::ASCII)
    $procB = Start-Process -FilePath "$wolf\Start.exe" -WorkingDirectory $wolf -WindowStyle Hidden -RedirectStandardInput $stdinB -PassThru
    Start-Sleep -Seconds 2
    $probeOkB = $false
    try { $probeB = New-Client 'Probe' 8891; $probeOkB = $true; Close-Client $probeB } catch {}
    Check 'A2 非法端口提示原因后重新输入成功（8891 监听成功）' $probeOkB
    Stop-Process -Id $procB.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 600

    # ============ B 段：紧凑 4 人局 PLIST 对齐（level2：白狼王+预言家+女巫+村民） ============
    $env:WOLF_VOTE_TIMEOUT_SECONDS = '6'
    $null = Start-RM 8888
    $portB = Get-FreePort
    $B = @(New-Client 'Alice')
    SendLine $B[0] ('CREATE|' + $portB)
    $null = RecvUntilStream $B[0].s 'CREATED' 3000
    foreach ($nm in @('Bob', 'Cathy', 'Dave')) {
        $cl = New-Client $nm
        SendLine $cl ('JOIN|' + $portB)
        $null = RecvUntilStream $cl.s 'JOINED' 3000
        $B += $cl
    }
    SendLine $B[0] 'LEVEL|2'
    $null = RecvUntilStream $B[0].s '档位已' 2000
    SendLine $B[0] 'VILLAGER|1'
    $null = RecvUntilStream $B[0].s '村民职业已启用' 2000
    SendLine $B[0] 'RATIO|1|0|2'
    $null = RecvUntilStream $B[0].s '比例已设为' 2000
    foreach ($cl in $B) { SendLine $cl 'READY' }
    Start-Sleep -Milliseconds 600
    SendLine $B[0] 'START'
    $gpsB = @()
    foreach ($cl in $B) { $gpsB += RecvUntilStream $cl.s 'GAME_PREPARE|' 6000 }
    $pidB = @()
    foreach ($g in $gpsB) { if ($g -match 'GAME_PREPARE\|') { $pidB += ($g -split '\|')[4] } }
    Check 'B1 紧凑局 GAME_PREPARE pid = 1,2,3,4（槽位序）' (($pidB -join ',') -eq '1,2,3,4')
    foreach ($cl in $B) { Close-Client $cl }

    $botsB = @()
    for ($k = 1; $k -le 4; $k++) { $botsB += New-Bot $k $portB }
    $dayB = $false
    $bobIllegal = $false; $aliceVoted = $false; $bombIllegal = $false
    $deadlineB = [DateTime]::Now.AddSeconds(60)
    $lastPingB = [DateTime]::Now
    while ([DateTime]::Now -lt $deadlineB) {
        if (([DateTime]::Now - $lastPingB).TotalSeconds -ge 1) {
            foreach ($b in $botsB) { try { $b.w.WriteLine('PING') } catch {} }
            $lastPingB = [DateTime]::Now
        }
        Pump-Bots $botsB
        Ensure-WolfTarget $botsB
        foreach ($b in $botsB) {
            while ($b.queue.Count -gt 0) {
                $line = $b.queue.Dequeue()
                Log-Line 'B' $b.k $line
                Handle-GameLine $b $line
                if ($line.Contains('白天发言阶段')) { $dayB = $true }
                if ($line -match '投票目标不合法|自爆目标不合法|投票给了玩家') { $script:dayHits += $line }
                if ($b.assigned) { $script:assignB = $b.assigned }
            }
        }
        if ($dayB) {
            if (-not $bobIllegal) {
                SendLine $botsB[1] ('PLAYER_2|VOTE|99')
                $bobIllegal = $true
            }
            if (-not $aliceVoted) {
                SendLine $botsB[0] 'PLAYER_1|VOTE|2'
                $aliceVoted = $true
            }
            if (-not $bombIllegal) {
                foreach ($b in $botsB) {
                    if ($b.role -eq 'whitewolf') {
                        SendLine $b ('PLAYER_' + $b.k + '|BOMB|0')
                        $bombIllegal = $true
                        break
                    }
                }
            }
        }
        if ($aliceVoted -and $bobIllegal -and $bombIllegal -and
            $script:dayHits -match '投票给了玩家' -and
            $script:dayHits -match '投票目标不合法' -and
            $script:dayHits -match '自爆目标不合法') { break }
        Start-Sleep -Milliseconds 30
    }
    $dbgB = "roles=" + (($botsB | ForEach-Object { $_.k.ToString() + ':' + $_.role }) -join ',') + "`r`n"
    $dbgB += "assigned=" + (($botsB | ForEach-Object { $_.k.ToString() + ':' + $_.assigned }) -join ',') + "`r`n"
    $dbgB += "plB1=" + $botsB[0].pl + "`r`n"
    $dbgB += "hits=" + (($script:dayHits | ForEach-Object { $_ }) -join ' || ') + "`r`n"
    $dbgB += "linelog:`r`n" + (($script:lineLog | ForEach-Object { $_ }) -join "`r`n") + "`r`n"
    [System.IO.File]::WriteAllText("$wolf\tests\round8_debug.txt", $dbgB, [System.Text.UTF8Encoding]::new($false))
    $assignedOkB = (@($botsB | Where-Object { $_.assigned -eq $_.k.ToString() }).Count -eq 4)
    Check 'B2 紧凑局每玩家欢迎语槽位 = GAME_PREPARE pid（1,2,3,4）' $assignedOkB
    Check 'B3 PLAYER_LIST 行 = 4|Alice|Bob|Cathy|Dave' ($botsB[0].pl -eq 'PLAYER_LIST|4|Alice|Bob|Cathy|Dave')
    Check 'B4 白天投票广播名字对齐（玩家Alice 投给玩家Bob 槽2）' (
        [bool]($script:dayHits -match '玩家Alice 投票给了玩家Bob（槽2）'))
    Check 'B5 VOTE 非法目标提示含具体原因与请重新输入' (
        [bool]($script:dayHits -match '投票目标不合法：必须是 1\.\.N 的存活玩家或 0。请重新输入。'))
    Check 'B6 白狼王 BOMB 非法目标提示含具体原因与请重新输入' (
        [bool]($script:dayHits -match '自爆目标不合法：必须是 1\.\.N 的存活玩家且不能是自己。请重新输入。'))
    foreach ($b in $botsB) { try { $b.c.Close() } catch {} }

    # ============ D 段：PICK/TRANSFER 目标不合法提示（不开局的房间） ============
    $portD = Get-FreePort
    $D = @(New-Client 'AliceD')
    SendLine $D[0] ('CREATE|' + $portD)
    $null = RecvUntilStream $D[0].s 'CREATED' 3000
    $D2 = New-Client 'BobD'
    SendLine $D2 ('JOIN|' + $portD)
    $null = RecvUntilStream $D2.s 'JOINED' 3000
    $D += $D2
    SendLine $D[0] 'PICK|99'
    $rPick = RecvUntilStream $D[0].s '目标玩家不存在' 3000
    Check 'D1 PICK 目标不合法输出具体原因+请重新输入' (
        $rPick -and $rPick.Contains('目标玩家不存在：99') -and $rPick.Contains('请重新输入'))
    SendLine $D[0] 'TRANSFER|Ghost'
    $rTf = RecvUntilStream $D[0].s '目标玩家不存在' 3000
    Check 'D2 TRANSFER 目标不合法输出具体原因+请重新输入' (
        $rTf -and $rTf.Contains('目标玩家不存在：Ghost') -and $rTf.Contains('请重新输入'))
    foreach ($cl in $D) { Close-Client $cl }

    # ============ C 段：槽位空洞局 PLIST 对齐（5 人房去 1 人后 4 人开局） ============
    $null = Start-RM 8888
    $portC = Get-FreePort
    $C = @(New-Client 'AliceC')
    SendLine $C[0] ('CREATE|' + $portC)
    $null = RecvUntilStream $C[0].s 'CREATED' 3000
    foreach ($nm in @('BobC', 'CathyC', 'DaveC', 'EveC')) {
        $cl = New-Client $nm
        SendLine $cl ('JOIN|' + $portC)
        $null = RecvUntilStream $cl.s 'JOINED' 3000
        $C += $cl
    }
    # CathyC 断开（槽 2）→ 非游戏期清槽，槽位空洞保留
    Close-Client $C[2]
    Start-Sleep -Milliseconds 800
    SendLine $C[0] 'VILLAGER|1'
    $null = RecvUntilStream $C[0].s '村民职业已启用' 2000
    SendLine $C[0] 'RATIO|1|0|2'
    $null = RecvUntilStream $C[0].s '比例已设为' 2000
    foreach ($cl in @($C[0], $C[1], $C[3], $C[4])) { SendLine $cl 'READY' }
    Start-Sleep -Milliseconds 600
    SendLine $C[0] 'START'
    $gpsC = @()
    foreach ($cl in @($C[0], $C[1], $C[3], $C[4])) { $gpsC += RecvUntilStream $cl.s 'GAME_PREPARE|' 6000 }
    $pidC = @()
    foreach ($g in $gpsC) { if ($g -match 'GAME_PREPARE\|') { $pidC += ($g -split '\|')[4] } }
    Check 'C1 空洞局 GAME_PREPARE pid = 压缩名单序 1,2,3,4' (($pidC -join ',') -eq '1,2,3,4')
    foreach ($cl in $C) { Close-Client $cl }

    $botsC = @()
    for ($k = 1; $k -le 4; $k++) { $botsC += New-Bot $k $portC }
    $script:dayHits = @()
    $dayC = $false
    $daveVoted = $false
    $deadlineC = [DateTime]::Now.AddSeconds(60)
    $lastPingC = [DateTime]::Now
    while ([DateTime]::Now -lt $deadlineC) {
        if (([DateTime]::Now - $lastPingC).TotalSeconds -ge 1) {
            foreach ($b in $botsC) { try { $b.w.WriteLine('PING') } catch {} }
            $lastPingC = [DateTime]::Now
        }
        Pump-Bots $botsC
        Ensure-WolfTarget $botsC
        foreach ($b in $botsC) {
            while ($b.queue.Count -gt 0) {
                $line = $b.queue.Dequeue()
                Log-Line 'C' $b.k $line
                Handle-GameLine $b $line
                if ($line.Contains('白天发言阶段')) { $dayC = $true }
                if ($line -match '投票给了玩家') { $script:dayHits += $line }
            }
        }
        if ($dayC -and -not $daveVoted) {
            SendLine $botsC[2] 'PLAYER_3|VOTE|1'
            $daveVoted = $true
        }
        if ($daveVoted -and $script:dayHits -match '投票给了玩家') { break }
        Start-Sleep -Milliseconds 30
    }
    $dbgC = "roles=" + (($botsC | ForEach-Object { $_.k.ToString() + ':' + $_.role }) -join ',') + "`r`n"
    $dbgC += "assigned=" + (($botsC | ForEach-Object { $_.k.ToString() + ':' + $_.assigned }) -join ',') + "`r`n"
    $dbgC += "plC1=" + $botsC[0].pl + "`r`n"
    $dbgC += "hits=" + (($script:dayHits | ForEach-Object { $_ }) -join ' || ') + "`r`n"
    $dbgC += "linelogC:" + ((@($script:lineLog | Where-Object { $_ -like 'C*' }) | ForEach-Object { $_ }) -join "`r`n") + "`r`n"
    [System.IO.File]::AppendAllText("$wolf\tests\round8_debug.txt", "`r`n=== C 段 ===`r`n" + $dbgC, [System.Text.UTF8Encoding]::new($false))
    Check 'C2 空洞局 PLAYER_LIST = 4|AliceC|BobC|DaveC|EveC（压缩名单序）' (
        $botsC[0].pl -eq 'PLAYER_LIST|4|AliceC|BobC|DaveC|EveC')
    $assignedOkC = (@($botsC | Where-Object { $_.assigned -eq $_.k.ToString() }).Count -eq 4)
    Check 'C3 空洞局欢迎语槽位 = 压缩名单序（DaveC=3、EveC=4）' $assignedOkC
    Check 'C4 空洞局白天广播名字对齐（玩家DaveC 投给玩家AliceC 槽1）' (
        [bool]($script:dayHits -match '玩家DaveC 投票给了玩家AliceC（槽1）'))
    foreach ($b in $botsC) { try { $b.c.Close() } catch {} }

    # ============ E 段：空洞局回滚后 REJOIN 按 gamePid 回原槽位 ============
    $env:WOLF_GAME_WAIT_SECONDS = '2'
    $null = Start-RM 8892
    $portE = Get-FreePort
    $E = @(New-Client 'EmmaE' 8892)
    SendLine $E[0] ('CREATE|' + $portE)
    $null = RecvUntilStream $E[0].s 'CREATED' 3000
    foreach ($nm in @('FredE', 'GinaE', 'HankE', 'IvyE')) {
        $cl = New-Client $nm 8892
        SendLine $cl ('JOIN|' + $portE)
        $null = RecvUntilStream $cl.s 'JOINED' 3000
        $E += $cl
    }
    Close-Client $E[2]
    Start-Sleep -Milliseconds 800
    SendLine $E[0] 'VILLAGER|1'
    $null = RecvUntilStream $E[0].s '村民职业已启用' 2000
    SendLine $E[0] 'RATIO|1|0|2'
    $null = RecvUntilStream $E[0].s '比例已设为' 2000
    foreach ($cl in @($E[0], $E[1], $E[3], $E[4])) { SendLine $cl 'READY' }
    Start-Sleep -Milliseconds 600
    SendLine $E[0] 'START'
    $gpsE = @()
    foreach ($cl in @($E[0], $E[1], $E[3], $E[4])) { $gpsE += RecvUntilStream $cl.s 'GAME_PREPARE|' 6000 }
    $roomIdE = $null
    if ($gpsE[0] -match 'GAME_PREPARE\|') { $roomIdE = ($gpsE[0] -split '\|')[2] }
    [System.IO.File]::AppendAllText("$wolf\tests\round8_debug.txt",
        "`r`n=== E 段 gpsE ===`r`n" + (($gpsE | ForEach-Object { $_ }) -join "`r`n") + "`r`nroomIdE=" + $roomIdE + "`r`n",
        [System.Text.UTF8Encoding]::new($false))
    foreach ($cl in @($E[0], $E[1], $E[3], $E[4])) { Close-Client $cl }
    # 模拟 Server.exe 启动即死（round7 起兜底回滚只对 Server 进程已死生效）：
    # 杀 Server → 2s 兜底窗口后房间回滚（gameStarted=false、ready 清空、gamePid 保留）
    Get-Process -Name Server -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 4
    $probeE = New-Client 'ProbeE' 8892
    SendLine $probeE 'LIST'
    $listE = RecvUntilStream $probeE.s 'ROOMS_LIST' 3000
    Close-Client $probeE
    [System.IO.File]::AppendAllText("$wolf\tests\round8_debug.txt",
        "`r`n=== E 段回滚后 LIST ===`r`n" + $listE + "`r`n", [System.Text.UTF8Encoding]::new($false))
    $rejoinOkE = 0
    $rejoinRespE = @()
    $mapE = @(@('EmmaE', 1), @('FredE', 2), @('HankE', 3), @('IvyE', 4))
    $Eback = @()
    foreach ($pair in $mapE) {
        $cl = New-Client $pair[0] 8892
        $joined = $false
        for ($k = 0; $k -lt 15; $k++) {
            SendLine $cl ('REJOIN|' + $roomIdE + '|' + $pair[1])
            $rE2 = RecvUntilStream $cl.s 'JOINED' 2000
            if ($rE2 -match 'JOINED') { $joined = $true; break }
            $rejoinRespE += ($pair[0] + ":" + $rE2)
        }
        if ($joined) { $rejoinOkE++ }
        $Eback += $cl
    }
    [System.IO.File]::AppendAllText("$wolf\tests\round8_debug.txt",
        "rejoinResp=" + (($rejoinRespE | ForEach-Object { $_ }) -join ' || ') + "`r`n",
        [System.Text.UTF8Encoding]::new($false))
    Check 'E1 空洞局回滚后全员按 gamePid REJOIN 回房' ($rejoinOkE -eq 4)
    SendLine $Eback[2] 'STATUS'
    $statusE = Recv-Status $Eback[2] 3000
    $dbgE = "statusE=`r`n" + $statusE + "`r`n"
    [System.IO.File]::AppendAllText("$wolf\tests\round8_debug.txt", "`r`n=== E 段 ===`r`n" + $dbgE, [System.Text.UTF8Encoding]::new($false))
    Check 'E2 STATUS 槽位对齐（HankE=ID4、IvyE=ID5，空洞 ID3 无名字，无 GinaE）' (
        $statusE -and $statusE -match '(?m)^\s*4\s+\|\s*HankE' -and
        $statusE -match '(?m)^\s*5\s+\|\s*IvyE' -and
        $statusE -notmatch 'GinaE')
    foreach ($cl in $Eback) { Close-Client $cl }
    Remove-Item Env:\WOLF_GAME_WAIT_SECONDS -ErrorAction SilentlyContinue
    Remove-Item Env:\WOLF_VOTE_TIMEOUT_SECONDS -ErrorAction SilentlyContinue
}
catch {
    Write-Output ("EXCEPTION: " + $_.Exception.Message)
    Write-Output $_.ScriptStackTrace
    $script:fail++
}

Kill-All
Remove-Item Env:\WOLF_GAME_WAIT_SECONDS -ErrorAction SilentlyContinue
Remove-Item Env:\WOLF_VOTE_TIMEOUT_SECONDS -ErrorAction SilentlyContinue
Remove-Item -LiteralPath "$wolf\tests\round8_stdin_a.txt", "$wolf\tests\round8_stdin_b.txt" -ErrorAction SilentlyContinue
Write-Output ("===== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " =====")
if ($script:fail -eq 0) { Write-Output 'ROUND8  RESULT: PASS'; exit 0 }
else { Write-Output 'ROUND8  RESULT: FAIL'; exit 1 }
