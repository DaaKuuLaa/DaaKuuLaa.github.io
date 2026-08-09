# 狼人杀第七轮验收脚本（测试工具，非游戏实现）
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File tests\round7_test.ps1 *> tests\round7_out.txt
# 覆盖 2026-08-07 稳定性修复与玩家列表功能：
#   A) 玩家列表：直连 4 人局开局后全员收到 PLAYER_LIST|4|<name1>|<name2>|... 广播
#      （顺序=Server 传参顺序、不含职业信息）
#   B) 心跳应答：Server 对客户端 PING 回一行 PING（半开死连检测的基础，
#      客户端据此每 1 秒必有字节到达，配合 SO_RCVTIMEO 15s 判定失联）
#   C) 兜底不误杀（断线重连失败根因回归）：WOLF_GAME_WAIT_SECONDS=2 注入 →
#      START 后全员断大厅、不连游戏服 → 等 4s（超时窗口已过）→ Server.exe 进程
#      仍在 + 房间仍显示 [游戏中]（旧逻辑此时已强杀 Server 致对局中断）→
#      4 人全连游戏服触发真正开局：PLAYER_LIST 中文名全链路正确 + PING 应答
# 大厅流程通过裸 socket 直连 Start.exe（8888）；直连局通过 Server.exe（Get-FreePort）。
# StreamWriter 必须显式无 BOM UTF8 编码（AGENTS.md 踩坑 17/21，脚本含中文名 石子轩）。

$ErrorActionPreference = 'Stop'
$wolf = Split-Path $PSScriptRoot -Parent
$script:pass = 0
$script:fail = 0

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

function Start-RM {
    Kill-All
    $proc = Start-Process -FilePath "$wolf\Start.exe" -WorkingDirectory $wolf -ArgumentList @('8888') -WindowStyle Hidden -PassThru
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

# 大厅客户端：HELLO/NAME 握手（无保活 runspace——本脚本大厅窗口都很短）
function New-Client($name) {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', 8888)
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
        playerList = $null; pingEcho = 0; misc = @()
    }
}

