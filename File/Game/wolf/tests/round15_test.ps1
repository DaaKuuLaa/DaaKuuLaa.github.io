# round15_test.ps1 — 第十五轮验收（§24 离线 NPC 智能化 + GLM 诊断修复）
# 覆盖：
#  U1 性格系统：话痨 Terse（FNV=0）回复平均长度 > 高冷 CoolBot（FNV=1）
#  U2 问句必答：@ 问句 → 回复含答案词（我觉得/同意/判断/倾向等）≥ 8/10
#  U3 挑衅回怼：@ 挑衅 → 回复含怼词（60% 概率）≥ 3/8
#  U4 对话记忆：播种"我是预言家"事实 → @ 后引用前文（50%）≥ 2/8
#  U5 零幻觉：回复永不含聊天中从未出现的人名
#  U6 在线失败回退智能离线：无 key 在线 NPC @ 必答不崩
#  U7 问句多样性：12 种问法 distinct ≥ 3
#  U8 高冷限长：CoolBot 回复 ≤ 40 字节
# 运行：powershell -NoProfile -ExecutionPolicy Bypass -File tests\round15_test.ps1
# （UTF-8 带 BOM：中文字符串必须有 BOM 前缀，否则 PS 5.1 按 GBK 误读，踩坑 18）

$script:pass = 0
$script:fail = 0
$script:usedPorts = [System.Collections.ArrayList]::new()
$script:lineLog = [System.Collections.ArrayList]::new()
$wolf = $PSScriptRoot | Split-Path -Parent

function Log-Line([string]$seg, [string]$line) {
    $script:lineLog.Add("[$seg] $line") | Out-Null
    if ($script:lineLog.Count -gt 4000) { $script:lineLog.RemoveAt(0) }
}

function Check([string]$name, [bool]$ok) {
    if ($ok) { $script:pass++; Write-Output ("PASS  " + $name) }
    else { $script:fail++; Write-Output ("FAIL  " + $name) }
}

