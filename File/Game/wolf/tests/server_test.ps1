# 狼人杀单局服务器（Server.exe）协议与流程验收脚本（测试工具，非游戏实现）
# 用法: powershell -File D:\Demon\wolf\tests\server_test.ps1
# 4 个裸 socket 客户端自动打完一局：连接分配 → 身份私信 → 夜晚行动（狼/预/巫）
# → 天亮结算 → 白天投票 → 胜负结算 → Game over。
# 配置：1 狼 + 0 中立 + 2 神（预言家+女巫，档位 0），村民开 —— 4 人局狼数 < 好人，
# 第一夜后必进白天投票，覆盖完整昼夜循环（2狼2神 4 人局狼数=好人，首夜平安夜即判狼胜）。

$ErrorActionPreference = 'Stop'
$wolf = Split-Path $PSScriptRoot -Parent
$script:pass = 0
$script:fail = 0
$script:conns = @()
$script:wolfTarget = $null
$script:wolfRejected = $false
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
        witchInputs = 0; wolfFirstIllegal = $false; wolfRetry = $false
        dayVoted = $false
    }
    $script:conns += $entry
}

# 泵取所有连接的可读字节，按行入队（整行按 UTF-8 解码）
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
    '5001', 'Alice', 'Bob', 'Cathy', 'Dave',
    '127.0.0.1', '8888', 'TESTRM', '1', '0', '2', '0', '1',
    'zh', 'zh', 'zh', 'zh'
) -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 2

for ($i = 1; $i -le 4; $i++) { New-Bot $i }

$deadline = [DateTime]::Now.AddSeconds(90)
$gameOver = $false
$winner = ''

while (-not $gameOver -and [DateTime]::Now -lt $deadline) {
    PumpAll

    foreach ($cl in $script:conns) {
        while ($cl.queue.Count -gt 0) {
            $line = $cl.queue.Dequeue()

            if ($line.Contains('你被分配到')) { $cl.assigned = $true }

            if ($line.Contains('ROLE|')) {
                $cl.role = $line.Substring(5)
            }

            if ($line.Contains('目标不合法') -and $cl.wolfRetry) {
                $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $script:wolfTarget)
                $cl.wolfRetry = $false
                $script:wolfRejected = $true
            }

            if ($line.Trim() -eq '__INPUT__') {
                if ($cl.role -eq 'werewolf') {
                    if (-not $cl.wolfFirstIllegal) {
                        # 第一次故意刀自己（非法），验证防御重试
                        $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $cl.k)
                        $cl.wolfFirstIllegal = $true
                        $cl.wolfRetry = $true
                    } elseif (-not $cl.wolfRetry) {
                        $cl.w.WriteLine('PLAYER_' + $cl.k + '|' + $script:wolfTarget)
                    }
                } elseif ($cl.role -eq 'seer') {
                    $cl.w.WriteLine('PLAYER_' + $cl.k + '|1')
                } elseif ($cl.role -eq 'witch') {
                    $cl.wolfRetry = $false
                    # 新协议：解药输入 1=救当夜狼刀目标、0=不救（不再是目标槽号，
                    # 传槽号会被 "Enter 1 or 0." 拒掉重问）；第二次输入毒药 0=不用
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

            if ($line.Contains('白天发言阶段')) { $script:dayStarted = $true }

            if ($line.Contains('本局结束')) {
                $winner = $line
                $gameOver = $true
            }

            if ($line.Contains('Game over')) { $gameOver = $true }
        }
    }

    # 身份齐了且还没定狼刀目标：找第一个非狼存活玩家（脚本全知身份）
    if (-not $script:wolfTarget) {
        $rolesKnown = $true
        foreach ($cl in $script:conns) { if ($cl.role -eq '') { $rolesKnown = $false; break } }
        if ($rolesKnown) {
            $script:wolfTarget = $null
            foreach ($cl in $script:conns) {
                if ($cl.role -ne 'werewolf') { $script:wolfTarget = $cl.k; break }
            }
            if (-not $script:wolfTarget) { $script:wolfTarget = 1 }
        }
    }

    # 白天：每个存活玩家投一票（非狼投第一个存活狼；狼投第一个存活非狼；无目标弃权）
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
$allAssigned = ($script:conns | Where-Object { $_.assigned }).Count -eq 4
Check '4 玩家连接并分配到槽位' $allAssigned

$roles = @()
foreach ($cl in $script:conns) { if ($cl.role) { $roles += $cl.role } }
$wolfCount = ($roles | Where-Object { $_ -eq 'werewolf' }).Count
Check '身份私信齐（1狼+预言家+女巫+村民）' (($roles.Count -eq 4) -and ($wolfCount -eq 1) -and ($roles -contains 'seer') -and ($roles -contains 'witch') -and ($roles -contains 'villager'))

Check '狼人非法刀自己被拒后重试成功' ([bool]$script:wolfRejected)
Check '游戏正常结束（收到本局结束）' ($gameOver -and $winner.Contains('本局结束'))
Check '夜晚到白天的流程推进（收到白天发言）' ([bool]$script:dayStarted)

Write-Output ''
Write-Output ("===== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " =====")
if ($script:fail -gt 0) { exit 1 } else { exit 0 }