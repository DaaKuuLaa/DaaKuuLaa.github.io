# 狼人杀第五轮验收脚本（测试工具，非游戏实现）
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File tests\round5_test.ps1 *> tests\round5_out.txt
# 覆盖 REQUIREMENTS.md §14.7 验收 1-9：
#   1. 白天输入（4 人直连局 1狼0中立2神 村民开）：狼刀预言家 → 死亡预言家收「你已死亡」、
#      白天收不到 __DAY_OPEN__、聊天不被广播；3 名存活者收到 __DAY_OPEN__ 且正常投票；
#      遗言正常广播；预言家真断线重连（欢迎回来）+ 重复连接 already connected 拒绝
#   2. 重连：正常结束局（__GAME_OVER__ 终态行）10 秒内回房 JOINED（房主/成员全测）
#   3. IP 命令：房主查询正确 IP、未知名 clean 报错、非房主被拒
#   4. LG：in/out 状态、房主专属、随房间销毁（新房间无旧记录）、行对齐
#   5. BAN 批量：名字+IP 一次拉黑全部生效（管道格式回归 §14.5 首项失效修复）、
#      被拉黑者 JOIN 被拒、UNBAN 批量解除
#   6. BAN 文件：相对/绝对 .ban 导入、缺失文件/路径穿越 clean 报错、非 .ban 按名字处理、
#      UNBAN 文件批量解除
#   7. NAME 白名单：空格/标点/emoji/全角 拒绝；字母数字下划线/汉字/混合 成功；
#      11 字符截断与 IP 形似拒绝不回归
#   8. 攻击：非房间成员伪造 GAME_ENDED 不破坏对局（外人 JOIN 仍被拒、房间仍在 LIST、
#      原成员可回房）；批量 BAN 混自己名字 → 拒绝 1 项其余生效；垃圾 __GAME_OVER__ 行无副作用
#   9. 汇总 PASS 数与 exit code
# 大厅流程通过裸 socket 直连 Start.exe（8888）；游戏局通过裸 socket 直连 Server.exe。
# 所有连接由后台 runspace 每 1 秒发 PING 保活（StreamWriter 加锁，AGENTS.md 踩坑 7/11）。

$ErrorActionPreference = 'Stop'
$wolf = Split-Path $PSScriptRoot -Parent
$script:pass = 0
$script:fail = 0

function Check($desc, $cond) {
    if ($cond) { $script:pass++; Write-Output ("PASS  " + $desc) }
    else       { $script:fail++; Write-Output ("FAIL  " + $desc) }
}

# ============ 保活 runspace（后台每 1 秒给所有在线连接发 PING） ============
# 长静默窗口内连接不发字节会被 Start/Server 的 3 秒失联判定误杀（踩坑 7）；
# 后台 runspace 与主线程共用 StreamWriter 必须加锁（踩坑 11）。
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

# ============ 裸 socket 客户端封装 ============

function Write-Locked($cl, $cmd) {
    [System.Threading.Monitor]::Enter($cl.wlock)
    try {
        $cl.w.WriteLine($cmd)
    } catch {
        # 连接已被服务端关闭（如 BAN 踢出）时写入抛 IOException：
        # 标记关闭静默吞掉，让后续 RecvUntil 超时产生对应 FAIL，而不是崩掉整个脚本
        $cl.closed = $true
        $cl.alive = $false
    } finally { [System.Threading.Monitor]::Exit($cl.wlock) }
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
        try {
            if ($s.DataAvailable) {
                $b = $s.ReadByte()
                if ($b -lt 0) { break }
                $bytes.Add([byte]$b)
            } else {
                Start-Sleep -Milliseconds 20
            }
        } catch {
            # 连接已被服务端关闭（如 BAN 踢出后继续读）：按无数据返回，不抛异常
            break
        }
    }
    return $bytes.ToArray()
}

function RecvAll($cl, $timeoutMs = 500) {
    $arr = ReadChunk $cl.s $timeoutMs
    if ($null -eq $arr) { return '' }
    return [System.Text.Encoding]::UTF8.GetString($arr)
}

# 大厅客户端：HELLO/NAME 握手完成后再注册进保活列表，保证首行不被 PING 打断
function New-Client($name) {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', 8888)
    $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s)
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

function Close-Client($cl) {
    try { if ($cl -and $script:liveClients.Contains($cl)) { $null = $script:liveClients.Remove($cl) } } catch {}
    try { if ($cl -and $cl.c) { $cl.c.Close() } } catch {}
}

# 游戏机器人：先发 PLAYER_ID|k 首行再注册保活（Server 接受线程只认首行为身份行，
# PING 先到会被当成非法身份拒绝连接）
function New-Bot($k, $port) {
    $c = Connect-Retry $port
    $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s)
    $w.NewLine = "`n"
    $w.AutoFlush = $true
    $w.WriteLine('PLAYER_ID|' + $k)
    $bot = @{
        k = $k; c = $c; s = $s; w = $w; wlock = [object]::new(); closed = $false
        role = ''; witchInputs = 0
        bytes = [System.Collections.Generic.List[byte]]::new()
        queue = [System.Collections.Queue]::new()
    }
    $null = $script:liveClients.Add($bot)
    return $bot
}

function Close-Bot($bot) {
    $bot.closed = $true
    try { if ($script:liveClients.Contains($bot)) { $null = $script:liveClients.Remove($bot) } } catch {}
    try { $bot.c.Close() } catch {}
}

