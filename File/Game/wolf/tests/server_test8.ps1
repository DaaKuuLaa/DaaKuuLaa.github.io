# 狼人杀单局服务器（Server.exe）8 人全流程模拟脚本（测试工具，非游戏实现）
# 用法: powershell -File D:\Demon\wolf\tests\server_test8.ps1
# 8 个裸 socket 机器人自动打完一局全程：身份分配 → 多夜行动（多狼归票/预言家/女巫）
# → 天亮结算 → 白天投票放逐 → … → 胜负结算。
# 配置：2 狼 + 0 中立 + 2 神（预言家+女巫）+ 村民开，共 8 人。
# 机器人策略：狼每晚刀第一个非狼存活者；好人白天投第一个存活狼。
# 目的：验证多玩家规模下夜晚输入序列（每狼一个输入）、狼人归票、
#     女巫救/毒、白天多轮投票与胜负判定不崩、正常收局。

$ErrorActionPreference = 'Stop'
$wolf = Split-Path $PSScriptRoot -Parent
$num = 8
$script:pass = 0
$script:fail = 0
$script:conns = @()
$script:wolfTarget = $null
$script:dayStarted = $false

function Check($desc, $cond) {
    if ($cond) { $script:pass++; Write-Output ("PASS  " + $desc) }
    else       { $script:fail++; Write-Output ("FAIL  " + $desc) }
}

# 每连接：socket + 接收缓冲 + 行队列 + 身份/状态
function New-Bot($k) {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', 5001)
    $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s)
    $w.NewLine = "`n"
    $w.AutoFlush = $true
    $w.WriteLine('PLAYER_ID|' + $k)
    $entry = @{
        k = $k; c = $c; s = $s; w = $w
        bytes = [System.Collections.Generic.List[byte]]::new()
        queue = [System.Collections.Queue]::new()
        role = ''; assigned = $false; alive = $true
        witchInputs = 0
        dayVoted = $false
    }
    $script:conns += $entry
}

