# 狼人杀第三轮验收脚本（测试工具，非游戏实现）
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File tests\round3_test.ps1 *> tests\round3_out.txt
# 覆盖 REQUIREMENTS.md §11.6 / §12.8 的六项新增验收：
#   1. 房间内 LIST 可用；房间内 CREATE/JOIN 仍拒（服务端"你已在房间中"双保险）
#   2. 短别名 CR/VG/ST/TF/CF 生效（Start 端）；V/B 裸 socket 直连生效（Server 端，白狼王局）
#   3. 心跳失联判定：不发 PING 的静默连接超 3 秒被清（房间销毁）；持续 PING 不误判
#   4. 白天投票超时自动弃权、不卡死（WOLF_VOTE_TIMEOUT_SECONDS=6 注入短窗口）
#   5. 白天断线重连后补发白天提示 + __DAY_OPEN__，重连者可继续投票
#   6. EN 语言：Start 按 LANG|en 输出英文；Server 按 argv 语言码输出英文广播；
#      中文命令在服务端仍被接受（服务端不管客户端语言，属设计）
# 游戏局通过裸 socket 直连 Server.exe（不经 Start），模拟客户端每 1 秒发 PING 保活，
# 与 speech_test.ps1 同一模式；大厅流程通过裸 socket 直连 Start.exe。

$ErrorActionPreference = 'Stop'
$wolf = Split-Path $PSScriptRoot -Parent
$script:pass = 0
$script:fail = 0
$script:conns = @()

function Check($desc, $cond) {
    if ($cond) { $script:pass++; Write-Output ("PASS  " + $desc) }
    else       { $script:fail++; Write-Output ("FAIL  " + $desc) }
}

# ============ Start.exe（大厅/房间）裸 socket 客户端 ============

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
    return @{ c = $c; s = $s; w = $w }
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

function SendLine($cl, $cmd) {
    $cl.w.WriteLine($cmd)
}

function RecvUntil($cl, $match, $timeoutMs = 5000) {
    return RecvUntilStream $cl.s $match $timeoutMs
}

# STATUS 多行回复：头行（ROOM_STATUS|ID | NAME | ST）的换行已被 RecvUntilStream
# 消费，补读的剩余行需补回换行再拼接（同 round5 Recv-LG 模式，§16.5 竖排表）
function Recv-Status($cl, $timeoutMs = 3000) {
    $hdr = RecvUntilStream $cl.s 'ROOM_STATUS' $timeoutMs
    if (-not $hdr) { return '' }
    $chunk = ReadChunk $cl.s 800
    $rows = [System.Text.Encoding]::UTF8.GetString($chunk)
    return ($hdr + "`n" + $rows)
}

function Close-Client($cl) {
    try { $cl.c.Close() } catch {}
}

# 心跳保活睡眠：睡眠期间持续发 PING（每 1 秒），避免 Start 的 3 秒失联判定
# 误杀测试连接（真实 Client.exe 每 1 秒发一次）
function Sleep-Ping($clients, $secs) {
    $end = [DateTime]::Now.AddSeconds($secs)
    while ([DateTime]::Now -lt $end) {
        foreach ($cl in $clients) { try { $cl.w.WriteLine('PING') } catch {} }
        Start-Sleep -Milliseconds 1000
    }
}

# ============ Server.exe（游戏）裸 socket 机器人 ============

function New-Bot($k, $port) {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', $port)
    $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s)
    $w.NewLine = "`n"
    $w.AutoFlush = $true
    $w.WriteLine('PLAYER_ID|' + $k)
    $entry = @{
        k = $k; c = $c; s = $s; w = $w; closed = $false
        bytes = [System.Collections.Generic.List[byte]]::new()
        queue = [System.Collections.Queue]::new()
        role = ''; alive = $true; dead = $false; witchInputs = 0
    }
    $script:conns += $entry
}