# ============ 进程 / 端口 / 连接工具 ============

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

# 空闲端口探测（1024-65535；Start 的房间端口即游戏服务器端口，一房一端口）
function Get-FreePort {
    for ($p = 8120; $p -lt 8190; $p++) {
        try {
            $l = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Any, $p)
            $l.Start()
            $l.Stop()
            return $p
        } catch {}
    }
    return 8120
}

# 带重试的连接：Server.exe 刚拉起时监听可能还没就绪
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

function Is-WolfRole($role) {
    return ($role -eq 'werewolf') -or ($role -eq 'whitewolf')
}

function Get-WolfTarget($bots) {
    foreach ($b in $bots) {
        if (-not $b.closed -and -not (Is-WolfRole $b.role)) { return $b.k }
    }
    return 0
}

function Get-SeerTarget($bots, $selfK) {
    foreach ($b in $bots) {
        if (-not $b.closed -and $b.k -ne $selfK) { return $b.k }
    }
    return 0
}

# 机器人泵（单线程轮询式收包，参考 server_test8/round4 模式）
function Pump-Bot($bot) {
    if ($bot.closed) { return }
    try {
        while ($bot.s.DataAvailable) {
            $b = $bot.s.ReadByte()
            if ($b -lt 0) { $bot.closed = $true; break }
            if ($b -eq 10) {
                $raw = $bot.bytes.ToArray()
                $bot.bytes.Clear()
                $line = [System.Text.Encoding]::UTF8.GetString($raw)
                $line = $line.TrimEnd("`r")
                if ($line.Length -gt 0) { $bot.queue.Enqueue($line) }
            } else {
                $bot.bytes.Add([byte]$b)
            }
        }
    } catch { $bot.closed = $true }
}

# 原始连接探测：给游戏端口灌垃圾行，返回是否无异常完成
function Probe-RawLines($port, $lines) {
    try {
        $c = New-Object Net.Sockets.TcpClient
        $c.Connect('127.0.0.1', $port)
        $s = $c.GetStream()
        $w = New-Object IO.StreamWriter($s)
        $w.NewLine = "`n"
        $w.AutoFlush = $true
        foreach ($ln in $lines) { $w.WriteLine($ln) }
        Start-Sleep -Milliseconds 400
        $c.Close()
        return $true
    } catch { return $false }
}

# 已连接槽位探测：向游戏端口以相同 PLAYER_ID 重复连接，返回收到的文本
function Probe-AlreadyConnected($port, $k) {
    try {
        $c = New-Object Net.Sockets.TcpClient
        $c.Connect('127.0.0.1', $port)
        $s = $c.GetStream()
        $w = New-Object IO.StreamWriter($s)
        $w.NewLine = "`n"
        $w.AutoFlush = $true
        $w.WriteLine('PLAYER_ID|' + $k)
        $deadline = [DateTime]::Now.AddSeconds(4)
        $text = ''
        $buf = New-Object System.Collections.Generic.List[byte]
        while ([DateTime]::Now -lt $deadline -and $text -notmatch 'already connected') {
            if ($s.DataAvailable) {
                $b = $s.ReadByte()
                if ($b -lt 0) { break }
                $buf.Add([byte]$b)
                if ($b -eq 10) {
                    $text += [System.Text.Encoding]::UTF8.GetString($buf.ToArray())
                    $buf.Clear()
                }
            } else {
                Start-Sleep -Milliseconds 20
            }
        }
        $c.Close()
        return $text
    } catch { return '' }
}

# LG 多行回复：头行（ROOM_MSG|Log List）的换行已被 RecvUntilStream 消费，
# 补读的剩余行需补回换行再拼接，否则首行记录会与头部粘连（2026-08-06 实测）
function Recv-LG($cl) {
    $hdr = RecvUntilStream $cl.s 'Log List' 3000
    if (-not $hdr) { return '' }
    $chunk = ReadChunk $cl.s 800
    $rows = [System.Text.Encoding]::UTF8.GetString($chunk)
    return ($hdr + "`n" + $rows)
}

# STATUS 多行回复（§16.5 竖排表）：头行换行已被消费，补读后拼回换行（同 Recv-LG）
function Recv-Status($cl, $timeoutMs = 3000) {
    $hdr = RecvUntilStream $cl.s 'ROOM_STATUS' $timeoutMs
    if (-not $hdr) { return '' }
    $chunk = ReadChunk $cl.s 800
    $rows = [System.Text.Encoding]::UTF8.GetString($chunk)
    return ($hdr + "`n" + $rows)
}

# 显示宽度（半角 1 列、全角 2 列，与 Start.cpp DisplayWidth 同语义）
function Get-DispWidth($s) {
    $w = 0
    foreach ($ch in $s.ToCharArray()) {
        if ([int][char]$ch -gt 255) { $w += 2 } else { $w += 1 }
    }
    return $w
}

