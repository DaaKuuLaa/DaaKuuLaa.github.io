# 狼人杀第六轮验收脚本（测试工具，非游戏实现）
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File tests\round6_test.ps1 *> tests\round6_out.txt
# 覆盖 REQUIREMENTS.md §16 验收 16.1-16.6：
#   16.1 IP/LG 命令服务端行为（房主专属拒绝、房内查询正常；客户端拦截层已解除 GameOnly）
#   16.2 NAME 长度≥2 码点：单字符/单数字拒绝、2 码点成功、空名回退 Player 不回归、
#        白名单优先于长度检查、BAN 参数不受长度限制（攻击：单字符名拉黑后 JOIN 被拒）
#   16.3 端口释放三路径：
#        E) 兜底回滚（WOLF_GAME_WAIT_SECONDS=2 注入）只对"Server 进程已死"
#           （启动即死）生效：断大厅等 4s 后进程仍在（不误杀正常对局，
#           2026-08-07 断线重连失败根因修复）→ 强杀 Server.exe → 回滚 →
#           REJOIN 回房 → 同端口重新 START 成功（新 Server bind 成功）
#        F) Server 25s 开局超时发 RELEASE → Start 销毁房间 → 同端口 CREATE 成功
#        G) BAN 踢空房间 → 房主断开 → 空房回收 → 同端口 CREATE 成功
#   16.4 短别名与全称混用：ST/STATUS、CR/CREATE、TF/TRANSFER、VG/VILLAGER 等效
#   16.5 STATUS 竖排表格：表头 ID|NAME|ST、数据行数、名字列对齐（全角 2 宽）、
#        ST 值随 READY 变化、中文名对齐
#   16.6 攻击：伪造 RELEASE/GAME_ENDED 未知房间不崩、聊天注入 | 不入指令
# 大厅流程通过裸 socket 直连 Start.exe（8888）；所有连接由后台 runspace 每 1 秒
# 发 PING 保活（StreamWriter 加锁，AGENTS.md 踩坑 7/11）。StreamWriter 必须显式
# UTF8 编码（踩坑 17，测试含中文名 石子轩）。

$ErrorActionPreference = 'Stop'
$wolf = Split-Path $PSScriptRoot -Parent
$script:pass = 0
$script:fail = 0

function Check($desc, $cond) {
    if ($cond) { $script:pass++; Write-Output ("PASS  " + $desc) }
    else       { $script:fail++; Write-Output ("FAIL  " + $desc) }
}

# ============ 保活 runspace（后台每 1 秒给所有在线连接发 PING） ============
# 长静默窗口内连接不发字节会被 Start 的 3 秒失联判定误杀（踩坑 7）；
# 后台 runspace 与主线程共用 StreamWriter 必须加锁（踩坑 11）。
$script:liveClients = [System.Collections.ArrayList]::new()
$script:kaStop = New-Object System.Threading.ManualResetEvent($false)

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
# 注意：IO.StreamWriter($s, [Text.Encoding]::UTF8) 首次写入会输出 UTF-8 BOM
# （EF BB BF），Start 握手行被前缀 BOM 破坏直接断开（本脚本踩过的坑）；
# 无编码参数则按 GBK 写（踩坑 17，中文名会写坏）。必须 UTF8Encoding($false)
# 显式无 BOM UTF-8。

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
    $w = New-Object IO.StreamWriter($s, [System.Text.UTF8Encoding]::new($false))
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

# 裸连接：只做 HELLO 握手（供 NAME 试探与大厅命令测试）
function Connect-Naked {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', 8888)
    $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s, [System.Text.UTF8Encoding]::new($false))
    $w.NewLine = "`n"
    $w.AutoFlush = $true
    $w.WriteLine('HELLO|3')
    $null = RecvUntilStream $s 'WELCOME' 3000
    $cl = @{ c = $c; s = $s; w = $w; wlock = [object]::new() }
    $null = $script:liveClients.Add($cl)
    return $cl
}

function Close-Client($cl) {
    try { if ($cl -and $script:liveClients.Contains($cl)) { $null = $script:liveClients.Remove($cl) } } catch {}
    try { if ($cl -and $cl.c) { $cl.c.Close() } } catch {}
}

# ============ 进程 / 端口 / 工具 ============

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

