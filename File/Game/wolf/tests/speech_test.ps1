# 狼人杀白天对话/极端输入/遗言/断线重连验收脚本（测试工具，非游戏实现）
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File tests\speech_test.ps1 *> tests\speech_out.txt
# 游戏1（5201）：进游戏清屏 __CLS__ / 白天聊天全角冒号展示 / 注入聊天 / 超长聊天 /
#                 非法投票拒绝（VOTE|999、VOTE|-1）/ VOTE|abc 按弃权 / 平票无人放逐 /
#                 白天断线重连 / 被放逐者遗言广播 / 一局完整结束（好人胜）。
# 游戏2（5202）：被放逐者沉默 → 遗言 10 秒超时后游戏继续（不卡死）。

$ErrorActionPreference = 'Stop'
$wolf = Split-Path $PSScriptRoot -Parent
$script:pass = 0
$script:fail = 0
$script:conns = @()

function Check($desc, $cond) {
    if ($cond) { $script:pass++; Write-Output ("PASS  " + $desc) }
    else       { $script:fail++; Write-Output ("FAIL  " + $desc) }
}

# 每连接：socket + 接收缓冲 + 行队列 + 身份/状态（closed 表示脚本侧已断开或流异常）
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
        role = ''; assigned = $false; alive = $true; dead = $false
        witchInputs = 0; dayVoted = $false
    }
    $script:conns += $entry
}

# 泵取所有连接的可读字节，按行入队（整行按 UTF-8 解码）；已关连接跳过
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

function All-Roles-Known {
    foreach ($cl in $script:conns) { if ($cl.role -eq '') { return $false } }
    return $true
}

# 第一个存活非狼槽（脚本全知身份，模拟狼人刀向与女巫解药对象）
function Get-WolfTarget {
    foreach ($cl in $script:conns) {
        if ($cl.alive -and $cl.role -ne 'werewolf') { return $cl.k }
    }
    return 0
}

# 预言家查验对象：第一个存活非预言家槽
function Get-SeerTarget($selfK) {
    foreach ($cl in $script:conns) {
        if ($cl.alive -and $cl.k -ne $selfK) { return $cl.k }
    }
    return 0
}