try {
    # ============ A/B 段：直连 4 人局 PLAYER_LIST 广播 + PING 应答 ============
    Kill-All
    $portA = Get-FreePort
    $argsA = @(
        $portA.ToString(), 'Alice', 'Bob', 'Cathy', 'Dave',
        '127.0.0.1', '8888', 'R7RM', '1', '0', '2', '0', '1',
        'zh', 'zh', 'zh', 'zh'
    )
    $srvA = Start-Process -FilePath "$wolf\Server.exe" -ArgumentList $argsA -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 2

    $bots = @()
    for ($k = 1; $k -le 4; $k++) { $bots += New-Bot $k $portA }

    # 阶段 1：等待全员收到 PLAYER_LIST（10s 超时），每 1 秒发 PING 保活
    $deadline = [DateTime]::Now.AddSeconds(10)
    $lastPing = [DateTime]::Now
    $gotAll = $false
    while (-not $gotAll -and [DateTime]::Now -lt $deadline) {
        if (([DateTime]::Now - $lastPing).TotalSeconds -ge 1) {
            foreach ($cl in $bots) { try { $cl.w.WriteLine('PING') } catch {} }
            $lastPing = [DateTime]::Now
        }
        foreach ($cl in $bots) {
            while ($cl.s.DataAvailable) {
                $b = $cl.s.ReadByte()
                if ($b -lt 0) { break }
                if ($b -eq 10) {
                    $raw = $cl.bytes.ToArray()
                    $cl.bytes.Clear()
                    $line = [System.Text.Encoding]::UTF8.GetString($raw).TrimEnd("`r")
                    if ($line -eq 'PING') { $cl.pingEcho++ }
                    elseif ($line -match '^PLAYER_LIST\|') { $cl.playerList = $line }
                    else { $cl.misc += $line }
                } else {
                    $cl.bytes.Add([byte]$b)
                }
            }
        }
        $gotAll = (@($bots | Where-Object { $_.playerList -ne $null }).Count -eq 4)
        Start-Sleep -Milliseconds 30
    }

    # 阶段 2：再等 4 秒收集 PING 应答（B 段验证；PLAYER_LIST 秒到后循环提前
    # 退出会导致首个 PING 尚未发出，必须留出心跳周期）
    $pingDeadline = [DateTime]::Now.AddSeconds(4)
    while ([DateTime]::Now -lt $pingDeadline) {
        if (([DateTime]::Now - $lastPing).TotalSeconds -ge 1) {
            foreach ($cl in $bots) { try { $cl.w.WriteLine('PING') } catch {} }
            $lastPing = [DateTime]::Now
        }
        foreach ($cl in $bots) {
            while ($cl.s.DataAvailable) {
                $b = $cl.s.ReadByte()
                if ($b -lt 0) { break }
                if ($b -eq 10) {
                    $raw = $cl.bytes.ToArray()
                    $cl.bytes.Clear()
                    $line = [System.Text.Encoding]::UTF8.GetString($raw).TrimEnd("`r")
                    if ($line -eq 'PING') { $cl.pingEcho++ }
                    elseif ($line -match '^PLAYER_LIST\|') { $cl.playerList = $line }
                } else {
                    $cl.bytes.Add([byte]$b)
                }
            }
        }
        Start-Sleep -Milliseconds 30
    }

    $haveAll = (@($bots | Where-Object { $_.playerList -ne $null }).Count -eq 4)
    $dbg = "PING echoes: " + (($bots | ForEach-Object { $_.pingEcho }) -join ',') + "`r`n"
    $dbg += "PLAYER_LIST lines:`r`n" + (($bots | ForEach-Object { $_.playerList }) -join "`r`n") + "`r`n"
    $dbg += "Misc lines (bot1):`r`n" + (($bots[0].misc) -join "`r`n")
    [System.IO.File]::WriteAllText("$wolf\tests\round7_debug.txt", $dbg, [System.Text.UTF8Encoding]::new($false))
    Check 'A1 开局后全员收到 PLAYER_LIST 广播' $haveAll
    if ($haveAll) {
        $sameLine = (@($bots | ForEach-Object { $_.playerList } | Where-Object { $_ -ne $bots[0].playerList }).Count -eq 0)
        Check 'A2 四份 PLAYER_LIST 内容一致' $sameLine
        $a3ok = $bots[0].playerList -eq 'PLAYER_LIST|4|Alice|Bob|Cathy|Dave'
        $fields = $bots[0].playerList -split '\|'
        [System.IO.File]::WriteAllText("$wolf\tests\round7_debug.txt",
            "a3ok=$a3ok type=$($bots[0].playerList.GetType().FullName) value=[$($bots[0].playerList)] fields=$($fields.Count)",
            [System.Text.UTF8Encoding]::new($false))
        Check 'A3 名单编号序 = Server 传参顺序' $a3ok
        Check 'A4 字段 = 前缀+总数+4 名字（6 段，无职业信息）' (
            ($fields.Count -eq 6) -and ($fields[0] -eq 'PLAYER_LIST'))
    }

    # B 段：PING 应答（Server 对每行 PING 回一行 PING）
    $pingOk = (@($bots | Where-Object { $_.pingEcho -ge 1 }).Count -eq 4)
    Check 'B1 每个客户端收到 PING 应答（半开死连检测基础）' $pingOk

    foreach ($cl in $bots) { try { $cl.c.Close() } catch {} }
    Stop-Process -Id $srvA.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 400

    # ============ C 段：兜底不误杀存活 Server（根因回归，§16.3 修复） ============
    $env:WOLF_GAME_WAIT_SECONDS = '2'
    $rmC = Start-RM
    $portC = Get-FreePort
    $C = @(New-Client 'Mona')
    SendLine -cl $C[0] -cmd ('CREATE|' + $portC)
    $null = RecvUntilStream $C[0].s 'CREATED' 3000
    foreach ($nm in @('Nina', 'Oscar', '石子轩')) {
        $cl = New-Client $nm
        SendLine -cl $cl -cmd ('JOIN|' + $portC)
        $null = RecvUntilStream $cl.s 'JOINED' 3000
        $C += $cl
    }
    SendLine -cl $C[0] -cmd 'VILLAGER|1'
    $null = RecvUntilStream $C[0].s '村民职业已启用' 2000
    SendLine -cl $C[0] -cmd 'RATIO|1|0|2'
    $null = RecvUntilStream $C[0].s '比例已设为' 2000
    foreach ($cl in $C) { SendLine -cl $cl -cmd 'READY' }
    Start-Sleep -Milliseconds 600
    SendLine -cl $C[0] -cmd 'START'
    $gpsC = @()
    foreach ($cl in $C) { $gpsC += RecvUntilStream $cl.s 'GAME_PREPARE|' 6000 }
    Check 'C1 START 成功（全员收到 GAME_PREPARE）' (@($gpsC | Where-Object { $_ -match 'GAME_PREPARE\|' }).Count -eq 4)
    # 全员断大厅、不连游戏服：注入的 2s 兜底窗口过后 Server 进程必须仍在
    foreach ($cl in $C) { Close-Client $cl }
    Start-Sleep -Seconds 4
    $srvAliveC = [bool](Get-Process -Name Server -ErrorAction SilentlyContinue)
    Check 'C2 兜底不误杀存活 Server.exe（4s 后进程仍在）' $srvAliveC
    # 房间未回滚：LIST 仍显示 [游戏中]（另起连接查）
    $probe = New-Client 'Probe'
    SendLine -cl $probe -cmd 'LIST'
    $r = RecvUntilStream $probe.s 'ROOMS_LIST' 3000
    Check 'C3 房间未被回滚（LIST 仍显示 [游戏中]）' (
        $r -and ($r -match ([regex]::Escape($portC) + '\s+4/12')) -and $r.Contains('[游戏中]'))
    Close-Client $probe
    # C4/C5：4 人全连游戏服触发真正开局（25s 开局窗口内）→ 中文名经
    # Start(宽字符)→Server→协议 全链路进 PLAYER_LIST；同时验证 PING 应答
    $gconns = @()
    for ($k = 1; $k -le 4; $k++) {
        $g = Connect-Retry $portC
        $gs = $g.GetStream()
        $gw = New-Object IO.StreamWriter($gs, [System.Text.UTF8Encoding]::new($false))
        $gw.NewLine = "`n"
        $gw.AutoFlush = $true
        $gw.WriteLine('PLAYER_ID|' + $k)
        $gconns += @{ c = $g; s = $gs; w = $gw; pl = $null; pe = 0; bytes = [System.Collections.Generic.List[byte]]::new() }
    }
    $deadlineC4 = [DateTime]::Now.AddSeconds(8)
    $lastPingC4 = [DateTime]::Now
    while ([DateTime]::Now -lt $deadlineC4 -and @($gconns | Where-Object { $_.pl -ne $null }).Count -lt 4) {
        if (([DateTime]::Now - $lastPingC4).TotalSeconds -ge 1) {
            foreach ($gc in $gconns) { try { $gc.w.WriteLine('PING') } catch {} }
            $lastPingC4 = [DateTime]::Now
        }
        foreach ($gc in $gconns) {
            while ($gc.s.DataAvailable) {
                $b = $gc.s.ReadByte()
                if ($b -lt 0) { break }
                if ($b -eq 10) {
                    $raw = $gc.bytes.ToArray()
                    $gc.bytes.Clear()
                    $line = [System.Text.Encoding]::UTF8.GetString($raw).TrimEnd("`r")
                    if ($line -eq 'PING') { $gc.pe++ }
                    elseif ($line -match '^PLAYER_LIST\|') { $gc.pl = $line }
                } else {
                    $gc.bytes.Add([byte]$b)
                }
            }
        }
        Start-Sleep -Milliseconds 30
    }
    Check 'C4 PLAYER_LIST 中文名链路（Start 宽字符→Server→协议）' (
        $gconns[0].pl -eq 'PLAYER_LIST|4|Mona|Nina|Oscar|石子轩')
    # C5：C4 循环可能因名单秒到而提前退出（PING 尚未发出），补等一个心跳周期
    $pingDeadC = [DateTime]::Now.AddSeconds(4)
    $lastPingC = [DateTime]::Now
    while ([DateTime]::Now -lt $pingDeadC) {
        if (([DateTime]::Now - $lastPingC).TotalSeconds -ge 1) {
            foreach ($gc in $gconns) { try { $gc.w.WriteLine('PING') } catch {} }
            $lastPingC = [DateTime]::Now
        }
        foreach ($gc in $gconns) {
            while ($gc.s.DataAvailable) {
                $b = $gc.s.ReadByte()
                if ($b -lt 0) { break }
                if ($b -eq 10) {
                    $raw = $gc.bytes.ToArray()
                    $gc.bytes.Clear()
                    $line = [System.Text.Encoding]::UTF8.GetString($raw).TrimEnd("`r")
                    if ($line -eq 'PING') { $gc.pe++ }
                } else {
                    $gc.bytes.Add([byte]$b)
                }
            }
        }
        Start-Sleep -Milliseconds 30
    }
    Check 'C5 真实开局链路 PING 应答（全员收到）' (@($gconns | Where-Object { $_.pe -ge 1 }).Count -eq 4)
    foreach ($gc in $gconns) { try { $gc.c.Close() } catch {} }
    Kill-All
    Remove-Item Env:\WOLF_GAME_WAIT_SECONDS -ErrorAction SilentlyContinue
}
catch {
    Write-Output ("EXCEPTION: " + $_.Exception.Message)
    Write-Output $_.ScriptStackTrace
    $script:fail++
}

Kill-All
Remove-Item Env:\WOLF_GAME_WAIT_SECONDS -ErrorAction SilentlyContinue
Write-Output ("===== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " =====")
if ($script:fail -eq 0) { Write-Output 'ROUND7  RESULT: PASS'; exit 0 }
else { Write-Output 'ROUND7  RESULT: FAIL'; exit 1 }