# 空闲端口探测：必须用 TcpListener([Net.IPAddress]::Any)（踩坑 16：Loopback
# 探测不到 0.0.0.0 监听，两个测试段会拿到同一端口导致 bind 10048）。
# 已分配过的端口登记进 usedPorts，防止同一 Start 内多个房间段撞端口
# （房间占用只存在于 Start 内存，TcpListener 探测不到，round6 实测撞车）
$script:usedPorts = @()

function Get-FreePort {
    for ($p = 8420; $p -lt 8490; $p++) {
        if ($script:usedPorts -contains $p) { continue }
        try {
            $l = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Any, $p)
            $l.Start()
            $l.Stop()
            $script:usedPorts += $p
            return $p
        } catch {}
    }
    return 8420
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

# NAME 试探：裸连接改名后取回复行（NAME_SET|... 或 ERROR|...），用完即关。
# 被拒名不会回 NAME_SET，若先等 NAME_SET 会把 ERROR 行吞掉再超时，故只发一次
# NAME，逐行读到 NAME_SET 或 ERROR 任一即返回（round5 G 段模式）
function Try-Name($name) {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', 8888)
    $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s, [System.Text.UTF8Encoding]::new($false))
    $w.NewLine = "`n"
    $w.AutoFlush = $true
    $w.WriteLine('HELLO|3')
    $null = RecvUntilStream $s 'WELCOME' 3000
    $w.WriteLine('NAME|' + $name)
    $line = $null
    $deadline = [DateTime]::Now.AddMilliseconds(3000)
    $pending = New-Object System.Collections.Generic.List[byte]
    while ([DateTime]::Now -lt $deadline -and -not $line) {
        while ($s.DataAvailable) {
            $b = $s.ReadByte()
            if ($b -lt 0) { break }
            $pending.Add([byte]$b)
            if ($b -eq 10) {
                $l = [System.Text.Encoding]::UTF8.GetString($pending.ToArray()).TrimEnd("`r", "`n")
                if ($l.Contains('NAME_SET') -or $l.Contains('ERROR')) { $line = $l; break }
                $pending.Clear()
            }
        }
        if (-not $line) { Start-Sleep -Milliseconds 20 }
    }
    $c.Close()
    return $line
}

# STATUS 多行回复（§16.5 竖排表）：头行换行已被 RecvUntilStream 消费，
# 剩余行补读后拼回换行（同 round5 Recv-LG 模式）
function Recv-Status($cl, $timeoutMs = 3000) {
    $hdr = RecvUntilStream $cl.s 'ROOM_STATUS' $timeoutMs
    if (-not $hdr) { return '' }
    $chunk = ReadChunk $cl.s 800
    $rows = [System.Text.Encoding]::UTF8.GetString($chunk)
    return ($hdr + "`n" + $rows)
}

# 提取 STATUS 数据行（ID | NAME | ST 行）
function Get-StatusRows($st) {
    return @($st -split "`n" | Where-Object { $_ -match '^\s*\d+\s+\| ' })
}

# 显示宽度（半角 1 列、全角 2 列，与 Start.cpp DisplayWidth 同语义）
function Get-DispWidth($s) {
    $w = 0
    foreach ($ch in $s.ToCharArray()) {
        if ([int][char]$ch -gt 255) { $w += 2 } else { $w += 1 }
    }
    return $w
}

# 4 人建房（含比例配置），返回客户端数组与端口
function New-Room4($port, $names) {
    $A = New-Client $names[0]
    SendLine $A ('CREATE|' + $port)
    $null = RecvUntil $A 'CREATED' 3000
    $clients = @($A)
    for ($i = 1; $i -lt 4; $i++) {
        $cl = New-Client $names[$i]
        SendLine $cl ('JOIN|' + $port)
        $null = RecvUntil $cl 'JOINED' 3000
        $clients += $cl
    }
    SendLine $A 'VILLAGER|1'
    $null = RecvUntil $A '村民职业已启用' 2000
    SendLine $A 'RATIO|1|0|2'
    $null = RecvUntil $A '比例已设为' 2000
    foreach ($cl in $clients) { SendLine $cl 'READY' }
    Start-Sleep -Milliseconds 600
    SendLine $A 'START'
    foreach ($cl in $clients) { $null = RecvUntil $cl 'GAME_PREPARE|' 6000 }
    return $clients
}

# ============ 汇总 ============