# NAME 试探：裸连接改名后取回复行（NAME_SET|... 或 ERROR|...），用完即关。
# 被拒名不会回 NAME_SET，若先等 NAME_SET 会把 ERROR 行吞掉再超时（第二轮 G 段全失的原因），
# 故只发一次 NAME，逐行读到 NAME_SET 或 ERROR 任一即返回
function Try-Name($name) {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', 8888)
    $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s)
    $w.NewLine = "`n"
    $w.AutoFlush = $true
    $w.WriteLine('HELLO|3')
    $null = RecvUntilStream $s 'WELCOME' 3000
    $w.WriteLine('NAME|' + $name)
    $deadline = [DateTime]::Now.AddSeconds(3)
    $pending = New-Object System.Collections.Generic.List[byte]
    $ret = $null
    while ([DateTime]::Now -lt $deadline) {
        while ($s.DataAvailable) {
            $b = $s.ReadByte()
            if ($b -lt 0) { $c.Close(); return $ret }
            $pending.Add([byte]$b)
            if ($b -eq 10) {
                $line = [System.Text.Encoding]::UTF8.GetString($pending.ToArray()).TrimEnd("`r", "`n")
                $pending.Clear()
                if ($line.Contains('NAME_SET') -or $line.Contains('ERROR')) { $ret = $line; $c.Close(); return $ret }
            }
        }
        Start-Sleep -Milliseconds 20
    }
    $c.Close()
    return $ret
}

# ============ 标准 4 人局驱动 ============
# 1 狼 0 中立 2 神（预言家+女巫）村民开：狼刀预言家、女巫双弃、白天全员投狼出局、
# 遗言跳过；按需附加：预言家断线重连（reconnect）、死者白天发聊天（ghostChat）、
# 存活者白天发 __GAME_OVER__ 聊天（fakeOver）。
# Server.exe 参数序（与 round3/4 已验证用法一致）：
#   port 名字... startIp startPort roomId 狼 中立 神 档位 村民 语言码...
# 尾部必须带 N 个语言码（总数 9+2N，缺了人数会算错，踩坑 13）。
function Play-StandardGame($port, $names, $reconnect, $ghostChat, $fakeOver) {
    $bots = @()
    for ($i = 1; $i -le 4; $i++) { $bots += New-Bot $i $port }

    foreach ($b in $bots) {
        $r = RecvUntilStream $b.s 'ROLE|' 10000
        if ($r) { $b.role = $r.Substring(5) }
    }

    $roles = @()
    foreach ($b in $bots) { if ($b.role) { $roles += $b.role } }
    $wolfB = $bots | Where-Object { Is-WolfRole $_.role } | Select-Object -First 1
    $seerB = $bots | Where-Object { $_.role -eq 'seer' } | Select-Object -First 1

    $stat = @{
        roles = $roles; wolfK = 0; wolfName = ''; seerK = 0; seerName = ''
        reconnectOk = $false; alreadyRejected = $false
        seerDeath = $false; seerDayOpen = 0; aliveDayOpenCount = 0
        ghostHeard = $false; ghostSent = $false
        fakeHeard = $false; fakeSent = $false; prematureOver = $false
        wolfExiled = $false; lastWord = ''
        winner = ''; over = $false; overCount = 0
        exile = $null; crashed = $false
        lastWordsSent = $false
        bots = $bots
    }

    if ($wolfB) { $stat.wolfK = $wolfB.k; $stat.wolfName = $names[$wolfB.k - 1] }
    if ($seerB) { $stat.seerK = $seerB.k; $stat.seerName = $names[$seerB.k - 1] }

    # 预言家断线重连（验证「欢迎回来」），重连后的新机器人顶替旧对象
    if ($reconnect -and $seerB) {
        $sk = $seerB.k
        Close-Bot $seerB
        Start-Sleep -Seconds 2
        $nb = New-Bot $sk $port
        $wel = RecvUntilStream $nb.s '欢迎回来' 5000
        $stat.reconnectOk = ($wel -match '欢迎回来')
        $nb.role = 'seer'
        $bots[$sk - 1] = $nb
        $seerB = $nb

        $probe = Probe-AlreadyConnected $port $sk
        $stat.alreadyRejected = ($probe -match 'already connected')
    }

    $dawnSeen = $false
    $aliveDayOpen = @{}
    foreach ($b in $bots) { $aliveDayOpen[$b.k] = 0 }

    $deadline = [DateTime]::Now.AddSeconds(150)
    $done = $false

    while (-not $done) {
        if ($srvProc.HasExited) { break }

        foreach ($b in $bots) { if (-not $b.closed) { Pump-Bot $b } }

        foreach ($b in $bots) {
            while ($b.queue.Count -gt 0) {
                $line = $b.queue.Dequeue()

                if ($line.Contains('天亮了')) { $dawnSeen = $true }
                if ($line.Contains('你已死亡')) {
                    if ($b.k -eq $stat.seerK) { $stat.seerDeath = $true }
                }
                if ($line.Trim() -eq '__DAY_OPEN__') {
                    if ($b.k -eq $stat.seerK) { $stat.seerDayOpen++ }
                    else { $aliveDayOpen[$b.k]++ }
                }
                if ($line -match '被放逐。') { $stat.exile = $line }
                if ($line -match '胜利方') { $stat.winner = $line }
                if ($line -match '遗言：') { $stat.lastWord = $line }
                if ($line.Contains('幽灵发言')) { $stat.ghostHeard = $true }
                if ($line -match '：__GAME_OVER__') { $stat.fakeHeard = $true }
                if ($line.Trim() -eq '__GAME_OVER__') {
                    $b.gameOverSeen = $true
                    $b.overAt = [DateTime]::Now
                    $stat.overCount++
                    if (-not $stat.exile) { $stat.prematureOver = $true }
                }

                if ($line.Trim() -eq '__INPUT__' -and -not $b.closed) {
                    if (-not $dawnSeen) {
                        # 夜晚阶段按角色应答：狼刀预言家、预言家查验、女巫双弃
                        if ($b.k -eq $stat.wolfK -and $stat.seerK -gt 0) {
                            SendLine $b ('PLAYER_' + $b.k + '|' + $stat.seerK)
                        } elseif ($b.k -eq $stat.seerK) {
                            $t = Get-SeerTarget $bots $stat.seerK
                            if ($t -gt 0) { SendLine $b ('PLAYER_' + $b.k + '|' + $t) }
                        } elseif ($b.role -eq 'witch') {
                            SendLine $b ('PLAYER_' + $b.k + '|0')
                        } else {
                            SendLine $b ('PLAYER_' + $b.k + '|0')
                        }
                    } else {
                        # 白天窗口期的 __INPUT__ = 被放逐者遗言
                        if (-not $stat.lastWordsSent) {
                            SendLine $b ('PLAYER_' + $b.k + '|遗言内容')
                            $stat.lastWordsSent = $true
                        }
                    }
                }
            }
        }

        # 死者白天发聊天（应被服务端静默丢弃，不广播）
        if ($ghostChat -and -not $stat.ghostSent -and $stat.seerDeath -and $dawnSeen) {
            SendLine $seerB ('PLAYER_' + $seerB.k + '|幽灵发言')
            $stat.ghostSent = $true
        }

        # 存活者白天发 __GAME_OVER__ 聊天（应作为普通聊天广播，不触发终态）
        if ($fakeOver -and -not $stat.fakeSent -and $dawnSeen) {
            foreach ($fb in $bots) {
                if (-not $fb.closed -and $fb.k -ne $stat.seerK -and $fb.k -ne $stat.wolfK) {
                    SendLine $fb ('PLAYER_' + $fb.k + '|__GAME_OVER__')
                    $stat.fakeSent = $true
                    break
                }
            }
        }

        # 白天投票：3 名存活者投狼出局
        if ($dawnSeen -and -not $stat.exile) {
            $aliveKs = @($bots | Where-Object { -not $_.closed -and $_.k -ne $stat.seerK } | ForEach-Object { $_.k })
            foreach ($b in $bots) {
                if ($b.closed -or $b.k -eq $stat.seerK -or $b.voted) { continue }
                if ($aliveDayOpen[$b.k] -le 0) { continue }
                $t = $stat.wolfK
                if ($b.k -eq $stat.wolfK) {
                    $t = 0
                    foreach ($ak in $aliveKs) { if ($ak -ne $stat.wolfK) { $t = $ak; break } }
                }
                if ($t -gt 0) { SendLine $b ('PLAYER_' + $b.k + '|VOTE|' + $t); $b.voted = $true }
            }
        }

        $allOver = $true
        foreach ($b in $bots) { if (-not $b.gameOverSeen) { $allOver = $false; break } }
        if ($allOver) { $stat.over = $true; $done = $true }
        if ([DateTime]::Now -gt $deadline) { $done = $true }

        Start-Sleep -Milliseconds 30
    }

    $stat.aliveDayOpenCount = @($bots | Where-Object { $_.k -ne $stat.seerK -and $aliveDayOpen[$_.k] -gt 0 }).Count
    $stat.wolfExiled = ($stat.exile -and ($stat.exile -match ('玩家' + $stat.wolfName + '（槽')))

    # 必须记录在 Stop-Process 之前：杀进程后 HasExited 恒 True，无法再判定崩溃（踩坑 15）
    $stat.crashed = $srvProc.HasExited
    foreach ($b in $bots) { Close-Bot $b }
    Stop-Process -Id $srvProc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300

    return $stat
}

