# 狼人杀房间系统协议级验收脚本（测试工具，非游戏实现）
# 用法: powershell -File D:\Demon\wolf\tests\proto_test.ps1
# 通过裸 socket 模拟多个客户端，验证 Start.exe 房间管理器的全部验收点：
# 建房/重名拒绝/入房/转移房主/PICK+禁入/档位/村民/比例/
# START 手动开局 / AUTO 自动开局 / 自动配置确认 / 拉黑（名字+IP）/
# 名字规则（IP 格式驳回、限长 10 字符） / 游戏结束 GAME_ENDED+REJOIN 回房

$ErrorActionPreference = 'Stop'
$wolf = Split-Path $PSScriptRoot -Parent
$pass = 0
$fail = 0

function Check($desc, $cond) {
    if ($cond) { $script:pass++; Write-Output ("PASS  " + $desc) }
    else       { $script:fail++; Write-Output ("FAIL  " + $desc) }
}

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

# 断开并全新连接大厅（模拟真实客户端：进游戏关大厅连接，游戏后重连回房）
function Reconnect-Client($name) {
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

# 从 NetworkStream 逐字节读（不经 StreamReader 缓冲），UTF-8 解码
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

function RecvAll($cl, $timeoutMs = 800) {
    $arr = ReadChunk $cl.s $timeoutMs
    return [System.Text.Encoding]::UTF8.GetString($arr)
}

function Close-Client($cl) {
    try { $cl.c.Close() } catch {}
}

# 心跳保活睡眠：睡眠期间模拟真实客户端持续发 PING（真实 Client.exe 每 1 秒发一次，
# 这里用 1 秒保险），避免 Start 的 3 秒失联判定误杀测试连接
function Sleep-Ping($clients, $secs) {
    $end = [DateTime]::Now.AddSeconds($secs)
    while ([DateTime]::Now -lt $end) {
        foreach ($cl in $clients) { try { $cl.w.WriteLine('PING') } catch {} }
        Start-Sleep -Milliseconds 1000
    }
}

# 模拟真实客户端回房：关掉旧大厅连接，重连后发 GAME_ENDED + REJOIN
function Rejoin-Room($old, $name, $roomId, $pidStr) {
    Close-Client $old
    Start-Sleep -Milliseconds 300
    $cl = Reconnect-Client $name
    SendLine $cl ('GAME_ENDED|' + $roomId)
    SendLine $cl ('REJOIN|' + $roomId + '|' + $pidStr)
    return $cl
}

# ---------- 清理与启动 ----------
Get-Process -Name Start,Server,Client -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
$startProc = Start-Process -FilePath "$wolf\Start.exe" -ArgumentList @('8888') -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 2

try {
    # ---------- 阶段 1: 建房 + 重名拒绝 ----------
    $A = New-Client 'Alice'
    SendLine $A 'CREATE|5000'
    $r = RecvUntil $A 'CREATED'
    Check 'A 建房成功' ($r -ne $null)

    $B = New-Client 'Bob'
    SendLine $B 'NAME|Alice'
    $r = RecvUntil $B 'ERROR'
    Check '重名拒绝（Bob 改名为 Alice 被拒）' ($r -match '已被占用')

    SendLine $B 'JOIN|5000'
    $r = RecvUntil $B 'JOINED'
    Check 'B 加入房间' ($r -ne $null)

    $C = New-Client 'Cathy'
    SendLine $C 'JOIN|5000'
    $r = RecvUntil $C 'JOINED'
    Check 'C 加入房间' ($r -ne $null)

    $D = New-Client 'Dave'
    SendLine $D 'JOIN|5000'
    $r = RecvUntil $D 'JOINED'
    Check 'D 加入房间' ($r -ne $null)

    # ---------- 阶段 2: 转移房主 + PICK ----------
    SendLine $A 'TRANSFER|Bob'
    $r = RecvUntil $A '已转交房主给' 2000
    Check 'A 转移房主给 B' ($r -match '已转交')
    $r = RecvUntil $B 'ADMIN' 2000
    Check 'B 成为房主' ($r -match 'ADMIN')

    SendLine $B 'PICK|Cathy'
    $r = RecvUntil $C 'KICKED' 2000
    Check 'B PICK 踢出 Cathy' ($r -match 'KICKED')
    $r = RecvUntil $B '已移出' 2000
    Check '房主收到 PICK 成功提示' ($r -match '已移出')

    # 禁入 10 秒
    $C2 = New-Client 'Cathy'
    SendLine $C2 'JOIN|5000'
    $r = RecvUntil $C2 'ERROR' 2000
    Check '禁入期内 Cathy 被拒' ($r -match '被移出房间')
    $C2.c.Close()

    # 等待禁入 10 秒结束；期间 A/B/D 持续发 PING 保活（否则 3 秒失联判定会把房间清空）
    Sleep-Ping @($A, $B, $D) 11
    $C3 = New-Client 'Cathy'
    SendLine $C3 'JOIN|5000'
    $r = RecvUntil $C3 'JOINED' 3000
    Check '禁入结束后 Cathy 重新加入' ($r -ne $null)

    # ---------- 阶段 3: 职业配置 ----------
    SendLine $B 'LEVEL|1'
    $r = RecvUntil $B '档位已设为' 2000
    Check 'LEVEL 1 合法设置' ($r -match '档位已设为')
    SendLine $B 'LEVEL|9'
    $r = RecvUntil $B 'ERROR' 2000
    Check 'LEVEL 9 非法拒绝' ($r -match '档位必须')

    SendLine $B 'VILLAGER|0'
    $r = RecvUntil $B '已禁用' 2000
    Check 'VILLAGER 0 禁用村民' ($r -match '已禁用')

    SendLine $B 'RATIO|2|0|2'
    $r = RecvUntil $B '比例已设为' 2000
    Check 'RATIO 2 0 2（4人合法）' ($r -match '比例已设为')
    SendLine $B 'RATIO|1|1|1'
    $r = RecvUntil $B 'ERROR' 2000
    Check 'RATIO 1 1 1（和=3≠4 非法拒绝）' ($r -match '比例不合法')

    # ---------- 阶段 4: 全准备 + START 手动开局（合法比例 2+0+2=4） ----------
    foreach ($cl in @($A, $B, $C3, $D)) { SendLine $cl 'READY' }
    Start-Sleep -Seconds 2
    $gameProc = Get-Process -Name Server -ErrorAction SilentlyContinue
    Check '全员准备但未 START 不自动开局' ($gameProc -eq $null)

    SendLine $A 'START'
    $r = RecvUntil $A 'ERROR' 2000
    Check '非房主 START 被拒' ($r -match '只有房主')

    SendLine $B 'START'
    Start-Sleep -Seconds 2
    $gameProc = Get-Process -Name Server -ErrorAction SilentlyContinue
    Check '房主 START 开局（Server.exe 出现）' ($gameProc -ne $null)
    Start-Sleep -Milliseconds 500

    # 收尾：模拟游戏结束，清理（房间 5000 保留为游戏中状态，不再复用）
    foreach ($cl in @($A, $B, $C3, $D)) {
        try { $cl.w.WriteLine('GAME_ENDED|XXXXXXXXXX'); $cl.c.Close() } catch {}
    }

    # ---------- 阶段 5: START 触发自动配置确认（4 人房，比例不符 1+1+1=3≠4） ----------
    Get-Process -Name Server -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    $A2 = New-Client 'Alice'
    SendLine $A2 'CREATE|6000'
    $null = RecvUntil $A2 'CREATED'
    $B2 = New-Client 'Bob'
    SendLine $B2 'JOIN|6000'
    $null = RecvUntil $B2 'JOINED'
    $C4 = New-Client 'Cathy'
    SendLine $C4 'JOIN|6000'
    $null = RecvUntil $C4 'JOINED'
    $D2 = New-Client 'Dave'
    SendLine $D2 'JOIN|6000'
    $null = RecvUntil $D2 'JOINED'

    SendLine $A2 'RATIO|1|1|1'
    $null = RecvUntil $A2 '比例已设为' 2000

    foreach ($cl in @($A2, $B2, $C4, $D2)) { SendLine $cl 'READY' }
    Start-Sleep -Seconds 1
    $r = RecvUntil $A2 'CONFIG_NEED_CONFIRM' 800
    Check '全准备但未 START 不触发自动配置请求' ($r -eq $null)

    SendLine $A2 'START'
    $r = RecvUntil $A2 'CONFIG_NEED_CONFIRM' 3000
    Check 'START 且比例不符触发自动配置请求' ($r -match 'CONFIG_NEED_CONFIRM')

    if ($r) {
        $parts = $r -split '\|'
        $w = $parts[1]; $n = $parts[2]; $g = $parts[3]
        Write-Output ("自动配置建议：狼 " + $w + " / 中立 " + $n + " / 神 " + $g)
        SendLine $A2 'CONFIRM|0'
        $r2 = RecvUntil $A2 '已保持当前配置' 2000
        Check '房主拒绝自动配置（保持原配置）' ($r2 -ne $null)

        # 拒绝后全员 ready 被重置；重新准备 + START 再次触发确认，再同意开局
        foreach ($cl in @($A2, $B2, $C4, $D2)) { SendLine $cl 'READY' }
        # 等 READY 全部被服务端处理（与第一轮 1 秒等待同理），否则 START 会看到"未全准备"
        Start-Sleep -Seconds 1
        SendLine $A2 'START'
        $r3 = RecvUntil $A2 'CONFIG_NEED_CONFIRM' 3000
        Check '拒绝后重新准备 + START 再次触发确认' ($r3 -ne $null)
        if ($r3) {
            SendLine $A2 'CONFIRM|1'
            Start-Sleep -Seconds 2
            $gameProc = Get-Process -Name Server -ErrorAction SilentlyContinue
            Check '房主同意后开局（Server.exe 出现）' ($gameProc -ne $null)
        }
    }

    foreach ($cl in @($A2, $B2, $C4, $D2)) { Close-Client $cl }
    Get-Process -Name Server -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

    # ---------- 阶段 6: 拉黑/取消拉黑（名字+IP）+ AUTO + REJOIN 回房 ----------
    $E = New-Client 'Eve'
    SendLine $E 'CREATE|7000'
    $r = RecvUntil $E 'CREATED'
    Check 'E 建房成功（7000）' ($r -ne $null)
    $roomId7000 = ''
    if ($r) { $roomId7000 = ($r -split '\|')[1] }

    $F = New-Client 'Frank'
    SendLine $F 'JOIN|7000'
    $null = RecvUntil $F 'JOINED'

    SendLine $E 'START'
    $r = RecvUntil $E 'ERROR' 2000
    Check '不足 4 人 START 被拒' ($r -match '至少 4 人')

    $G = New-Client 'Grace'
    SendLine $G 'JOIN|7000'
    $null = RecvUntil $G 'JOINED'
    $H = New-Client 'Henry'
    SendLine $H 'JOIN|7000'
    $null = RecvUntil $H 'JOINED'

    SendLine $E 'START'
    $r = RecvUntil $E 'ERROR' 2000
    Check '有人未准备 START 被拒' ($r -match '还有玩家未准备')

    SendLine $E 'BAN|Eve'
    $r = RecvUntil $E 'ERROR' 2000
    Check '拉黑自己被拒' ($r -match '不能拉黑自己')

    SendLine $F 'BAN|Grace'
    $r = RecvUntil $F 'ERROR' 2000
    Check '非房主 BAN 被拒' ($r -match '只有房主')

    # 拉黑房内玩家：立即踢出 + 永久禁入
    SendLine $E 'BAN|Grace'
    $r = RecvUntil $G 'KICKED' 2000
    Check 'BAN 房内玩家立即踢出' ($r -match '被房主拉黑')
    $r = RecvUntil $E '已拉黑' 2000
    Check '房主收到 BAN 成功提示' ($r -match '已拉黑 Grace')

    $G2 = New-Client 'Grace'
    SendLine $G2 'JOIN|7000'
    $r = RecvUntil $G2 'ERROR' 2000
    Check '拉黑后改名入房仍被拒（按名字）' ($r -match '你已被拉黑')
    $G2.c.Close()

    SendLine $E 'UNBAN|Grace'
    $r = RecvUntil $E '已取消拉黑' 2000
    Check '取消拉黑成功' ($r -match '已取消拉黑')

    $G3 = New-Client 'Grace'
    SendLine $G3 'JOIN|7000'
    $r = RecvUntil $G3 'JOINED' 2000
    Check '取消拉黑后重新加入成功' ($r -ne $null)

    # 拉黑 IP：房内同 IP 玩家（房主除外）一并移出
    SendLine $E 'BAN|127.0.0.1'
    $r = RecvUntil $E '已拉黑 IP' 2000
    Check '拉黑 IP 成功' ($r -match '已拉黑 IP')
    $r = RecvUntil $F 'KICKED' 2000
    Check '同 IP 玩家 Frank 被移出' ($r -match 'IP 已被房主拉黑')
    $r = RecvUntil $G3 'KICKED' 2000
    Check '同 IP 玩家 Grace 被移出' ($r -match 'IP 已被房主拉黑')

    $Z = New-Client 'Zoe'
    SendLine $Z 'JOIN|7000'
    $r = RecvUntil $Z 'ERROR' 2000
    Check '拉黑 IP 后新连接被拒' ($r -match 'IP 已被拉黑')
    $Z.c.Close()

    SendLine $E 'UNBAN|127.0.0.1'
    $r = RecvUntil $E '已取消拉黑' 2000
    Check '取消拉黑 IP 成功' ($r -match '已取消拉黑：127.0.0.1')

    SendLine $E 'UNBAN|127.0.0.1'
    $r = RecvUntil $E 'ERROR' 2000
    Check '取消不在名单中的 IP 报错' ($r -match '不在拉黑名单')

    # 不在房内的名字直接入黑名单
    SendLine $E 'BAN|Nobody'
    $r = RecvUntil $E '已拉黑 Nobody' 2000
    Check '拉黑不在房内的名字成功' ($r -match '已拉黑 Nobody')
    SendLine $E 'UNBAN|Nobody'
    $r = RecvUntil $E '已取消拉黑：Nobody' 2000
    Check '取消不在房内的名字成功' ($r -match '已取消拉黑：Nobody')

    # 恢复 4 人准备 AUTO 自动开局
    $F2 = New-Client 'Frank'
    SendLine $F2 'JOIN|7000'
    $null = RecvUntil $F2 'JOINED'
    $G4 = New-Client 'Grace'
    SendLine $G4 'JOIN|7000'
    $null = RecvUntil $G4 'JOINED'
    $H2 = New-Client 'Henry'
    SendLine $H2 'JOIN|7000'
    $null = RecvUntil $H2 'JOINED'

    SendLine $E 'AUTO'
    $r = RecvUntil $E '自动开局已开启' 2000
    Check 'AUTO 开启成功' ($r -match '自动开局已开启')

    foreach ($cl in @($E, $F2, $G4, $H2)) { SendLine $cl 'READY' }
    $r = RecvUntil $E 'CONFIG_NEED_CONFIRM' 3000
    Check 'AUTO 全准备触发自动配置请求' ($r -match 'CONFIG_NEED_CONFIRM')
    if ($r) {
        SendLine $E 'CONFIRM|1'
        Start-Sleep -Seconds 2
        $gameProc = Get-Process -Name Server -ErrorAction SilentlyContinue
        Check 'AUTO + 同意后自动开局（Server.exe 出现）' ($gameProc -ne $null)
    }

    # 捕获各玩家 GAME_PREPARE 中的玩家编号（回房 REJOIN 用）。
    # 键用本地端口区分四条连接（远程端都是 8888，不可作键）。
    $pids = @{}
    foreach ($cl in @($E, $F2, $G4, $H2)) {
        $gp = RecvUntil $cl 'GAME_PREPARE' 3000
        if ($gp) {
            $pp = $gp -split '\|'
            $pids[$cl.c.Client.LocalEndPoint.Port] = $pp[4]
        }
    }
    Check '4 个客户端都收到 GAME_PREPARE' ($pids.Count -eq 4)

    # 模拟游戏结束：杀掉 Server.exe，客户端断线重连 + GAME_ENDED + REJOIN 回房
    Get-Process -Name Server -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1

    $E2 = Rejoin-Room $E 'Eve' $roomId7000 $pids[$E.c.Client.LocalEndPoint.Port]
    $r = RecvUntil $E2 'JOINED' 3000
    Check 'Eve 游戏结束 REJOIN 回房成功' ($r -ne $null)
    $r = RecvUntil $E2 'ADMIN' 2000
    Check 'Eve 回房后仍为房主' ($r -match 'ADMIN')

    $F3 = Rejoin-Room $F2 'Frank' $roomId7000 $pids[$F2.c.Client.LocalEndPoint.Port]
    $r = RecvUntil $F3 'JOINED' 3000
    Check 'Frank 游戏结束 REJOIN 回房成功' ($r -ne $null)

    $G5 = Rejoin-Room $G4 'Grace' $roomId7000 $pids[$G4.c.Client.LocalEndPoint.Port]
    $r = RecvUntil $G5 'JOINED' 3000
    Check 'Grace 游戏结束 REJOIN 回房成功' ($r -ne $null)

    $H3 = Rejoin-Room $H2 'Henry' $roomId7000 $pids[$H2.c.Client.LocalEndPoint.Port]
    $r = RecvUntil $H3 'JOINED' 3000
    Check 'Henry 游戏结束 REJOIN 回房成功' ($r -ne $null)

    # AUTO 关闭后：全员准备不再自动开局
    SendLine $E2 'AUTO'
    $r = RecvUntil $E2 '自动开局已关闭' 2000
    Check 'AUTO 关闭成功' ($r -match '自动开局已关闭')

    foreach ($cl in @($E2, $F3, $G5, $H3)) { SendLine $cl 'READY' }
    Start-Sleep -Seconds 2
    $gameProc = Get-Process -Name Server -ErrorAction SilentlyContinue
    Check 'AUTO 关闭后全准备不自动开局' ($gameProc -eq $null)

    foreach ($cl in @($E2, $F3, $G5, $H3)) { Close-Client $cl }

    # ---------- 阶段 7: 名字规则（IP 格式驳回 / 限长 10 / 竖线拒绝） ----------
    $X = New-Client 'Ike'
    SendLine $X 'NAME|1.2.3.4'
    $r = RecvUntil $X 'ERROR' 2000
    Check '名字为 IP 格式被拒（特殊字符白名单拦截）' ($r -match '名字只能包含中英文、数字与下划线')

    SendLine $X 'NAME|192.168.1.1'
    $r = RecvUntil $X 'ERROR' 2000
    Check '名字为真实 IP 格式被拒（特殊字符白名单拦截）' ($r -match '名字只能包含中英文、数字与下划线')

    SendLine $X 'NAME|abcdefghijk'
    $r = RecvUntil $X 'NAME_SET' 2000
    Check '超 10 字符名字被截断' ($r -match 'NAME_SET\|abcdefghij$')

    SendLine $X 'NAME|一二三四五六七八九十'
    $r = RecvUntil $X 'NAME_SET' 2000
    Check '10 个汉字名字被截断为 10 字' ($r -match 'NAME_SET\|一二三四五六七八九十')

    SendLine $X 'NAME|一二三四五六七八九十壹'
    $r = RecvUntil $X 'NAME_SET' 2000
    Check '11 个汉字名字截断为 10 字' ($r -match 'NAME_SET\|一二三四五六七八九十$')

    SendLine $X 'NAME|a|b|c'
    $r = RecvUntil $X 'ERROR' 2000
    Check '名字含竖线被拒（特殊符号不再净化）' ($r -match '名字只能包含中英文、数字与下划线')

    SendLine $X 'CREATE|8000'
    $r = RecvUntil $X 'CREATED' 2000
    Check '改名后正常建房' ($r -ne $null)

    Close-Client $X
}
catch {
    Write-Output ("脚本异常: " + $_)
    $script:fail++
}
finally {
    Get-Process -Name Start,Server,Client -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

Write-Output ""
Write-Output ("===== 结果: PASS=" + $pass + " FAIL=" + $fail + " =====")
if ($fail -gt 0) { exit 1 } else { exit 0 }
