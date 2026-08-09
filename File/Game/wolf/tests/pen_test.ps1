# 狼人杀房间系统渗透测试脚本（测试工具，非游戏实现）
# 用法: powershell -File D:\Demon\wolf\tests\pen_test.ps1
#
# 只做测试与报告，不修改任何 .cpp/.h 与既有测试脚本。
# 通过裸 socket 模拟多个客户端，对 Start.exe 房间管理器做安全/健壮性验证：
#   1) 名字注入清洗（竖线/引号/CR/控制字符、注入不得变成第二条指令）
#   2) 重名拒绝
#   3) 名字长度截断（ASCII 10 / 汉字 10 码点不劈字）
#   4) IP 格式名字拒绝（含前导零异写）
#   5) 空名字回退默认名
#   6) Start.exe 传非法端口参数的行为
#   7) 非房主越权（START/AUTO/BAN/UNBAN/PICK/TRANSFER 全被拒）
#   8) 黑名单绕过（同名重连/大小写/空格/超长/尾部空格回退 Player；IP 拉黑换名重连）
#   9) 职业配置非法值流程 + EXIT 干净退出 + 之后命令正常
#   10) 比例配置非法输入不修改当前比例
#   11) 房间内断线：服务端不崩、其余玩家正常、断线者重连不崩
#   12) 大厅命令隔离（未入房执行房间命令被拒不崩）
$ErrorActionPreference = 'Stop'

$wolf = Split-Path $PSScriptRoot -Parent
$tests = "$wolf\tests"
$pass = 0
$fail = 0
$log = New-Object System.Collections.Generic.List[string]

function Check($desc, $cond) {
    if ($cond) { $script:pass++; Write-Output ("PASS  " + $desc); $tag = 'PASS' }
    else       { $script:fail++; Write-Output ("FAIL  " + $desc); $tag = 'FAIL' }
    $script:log.Add(($tag + "  " + $desc))
}

# ---------- 基础 socket 封装（复用 proto_test 的套路） ----------

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

# 裸连接：只做 HELLO 握手，不设名字（供需要自定义 NAME 的用例）
function Connect-Naked() {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', 8888)
    $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s)
    $w.NewLine = "`n"
    $w.AutoFlush = $true
    $w.WriteLine('HELLO|3')
    $null = RecvUntilStream $s 'WELCOME' 3000
    return @{ c = $c; s = $s; w = $w; wlock = [object]::new() }
}

# 完整客户端：HELLO + NAME
function Open-Client($name) {
    $cl = Connect-Naked
    Send-Raw $cl ('NAME|' + $name)
    $null = Recv $cl 'NAME_SET' 3000
    $null = $script:liveClients.Add($cl)
    return $cl
}

# 主线程与保活线程并发写同一 StreamWriter 不线程安全，写操作统一持锁串行化
function Write-Locked($cl, $cmd) {
    [System.Threading.Monitor]::Enter($cl.wlock)
    try { $cl.w.WriteLine($cmd) } finally { [System.Threading.Monitor]::Exit($cl.wlock) }
}

function Send-Raw($cl, $cmd) { Write-Locked $cl $cmd }

function Recv($cl, $match, $timeoutMs = 5000) {
    return RecvUntilStream $cl.s $match $timeoutMs
}

# STATUS 多行回复（§16.5 竖排表）：头行换行已被 RecvUntilStream 消费，
# 剩余行用 RecvAll 补读后拼回换行（同 round5 Recv-LG 模式）
function Recv-Status($cl, $timeoutMs = 3000) {
    $hdr = Recv $cl 'ROOM_STATUS' $timeoutMs
    if (-not $hdr) { return '' }
    $chunk = RecvAll $cl 800
    return ($hdr + "`n" + $chunk)
}

function RecvAll($cl, $timeoutMs = 500) {
    $deadline = [DateTime]::Now.AddMilliseconds($timeoutMs)
    $bytes = New-Object System.Collections.Generic.List[byte]
    while ([DateTime]::Now -lt $deadline) {
        if ($cl.s.DataAvailable) {
            $b = $cl.s.ReadByte()
            if ($b -lt 0) { break }
            $bytes.Add([byte]$b)
        } else {
            Start-Sleep -Milliseconds 20
        }
    }
    return [System.Text.Encoding]::UTF8.GetString($bytes.ToArray())
}