# ============ 启动 ============
Kill-All
Start-Keepalive
$env:WOLF_VOTE_TIMEOUT_SECONDS = '8'

# ============ 验收 1：白天输入（4 人直连局，狼刀预言家） ============
$portA = Get-FreePort
$namesA = @('Amy', 'Ben', 'Cara', 'Dan')
$srvArgsA = @($portA) + $namesA + @('127.0.0.1', '8888', 'R5DAY', '1', '0', '2', '0', '1', 'zh', 'zh', 'zh', 'zh')
$srvProc = Start-Process -FilePath "$wolf\Server.exe" -WorkingDirectory $wolf -ArgumentList $srvArgsA -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 2
$stA = Play-StandardGame $portA $namesA $true $true $true

Check 'A1 4 机器人身份齐（狼/预言家/女巫/村民都在场）' (
    ($stA.roles.Count -eq 4) -and ($stA.roles -contains 'werewolf') -and ($stA.roles -contains 'seer') -and ($stA.roles -contains 'witch'))
Check 'A2 预言家断线重连收到「欢迎回来」' $stA.reconnectOk
Check 'A3 重复连接同槽位被拒（already connected）' $stA.alreadyRejected
Check 'A4 狼刀预言家后死者收到「你已死亡」提示' $stA.seerDeath
Check 'A5 死亡预言家白天未收到 __DAY_OPEN__（死者静默）' ($stA.seerDayOpen -eq 0)
Check 'A6 3 名存活者均收到 __DAY_OPEN__ 且投票正常' ($stA.aliveDayOpenCount -eq 3 -and $stA.wolfExiled)
Check 'A7 死者聊天不被广播（幽灵发言未出现在任何玩家）' (-not $stA.ghostHeard)
Check 'A8 伪造 __GAME_OVER__ 聊天被当作普通聊天广播、无提前终态' ($stA.fakeHeard -and -not $stA.prematureOver)
Check 'A9 狼被放逐且遗言广播正常' ($stA.wolfExiled -and ($stA.lastWord -match '遗言：'))
Check 'A10 本局正常结束（__GAME_OVER__ 全员收到）' ($stA.overCount -eq 4)
Check 'A11 胜利方为好人阵营' ($stA.winner -match '好人阵营')
Check 'A12 白天进程未崩溃' (-not $stA.crashed)