function Finish {
    Stop-Keepalive
    foreach ($cl in $script:liveClients.ToArray()) { Close-Client $cl }
    Kill-All
    Remove-Item Env:\WOLF_GAME_WAIT_SECONDS -ErrorAction SilentlyContinue
    Write-Output ("===== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " =====")
    if ($script:fail -eq 0) { Write-Output 'ROUND6  RESULT: PASS'; exit 0 }
    else { Write-Output 'ROUND6  RESULT: FAIL'; exit 1 }
}

try {
    Start-Keepalive

    # ============ A 段：NAME 长度≥2 码点（§16.2） ============
    Start-RM | Out-Null
    $r = Try-Name 'a'
    Check 'A1 单字符 ASCII 名字拒绝' ($r -match '名字至少需要 2 个字符')
    $r = Try-Name '1'
    Check 'A2 单数字名字拒绝' ($r -match '名字至少需要 2 个字符')
    $r = Try-Name '我'
    Check 'A3 单汉字名字拒绝' ($r -match '名字至少需要 2 个字符')
    $r = Try-Name 'ab'
    Check 'A4 2 码点 ASCII 名字成功' ($r -eq 'NAME_SET|ab')
    $r = Try-Name '12'
    Check 'A5 2 码点数字名字成功' ($r -eq 'NAME_SET|12')
    $r = Try-Name '玩家'
    Check 'A6 2 码点中文名字成功' ($r -eq 'NAME_SET|玩家')
    $r = Try-Name ''
    Check 'A7 空名回退默认 Player（不回归 pen 5a）' ($r -eq 'NAME_SET|Player')
    $r = Try-Name 'a b'
    Check 'A8 白名单检查先于长度（空格名仍被白名单拒）' ($r -match '名字只能包含中英文、数字与下划线')

    # ============ B 段：STATUS 竖排表格（§16.5） ============
    # 注意：不能用 New-Room4（会自动 READY+START 破坏未开局状态），手动建房
    $portB = Get-FreePort
    $B = @(New-Client 'Alice')
    SendLine $B[0] ('CREATE|' + $portB)
    $null = RecvUntil $B[0] 'CREATED' 3000
    foreach ($nm in @('Bob', 'Cathy', '石子轩')) {
        $cl = New-Client $nm
        SendLine $cl ('JOIN|' + $portB)
        $null = RecvUntil $cl 'JOINED' 3000
        $B += $cl
    }
    SendLine $B[0] 'STATUS'
    $st = Recv-Status $B[0] 3000
    Check 'B1 STATUS 表头含 ID/NAME/ST 列' (($st -match 'ID\s*\|\s*NAME') -and ($st -match '\|\s*ST'))
    $rows = Get-StatusRows $st
    Check 'B2 STATUS 数据行数 = 4' ($rows.Count -eq 4)
    Check 'B3 STATUS 含全部 4 名玩家（含中文名）' (
        $st.Contains('Alice') -and $st.Contains('Bob') -and $st.Contains('Cathy') -and $st.Contains('石子轩'))
    $barIdx = @($rows | ForEach-Object { $_.IndexOf('|') })
    $aligned = ($barIdx.Count -gt 0) -and (@($barIdx | Where-Object { $_ -ne $barIdx[0] }).Count -eq 0)
    Check 'B4 STATUS ID 列右对齐起点一致' $aligned
    $pipePos = @($rows | ForEach-Object {
        $first = $_.IndexOf('|')
        $second = $_.IndexOf('|', $first + 1)
        Get-DispWidth $_.Substring(0, $second + 1)
    })
    $sameW = ($pipePos.Count -gt 0) -and (@($pipePos | Where-Object { $_ -ne $pipePos[0] }).Count -eq 0)
    Check 'B5 STATUS 名字列起点显示列一致（全角 2 宽）' $sameW
    $allZero = @($rows | Where-Object { $_ -notmatch '\|\s*0\s*$' }).Count -eq 0
    Check 'B6 STATUS 全员未准备时 ST 列全 0' $allZero
    SendLine $B[1] 'READY'
    $null = RecvUntil $B[1] 'READY_STATUS' 2000
    Start-Sleep -Milliseconds 300
    SendLine $B[0] 'ST'
    $st2 = Recv-Status $B[0] 3000
    $bobLine = @($st2 -split "`n" | Where-Object { $_ -match '\| Bob ' })
    Check 'B7 READY 后 ST 短别名刷新为 1' (($bobLine.Count -eq 1) -and ($bobLine[0] -match '\|\s*1\s*$'))

    # ============ C 段：短别名与全称混用（§16.4） ============
    SendLine $B[0] 'STATUS'
    $r = Recv-Status $B[0] 3000
    Check 'C1 STATUS 全称可用' ($r -match 'ID\s*\|\s*NAME')
    SendLine $B[2] 'ST'
    $r = Recv-Status $B[2] 3000
    Check 'C2 ST 短别名非房主可用' ($r -match 'ID\s*\|\s*NAME')
    $portC = Get-FreePort
    $C1 = New-Client 'Carol'
    SendLine $C1 ('CR ' + $portC)
    $r = RecvUntil $C1 'CREATED' 3000
    Check 'C3 CR 短别名建房成功' ($r -match 'CREATED')
    $C2 = New-Client 'Dan'
    SendLine $C2 ('JOIN|' + $portC)
    $null = RecvUntil $C2 'JOINED' 3000
    SendLine $C1 'TF Dan'
    $r = RecvUntil $C1 '已转交房主给' 3000
    Check 'C4 TF 短别名转移房主成功' ($r -match '已转交房主给')
    $r = RecvUntil $C2 'ADMIN' 3000
    Check 'C5 TF 转移目标收到 ADMIN' ($r -match 'ADMIN')
    SendLine $C2 'TRANSFER Carol'
    $r = RecvUntil $C2 '已转交房主给' 3000
    Check 'C6 TRANSFER 全称转移成功' ($r -match '已转交房主给')
    SendLine $C1 'VG 1'
    $r = RecvUntil $C1 '村民职业已启用' 2000
    Check 'C7 VG 短别名开启村民' ($r -match '村民职业已启用')
    SendLine $C1 'VILLAGER 0'
    $r = RecvUntil $C1 '村民职业已禁用' 2000
    Check 'C8 VILLAGER 全称关闭村民' ($r -match '村民职业已禁用')
    foreach ($cl in @($C1, $C2)) { Close-Client $cl }

    # ============ D 段：IP/LG 服务端行为（§16.1） ============
    $N1 = Connect-Naked
    SendLine $N1 'IP|Alice'
    $r = RecvUntil $N1 'ERROR' 3000
    Check 'D1 大厅发 IP 被拒（房主专属）' ($r -match '只有房主可以执行该操作')
    SendLine $N1 'LG'
    $r = RecvUntil $N1 'ERROR' 3000
    Check 'D2 大厅发 LG 被拒（房主专属）' ($r -match '只有房主可以执行该操作')
    SendLine $B[0] 'IP|石子轩'
    $r = RecvUntil $B[0] '的 IP' 3000
    Check 'D3 房内房主 IP 查询返回正确 IP' ($r -match '石子轩 的 IP：127\.0\.0\.1')
    SendLine $B[3] 'IP|Alice'
    $r = RecvUntil $B[3] 'ERROR' 3000
    Check 'D4 房内非房主 IP 被拒' ($r -match '只有房主可以执行该操作')
    Close-Client $N1

    # ============ E 段：开局兜底回滚 → 杀孤儿 Server → 端口复用（§16.3） ============
    foreach ($cl in @($B[0], $B[1], $B[2], $B[3])) { Close-Client $cl }
    Kill-All
    $env:WOLF_GAME_WAIT_SECONDS = '2'
    $rmE = Start-RM
    $portE = Get-FreePort
    $E = @(New-Client 'Emma')
    SendLine $E[0] ('CREATE|' + $portE)
    $null = RecvUntil $E[0] 'CREATED' 3000
    foreach ($nm in @('Fred', 'Gina', 'Hank')) {
        $cl = New-Client $nm
        SendLine $cl ('JOIN|' + $portE)
        $null = RecvUntil $cl 'JOINED' 3000
        $E += $cl
    }
    SendLine $E[0] 'VILLAGER|1'
    $null = RecvUntil $E[0] '村民职业已启用' 2000
    SendLine $E[0] 'RATIO|1|0|2'
    $null = RecvUntil $E[0] '比例已设为' 2000
    foreach ($cl in $E) { SendLine $cl 'READY' }
    Start-Sleep -Milliseconds 600
    SendLine $E[0] 'START'
    $gpsE = @()
    foreach ($cl in $E) { $gpsE += RecvUntil $cl 'GAME_PREPARE|' 6000 }
    Check 'E1 START 成功（全员收到 GAME_PREPARE）' (@($gpsE | Where-Object { $_ -match 'GAME_PREPARE\|' }).Count -eq 4)
    $roomIdE = ($gpsE[0] -split '\|')[2]
    $pidsE = @($gpsE | ForEach-Object { ($_ -split '\|')[4] })
    # 全员关闭大厅连接（模拟进游戏断大厅）；不连游戏服。旧逻辑 Start 的
    # GAME_WAIT_SECONDS 兜底会在此误杀活着的 Server.exe——对局中断线重连
    # 失败的根因（2026-08-07 实测：对局 120s+ 被强杀）。修复后：进程存活
    # → 兜底永不回滚，善后交给 Server 自己的超时/RELEASE
    foreach ($cl in $E) { Close-Client $cl }
    Start-Sleep -Seconds 4
    $serverAliveE = [bool](Get-Process -Name Server -ErrorAction SilentlyContinue)
    Check 'E2 兜底不误杀存活 Server.exe（进程仍在）' $serverAliveE
    # 模拟"启动即死"：强杀 Server.exe → 进程已死 → 兜底回滚（房间保留）
    Get-Process -Name Server -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    # 回滚后房间保留：原玩家重连 REJOIN 回房（同名 JOIN 会被名字占用拦截，
    # 与真实客户端一致走 REJOIN；回滚前 REJOIN 会被"游戏仍在进行中"拒绝，
    # 轮询重试直到回滚完成），房主重得身份，同端口重新开局
    $R = @()
    $rejoinOkE = 0
    for ($i = 0; $i -lt 4; $i++) {
        $cl = New-Client @('Emma', 'Fred', 'Gina', 'Hank')[$i]
        $joined = $false
        for ($k = 0; $k -lt 15; $k++) {
            SendLine $cl ('REJOIN|' + $roomIdE + '|' + $pidsE[$i])
            $rE3 = RecvUntil $cl 'JOINED' 2000
            if ($rE3 -match 'JOINED') { $joined = $true; break }
        }
        if ($joined) { $rejoinOkE++ }
        $R += $cl
    }
    Check 'E3 进程死亡后兜底回滚，原玩家全部 REJOIN 回房' ($rejoinOkE -eq 4)
    foreach ($cl in $R) { SendLine $cl 'READY' }
    Start-Sleep -Milliseconds 600
    SendLine $R[0] 'START'
    $okE = $true
    foreach ($cl in $R) { $r = RecvUntil $cl 'GAME_PREPARE|' 6000; if (-not ($r -match 'GAME_PREPARE\|')) { $okE = $false } }
    Check 'E4 同端口重新 START 成功（端口已释放）' $okE
    # 新 Server 正常监听：连游戏服收到欢迎语（Player 1 分配成功）
    $g = Connect-Retry $portE
    $gs = $g.GetStream()
    $gw = New-Object IO.StreamWriter($gs, [System.Text.UTF8Encoding]::new($false))
    $gw.NewLine = "`n"
    $gw.AutoFlush = $true
    $gw.WriteLine('PLAYER_ID|1')
    $gr = RecvUntilStream $gs '你被分配到 1 号位' 3000
    Check 'E5 新 Server 监听正常（连入收到 Player 1 分配欢迎语）' ($gr -match '你被分配到 1 号位')
    $g.Close()
    foreach ($cl in $R) { Close-Client $cl }
    Kill-All
    Remove-Item Env:\WOLF_GAME_WAIT_SECONDS -ErrorAction SilentlyContinue

    # ============ F 段：Server 开局超时发 RELEASE → 房间销毁 → 端口复用（§16.3） ============
    $rmF = Start-RM
    $portF = Get-FreePort
    $F = New-Room4 $portF @('Iris', 'Jake', 'Kate', 'Leo')
    # 玩家保持大厅连接但不连游戏服 → Server WaitForGameStart 25s 超时 → 全部失联 → RELEASE
    $goneF = $false
    for ($i = 0; $i -lt 40; $i++) {
        SendLine $F[0] 'LIST'
        $r = RecvUntil $F[0] 'ROOMS_LIST' 3000
        if ($r -and -not $r.Contains($portF)) { $goneF = $true; break }
        Start-Sleep -Seconds 1
    }
    Check 'F1 Server 超时后房间被 RELEASE 销毁（LIST 消失）' $goneF
    $serverGoneF = -not (Get-Process -Name Server -ErrorAction SilentlyContinue)
    Check 'F2 RELEASE 后 Server.exe 已退出' $serverGoneF
    $FX = New-Client 'Iris2'
    SendLine $FX ('CREATE|' + $portF)
    $r = RecvUntil $FX 'CREATED' 3000
    Check 'F3 同端口重建房间成功（端口已释放）' ($r -match 'CREATED')
    Close-Client $FX
    foreach ($cl in $F) { Close-Client $cl }
    Kill-All

    # ============ G 段：BAN 踢空 → 空房回收 → 端口复用（§16.3） ============
    $rmG = Start-RM
    $portG = Get-FreePort
    $G1 = New-Client 'Mia'
    SendLine $G1 ('CREATE|' + $portG)
    $null = RecvUntil $G1 'CREATED' 3000
    $G2 = New-Client 'Noah'
    SendLine $G2 ('JOIN|' + $portG)
    $null = RecvUntil $G2 'JOINED' 3000
    $G3 = New-Client 'Owen'
    SendLine $G3 ('JOIN|' + $portG)
    $null = RecvUntil $G3 'JOINED' 3000
    SendLine $G1 'BAN|Noah'
    $null = RecvUntil $G1 '已拉黑' 3000
    SendLine $G1 'BAN|Owen'
    $null = RecvUntil $G1 '已拉黑' 3000
    Close-Client $G2
    Close-Client $G3
    # 只剩房主，房主断开 → 空房销毁
    Close-Client $G1
    Start-Sleep -Seconds 2
    $GX = New-Client 'Mia2'
    SendLine $GX ('CREATE|' + $portG)
    $r = RecvUntil $GX 'CREATED' 3000
    Check 'G1 BAN 踢空+房主断开后同端口可重建' ($r -match 'CREATED')
    Close-Client $GX
    Kill-All

    # ============ H 段：攻击用例（§16.6） ============
    $rmH = Start-RM
    $portH = Get-FreePort
    $H1 = New-Client 'Pia'
    SendLine $H1 ('CREATE|' + $portH)
    $null = RecvUntil $H1 'CREATED' 3000
    $H3 = New-Client 'Quinn'
    SendLine $H3 ('JOIN|' + $portH)
    $null = RecvUntil $H3 'JOINED' 3000
    # H1/H2：伪造未知房间的 RELEASE / GAME_ENDED → 不崩、Start 继续正常服务
    $HX = Connect-Naked
    SendLine $HX 'RELEASE|ZZZZ'
    Start-Sleep -Milliseconds 300
    SendLine $HX 'GAME_ENDED|ZZZZ'
    Start-Sleep -Milliseconds 300
    SendLine $HX 'LIST'
    $r = RecvUntil $HX 'ROOMS_LIST' 3000
    Check 'H1 伪造 RELEASE/GAME_ENDED 未知房间不崩（LIST 仍正常）' ($r -match 'ROOMS_LIST')
    Close-Client $HX
    # H2：BAN 参数不受长度限制——单字符名可被拉黑（服务端按 NAME 同规则入库）
    SendLine $H1 'BAN|q'
    $r = RecvUntil $H1 '已拉黑 q' 3000
    Check 'H2 单字符名可被 BAN（长度规则不影响拉黑参数）' ($r -match '已拉黑 q')
    # H3：拉黑匹配大小写不敏感——BAN|QQ 后小写 qq JOIN 被拒（NameEquals）
    SendLine $H1 'BAN|QQ'
    $null = RecvUntil $H1 '已拉黑 QQ' 3000
    $H2 = New-Client 'qq'
    SendLine $H2 ('JOIN|' + $portH)
    $r = RecvUntil $H2 'ERROR' 3000
    Check 'H3 被拉黑名字大小写变体 JOIN 被拒' ($r -match '你已被拉黑')
    Close-Client $H2
    # H4：房间内聊天注入 | 不触发指令、原样广播（发送者不回显，由 Quinn 接收）
    SendLine $H1 '你好|世界|EXIT'
    $r = RecvUntil $H3 'ROOM_MSG|Pia：你好|世界|EXIT' 3000
    Check 'H4 聊天注入竖线原样广播不入指令（不崩）' ($r -match 'Pia：你好\|世界\|EXIT')
    Close-Client $H1
    Close-Client $H3
    Kill-All
}
catch {
    Write-Output ("EXCEPTION: " + $_.Exception.Message)
    Write-Output $_.ScriptStackTrace
    $script:fail++
}

Finish