function PumpAll {
    foreach ($cl in $script:conns) {
        while ($cl.s.DataAvailable) {
            $b = $cl.s.ReadByte()
            if ($b -lt 0) { $cl.alive = $false; break }
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
    }
}

# ---------- 清理与启动 ----------
Get-Process -Name Start,Server,Client -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400
Remove-Item -LiteralPath "$wolf\server.log" -ErrorAction SilentlyContinue
$srvProc = Start-Process -FilePath "$wolf\Server.exe" -ArgumentList @(
    '5001', 'Alice', 'Bob', 'Cathy', 'Dave', 'Eve', 'Frank', 'Grace', 'Hank',
    '127.0.0.1', '8888', 'TESTRM8', '2', '0', '2', '0', '1',
    'zh', 'zh', 'zh', 'zh', 'zh', 'zh', 'zh', 'zh'
) -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 2

for ($i = 1; $i -le $num; $i++) { New-Bot $i }

$deadline = [DateTime]::Now.AddSeconds(180)
$gameOver = $false
$winner = ''
$wolfKitchen = 0          # 夜晚收到"狼人/白狼王归票"输入的计数（数夜则多）

while (-not $gameOver -and [DateTime]::Now -lt $deadline) {
    PumpAll

    foreach ($cl in $script:conns) {
        while ($cl.queue.Count -gt 0) {
            $line = $cl.queue.Dequeue()

            if ($line.Contains('你被分配到')) { $cl.assigned = $true }

            if ($line.Contains('ROLE|')) { $cl.role = $line.Substring(5) }

            if ($line.Trim() -eq '__INPUT__') {
                if ($cl.role -eq 'werewolf') {
                    # 每晚：每个狼人输入一个目标（刀第一个非狼存活者，且不能是自己）
                    $t = 0
                    foreach ($c2 in $script:conns) {
                        if ($c2.k -ne $cl.k -and $c2.alive -and $c2.role -ne 'werewolf') { $t = $c2.k; break }
                    }
                    if ($t -eq 0) { $cl.w.WriteLine('PLAYER_' + $cl.k + '|0') }
                    else { $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $t) }
                    $script:wolfKitchen++
                } elseif ($cl.role -eq 'seer') {
                    # 预言家验第一个存活者（除自己）
                    $t = 1
                    foreach ($c2 in $script:conns) { if ($c2.k -ne $cl.k -and $c2.alive) { $t = $c2.k; break } }
                    $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $t)
                } elseif ($cl.role -eq 'witch') {
                    # 新协议：解药输入 1=救当夜狼刀目标、0=不救（不再是目标槽号）；
                    # 第二次输入毒药弃用（发 0）
                    if ($cl.witchInputs -eq 0) {
                        $cl.w.WriteLine('PLAYER_' + $cl.k + '|1')
                    } else {
                        $cl.w.WriteLine('PLAYER_' + $cl.k + '|0')
                    }
                    $cl.witchInputs++
                } else {
                    $cl.w.WriteLine('PLAYER_' + $cl.k + '|0')
                }
            }

            if ($line -match '槽(\d+)') {
                if ($line.Contains('被狼人击杀') -or $line.Contains('被放逐') -or $line.Contains('被女巫毒杀')) {
                    $dead = [int]$Matches[1]
                    foreach ($c2 in $script:conns) { if ($c2.k -eq $dead) { $c2.alive = $false } }
                }
            }

            if ($line.Contains('白天发言阶段')) {
                $script:dayStarted = $true
                # 每一天开始时重置投票标记（一局可能多天）
                foreach ($c2 in $script:conns) { $c2.dayVoted = $false }
            }

            if ($line.Contains('本局结束')) {
                $winner = $line
                $gameOver = $true
            }

            if ($line.Contains('Game over')) { $gameOver = $true }
        }
    }

    # 身份齐了且还没定狼刀归票目标：取第一个非狼存活玩家
    if (-not $script:wolfTarget) {
        $rolesKnown = $true
        foreach ($cl in $script:conns) { if ($cl.role -eq '') { $rolesKnown = $false; break } }
        if ($rolesKnown) {
            foreach ($cl in $script:conns) { if ($cl.role -ne 'werewolf') { $script:wolfTarget = $cl.k; break } }
            if (-not $script:wolfTarget) { $script:wolfTarget = 1 }
        }
    }

    # 白天：存活玩家投票（好票狼、狼票好，无目标弃权）
    if ($script:dayStarted) {
        foreach ($cl in $script:conns) {
            if ($cl.alive -and -not $cl.dayVoted) {
                $target = 0
                if ($cl.role -ne 'werewolf') {
                    foreach ($c2 in $script:conns) { if ($c2.k -ne $cl.k -and $c2.alive -and $c2.role -eq 'werewolf') { $target = $c2.k; break } }
                } else {
                    foreach ($c2 in $script:conns) { if ($c2.k -ne $cl.k -and $c2.alive -and $c2.role -ne 'werewolf') { $target = $c2.k; break } }
                }
                if ($target -eq 0) { $cl.w.WriteLine('PLAYER_' + $cl.k + '|VOTE|0') }
                else { $cl.w.WriteLine('PLAYER_' + $cl.k + '|VOTE|' + $target) }
                $cl.dayVoted = $true
            }
        }
    }

    Start-Sleep -Milliseconds 30
}

# ---------- 清理 ----------
foreach ($cl in $script:conns) { try { $cl.c.Close() } catch {} }
Stop-Process -Id $srvProc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

# ---------- 验收点 ----------
$allAssigned = ($script:conns | Where-Object { $_.assigned }).Count -eq $num
Check '8 玩家连接并分配到槽位' $allAssigned

$roles = @()
foreach ($cl in $script:conns) { if ($cl.role) { $roles += $cl.role } }
$wolfCount = ($roles | Where-Object { $_ -eq 'werewolf' }).Count
$villagerCount = ($roles | Where-Object { $_ -eq 'villager' }).Count
Check '身份私信齐（2狼+预言家+女巫+4村民）' (($roles.Count -eq 8) -and ($wolfCount -eq 2) -and ($roles -contains 'seer') -and ($roles -contains 'witch') -and ($villagerCount -eq 4))

Check '狼人夜晚行动发生了归票输入' ($script:wolfKitchen -ge 2)
Check '游戏正常结束（收到本局结束）' ($gameOver -and $winner.Contains('本局结束'))
Check '夜晚到白天的流程推进（收到白天发言）' ([bool]$script:dayStarted)

Write-Output ''
Write-Output ("===== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " =====")
if ($script:fail -gt 0) { exit 1 } else { exit 0 }