# ============ 验收 2：重连（正常结束局 10 秒内回房 JOINED） ============
$rmB = Start-RM
$portB = Get-FreePort
$B1 = New-Client 'Eva'
SendLine $B1 ('CREATE|' + $portB)
$null = RecvUntil $B1 'CREATED' 3000
$B2 = New-Client 'Finn'
SendLine $B2 ('JOIN|' + $portB)
$null = RecvUntil $B2 'JOINED' 3000
$B3 = New-Client 'Gina'
SendLine $B3 ('JOIN|' + $portB)
$null = RecvUntil $B3 'JOINED' 3000
$B4 = New-Client 'Hugo'
SendLine $B4 ('JOIN|' + $portB)
$null = RecvUntil $B4 'JOINED' 3000

SendLine $B1 'VILLAGER|1'
$null = RecvUntil $B1 '村民职业已启用' 2000
SendLine $B1 'RATIO|1|0|2'
$null = RecvUntil $B1 '比例已设为' 2000
foreach ($cl in @($B1, $B2, $B3, $B4)) { SendLine $cl 'READY' }
Start-Sleep -Seconds 1
SendLine $B1 'START'
$gpB = RecvUntil $B1 'GAME_PREPARE|' 6000
$roomB = $gpB.Split('|')[2]
$gportB = $gpB.Split('|')[1]
foreach ($cl in @($B1, $B2, $B3, $B4)) { Close-Client $cl }
Start-Sleep -Milliseconds 500

$namesB = @('Eva', 'Finn', 'Gina', 'Hugo')
$srvArgsB = @($gportB) + $namesB + @('127.0.0.1', '8888', $roomB, '1', '0', '2', '0', '1', 'zh', 'zh', 'zh', 'zh')
$srvProc = Start-Process -FilePath "$wolf\Server.exe" -WorkingDirectory $wolf -ArgumentList $srvArgsB -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 2
$stB = Play-StandardGame $gportB $namesB $false $false $false
Check 'B0 完整对局正常结束（胜利方 + __GAME_OVER__ 全员）' ($stB.over -and ($stB.winner -match '好人阵营') -and ($stB.overCount -eq 4))

$rejoins = @()
foreach ($b in $stB.bots) {
    $nm = $namesB[$b.k - 1]
    Close-Bot $b
    $cl = New-Client $nm
    SendLine $cl ('GAME_ENDED|' + $roomB)
    SendLine $cl ('REJOIN|' + $roomB + '|' + $b.k)
    $rj = RecvUntil $cl 'JOINED' 5000
    $el = ([DateTime]::Now - $b.overAt).TotalSeconds
    $rejoins += @{ k = $b.k; ok = ($rj -match 'JOINED'); el = $el; cl = $cl }
}
$hostRj = $rejoins | Where-Object { $_.k -eq 1 } | Select-Object -First 1
$otherRj = $rejoins | Where-Object { $_.k -ne 1 }
Check 'B1 房主回房 JOINED 且 10 秒内（正常结束局）' ($hostRj.ok -and $hostRj.el -lt 10)
Check 'B2 其余 3 名玩家回房 JOINED 且均 10 秒内' (@($otherRj | Where-Object { -not $_.ok -or $_.el -ge 10 }).Count -eq 0)

$L1 = New-Client 'Lisa'
SendLine $L1 'LIST'
$r = RecvUntil $L1 'ROOMS_LIST' 3000
Check 'B3 全员回房后房间保留且无 [游戏中] 标记（可再开）' (
    $r -and ($r -match ([regex]::Escape($gportB) + '\s+4/12')) -and (-not $r.Contains('[游戏中]')))
Close-Client $L1

foreach ($rj in $rejoins) { Close-Client $rj.cl }
Stop-Process -Id $rmB.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

# ============ 验收 3：IP 命令 ============
$rmC = Start-RM
$portC = Get-FreePort
$C1 = New-Client 'Ivy'
SendLine $C1 ('CREATE|' + $portC)
$null = RecvUntil $C1 'CREATED' 3000
$C2 = New-Client 'Jack'
SendLine $C2 ('JOIN|' + $portC)
$null = RecvUntil $C2 'JOINED' 3000

SendLine $C1 'IP|Jack'
$r = RecvUntil $C1 '的 IP' 3000
Check 'C1 房主 IP 查询返回正确 IP' ($r -match 'Jack 的 IP：127\.0\.0\.1')

SendLine $C1 'IP|Nobody'
$r = RecvUntil $C1 'ERROR' 3000
Check 'C2 IP 未知名 clean 报错（未找到玩家）' ($r -match '未找到玩家 Nobody')

SendLine $C2 'IP|Ivy'
$r = RecvUntil $C2 'ERROR' 3000
Check 'C3 非房主 IP 命令被拒（只有房主）' ($r -match '只有房主')

foreach ($cl in @($C1, $C2)) { Close-Client $cl }
Stop-Process -Id $rmC.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