function PumpAll {
    foreach ($cl in $script:conns) {
        if ($cl.closed) { continue }
        try {
            while ($cl.s.DataAvailable) {
                $b = $cl.s.ReadByte()
                if ($b -lt 0) { $cl.alive = $false; $cl.closed = $true; break }
                if ($b -eq 10) {
                    $raw = $cl.bytes.ToArray()
                    $cl.bytes.Clear()
                    $line = [System.Text.Encoding]::UTF8.GetString($raw)
                    $line = $line.TrimEnd("`r")
                    if ($line.Length -gt 0) { $cl.queue.Enqueue($line) }
                } else {
                    $cl.bytes.Add([byte]$b)
                }
            }
        } catch { $cl.closed = $true }
    }
}

function Is-WolfRole($role) {
    # 狼人阵营两个职业都算狼：夜刀归票与刀人目标都须排除（白狼王白天自爆、夜晚参与行刀）
    return ($role -eq 'werewolf') -or ($role -eq 'whitewolf')
}

function All-Roles-Known {
    foreach ($cl in $script:conns) { if ($cl.role -eq '') { return $false } }
    return $true
}

function Get-WolfTarget {
    foreach ($cl in $script:conns) {
        if ($cl.alive -and -not (Is-WolfRole $cl.role)) { return $cl.k }
    }
    return 0
}

function Get-SeerTarget($selfK) {
    foreach ($cl in $script:conns) {
        if ($cl.alive -and $cl.k -ne $selfK) { return $cl.k }
    }
    return 0
}