function SendNull($cl, $cmd) { Write-Locked $cl $cmd }

function Close-Client($cl) {
    try { if ($cl -and $cl.c) { $null = $script:liveClients.Remove($cl); $cl.c.Close() } } catch {}
}

function Kill-Process {
    Get-Process -Name Start,Server,Client -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

# §12.7 保活基础设施：Start 第三轮新增 10 秒心跳失联判定（现改为 3 秒），本脚本的长生命周期
# 裸 socket 连接必须持续 PING，否则用例中途被误判失联（2026-08-05 实测：
# 用例 8h 的 Frank 静默超过 10 秒被 Start 按失联清掉，8h 误报失败）。
# 后台 runspace 每 1 秒遍历所有在线客户端发一行 PING（Start 视为保留字不处理）。
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

# 汉字字符串不借助 BOM 也能在正文中准确拼接（依赖脚本以 UTF-8 BOM 保存）
$H10 = '一二三四五六七八九十'

# ============ 准备与清理 ============
try {
    # ---------- 阶段 0：端口参数启动 Start.exe ----------
    # Start.exe 支持可选端口参数：非法值直接报中文错误退出（不进入交互），
    # 合法值（1024-65535）绑定该自定义端口监听。
    $badPorts = @('80', '1023', '65536', 'abc')
    foreach ($p in $badPorts) {
        Kill-Process
        Start-Sleep -Milliseconds 500
        $tmpOut = "$tests\pen_port_out.txt"
        $tmpErr = "$tests\pen_port_err.txt"
        if (Test-Path $tmpOut) { Remove-Item $tmpOut -Force }
        if (Test-Path $tmpErr) { Remove-Item $tmpErr -Force }
        $proc = Start-Process -FilePath "$wolf\Start.exe" -ArgumentList @($p) `
            -WindowStyle Hidden -RedirectStandardOutput $tmpOut `
            -RedirectStandardError $tmpErr -PassThru
        Start-Sleep -Seconds 3
        $ok = $false
        if ($proc.HasExited) {
            $code = $proc.ExitCode
            $errText = $null
            if (Test-Path $tmpOut) { $errText = [IO.File]::ReadAllText($tmpOut) }
            if ([string]::IsNullOrEmpty($errText) -and (Test-Path $tmpErr)) { $errText = [IO.File]::ReadAllText($tmpErr) }
            $hasChinese = $false
            if ($errText) { $hasChinese = $errText -match '[\u4e00-\u9fff]' }
            $ok = ($code -ne 0) -or $hasChinese
        }
        Check ("非法端口 [" + $p + "] 启动应报中文错误退出") $ok
        if (-not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
    }
    # 合法端口边界：应绑定自定义端口监听（而非固定 8888）
    foreach ($p in @('1024', '65535')) {
        Kill-Process
        Start-Sleep -Milliseconds 500
        # 若本机其他进程（如系统服务）已占用该端口，无法验证 Start 的绑定行为，
        # 记为 SKIP 而非 FAIL（环境条件，与 Start.exe 自身行为无关）
        $occupied = $null -ne (Get-NetTCPConnection -LocalPort ([int]$p) -State Listen -ErrorAction SilentlyContinue)
        if ($occupied) {
            Write-Output ("SKIP  端口 [" + $p + "] 已被本机其他进程占用，跳过验证")
            continue
        }
        $tmpOut = "$tests\pen_port_out.txt"
        $tmpErr = "$tests\pen_port_err.txt"
        if (Test-Path $tmpOut) { Remove-Item $tmpOut -Force }
        if (Test-Path $tmpErr) { Remove-Item $tmpErr -Force }
        $proc = Start-Process -FilePath "$wolf\Start.exe" -ArgumentList @($p) `
            -WindowStyle Hidden -RedirectStandardOutput $tmpOut `
            -RedirectStandardError $tmpErr -PassThru
        Start-Sleep -Seconds 3
        $listening = $false
        if (-not $proc.HasExited) {
            try {
                $probe = New-Object Net.Sockets.TcpClient
                $probe.Connect('127.0.0.1', [int]$p)
                $listening = $true
                $probe.Close()
            } catch {}
        }
        Check ("端口 [" + $p + "] Start.exe 正常监听自定义端口") $listening
        if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    }

    # ---------- 拉起房间管理器（供后续所有用例复用） ----------
    Kill-Process
    Start-Sleep -Milliseconds 500
    $startProc = Start-Process -FilePath "$wolf\Start.exe" -ArgumentList @('8888') -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 2
    Start-Keepalive

    # ---------- 用例 1：名字注入清洗 ----------
    # 竖线注入：整行是一条 NAME，注入的 |READY|1 不得成为第二条命令
    $A = Open-Client 'Alice'
    Send-Raw $A 'CREATE|5002'
    $r = Recv $A 'CREATED'
    Check '1a 建房成功' ($r -ne $null)

    $X = Connect-Naked
    Send-Raw $X 'NAME|Trap|READY|1'
    $r = Recv $X 'ERROR'
    Check '1b NAME 含竖线被拒且不成第二条指令（白名单）' ($r -match '名字只能包含中英文、数字与下划线')

    Send-Raw $X 'CREATE|5003'
    $r = Recv $X 'CREATED'
    Check '1c 注入者建房可正常进行' ($r -ne $null)
    Send-Raw $X 'STATUS'
    $r = Recv-Status $X
    Check '1d 注入尾置 READY 未生效（房主未处于已准备态）' ($r -match '\| Player\s*\|\s*0')

    # 引号 / 控制字符 0x01 / 0x0C 被白名单拒绝（不再净化后接受）
    $C = Connect-Naked
    $nameRaw = 'c"d' + [char]1 + [char]12 + '|e'
    Send-Raw $C ('NAME|' + $nameRaw)
    $r = Recv $C 'ERROR'
    Check '1e NAME 双引号/控制字符/竖线被拒且不崩' ($r -match '名字只能包含中英文、数字与下划线')

    # 内嵌 CR 被白名单拒绝（不再净化后接受）
    $N = Connect-Naked
    $crName = 'cr' + [char]13 + 'ab'
    Send-Raw $N ('NAME|' + $crName)
    $r = Recv $N 'ERROR'
    Check '1f NAME 内嵌 CR 被拒' ($r -match '名字只能包含中英文、数字与下划线')

    # 换行切分：单次发送里内嵌换行，第二行按普通未知行处理，服务端不崩
    $M = Connect-Naked
    $M.w.Write("NAME|L1`njunk2`n")
    $r1 = Recv $M 'NAME_SET'
    $r2 = Recv $M 'ERROR'
    Check '1g 换行切分第二行被当未知行（无崩溃）' (($r1 -match 'NAME_SET\|L1') -and ($r2 -match '不支持的命令'))
    Send-Raw $M 'NAME|Ok1'
    $r = Recv $M 'NAME_SET'
    Check '1h 注入序列后服务端仍可正常改名' ($r -match 'NAME_SET\|Ok1')

    foreach ($cl in @($A, $X, $C, $N, $M)) { Close-Client $cl }
    Start-Sleep -Milliseconds 300

    # ---------- 用例 2：重名拒绝 ----------
    $D1 = Connect-Naked
    Send-Raw $D1 'NAME|SameName'
    $r = Recv $D1 'NAME_SET'
    Check '2a 首人占用名字成功' ($r -match 'NAME_SET\|SameName')

    $D2 = Connect-Naked
    Send-Raw $D2 'NAME|SameName'
    $r = Recv $D2 'ERROR'
    Check '2b 重名被明确拒绝（中文提示）' ($r -match '已被占用')

    $D3 = Open-Client 'Another'
    Send-Raw $D3 'NAME|SameName'
    $r = Recv $D3 'ERROR'
    Check '2c 已连接玩家改名撞重名同样被拒' ($r -match '已被占用')

    foreach ($cl in @($D1, $D2, $D3)) { Close-Client $cl }

    # ---------- 用例 3：名字长度 ----------
    $L1 = Connect-Naked
    Send-Raw $L1 'NAME|abcdefghijk'
    $r = Recv $L1 'NAME_SET'
    Check '3a 11 个 ASCII 字符截成 10 个' ($r -eq 'NAME_SET|abcdefghij')

    $L2 = Connect-Naked
    Send-Raw $L2 ('NAME|' + $H10 + '壹')
    $r = Recv $L2 'NAME_SET'
    Check '3b 11 个汉字截到 10 个码点（不劈字）' ($r -eq 'NAME_SET|' + $H10)

    foreach ($cl in @($L1, $L2)) { Close-Client $cl }

    # ---------- 用例 4：IP 格式名字拒绝 ----------
    $P1 = Connect-Naked
    Send-Raw $P1 'NAME|192.168.1.1'
    $r = Recv $P1 'ERROR'
    Check '4a 真实 IP 形式名字被拒' ($r -match '名字只能包含中英文、数字与下划线')

    $P2 = Connect-Naked
    Send-Raw $P2 'NAME|199.9.1.1'
    $r = Recv $P2 'ERROR'
    Check '4b 无前导零 IP 形式名字被拒' ($r -match '名字只能包含中英文、数字与下划线')

    $P3 = Connect-Naked
    Send-Raw $P3 'NAME|199.09.1.1'
    $r = Recv $P3 'NAME_SET'
    Check '4c 前导零 IP 异写应被拒（actual: 若成功即 FAIL）' ($r -eq $null)

    foreach ($cl in @($P1, $P2, $P3)) { Close-Client $cl }

    # ---------- 用例 5：空名字回退默认名 ----------
    $E1 = Connect-Naked
    Send-Raw $E1 'NAME|'
    $r = Recv $E1 'NAME_SET'
    Check '5a 空名字回退为默认 Player（非裸奔空名）' ($r -eq 'NAME_SET|Player')
    Send-Raw $E1 'NAME|Filled'
    $r = Recv $E1 'NAME_SET'
    Check '5b 空名处理后仍可正常改名后续操作' ($r -match 'NAME_SET\|Filled')
    Close-Client $E1

    # ---------- 用例 12：大厅命令隔离 ----------
    $H = Connect-Naked
    Send-Raw $H 'PICK|x'
    $r = Recv $H 'ERROR'
    Check '12a 大厅执行 PICK 被拒' ($r -match '只有房主')
    Send-Raw $H 'READY'
    $r = Recv $H 'ERROR'
    Check '12b 大厅执行 READY 被拒' ($r -match '不在房间中')
    Send-Raw $H 'BAN|x'
    $r = Recv $H 'ERROR'
    Check '12c 大厅执行 BAN 被拒' ($r -match '只有房主')
    Send-Raw $H 'UNBAN|x'
    $r = Recv $H 'ERROR'
    Check '12d 大厅执行 UNBAN 被拒' ($r -match '只有房主')
    Send-Raw $H 'TRANSFER|1'
    $r = Recv $H 'ERROR'
    Check '12e 大厅执行 TRANSFER 被拒' ($r -match '只有房主')
    Send-Raw $H 'START'
    $r = Recv $H 'ERROR'
    Check '12f 大厅执行 START 被拒' ($r -match '只有房主')
    Send-Raw $H 'AUTO'
    $r = Recv $H 'ERROR'
    Check '12g 大厅执行 AUTO 被拒' ($r -match '只有房主')
    Send-Raw $H 'CREATE|5010'
    $r = Recv $H 'CREATED'
    Check '12h 拒绝后大厅命令仍正常（服务端未崩溃）' ($r -ne $null)
    Close-Client $H
    Start-Sleep -Milliseconds 300

    # ---------- 用例 7：非房主越权 ----------
    $G = Open-Client 'Grace'
    Send-Raw $G 'CREATE|5011'
    $null = Recv $G 'CREATED'
    $H2 = Open-Client 'Henry'
    Send-Raw $H2 'JOIN|5011'
    $null = Recv $H2 'JOINED'
    $I2 = Open-Client 'Iris'
    Send-Raw $I2 'JOIN|5011'
    $null = Recv $I2 'JOINED'

    $J2 = Open-Client 'Jack'
    Send-Raw $J2 'JOIN|5011'
    $null = Recv $J2 'JOINED'

    Send-Raw $J2 'START'
    $r = Recv $J2 'ERROR'
    Check '7a 非房主 START 被拒' ($r -match '只有房主')
    Send-Raw $J2 'AUTO'
    $r = Recv $J2 'ERROR'
    Check '7b 非房主 AUTO 被拒' ($r -match '只有房主')
    Send-Raw $J2 'BAN|Iris'
    $r = Recv $J2 'ERROR'
    Check '7c 非房主 BAN 被拒' ($r -match '只有房主')
    Send-Raw $J2 'UNBAN|Iris'
    $r = Recv $J2 'ERROR'
    Check '7d 非房主 UNBAN 被拒' ($r -match '只有房主')
    Send-Raw $J2 'PICK|Grace'
    $r = Recv $J2 'ERROR'
    Check '7e 非房主 PICK 被拒' ($r -match '只有房主')
    Send-Raw $J2 'TRANSFER|Grace'
    $r = Recv $J2 'ERROR'
    Check '7f 非房主 TRANSFER 被拒' ($r -match '只有房主')

    Send-Raw $G 'STATUS'
    $r = Recv-Status $G
    Check '7g 越权攻击不破坏房主视角状态' ($r -match 'Grace')

    foreach ($cl in @($G, $H2, $I2, $J2)) { Close-Client $cl }
    Start-Sleep -Milliseconds 300

    # ---------- 用例 8：黑名单绕过 ----------
    $Room810 = '5013'
    $E = Open-Client 'Eve'
    Send-Raw $E "CREATE|$Room810"
    $null = Recv $E 'CREATED'
    $Fran = Open-Client 'Frank'
    Send-Raw $Fran "JOIN|$Room810"
    $null = Recv $Fran 'JOINED'
    $Grac = Open-Client 'Grace'
    Send-Raw $Grac "JOIN|$Room810"
    $null = Recv $Grac 'JOINED'
    $Hen = Open-Client 'Henry'
    Send-Raw $Hen "JOIN|$Room810"
    $null = Recv $Hen 'JOINED'

    Send-Raw $E 'BAN|Grace'
    $r = Recv $Grac 'KICKED' 3000
    Check '8a 房主拉黑房内玩家立即踢出' ($r -match '拉黑')

    # 其余玩家操作不受影响
    Send-Raw $E 'PING'
    Send-Raw $Fran 'STATUS'
    $r = Recv-Status $Fran
    Check '8b 拉黑踢人后房内其余玩家仍可正常操作' (($r -match 'Frank') -and ($r -match 'Henry'))

    # (a) 同名字原样重连 → 拒绝
    Send-Raw $E 'PING'
    $G2 = Open-Client 'Grace'
    Send-Raw $G2 "JOIN|$Room810"
    $r = Recv $G2 'ERROR'
    Check '8c 被拉黑者同名字重连被拒' ($r -match '已被拉黑')
    Close-Client $G2

    # (b) 大小写变体：按 Start.cpp 比较逻辑应为大小写敏感放行（绕过点 → FAIL）
    Send-Raw $E 'PING'
    $gl = Open-Client 'grace'
    Send-Raw $gl "JOIN|$Room810"
    $r = Recv $gl 'JOINED'
    Check '8d 大小写变体重连应被拒（若成功即绕过点）' ($r -eq $null)
    Close-Client $gl

    # (c1) 尾部空格 → argStr 被解析成空 → 回退默认名 Player → 绕过拉黑
    Send-Raw $E 'PING'
    $gs = Open-Client 'Grace '
    Send-Raw $gs "JOIN|$Room810"
    $r = Recv $gs 'JOINED'
    Check '8e 尾随空格变体应被拒（实际回退 Player 加入=绕过）' ($r -eq $null)
    Close-Client $gs

    # (c2) 超长名截断：先拉黑 15 字符长名，服务端入库时按 10 码点截断；
    #     同名（同样会截断到同一串）重连应命中黑名单而非绕过
    Send-Raw $E 'BAN|GraceBanEvader'
    $r = Recv $E '已拉黑'
    Check '8f1 拉黑超长名被规范化截断后入库' ($r -match '已拉黑')
    $gl2 = Open-Client 'GraceBanEvader'
    Send-Raw $gl2 "JOIN|$Room810"
    $r = Recv $gl2 'ERROR'
    Check '8f2 超长截断同串重连仍被拒' ($r -match '已被拉黑')
    Close-Client $gl2

    # 取消按名拉黑后，再按 IP 拉黑
    Send-Raw $E 'UNBAN|Grace'
    $r = Recv $E '已取消拉黑'
    Check '8g 取消按名拉黑成功' ($r -match '已取消拉黑')

    Send-Raw $E "BAN|127.0.0.1"
    $r = Recv $Fran 'KICKED' 3000
    Check '8h 按 IP 拉黑同 IP 玩家被移出' ($r -match 'IP')

    $Zed = Open-Client 'Zed'
    Send-Raw $Zed "JOIN|$Room810"
    $r = Recv $Zed 'ERROR'
    Check '8i 同 IP 换新名字重连仍被拒' ($r -match 'IP 已被拉黑')
    Close-Client $Zed

    Send-Raw $E 'UNBAN|127.0.0.1'
    $r = Recv $E '已取消拉黑'
    Check '8j 取消 IP 拉黑' ($r -match '已取消拉黑')

    foreach ($cl in @($E, $Fran, $Hen)) { Close-Client $cl }
    Start-Sleep -Milliseconds 300

    # ---------- 用例 9：职业配置 ----------
    $K = Open-Client 'Kate'
    Send-Raw $K 'CREATE|5015'
    $null = Recv $K 'CREATED'
    $Lx = Open-Client 'Leo'
    Send-Raw $Lx 'JOIN|5015'
    $null = Recv $Lx 'JOINED'

    Send-Raw $K 'LEVEL|2'
    $r = Recv $K '职业档位已设为'
    Check '9a 档位 2（豪华）合法设置' ($r -match '职业档位已设为')

    Send-Raw $K 'LEVEL|9'
    $r = Recv $K 'ERROR'
    Check '9b 档位 9 非法拒绝' ($r -match '档位必须')

    Send-Raw $K 'LEVEL|-1'
    $r = Recv $K 'ERROR'
    Check '9c 档位 -1 非法拒绝' ($r -match '档位必须')

    Send-Raw $K 'LEVEL|abc'
    $r = Recv $K 'ERROR'
    Check '9d 档位 abc 非法拒绝' ($r -match '档位必须')

    Send-Raw $K 'VILLAGER|2'
    $r = Recv $K 'ERROR'
    Check '9e 村民开关非法值拒绝' ($r -match '村民开关必须')

    Send-Raw $K 'STATUS'
    $r = Recv-Status $K
    Check '9f 连续非法输入后服务未挂（状态正常）' ($r -match 'Kate')

    # EXIT 干净退出
    Send-Raw $K 'EXIT'
    $r = Recv $K 'LEFT_ROOM'
    Check '9g EXIT 干净退出房间' ($r -match 'LEFT_ROOM')
    Send-Raw $K 'READY'
    $r = Recv $K 'ERROR'
    Check '9h 退出后 READY 被拒（不在房间）' ($r -match '不在房间中')

    Send-Raw $Lx 'READY'
    $r = Recv $Lx 'READY_STATUS'
    Check '9i 房主退出后房内其余玩家 READY 正常' ($r -match 'READY_STATUS')

    Send-Raw $K 'CREATE|5017'
    $r = Recv $K 'CREATED'
    Check '9j EXIT 离去后原房主可重新建房（服务端未坏）' ($r -ne $null)

    foreach ($cl in @($Lx)) { Close-Client $cl }
    Close-Client $K
    Start-Sleep -Milliseconds 300

    # ---------- 用例 10：比例配置 ----------
    $M2 = Open-Client 'Mia'
    Send-Raw $M2 'CREATE|5016'
    $null = Recv $M2 'CREATED'
    $N2 = Open-Client 'Noah'
    Send-Raw $N2 'JOIN|5016'
    $null = Recv $N2 'JOINED'
    $O2 = Open-Client 'Owen'
    Send-Raw $O2 'JOIN|5016'
    $null = Recv $O2 'JOINED'
    $Q2 = Open-Client 'Quinn'
    Send-Raw $Q2 'JOIN|5016'
    $null = Recv $Q2 'JOINED'

    Send-Raw $M2 'RATIO|1|1|2'
    $r = Recv $M2 '比例已设为'
    Check '10a 合法比例 1|1|2（4 人总和=4）设置成功' ($r -match '比例已设为')

    Send-Raw $M2 'RATIO|a|b|c'
    $r = Recv $M2 'ERROR'
    Check '10b 非数字比例被拒' ($r -match '比例不合法')

    Send-Raw $M2 'RATIO|-1|3|0'
    $r = Recv $M2 'ERROR'
    Check '10c 负数比例被拒' ($r -match '比例不合法')

    Send-Raw $M2 'RATIO|2|2'
    $r = Recv $M2 'ERROR'
    Check '10d 参数个数不足被拒' ($r -match '需三个数字')

    Send-Raw $M2 'RATIO 2 0 2'
    $rspace = Recv $M2 '比例已设为'
    Check '10e 空格分隔形式被容忍（贴合 Start 实现接受）' ($rspace -match '比例已设为')

    Send-Raw $M2 'RATIO|5|0|0'
    $r2 = Recv $M2 'ERROR'
    Check '10f 总和与人数不符被拒' ($r2 -match '比例不合法')

    # 验证非法比例未改变：重新设置后再查，应仍是合法状态可接收后续命令
    Send-Raw $M2 'RATIO|2|0|2'
    $r = Recv $M2 '比例已设为'
    Check '10g 非法输入后服务端状完好（重新合法设置成功）' ($r -match '比例已设为')

    foreach ($cl in @($M2, $N2, $O2, $Q2)) { Close-Client $cl }
    Start-Sleep -Milliseconds 300

    # ---------- 用例 11：游戏中断线 ----------
    $R1 = Open-Client 'Ruby'
    Send-Raw $R1 'CREATE|5018'
    $null = Recv $R1 'CREATED'
    $S2 = Open-Client 'Sasha'
    Send-Raw $S2 'JOIN|5018'
    $null = Recv $S2 'JOINED'
    $T2 = Open-Client 'Tom'
    Send-Raw $T2 'JOIN|5018'
    $null = Recv $T2 'JOINED'
    $U2 = Open-Client 'Uma'
    Send-Raw $U2 'JOIN|5018'
    $null = Recv $U2 'JOINED'

    # 故意粗暴断线（不发 EXIT）
    Close-Client $U2
    Start-Sleep -Milliseconds 400

    Send-Raw $S2 'STATUS'
    $r = Recv-Status $S2
    Check '11a 断线后服务端不崩，其余玩家仍可操作' ($r -match 'Sasha')

    # 断线后聊天功能正常：发言广播给房间内其他人（发送者不回显，见 Start.cpp RoomMsg）
    Send-Raw $S2 'hello'
    $r = Recv $T2 'ROOM_MSG|Sasha：' 3000
    Check '11b 断线后聊天功能正常（他人收到发言）' ($r -eq 'ROOM_MSG|Sasha：hello')

    # 断线者重连
    $U2b = Open-Client 'Uma'
    Send-Raw $U2b 'JOIN|5018'
    $r = Recv $U2b 'JOINED'
    Check '11c 断线玩家重连 JOIN 成功（服务端未崩）' ($r -ne $null)

    # 断线玩家重新建房
    Send-Raw $U2b 'EXIT'
    $null = Recv $U2b 'LEFT_ROOM'
    Send-Raw $U2b 'CREATE|5019'
    $r = Recv $U2b 'CREATED'
    Check '11d 断线玩家回大厅重新建房成功' ($r -ne $null)

    foreach ($cl in @($R1, $S2, $T2, $U2b)) { Close-Client $cl }
    Start-Sleep -Milliseconds 300

    # 收尾探针：服务端整体存活
    $Alive = Open-Client 'Probe'
    Send-Raw $Alive 'LIST'
    $r = Recv $Alive 'ROOMS_LIST'
    Check '13 全程渗透后房间管理器存活可应答' ($r -ne $null)
    Close-Client $Alive
}
catch {
    Write-Output ("脚本异常: " + $_)
    $script:fail++
}
finally {
    Stop-Keepalive
    Kill-Process
}

$script:log.Add("")
$script:log.Add(("===== 结果: PASS=" + $pass + " FAIL=" + $fail + " ====="))
Write-Output ""
Write-Output ("===== 结果: PASS=" + $pass + " FAIL=" + $fail + " =====")
try {
    $utf8 = New-Object System.Text.UTF8Encoding($true)
    [System.IO.File]::WriteAllLines("$tests\pen_out.txt", $script:log, $utf8)
} catch {}
if ($fail -gt 0) { exit 1 } else { exit 0 }