# ============ 验收 4：LG 进出记录 ============
$rmD = Start-RM
$portD = Get-FreePort
$D1 = New-Client 'Liam'
SendLine $D1 ('CREATE|' + $portD)
$null = RecvUntil $D1 'CREATED' 3000
$D2 = New-Client 'Mia'
SendLine $D2 ('JOIN|' + $portD)
$null = RecvUntil $D2 'JOINED' 3000
$D3 = New-Client 'Nora'
SendLine $D3 ('JOIN|' + $portD)
$null = RecvUntil $D3 'JOINED' 3000

SendLine $D1 'LG'
$lg1 = Recv-LG $D1
Check 'D1 LG 含 Log List 头与 3 名玩家 [in] 记录' (
    $lg1.Contains('Log List') -and $lg1.Contains('Liam') -and $lg1.Contains('Mia') -and $lg1.Contains('Nora') -and $lg1.Contains('[in]'))

SendLine $D2 'EXIT'
Start-Sleep -Milliseconds 600
SendLine $D1 'LG'
$lg2 = Recv-LG $D1
Check 'D2 离开后 LG 记录该玩家 [out]' ($lg2.Contains('Mia') -and $lg2.Contains('[out]'))

SendLine $D3 'LG'
$r = RecvUntil $D3 'ERROR' 3000
Check 'D3 非房主 LG 被拒（只有房主）' ($r -match '只有房主')

$rows = @($lg2 -split "`n" | Where-Object { $_ -match '\[(in|out)\]$' } | ForEach-Object { $_.TrimEnd("`r") })
$wBase = 0
$alignOk = $true
foreach ($row in $rows) {
    $core = $row -replace '\[(in|out)\]$', ''
    $w = Get-DispWidth $core
    if ($wBase -eq 0) { $wBase = $w }
    elseif ($w -ne $wBase) { $alignOk = $false }
}
Check 'D4 LG 行对齐（名字/IP 列等宽）' ($rows.Count -ge 2 -and $alignOk)

# 随房间销毁：全员离开 → 房间销毁 → 重建新房 LG 无旧记录
SendLine $D1 'EXIT'
SendLine $D3 'EXIT'
Start-Sleep -Seconds 2
$D4 = New-Client 'Liam'
SendLine $D4 ('CREATE|' + $portD)
$r = RecvUntil $D4 'CREATED' 3000
Start-Sleep -Milliseconds 400
SendLine $D4 'LG'
$lg3 = Recv-LG $D4
Check 'D5 房间销毁后新建房间 LG 无旧玩家记录（记录随房间销毁）' (
    (-not $lg3.Contains('Mia')) -and (-not $lg3.Contains('Nora')) -and (-not $lg3.Contains('[out]')))

foreach ($cl in @($D4)) { Close-Client $cl }
Stop-Process -Id $rmD.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

# ============ 验收 5：BAN 批量 ============
$rmE = Start-RM
$portE = Get-FreePort
$E1 = New-Client 'Owen'
SendLine $E1 ('CREATE|' + $portE)
$E2 = New-Client 'Pete'
SendLine $E2 ('JOIN|' + $portE)
$E3 = New-Client 'Quinn'
SendLine $E3 ('JOIN|' + $portE)
$null = RecvUntil $E3 'JOINED' 3000

# 管道格式批量（客户端实际发送格式）：Pete+Quinn 名字、127.0.0.1 IP，全部生效
SendLine $E1 'BAN|Pete Quinn 127.0.0.1'
$r = RecvUntil $E1 '批量拉黑完成' 3000
Check 'E1 批量拉黑名字+IP 全部生效（成功 3 项，首项不丢）' (
    $r -match '批量拉黑完成：成功 3 项（名字 2、IP 1）')

$kickedP = RecvAll $E2 1000
Check 'E2 房内成员被批量拉黑即时移出' ($kickedP.Contains('拉黑并移出'))

$p2 = New-Client 'Pete'
SendLine $p2 ('JOIN|' + $portE)
$r = RecvUntil $p2 'ERROR' 3000
Check 'E3 被拉黑名字重新 JOIN 被拒（你已被拉黑）' ($r -match '你已被拉黑')

$v1 = New-Client 'Vera'
SendLine $v1 ('JOIN|' + $portE)
$r = RecvUntil $v1 'ERROR' 3000
Check 'E4 批量拉黑 IP 后新连接 JOIN 被拒（IP 已被拉黑）' ($r -match 'IP 已被拉黑')

SendLine $E1 'UNBAN|Pete 127.0.0.1'
$r = RecvUntil $E1 '批量取消拉黑完成' 3000
Check 'E5 批量 UNBAN 生效（成功 2 项：名字 1、IP 1）' (
    $r -match '批量取消拉黑完成：成功 2 项（名字 1、IP 1）')

$v2 = New-Client 'Vera'
SendLine $v2 ('JOIN|' + $portE)
$r = RecvUntil $v2 'JOINED' 3000
Check 'E6 UNBAN 后重新 JOIN 成功' ($r -match 'JOINED')

foreach ($cl in @($E1, $E3, $p2, $v1, $v2)) { Close-Client $cl }
Stop-Process -Id $rmE.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

# ============ 验收 6：BAN 文件导入 ============
$rmF = Start-RM
$portF = Get-FreePort
# 临时 .ban 文件（Start.exe 工作目录 = $wolf，相对路径基于它解析）
[IO.File]::WriteAllLines("$wolf\r5_ban1.ban", @('Vera', '127.0.0.4', '', 'Vince'), (New-Object Text.UTF8Encoding($false)))
[IO.File]::WriteAllText("$wolf\r5_ban2.ban", "AbsA`n", (New-Object Text.UTF8Encoding($false)))
[IO.File]::WriteAllText("$wolf\r5_ban3.ban", "AbsB`n", (New-Object Text.UTF8Encoding($false)))