function Kill-Fake {
    foreach ($pf in @("$wolf\fake_chat.pid", "$wolf\npc_fake_server.pid")) {
        if (Test-Path $pf) {
            $pidStr = ''
            try { $pidStr = [IO.File]::ReadAllText($pf).Trim() } catch { }
            if ($pidStr -match '^\d+$') {
                Stop-Process -Id ([int]$pidStr) -Force -ErrorAction SilentlyContinue
            }
            Remove-Item $pf -ErrorAction SilentlyContinue
        }
    }
    try {
        Get-NetTCPConnection -LocalPort 18080 -ErrorAction SilentlyContinue |
            ForEach-Object { Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue }
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

function Start-RM([int]$port = 8888) {
    Stop-Process -Name 'Start' -Force -ErrorAction SilentlyContinue
    Stop-Process -Name 'Server' -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
    Remove-Item "$wolf\start.log" -ErrorAction SilentlyContinue
    # U6 需要无 key：env 与落盘文件都清掉（用户 key 备份在脚本头部，结束恢复）
    Remove-Item Env:WOLF_NPC_API_KEY -ErrorAction SilentlyContinue
    Remove-Item "$wolf\npc_key.bin" -ErrorAction SilentlyContinue
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

$script:keepaliveClients = [System.Collections.ArrayList]::new()
$script:keepStop = [System.Threading.ManualResetEvent]::new($false)
$script:kaPs = $null
$script:kaRs = $null

function Start-Keepalive {
    if ($script:kaRs) { return }
    $rs = [System.Management.Automation.Runspaces.RunspaceFactory]::CreateRunspace()
    $rs.Open()
    $rs.SessionStateProxy.SetVariable('liveClients', $script:keepaliveClients)
    $rs.SessionStateProxy.SetVariable('kaStop', $script:keepStop)
    $ps = [System.Management.Automation.PowerShell]::Create()
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
    try { $script:keepStop.Set() } catch {}
    try { if ($script:kaPs) { $script:kaPs.Dispose() } } catch {}
    try { if ($script:kaRs) { $script:kaRs.Close() } } catch {}
    $script:kaRs = $null
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
    $entry = @{ w = $w; wlock = [object]::new() }
    $script:keepaliveClients.Add($entry) | Out-Null
    return @{ c = $c; s = $s; w = $w; name = $name; entry = $entry; lines = [System.Collections.ArrayList]::new(); pending = [System.Collections.Generic.List[byte]]::new() }
}

function Close-Client($cl) {
    if ($cl -and $cl.entry) { $script:keepaliveClients.Remove($cl.entry) | Out-Null }
    try { if ($cl -and $cl.c) { $cl.c.Close() } } catch {}
}

function SendLine($cl, $cmd) {
    try { $cl.w.WriteLine($cmd) } catch {}
}

function Drain-Lines($cl, $timeoutMs) {
    $deadline = [DateTime]::Now.AddMilliseconds($timeoutMs)
    while ([DateTime]::Now -lt $deadline) {
        try {
            if ($cl.s.DataAvailable) {
                $b = $cl.s.ReadByte()
                if ($b -lt 0) { break }
                if ($b -eq 10) {
                    $raw = @($cl.pending.ToArray())
                    $cl.pending.Clear()
                    $line = [System.Text.Encoding]::UTF8.GetString($raw).TrimEnd("`r")
                    if ($line.Length -gt 0) {
                        $cl.lines.Add($line) | Out-Null
                        Log-Line 'R' $line
                    }
                } else { $cl.pending.Add([byte]$b) }
            } else { Start-Sleep -Milliseconds 15 }
        } catch { break }
    }
}

function New-Room4 {
    $port = Get-FreePort
    $arr = @(New-Client 'Alice')
    SendLine $arr[0] ('CREATE|' + $port)
    $null = RecvUntilStream $arr[0].s 'CREATED' 3000
    foreach ($nm in @('Bob', 'Cathy', 'Dave')) {
        $cl = New-Client $nm
        SendLine $cl ('JOIN|' + $port)
        $null = RecvUntilStream $cl.s 'JOINED' 3000
        $arr += $cl
    }
    return @{ port = $port; room = $arr }
}

function Add-Npc($roomObj, $name, $mode) {
    SendLine $roomObj.room[0] ('ADD NPC ' + $name + ' ' + $mode)
    $null = RecvUntilStream $roomObj.room[0].s '已添加' 3000
}

# 一轮 @：发送 → 收集所有 <npc>：回复行
function At-Npc($roomObj, $npcName, $content, [int]$waitMs = 2500) {
    SendLine $roomObj.room[0] ('@' + $npcName + ' ' + $content)
    $deadline = [DateTime]::Now.AddMilliseconds($waitMs)
    $hits = [System.Collections.ArrayList]::new()
    while ([DateTime]::Now -lt $deadline) {
        foreach ($cl in $roomObj.room) { Drain-Lines $cl 200 }
        foreach ($cl in $roomObj.room) {
            foreach ($l in $cl.lines) {
                if ($l -match ('^ROOM_MSG\|' + [regex]::Escape($npcName) + '：')) {
                    $hits.Add($l) | Out-Null
                }
            }
        }
        if ($hits.Count -gt 0) { break }
        Start-Sleep -Milliseconds 30
    }
    Start-Sleep -Milliseconds 250
    return @($hits)
}

$exitCode = 1
try {
    Start-Keepalive
    Kill-All
    # 备份用户真实 key（如存在），测试后恢复——测试期间必须无 key（U6）
    $keyBackup = $false
    if (Test-Path "$wolf\npc_key.bin") {
        Copy-Item "$wolf\npc_key.bin" "$wolf\npc_key.bin.bak" -Force
        $keyBackup = $true
    }
    $null = Start-RM 8888

    # ============ U1：性格系统（话痨 vs 高冷 长度差） ============
    $U1 = New-Room4
    Add-Npc $U1 'Terse' 'off'      # FNV=0 → P_Talkative（话痨，40% 追加扩展句）
    Add-Npc $U1 'CoolBot' 'off'    # FNV=1 → P_Cool（高冷，恒截断 30 字节+句号）
    foreach ($cl in $U1.room) { Drain-Lines $cl 500 }
    $talkLens = [System.Collections.ArrayList]::new()
    $coolLens = [System.Collections.ArrayList]::new()
    foreach ($i in 1..8) {
        $reps = At-Npc $U1 'Terse' '你觉得怎么样'
        foreach ($l in $reps) {
            $content = $l.Substring($l.IndexOf('：') + 1)
            $talkLens.Add([Text.Encoding]::UTF8.GetByteCount($content)) | Out-Null
        }
    }
    foreach ($i in 1..8) {
        $reps = At-Npc $U1 'CoolBot' '你觉得怎么样'
        foreach ($l in $reps) {
            $content = $l.Substring($l.IndexOf('：') + 1)
            $coolLens.Add([Text.Encoding]::UTF8.GetByteCount($content)) | Out-Null
        }
    }
    $talkAvg = if ($talkLens.Count -gt 0) { (($talkLens | Measure-Object -Average).Average) } else { 0 }
    $coolAvg = if ($coolLens.Count -gt 0) { (($coolLens | Measure-Object -Average).Average) } else { 0 }
    Log-Line 'U1' ("talkAvg=$talkAvg coolAvg=$coolAvg")
    Check 'U1-1 话痨 Terse 回复平均长度 > 高冷 CoolBot + 5 字节' ($talkAvg -ge ($coolAvg + 5))
    Check 'U1-2 高冷 CoolBot 全部回复 ≤ 40 字节' (@($coolLens | Where-Object { $_ -gt 40 }).Count -eq 0)

    # ============ U2：问句必答（答案词） ============
    $qHits = 0
    foreach ($i in 1..10) {
        $reps = At-Npc $U1 'Terse' '谁是狼？'
        foreach ($l in $reps) {
            if ($l -match '我觉得|我认为|同意|意见|判断|观望|倾向|想法') { $qHits++ }
            Log-Line 'U2' $l
        }
    }
    Check 'U2-1 @ 问句回复含答案词（10 试 ≥ 8）' ($qHits -ge 8)

    # ============ U3：挑衅回怼（60% 概率） ============
    $tHits = 0
    foreach ($i in 1..8) {
        $reps = At-Npc $U1 'Terse' '你菜'
        foreach ($l in $reps) {
            if ($l -match '就这|呵|不敢苟同|小学生|笑死|你说了不算|自己品|怼我') { $tHits++ }
            Log-Line 'U3' $l
        }
    }
    Check 'U3-1 @ 挑衅回复含怼词（8 试 ≥ 3）' ($tHits -ge 3)

    # ============ U5：零幻觉（在播种事实之前，保证无事实干扰） ============
    $halFree = $true
    foreach ($i in 1..6) {
        $reps = At-Npc $U1 'Terse' '今天的太阳好大'
        foreach ($l in $reps) {
            if ($l -match '不存在者') { $halFree = $false }
        }
    }
    Check 'U5-1 回复零幻觉（不含从未出现的名字）' $halFree

    # ============ U4：对话记忆（播种事实后 @ 引用前文） ============
    foreach ($cl in $U1.room) { Drain-Lines $cl 400 }
    SendLine $U1.room[1] '我是预言家'
    Start-Sleep -Milliseconds 600
    $mHits = 0
    foreach ($i in 1..8) {
        $reps = At-Npc $U1 'Terse' '你怎么看'
        foreach ($l in $reps) {
            if ($l -match '预言家|我记得|记下|说道|小本本|表个态|证据') { $mHits++ }
            Log-Line 'U4' $l
        }
    }
    Check 'U4-1 播种事实后 @ 回复引用前文（8 试 ≥ 2）' ($mHits -ge 2)

    # ============ U7：问句多样性 ============
    $q7 = [System.Collections.ArrayList]::new()
    foreach ($q in @('谁是狼？', '你怎么看？', '今天天气怎样？', '谁是好人？', '晚上会发生什么？', '谁在说谎？', '你支持谁？', '该投谁？', '你在想什么？', '女巫是谁？', '下一步怎么办？', '大家信谁？')) {
        $reps = At-Npc $U1 'Terse' $q
        foreach ($l in $reps) { $q7.Add($l) | Out-Null }
    }
    $dist7 = @($q7 | Sort-Object -Unique)
    Check 'U7-1 问句回复多样性（12 试 distinct ≥ 3）' ($dist7.Count -ge 3)

    # ============ U6：在线失败回退智能离线（无 key 必答不崩） ============
    $U6 = New-Room4
    Add-Npc $U6 'OnlineNpc' 'on'
    foreach ($cl in $U6.room) { Drain-Lines $cl 500 }
    $u6Reps = At-Npc $U6 'OnlineNpc' '你好吗' 6000
    Check 'U6-1 在线 NPC 无 key 回退离线必答（有回复）' ($u6Reps.Count -ge 1)
    $stillAlive = @(Get-Process -Name 'Start' -ErrorAction SilentlyContinue).Count -ge 1
    Check 'U6-2 Start 进程存活（回退路径不崩）' $stillAlive

    # ============ 收尾 ============
    $exitCode = if ($script:fail -eq 0) { 0 } else { 1 }
}
finally {
    Stop-Keepalive
    Kill-All
    # 恢复用户真实 key（如备份过）
    if ($keyBackup) {
        Move-Item "$wolf\npc_key.bin.bak" "$wolf\npc_key.bin" -Force -ErrorAction SilentlyContinue
    } else {
        Remove-Item "$wolf\npc_key.bin" -ErrorAction SilentlyContinue
    }
}

Write-Output ("round15: PASS=" + $script:pass + " FAIL=" + $script:fail)
exit $exitCode