# 跑一局 4 人直连对局（1 狼 / 0 中立 / 2 神 / 村民开）。
# scenario 决定白天 1 的策略：alias=白狼王 B 0 + V 3；timeout=只投 1 票其余静默；
# reconnect=玩家 1 断线重连续投；en=中文投票命令 + 全英文断言。
# level 2 时唯一狼为白狼王（BuildJobPool：level>=2 且 W=1 直接放白狼王）。
function Run-Game($port, $rmName, $scenario, $level, $langCode) {
    $script:conns = @()

    $flags = @{ cls = 0; dayClose = 0; timeoutBc = 0
        exileBc = $false; voteBc = $false; bombErr = $false
        rePrompt = $false; reVoteBc = $false
        welcomeEn = $false; dayPhaseEn = $false; voteEn = $false; exileEn = $false }
    $st = @{ done = $false; wolfSlot = 0; day1Started = $false; dayCount = 0
        nightAfterDay1Start = $false
        bombSent = $false; voterSent = $false; vote1Sent = $false
        reconnectSent = $false; disconnectedAt = $null; reconnected = $false
        reVoteSent = $false; restVoted = $false
        lastPing = [DateTime]::Now }

    $srvArgs = @($port, 'Alice', 'Bob', 'Cathy', 'Dave',
        '127.0.0.1', '8888', $rmName, '1', '0', '2', $level, '1')
    $srvArgs += @($langCode, $langCode, $langCode, $langCode)
    $srvProc = Start-Process -FilePath "$wolf\Server.exe" -ArgumentList $srvArgs -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 2

    for ($i = 1; $i -le 4; $i++) { New-Bot $i $port }

    $deadline = [DateTime]::Now.AddSeconds(90)
    $timedOut = $false
    $crashed = $false
    $trace = (Join-Path $PSScriptRoot 'round3_trace_') + $rmName + '.txt'
    Remove-Item -LiteralPath $trace -ErrorAction SilentlyContinue

    while (-not $st.done -and [DateTime]::Now -lt $deadline) {
        if ($srvProc.HasExited) { $crashed = $true; break }

        PumpAll

        # 心跳保活：模拟真实客户端每 1 秒发 PING（服务端 3 秒无字节判定失联）。
        # 投票超时/重连等待等刻意静默窗口期间连接必须维持，否则会先被按失联清掉
        if ([DateTime]::Now -ge $st.lastPing.AddSeconds(1)) {
            foreach ($cl in $script:conns) {
                if ($cl.closed) { continue }
                try { $cl.w.WriteLine('PING') } catch {}
            }
            $st.lastPing = [DateTime]::Now
        }

        foreach ($cl in $script:conns) {
            if ($cl.closed) { continue }
            while ($cl.queue.Count -gt 0) {
                $line = $cl.queue.Dequeue()
                Add-Content -LiteralPath $trace -Encoding UTF8 -Value ('[' + $cl.k + '] ' + $line)

                if ($line -eq '__CLS__') { $flags.cls++ }
                if ($line -eq '__DAY_CLOSE__') { $flags.dayClose++ }
                if ($line.Contains('ROLE|')) { $cl.role = $line.Substring(5) }

                # 夜晚行动 / 输入门（狼人（含白狼王）/预言家/女巫按角色作答）
                if ($line.Trim() -eq '__INPUT__') {
                    if (-not $cl.dead) {
                        if (Is-WolfRole $cl.role) {
                            $t = Get-WolfTarget
                            if ($t -gt 0) { $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $t) }
                        } elseif ($cl.role -eq 'seer') {
                            $t = Get-SeerTarget ($cl.k)
                            if ($t -gt 0) { $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $t) }
                        } elseif ($cl.role -eq 'witch') {
                            # 第一次输入是解药（救狼刀目标保平安夜），第二次是毒药（不用）
                            if ($cl.witchInputs -eq 0) {
                                $t = Get-WolfTarget
                                if ($t -gt 0) { $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $t) }
                            } else {
                                $cl.w.WriteLine('PLAYER_' + $cl.k + '|0')
                            }
                            $cl.witchInputs++
                        } else {
                            $cl.w.WriteLine('PLAYER_' + $cl.k + '|0')
                        }
                    }
                }

                # 死亡广播标记脚本侧状态（放逐/被狼刀/被毒杀）
                if ($line -match '槽(\d+)') {
                    if ($line.Contains('被狼人击杀') -or $line.Contains('被放逐') -or $line.Contains('被女巫毒杀')) {
                        $deadSlot = [int]$Matches[1]
                        foreach ($c2 in $script:conns) {
                            if ($c2.k -eq $deadSlot) { $c2.alive = $false; $c2.dead = $true }
                        }
                    }
                }

                # 白天开始广播每 socket 一份：靠"夜晚推进过一次"区分第几天，不重复累加。
                # EN 局白天广播为英文（Day phase.），两种文案都要能识别（2026-08-05 实测 EN 局漏判）
                if ($line.Contains('白天发言阶段') -or $line.Contains('Day phase.')) {
                    if (-not $st.day1Started) {
                        $st.day1Started = $true
                        $st.dayCount = 1
                    } elseif ($st.nightAfterDay1Start -and $st.dayCount -lt 2) {
                        $st.dayCount = 2
                    }
                }

                if ($line -eq '天黑请闭眼，开始今夜行动。') {
                    if ($st.day1Started) { $st.nightAfterDay1Start = $true }
                }

                # ---------- 断言采集 ----------
                if ($line.Contains('超时未投票')) { $flags.timeoutBc++ }
                if ($line.Contains('被放逐')) { $flags.exileBc = $true }
                if ($line.Contains('投票给了玩家')) { $flags.voteBc = $true }
                if ($line.Contains('自爆目标不合法')) { $flags.bombErr = $true }
                if ($line.Contains('白天仍在进行')) { $flags.rePrompt = $true }
                if ($line.Contains('投票给了玩家Dave（槽4）')) { $flags.reVoteBc = $true }
                if ($line.Contains('You are assigned to slot')) { $flags.welcomeEn = $true }
                if ($line.Contains('Day phase.')) { $flags.dayPhaseEn = $true }
                if ($line.Contains('voted for player')) { $flags.voteEn = $true }
                if ($line.Contains('was exiled.')) { $flags.exileEn = $true }
            }
        }

        # ---------- 白天/行动策略 ----------
        $rolesKnown = All-Roles-Known

        if ($rolesKnown -and -not $st.wolfSlot) {
            foreach ($cl in $script:conns) { if (Is-WolfRole $cl.role) { $st.wolfSlot = $cl.k; break } }
        }

        if ($rolesKnown -and $st.day1Started -and $st.dayCount -lt 2) {
            switch ($scenario) {
                'alias' {
                    # 白狼王发 B 0（非法目标）：收到"自爆目标不合法"即证明 B 被识别为自爆命令
                    if (-not $st.bombSent -and $st.wolfSlot -gt 0) {
                        $script:conns[$st.wolfSlot - 1].w.WriteLine('PLAYER_' + $st.wolfSlot + '|B 0')
                        $st.bombSent = $true
                    }
                    # 任一生存非狼玩家发 V 3：计票广播即证明 V 被识别为投票命令
                    if (-not $st.voterSent) {
                        foreach ($cl in $script:conns) {
                            if ($cl.alive -and -not (Is-WolfRole $cl.role)) {
                                $cl.w.WriteLine('PLAYER_' + $cl.k + '|V 3')
                                break
                            }
                        }
                        $st.voterSent = $true
                    }
                    if ($flags.bombErr -and $flags.voteBc) { $st.done = $true }
                }
                'timeout' {
                    # 只有玩家 1 投票，其余静默（仍发 PING 保活）；注入的 6 秒窗口到期后自动弃权
                    if (-not $st.vote1Sent) {
                        $script:conns[0].w.WriteLine('PLAYER_1|VOTE 2')
                        $st.vote1Sent = $true
                    }
                    # 三条超时弃权广播 + 计票放逐广播齐了才算"超时后不卡死"
                    if ($flags.timeoutBc -ge 3 -and $flags.exileBc) { $st.done = $true }
                }
                'reconnect' {
                    # 玩家 1 未投票即断线 → 服务端进入重连等待 → 2.5 秒后重连
                    if (-not $st.reconnectSent) {
                        $script:conns[0].c.Close()
                        $script:conns[0].closed = $true
                        $st.reconnectSent = $true
                        $st.disconnectedAt = [DateTime]::Now
                    }

                    if ($st.reconnectSent -and -not $st.reconnected -and
                        ([DateTime]::Now - $st.disconnectedAt).TotalSeconds -ge 2.5) {
                        $c = New-Object Net.Sockets.TcpClient
                        $c.Connect('127.0.0.1', $port)
                        $s = $c.GetStream()
                        $w = New-Object IO.StreamWriter($s)
                        $w.NewLine = "`n"
                        $w.AutoFlush = $true
                        $w.WriteLine('PLAYER_ID|1')
                        $old = $script:conns[0]
                        $script:conns[0] = @{ k = 1; c = $c; s = $s; w = $w; closed = $false
                            bytes = [System.Collections.Generic.List[byte]]::new()
                            queue = [System.Collections.Queue]::new()
                            role = $old.role; alive = $true; dead = $false; witchInputs = $old.witchInputs }
                        $st.reconnected = $true
                    }

                    # 补发"白天仍在进行"提示后，重连者用短别名续投
                    if ($st.reconnected -and $flags.rePrompt -and -not $st.reVoteSent) {
                        $script:conns[0].w.WriteLine('PLAYER_1|V 4')
                        $st.reVoteSent = $true
                    }

                    # 其余玩家投票形成 2:2 平票，白天正常收束（不触发遗言等待）
                    if ($st.reVoteSent -and -not $st.restVoted) {
                        $script:conns[1].w.WriteLine('PLAYER_2|VOTE 3')
                        $script:conns[2].w.WriteLine('PLAYER_3|VOTE 4')
                        $script:conns[3].w.WriteLine('PLAYER_4|VOTE 3')
                        $st.restVoted = $true
                    }

                    if ($flags.reVoteBc -and $flags.dayClose -ge 1) { $st.done = $true }
                }
                'en' {
                    # 中文投票别名在服务端仍被接受（服务端不管客户端语言，属设计），
                    # 但广播按玩家语言渲染为英文
                    if (-not $st.vote1Sent) {
                        $script:conns[0].w.WriteLine('PLAYER_1|投票 2')
                        $st.vote1Sent = $true
                    }

                    if ($st.vote1Sent -and -not $st.restVoted) {
                        $script:conns[1].w.WriteLine('PLAYER_2|V 3')
                        $script:conns[2].w.WriteLine('PLAYER_3|V 4')
                        $script:conns[3].w.WriteLine('PLAYER_4|V 3')
                        $st.restVoted = $true
                    }

                    # 英文投票广播 + 英文放逐公告齐了才算整条 EN 链路通
                    if ($flags.voteEn -and $flags.exileEn) { $st.done = $true }
                }
            }
        }

        Start-Sleep -Milliseconds 30
    }

    if (-not $st.done -and [DateTime]::Now -ge $deadline) {
        $timedOut = $true
        Add-Content -LiteralPath $trace -Encoding UTF8 -Value ('!! ' + $rmName + ' 超时未完成')
    }

    Add-Content -LiteralPath $trace -Encoding UTF8 -Value ('!!! ' + $rmName + ' LOOPEND done=' + $st.done + ' timedOut=' + $timedOut +
        ' crash=' + $srvProc.HasExited + ' dayCount=' + $st.dayCount +
        ' wolfSlot=' + $st.wolfSlot + ' bombErr=' + $flags.bombErr +
        ' voteBc=' + $flags.voteBc + ' timeoutBc=' + $flags.timeoutBc +
        ' exileBc=' + $flags.exileBc + ' rePrompt=' + $flags.rePrompt +
        ' reVoteBc=' + $flags.reVoteBc + ' dayClose=' + $flags.dayClose +
        ' voteEn=' + $flags.voteEn + ' exileEn=' + $flags.exileEn)

    foreach ($cl in $script:conns) { try { $cl.c.Close() } catch {} }
    Stop-Process -Id $srvProc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300

    return @{ flags = $flags; st = $st; timedOut = $timedOut; crashed = $crashed }
}