$F1 = New-Client 'Rex'
SendLine $F1 ('CREATE|' + $portF)
$null = RecvUntil $F1 'CREATED' 3000
$F2 = New-Client 'Sam'
SendLine $F2 ('JOIN|' + $portF)
$null = RecvUntil $F2 'JOINED' 3000

SendLine $F1 'BAN|r5_ban1.ban'
$r = RecvUntil $F1 '批量拉黑完成' 3000
# 实现语义：文件项只计入汇总总数，名字/IP 细分计数为 0（§14.5 展示层行为），
# 断言总数；逐项生效由 F2 的 JOIN 拒绝验证
Check 'F1 相对路径 .ban 文件导入生效（成功 3 项，空行跳过）' (
    $r -match '批量拉黑完成：成功 3 项')

$fv = New-Client 'Vera'
SendLine $fv ('JOIN|' + $portF)
$r1 = RecvUntil $fv 'ERROR' 3000
$fvc = New-Client 'Vince'
SendLine $fvc ('JOIN|' + $portF)
$r2 = RecvUntil $fvc 'ERROR' 3000
Check 'F2 文件拉黑的名字与 IP 均生效（Vera/Vince JOIN 被拒）' (
    ($r1 -match '你已被拉黑') -and ($r2 -match '你已被拉黑'))

SendLine $F1 ('BAN|' + $wolf + '\r5_ban2.ban')
$r = RecvUntil $F1 '批量拉黑完成' 3000
$fa = New-Client 'AbsA'
SendLine $fa ('JOIN|' + $portF)
$rA = RecvUntil $fa 'ERROR' 3000
Check 'F3 绝对路径 .ban 文件导入生效' (
    ($r -match '成功 1 项') -and ($rA -match '你已被拉黑'))

SendLine $F1 'BAN|r5_ban_missing.ban'
$r = RecvUntil $F1 '无法读取' 3000
Check 'F4 不存在 .ban 文件 clean 报错（无法读取黑名单文件）' (
    $r -and $r.Contains('无法读取黑名单文件 r5_ban_missing.ban'))

SendLine $F1 'BAN|..\..\x.ban'
$r = RecvUntil $F1 '无法读取' 3000
Check 'F5 路径穿越文件名 clean 报错不崩溃' (
    $r -and $r.Contains('无法读取黑名单文件 ..\..\x.ban'))

SendLine $F1 'BAN|plainname.txt'
$r = RecvUntil $F1 '已拉黑' 3000
# 实现语义：非 .ban 参数按名字处理；plainname.txt 13 字符被名字限长截成 10 字符 plainname.（2026-08-06 探针确认）
Check 'F6 非 .ban 结尾参数按名字处理（不读文件）' ($r -match '已拉黑 plainname\.')

SendLine $F1 'UNBAN|r5_ban1.ban'
$r = RecvUntil $F1 '批量取消拉黑完成' 3000
$fv2 = New-Client 'Vera'
SendLine $fv2 ('JOIN|' + $portF)
$rV = RecvUntil $fv2 'JOINED' 3000
Check 'F7 UNBAN 文件批量解除后重新可加入' (
    ($r -match '成功 3 项') -and ($rV -match 'JOINED'))

foreach ($cl in @($F1, $F2, $fv, $fvc, $fa, $fv2)) { Close-Client $cl }
Stop-Process -Id $rmF.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

# ============ 验收 7：NAME 白名单 ============
$rmG = Start-RM

$r = Try-Name 'a b'
Check 'G1 名字含空格被拒（白名单）' ($r -match '名字只能包含中英文、数字与下划线')
$r = Try-Name 'a-b'
Check 'G2 名字含连字符被拒（白名单）' ($r -match '名字只能包含中英文、数字与下划线')
$r = Try-Name 'a.b'
Check 'G3 名字含点号被拒（白名单）' ($r -match '名字只能包含中英文、数字与下划线')
$r = Try-Name 'a😀'
Check 'G4 名字含 emoji 被拒（白名单）' ($r -match '名字只能包含中英文、数字与下划线')
$r = Try-Name 'ＡＢ'
Check 'G5 全角字母名字被拒（白名单）' ($r -match '名字只能包含中英文、数字与下划线')

$r = Try-Name 'abc123'
Check 'G6 字母数字组合名字成功' ($r -match 'NAME_SET\|abc123')
$r = Try-Name '小明'
Check 'G7 汉字名字成功' ($r -match 'NAME_SET\|小明')
$r = Try-Name 'Mix1_汉'
Check 'G8 中英混排+下划线名字成功' ($r -match 'NAME_SET\|Mix1_汉')
$r = Try-Name 'abcdefghijk'
Check 'G9 11 字符名字截断 10 不回归' ($r -match 'NAME_SET\|abcdefghij')
$r = Try-Name '192.168.1.1'
Check 'G10 IP 形似名字拒绝不回归（白名单）' ($r -match '名字只能包含中英文、数字与下划线')

Stop-Process -Id $rmG.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