# 运行一局直连对局；返回 flags/st/roles/timedOut/crashed 结果集
function Run-Game($port, $rmName, $gameTag) {
    $script:conns = @()

    $flags = @{ cls = 0; aChat = $false; aChatLine = ''; bChat = $false; bChatLine = ''
        cChat = $false; cChatLine = ''; dRej = 0; tie = $false; abstain = $false
        lastWord = ''; rePrompt = $false; bobVote = $false; peaceful = $false
        wolfKill = $false; gameOver = $false; winner = ''
        nightAfterExile = $false; night2Time = $null }
    $st = @{ dayCount = 0; nightCount = 0; day1Started = $false; day2Started = $false
        nightAfterDay1Start = $false
        speechSent = $false
        bobVoted = $false; bobClosed = $false; bobReconnected = $false; restVoted = $false
        disconnectAt = $null; exileAt = $null
        day1Voted = $false; day2Voted = $false
        wolfSlot = 0; X = 0; g1 = 0; g2 = 0
        lastPing = [DateTime]::Now }

    $srvProc = Start-Process -FilePath "$wolf\Server.exe" -ArgumentList @(
        $port, 'Alice', 'Bob', 'Cathy', 'Dave',
        '127.0.0.1', '8888', $rmName, '1', '0', '2', '0', '1',
        'zh', 'zh', 'zh', 'zh'
    ) -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 2

    for ($i = 1; $i -le 4; $i++) { New-Bot $i $port }

    $deadline = [DateTime]::Now.AddSeconds(150)
    $timedOut = $false
    $crashed = $false
    $trace = (Join-Path $PSScriptRoot 'trace_') + $gameTag + '.txt'
    Remove-Item -LiteralPath $trace -ErrorAction SilentlyContinue
    $tick = 0

    while (-not $flags.gameOver -and [DateTime]::Now -lt $deadline) {
        if ($srvProc.HasExited) { $crashed = $true; break }

        $tick++
        PumpAll

        # 心跳保活：模拟真实客户端每秒发 PING（服务端 3 秒无字节判定失联）。
        # 遗言超时等刻意静默窗口期间连接必须维持，否则服务端会先按失联清掉所有玩家
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
                Add-Content -LiteralPath $trace -Value ('[' + $cl.k + '] ' + $line)

                if ($line -eq '__CLS__') { $flags.cls++ }

                if ($line.Contains('你被分配到')) { $cl.assigned = $true }

                if ($line.Contains('ROLE|')) { $cl.role = $line.Substring(5) }

                # 夜晚行动 / 遗言输入门
                if ($line.Trim() -eq '__INPUT__') {
                    if ($cl.dead) {
                        # 遗言窗口：游戏1被放逐的狼发正向遗言；游戏2被放逐者保持沉默（超时测试）
                        if ($gameTag -eq 'g1' -and $cl.k -eq $st.wolfSlot) {
                            $cl.w.WriteLine('PLAYER_' + $cl.k + '|这是我的遗言')
                        } elseif ($gameTag -eq 'g2' -and $cl.k -eq $st.wolfSlot) {
                            $cl.w.WriteLine('PLAYER_' + $cl.k + '|再见')
                        }
                    } elseif ($cl.role -eq 'werewolf') {
                        $t = Get-WolfTarget
                        if ($t -gt 0) { $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $t) }
                    } elseif ($cl.role -eq 'seer') {
                        $t = Get-SeerTarget ($cl.k)
                        if ($t -gt 0) { $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $t) }
                    } elseif ($cl.role -eq 'witch') {
                        # 第一次输入是解药（救狼刀目标），第二次输入是毒药（不用）
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

                # 死亡广播：被狼人击杀/被放逐/被女巫毒杀 → 标记脚本侧状态
                if ($line -match '槽(\d+)') {
                    if ($line.Contains('被狼人击杀') -or $line.Contains('被放逐') -or $line.Contains('被女巫毒杀')) {
                        $deadSlot = [int]$Matches[1]
                        foreach ($c2 in $script:conns) {
                            if ($c2.k -eq $deadSlot) { $c2.alive = $false; $c2.dead = $true }
                        }
                    }
                }

                if ($line.Contains('白天发言阶段')) {
                    # 白天开始广播发往所有存活玩家，每个 socket 各收到一份；
                    # 只能靠“白天2 标志（夜已推进过一次）”区分第几天，不能在副本上累加计数
                    if (-not $st.day1Started) {
                        $st.day1Started = $true
                        $st.dayCount = 1
                    } elseif ($st.nightAfterDay1Start) {
                        $st.day2Started = $true
                        $st.dayCount = 2
                    }
                }

                if ($line -eq '天黑请闭眼，开始今夜行动。') {
                    # 夜晚开始广播也是每 socket 一份；这里只关心“白天 1 之后推进过一次夜晚”
                    if ($st.day1Started) { $st.nightAfterDay1Start = $true }
                    $st.nightCount++
                    # 游戏2：遗言超时（约10秒）之后才出现夜晚 2 推进标志
                    if ($st.exileAt) { $flags.nightAfterExile = $true; $flags.night2Time = [DateTime]::Now }
                }

                # ---------- 断言采集 ----------
                if ($line.StartsWith('Alice：')) { $flags.aChat = $true; $flags.aChatLine = $line }
                if ($line.StartsWith('Bob：')) { $flags.bChat = $true; $flags.bChatLine = $line }
                if ($line.StartsWith('Cathy：')) { $flags.cChat = $true; $flags.cChatLine = $line }
                if ($line.Contains('投票目标不合法')) { $flags.dRej++ }
                if ($line.Contains('最高票平票')) { $flags.tie = $true }
                if ($line.Contains(' 弃权。')) { $flags.abstain = $true }
                if ($line.Contains('白天仍在进行')) { $flags.rePrompt = $true }
                if ($line.Contains('玩家Bob 投票给了玩家')) { $flags.bobVote = $true }
                if ($line.Contains('平安夜')) { $flags.peaceful = $true }
                if ($line.Contains('被狼人击杀')) { $flags.wolfKill = $true }
                if ($line.StartsWith('遗言：')) { $flags.lastWord = $line }
                if ($line.Contains('被放逐') -and $gameTag -eq 'g2' -and -not $st.exileAt) { $st.exileAt = [DateTime]::Now }
                if ($line.Contains('本局结束')) { $flags.winner = $line; $flags.gameOver = $true }
                if ($line.Contains('Game over')) { $flags.gameOver = $true }
            }
        }

        # ---------- 白天/行动策略 ----------
        $rolesKnown = All-Roles-Known

        if ($rolesKnown -and -not $st.wolfSlot) {
            foreach ($cl in $script:conns) { if ($cl.role -eq 'werewolf') { $st.wolfSlot = $cl.k; break } }
        }

        if ($rolesKnown -and $st.dayCount -eq 1) {
            Add-Content -LiteralPath $trace -Value ('POLICY day1 ' + $gameTag + ' rolesKnown=' + $rolesKnown + ' speechSent=' + $st.speechSent + ' X=' + $st.X + ' g1=' + $st.g1 + ' g2=' + $st.g2)
            if ($gameTag -eq 'g1') {
                $goodSlots = @()
                foreach ($cl in $script:conns) { if ($cl.alive -and $cl.role -ne 'werewolf') { $goodSlots += $cl.k } }
                if (-not $st.g1 -and $goodSlots.Count -ge 2) { $st.g1 = $goodSlots[0]; $st.g2 = $goodSlots[1] }

                # 第一步：正常聊天 / 注入 / 超长 / 非法投票（投票前全部发出，保证服务端先当聊天处理）
                if (-not $st.speechSent) {
                    Add-Content -LiteralPath $trace -Value 'POLICY send speech'
                    $script:conns[0].w.WriteLine('PLAYER_1|大家好')
                    $script:conns[1].w.WriteLine('PLAYER_2|hello|READY|1')
                    $script:conns[2].w.WriteLine('PLAYER_3|' + ('x' * 3000))
                    $script:conns[3].w.WriteLine('PLAYER_4|VOTE|999')
                    $script:conns[3].w.WriteLine('PLAYER_4|VOTE|-1')
                    $st.speechSent = $true
                }

                # 第二步：Bob 先投（G2，配合槽位 1/3 投 G1、4 投 G2 形成 2:2 平票）
                if ($st.speechSent -and -not $st.bobVoted -and $st.g2 -gt 0) {
                    $script:conns[1].w.WriteLine('PLAYER_2|VOTE|' + $st.g2)
                    $st.bobVoted = $true
                }

                # 第三步：Bob 投完立即断线 → 服务端进入重连等待 → 2.5 秒后重连
                if ($st.bobVoted -and -not $st.bobClosed) {
                    Start-Sleep -Milliseconds 300
                    $script:conns[1].c.Close()
                    $script:conns[1].closed = $true
                    $st.bobClosed = $true
                    $st.disconnectAt = [DateTime]::Now
                }

                if ($st.bobClosed -and -not $st.bobReconnected -and
                    ([DateTime]::Now - $st.disconnectAt).TotalSeconds -ge 2.5) {
                    $c = New-Object Net.Sockets.TcpClient
                    $c.Connect('127.0.0.1', $port)
                    $s = $c.GetStream()
                    $w = New-Object IO.StreamWriter($s)
                    $w.NewLine = "`n"
                    $w.AutoFlush = $true
                    $w.WriteLine('PLAYER_ID|2')
                    $old = $script:conns[1]
                    $script:conns[1] = @{ k = 2; c = $c; s = $s; w = $w; closed = $false
                        bytes = [System.Collections.Generic.List[byte]]::new()
                        queue = [System.Collections.Queue]::new()
                        role = $old.role; assigned = $true; alive = $true; dead = $false
                        witchInputs = $old.witchInputs; dayVoted = $true }
                    $st.bobReconnected = $true
                }

                # 第四步：重连完成后其余玩家投票，形成平票
                if ($st.bobReconnected -and -not $st.restVoted -and $st.g1 -gt 0) {
                    $script:conns[0].w.WriteLine('PLAYER_1|VOTE|' + $st.g1)
                    $script:conns[2].w.WriteLine('PLAYER_3|VOTE|' + $st.g1)
                    $script:conns[3].w.WriteLine('PLAYER_4|VOTE|' + $st.g2)
                    $st.restVoted = $true
                }
            } else {
                # 游戏2 白天1：全员（含狼与X本人）投第一个非狼存活者 X → 放逐后沉默测超时
                if (-not $st.X) {
                    foreach ($cl in $script:conns) {
                        if ($cl.alive -and $cl.role -ne 'werewolf') { $st.X = $cl.k; break }
                    }
                }
                if ($st.X -gt 0 -and -not $st.day1Voted) {
                    foreach ($cl in $script:conns) {
                        if ($cl.alive -and -not $cl.dayVoted) {
                            $cl.w.WriteLine('PLAYER_' + $cl.k + '|VOTE|' + $st.X)
                            $cl.dayVoted = $true
                        }
                    }
                    $st.day1Voted = $true
                }
            }
        }

        if ($rolesKnown -and $st.dayCount -eq 2 -and -not $st.day2Voted) {
            $goodSlots = @()
            foreach ($cl in $script:conns) { if ($cl.alive -and $cl.role -ne 'werewolf') { $goodSlots += $cl.k } }
            if ($st.wolfSlot -gt 0) {
                if ($gameTag -eq 'g1') {
                    # 游戏1 白天2：首位好人 VOTE|abc 按弃权处理；次位好人投狼；狼投自己 → 狼被放逐
                    $script:conns[$goodSlots[0] - 1].w.WriteLine('PLAYER_' + $goodSlots[0] + '|VOTE|abc')
                    if ($goodSlots.Count -ge 2) {
                        $script:conns[$goodSlots[1] - 1].w.WriteLine('PLAYER_' + $goodSlots[1] + '|VOTE|' + $st.wolfSlot)
                    }
                } else {
                    # 游戏2 白天2：唯一好人投狼；狼投自己
                    $script:conns[$goodSlots[0] - 1].w.WriteLine('PLAYER_' + $goodSlots[0] + '|VOTE|' + $st.wolfSlot)
                }
                $script:conns[$st.wolfSlot - 1].w.WriteLine('PLAYER_' + $st.wolfSlot + '|VOTE|' + $st.wolfSlot)
                $st.day2Voted = $true
            }
        }

        Start-Sleep -Milliseconds 30
    }

    if (-not $flags.gameOver -and [DateTime]::Now -ge $deadline) {
        $timedOut = $true
        Add-Content -LiteralPath $trace -Value ('!! ' + $gameTag + ' 超时未结束')
    }

    Add-Content -LiteralPath $trace -Value ('!!! ' + $gameTag + ' LOOPEND gameOver=' + $flags.gameOver + ' timedOut=' + $timedOut +
        ' crash=' + $srvProc.HasExited + ' dayCount=' + $st.dayCount + ' nightCount=' + $st.nightCount +
        ' wolfSlot=' + $st.wolfSlot + ' X=' + $st.X + ' g1=' + $st.g1 + ' g2=' + $st.g2 +
        ' speechSent=' + $st.speechSent + ' bobReconnected=' + $st.bobReconnected +
        ' aChat=' + $flags.aChat + ' tie=' + $flags.tie + ' rePrompt=' + $flags.rePrompt +
        ' abstain=' + $flags.abstain + ' lastWord=' + $flags.lastWord)

    if (-not $flags.gameOver) {
        Add-Content -LiteralPath $trace -Value ('!! NOTOVER ' + $gameTag + ' crash=' + $srvProc.HasExited +
            ' cls=' + $flags.cls + ' dayCount=' + $st.dayCount + ' nightCount=' + $st.nightCount +
            ' wolfSlot=' + $st.wolfSlot + ' X=' + $st.X + ' g1=' + $st.g1 + ' g2=' + $st.g2 +
            ' speechSent=' + $st.speechSent + ' bobReconnected=' + $st.bobReconnected)
        for ($i = 0; $i -lt $script:conns.Count; $i++) {
            $q = @($script:conns[$i].queue.ToArray())
            $sub = ($q | Select-Object -First 25) -join ' || '
            Add-Content -LiteralPath $trace -Value ('连接' + ($i + 1) + ' [alive=' + $script:conns[$i].alive + ' dead=' + $script:conns[$i].dead + ']：' + $sub)
        }
    }

    $roles = @()
    foreach ($cl in $script:conns) { if ($cl.role) { $roles += $cl.role } }

    foreach ($cl in $script:conns) { try { $cl.c.Close() } catch {} }
    Stop-Process -Id $srvProc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300

    return @{ flags = $flags; st = $st; roles = $roles; timedOut = $timedOut; crashed = $crashed }
}