# ============ 清理与启动 ============
Get-Process -Name Start,Server,Client,Client_en -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Remove-Item -LiteralPath "$wolf\start.log" -ErrorAction SilentlyContinue
Remove-Item -LiteralPath "$wolf\server.log" -ErrorAction SilentlyContinue

$startProc = Start-Process -FilePath "$wolf\Start.exe" -ArgumentList @('8888') -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 2

try {
    # ---------- 验收 1：房间内 LIST；房间内 CREATE/JOIN 仍拒 ----------
    $A = New-Client 'Alice'
    SendLine $A 'CREATE|7301'
    $r = RecvUntil $A 'CREATED' 3000
    Check '1.1 A 建房成功（7301）' ($r -ne $null)

    $B = New-Client 'Bob'
    SendLine $B 'JOIN|7301'
    $r = RecvUntil $B 'JOINED' 3000
    Check '1.2 B 入房成功' ($r -ne $null)

    SendLine $B 'LIST'
    $r = RecvUntil $B 'ROOMS_LIST' 3000
    Check '1.3 房间内 LIST 可用（ROOMS_LIST 含 7301）' ($r -match '7301')

    SendLine $B 'CREATE|7302'
    $r = RecvUntil $B 'ERROR' 3000
    Check '1.4 房间内 CREATE 被拒（服务端"你已在房间中"）' ($r -match '你已在房间中')

    SendLine $B 'JOIN|7302'
    $r = RecvUntil $B 'ERROR' 3000
    Check '1.5 房间内 JOIN 被拒（服务端"你已在房间中"）' ($r -match '你已在房间中')

    Close-Client $A
    Close-Client $B
    Start-Sleep -Milliseconds 600

    # ---------- 验收 2：短别名 CR/VG/ST/TF/CF（Start 端）+ V/B（Server 端） ----------
    $A = New-Client 'Alice'
    SendLine $A 'CR 7302'
    $r = RecvUntil $A 'CREATED' 3000
    Check '2.1 CR 短别名建房成功（等效 CREATE）' ($r -ne $null)

    $B = New-Client 'Bob'
    SendLine $B 'JOIN|7302'
    $null = RecvUntil $B 'JOINED' 3000
    $C = New-Client 'Cathy'
    SendLine $C 'JOIN|7302'
    $null = RecvUntil $C 'JOINED' 3000
    $D = New-Client 'Dave'
    SendLine $D 'JOIN|7302'
    $null = RecvUntil $D 'JOINED' 3000

    SendLine $A 'VG 1'
    $r = RecvUntil $A '村民职业已启用' 2000
    Check '2.2 VG 短别名开启村民' ($r -match '村民职业已启用')

    # 关回村民：默认比例 0/0/0 与 4 人不符，START 才会走自动配置确认（CF 测试的前置状态）
    SendLine $A 'VG 0'
    $r = RecvUntil $A '村民职业已禁用' 2000
    Check '2.3 VG 0 关闭村民' ($r -match '村民职业已禁用')

    SendLine $A 'ST'
    $r = Recv-Status $A 2000
    Check '2.4 ST 短别名查看本房状态' (($r -match 'ID\s*\|\s*NAME') -and $r.Contains('Alice'))

    SendLine $A 'TF Bob'
    $r = RecvUntil $A '已转交房主给' 2000
    Check '2.5 TF 短别名转移房主' ($r -match '已转交房主给')
    $r = RecvUntil $B 'ADMIN' 2000
    Check '2.6 Bob 收到 ADMIN 成为房主' ($r -match 'ADMIN')

    SendLine $B 'TF Alice'
    $null = RecvUntil $B '已转交房主给' 2000
    $r = RecvUntil $A 'ADMIN' 2000
    Check '2.7 转回后 Alice 恢复房主' ($r -match 'ADMIN')

    foreach ($cl in @($A, $B, $C, $D)) { SendLine $cl 'READY' }
    Start-Sleep -Seconds 1
    SendLine $A 'START'
    $r = RecvUntil $A 'CONFIG_NEED_CONFIRM' 4000
    Check '2.8 比例不符进入自动配置确认（CONFIG_NEED_CONFIRM）' ($r -match 'CONFIG_NEED_CONFIRM')
    SendLine $A 'CF 1'
    Start-Sleep -Seconds 2
    $gameProc = Get-Process -Name Server -ErrorAction SilentlyContinue
    Check '2.9 CF 1 同意自动配置并开局（Server.exe 出现）' ($gameProc -ne $null)

    # 游戏内 V/B 直连验证：level 2 时唯一狼即白狼王（BuildJobPool），B 命令只有白狼王会解析
    $gAlias = Run-Game 7351 'R3ALIAS' 'alias' 2 'zh'
    Check '2.10 游戏内 V 3 正常计票（投票给了玩家3 广播）' ($gAlias.flags.voteBc)
    Check '2.11 游戏内 B 0 被识别为自爆命令（白狼王收"自爆目标不合法"）' ($gAlias.flags.bombErr)
    Check '2.12 别名局进程未崩溃' (-not $gAlias.crashed)

    foreach ($cl in @($A, $B, $C, $D)) { Close-Client $cl }
    Get-Process -Name Server -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 600

    # ---------- 验收 3：心跳失联判定（不发 PING 被清；发 PING 不误判） ----------
    $S1 = New-Client 'Silent'
    SendLine $S1 'CREATE|7303'
    $r = RecvUntil $S1 'CREATED' 3000
    Check '3.1 静默客户端建房成功（7303）' ($r -ne $null)

    # 静默 12 秒（不发任何字节）：Start 应在 3 秒后判定失联并销毁空房、关闭 socket
    Start-Sleep -Seconds 12

    # EOF 探测：对端 FIN 到达时 Poll 报"可读"但 Available=0（DataAvailable 恒 false，
    # 只靠它永远探不到 EOF，2026-08-05 实测 3.2 误判失败）
    $deadlineE = [DateTime]::Now.AddSeconds(4)
    $eof = $false
    while ([DateTime]::Now -lt $deadlineE -and -not $eof) {
        try {
            if ($S1.c.Client.Poll(0, [Net.Sockets.SelectMode]::SelectRead)) {
                if ($S1.c.Client.Available -eq 0) { $eof = $true }
                else { $null = $S1.s.ReadByte() }
            } else {
                Start-Sleep -Milliseconds 200
            }
        } catch { $eof = $true }
    }
    Check '3.2 静默 12 秒连接被服务端关闭（EOF）' $eof

    $Y = New-Client 'Yuri'
    SendLine $Y 'LIST'
    $r = RecvUntil $Y 'ROOMS_LIST' 3000
    Check '3.3 静默连接失联后房间被销毁（LIST 无 7303）' ($r -ne $null -and -not ($r -match '7303'))
    Close-Client $Y
    Close-Client $S1

    # 对照：持续 PING 15 秒的连接不被误判为失联
    $P = New-Client 'Pinger'
    SendLine $P 'CREATE|7304'
    $null = RecvUntil $P 'CREATED' 3000
    Sleep-Ping @($P) 15
    SendLine $P 'LIST'
    $r = RecvUntil $P 'ROOMS_LIST' 3000
    Check '3.4 持续 PING 15 秒仍在线（LIST 含 7304）' ($r -match '7304')
    Close-Client $P
    Start-Sleep -Milliseconds 600

    # ---------- 验收 4：白天投票超时自动弃权、不卡死（注入 6 秒窗口） ----------
    $env:WOLF_VOTE_TIMEOUT_SECONDS = '6'
    $g4 = Run-Game 7352 'R3TIMEOUT' 'timeout' 0 'zh'
    Remove-Item Env:\WOLF_VOTE_TIMEOUT_SECONDS -ErrorAction SilentlyContinue
    Check '4.1 6 秒后未投票玩家自动弃权广播（≥3 条"超时未投票"）' ($g4.flags.timeoutBc -ge 3)
    Check '4.2 超时后正常计票推进（玩家被放逐广播）' ($g4.flags.exileBc)
    Check '4.3 白天未卡死（__DAY_CLOSE__ 已发出）' ($g4.flags.dayClose -ge 1)
    Check '4.4 超时局进程未崩溃' (-not $g4.crashed)

    # ---------- 验收 5：白天断线重连后补发提示并继续投票 ----------
    $g5 = Run-Game 7353 'R3RECON' 'reconnect' 0 'zh'
    Check '5.1 重连后收到"白天仍在进行"补发提示' ($g5.flags.rePrompt)
    Check '5.2 重连后投票被接受（玩家Alice 投票给了玩家4 广播）' ($g5.flags.reVoteBc)
    Check '5.3 白天投票继续推进（__DAY_CLOSE__ 发出）' ($g5.flags.dayClose -ge 1)
    Check '5.4 重连局进程未崩溃' (-not $g5.crashed)

    # ---------- 验收 6：EN 语言输出（Start 按 LANG|en；Server 按语言码广播英文） ----------
    $E = New-Client 'Eva'
    SendLine $E 'LANG|en'
    SendLine $E 'BOGUS'
    $r = RecvUntil $E 'ERROR' 3000
    Check '6.1 EN 客户端未知命令提示为英文（Unknown command）' ($r -match 'Unknown command')
    SendLine $E 'CREATE|7306'
    $r = RecvUntil $E 'CREATED' 3000
    Check '6.2 EN 客户端建房成功' ($r -ne $null)
    $r = RecvUntil $E 'created the room' 3000
    Check '6.3 EN 客户端建房公告为英文（created the room）' ($r -match 'created the room')
    Close-Client $E
    Start-Sleep -Milliseconds 400

    $g6 = Run-Game 7354 'R3EN' 'en' 0 'en'
    Check '6.4 EN 局欢迎语为英文（You are assigned to slot）' ($g6.flags.welcomeEn)
    Check '6.5 EN 局白天广播为英文（Day phase.）' ($g6.flags.dayPhaseEn)
    Check '6.6 EN 局中文投票命令被接受且广播为英文（voted for player）' ($g6.flags.voteEn)
    Check '6.7 EN 局放逐公告为英文（was exiled）' ($g6.flags.exileEn)
    Check '6.8 EN 局进程未崩溃' (-not $g6.crashed)
}
catch {
    Write-Output ("脚本异常: " + $_)
    $script:fail++
}
finally {
    Remove-Item Env:\WOLF_VOTE_TIMEOUT_SECONDS -ErrorAction SilentlyContinue
    Get-Process -Name Start,Server,Client,Client_en -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

Write-Output ""
Write-Output ("===== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " =====")
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