# ============ 验收 8：攻击用例 ============
$rmH = Start-RM
$portH = Get-FreePort
$H1 = New-Client 'Ana'
SendLine $H1 ('CREATE|' + $portH)
$null = RecvUntil $H1 'CREATED' 3000
$H2 = New-Client 'Brad'
SendLine $H2 ('JOIN|' + $portH)
$null = RecvUntil $H2 'JOINED' 3000
$H3 = New-Client 'Cid'
SendLine $H3 ('JOIN|' + $portH)
$null = RecvUntil $H3 'JOINED' 3000
$H4 = New-Client 'Dev'
SendLine $H4 ('JOIN|' + $portH)
$null = RecvUntil $H4 'JOINED' 3000

SendLine $H1 'VILLAGER|1'
$null = RecvUntil $H1 '村民职业已启用' 2000
SendLine $H1 'RATIO|1|0|2'
$null = RecvUntil $H1 '比例已设为' 2000
foreach ($cl in @($H1, $H2, $H3, $H4)) { SendLine $cl 'READY' }
Start-Sleep -Seconds 1
SendLine $H1 'START'
$gpH = RecvUntil $H1 'GAME_PREPARE|' 6000
$roomH = $gpH.Split('|')[2]
$gportH = $gpH.Split('|')[1]
foreach ($cl in @($H1, $H2, $H3, $H4)) { Close-Client $cl }
Start-Sleep -Milliseconds 500

$botH = @()
for ($i = 1; $i -le 4; $i++) { $botH += New-Bot $i $gportH }
foreach ($bt in $botH) {
    $r = RecvUntilStream $bt.s 'ROLE|' 10000
    if ($r) { $bt.role = $r.Substring(5) }
}
Check 'H1 攻击局游戏服就绪（4 机器人收到身份）' (@($botH | Where-Object { $_.role }).Count -eq 4)

# 非房间成员伪造 GAME_ENDED：房间标记结束但房间不销毁、外人仍不能 JOIN
$hacker = New-Client 'Hacker'
SendLine $hacker ('GAME_ENDED|' + $roomH)
Start-Sleep -Milliseconds 400
SendLine $hacker ('JOIN|' + $gportH)
$r = RecvUntil $hacker 'ERROR' 3000
Check 'H2 伪造 GAME_ENDED 后外人 JOIN 仍被拒（房间未破坏）' ($r -match '该房间正在游戏中')

SendLine $hacker 'LIST'
$r = RecvUntil $hacker 'ROOMS_LIST' 3000
Check 'H3 伪造 GAME_ENDED 后房间仍在列表（4/12）' (
    $r -and ($r -match ([regex]::Escape($gportH) + '\s+4/12')))

$a2 = New-Client 'Ana'
SendLine $a2 ('GAME_ENDED|' + $roomH)
SendLine $a2 ('REJOIN|' + $roomH + '|1')
$r = RecvUntil $a2 'JOINED' 3000
Check 'H4 伪造 GAME_ENDED 后原玩家仍可正常回房（JOINED）' ($r -match 'JOINED')

# 垃圾 __GAME_OVER__ 行灌入游戏端口：不崩溃、不影响房间
$g1 = Probe-RawLines $gportH @('__GAME_OVER__', '%%%%garbage%%%')
Check 'H5 垃圾 __GAME_OVER__ 行灌入游戏端口不崩溃' $g1

SendLine $hacker 'LIST'
$r = RecvUntil $hacker 'ROOMS_LIST' 3000
Check 'H6 攻击后 LIST 仍正常（房间在列、不崩）' (
    $r -and ($r -match ([regex]::Escape($gportH) + '\s+4/12')))

# 批量 BAN 混入自己名字：该项拒绝、其余生效
$portH2 = Get-FreePort
$e1 = New-Client 'Eve'
SendLine $e1 ('CREATE|' + $portH2)
$e2 = New-Client 'Zoe'
SendLine $e2 ('JOIN|' + $portH2)

SendLine $e1 'BAN|Eve Zoe'
$r = RecvUntil $e1 '批量拉黑完成' 10000
Check 'H7 批量 BAN 混自己名字 → 拒绝 1 项其余生效' (
    $r -match '批量拉黑完成：成功 1 项（名字 1、IP 0），拒绝 1 项')

$z2 = New-Client 'Zoe'
SendLine $z2 ('JOIN|' + $portH2)
$r = RecvUntil $z2 'ERROR' 3000
Check 'H8 批量拉黑中其余名字生效（Zoe 被拒）' ($r -match '你已被拉黑')

SendLine $e1 'STATUS'
$r = Recv-Status $e1 3000
Check 'H9 房主未被自己名字项误踢（仍在房内）' ($r -and $r.Contains('Eve'))

foreach ($cl in @($hacker, $a2, $e1, $e2, $z2)) { Close-Client $cl }
foreach ($bt in $botH) { Close-Bot $bt }
Stop-Process -Id $rmH.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

# ============ 收尾 ============
Stop-Keepalive
foreach ($cl in $script:liveClients.ToArray()) { Close-Client $cl }
Remove-Item -LiteralPath "$wolf\r5_ban1.ban", "$wolf\r5_ban2.ban", "$wolf\r5_ban3.ban" -ErrorAction SilentlyContinue
Remove-Item Env:\WOLF_VOTE_TIMEOUT_SECONDS -ErrorAction SilentlyContinue
Kill-All
Start-Sleep -Seconds 1

Write-Output ('ROUND5  PASS=' + $script:pass + '  FAIL=' + $script:fail)
if ($script:fail -gt 0) {
    Write-Output 'ROUND5  RESULT: FAIL'
    exit 1
}
Write-Output 'ROUND5  RESULT: ALL PASS'
exit 0