# ---------- 清理与启动 ----------
Get-Process -Name Start,Server,Client -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400
Remove-Item -LiteralPath "$wolf\server.log" -ErrorAction SilentlyContinue

$r1 = Run-Game 5201 'TESTSP1' 'g1'
Copy-Item -Path "$wolf\server.log" -Destination "$wolf\tests\server_g1.log" -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath "$wolf\server.log" -ErrorAction SilentlyContinue
$r2 = Run-Game 5202 'TESTSP2' 'g2'

# ---------- 游戏 1 验收 ----------
$f1 = $r1.flags
Check '游戏1：全员收到 __CLS__（进游戏清屏）' ($f1.cls -ge 4)
Check '游戏1：身份私信齐（1狼+预言家+女巫+村民）' (($r1.roles.Count -eq 4) -and (($r1.roles | Where-Object { $_ -eq 'werewolf' }).Count -eq 1) -and ($r1.roles -contains 'seer') -and ($r1.roles -contains 'witch') -and ($r1.roles -contains 'villager'))
Check '游戏1：白天聊天全角冒号展示（Alice：大家好）' ($f1.aChat -and $f1.aChatLine.StartsWith('Alice：') -and -not $f1.aChatLine.Contains('['))
Check '游戏1：注入聊天不触发指令（Bob：hello... 原样广播）' ($f1.bChat -and $f1.bChatLine.StartsWith('Bob：hello'))
Check '游戏1：注入不占用投票（Bob 仍须真实投票）' ($f1.bobVote)
Check '游戏1：超长聊天（3000 字符）正常广播不崩' ($f1.cChat -and $f1.cChatLine.Length -gt 2000)
Check '游戏1：VOTE|999 与 VOTE|-1 被拒绝（目标不合法）' ($f1.dRej -ge 2)
Check '游戏1：白天断线重连后收到继续提示' ($f1.rePrompt)
Check '游戏1：平票无人被放逐' ($f1.tie)
Check '游戏1：VOTE|abc 按弃权处理（弃权广播）' ($f1.abstain)
Check '游戏1：被放逐者遗言广播（遗言：这是我的遗言）' ($f1.lastWord -eq '遗言：这是我的遗言')
Check '游戏1：首夜女巫救人（平安夜）' ($f1.peaceful)
Check '游戏1：游戏正常结束（本局结束）' ($f1.gameOver -and $f1.winner.Contains('本局结束'))
Check '游戏1：进程未崩溃' (-not $r1.crashed)

# ---------- 游戏 2 验收 ----------
$f2 = $r2.flags
Check '游戏2：全员收到 __CLS__（进游戏清屏）' ($f2.cls -ge 4)
Check '游戏2：身份私信齐' (($r2.roles.Count -eq 4) -and (($r2.roles | Where-Object { $_ -eq 'werewolf' }).Count -eq 1))
Check '游戏2：遗言超时（约10秒）后游戏继续（夜晚2推进）' ($f2.nightAfterExile -and $f2.night2Time -and (($f2.night2Time - $r2.st.exileAt).TotalSeconds -ge 8))
Check '游戏2：夜晚狼刀死讯广播' ($f2.wolfKill)
Check '游戏2：游戏正常结束（本局结束）' ($f2.gameOver -and $f2.winner.Contains('本局结束'))
Check '游戏2：进程未崩溃' (-not $r2.crashed)

Write-Output ''
Write-Output ("===== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " =====")
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
