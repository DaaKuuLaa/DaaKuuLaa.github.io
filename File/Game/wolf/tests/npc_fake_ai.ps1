# 在线 AI 假服务器（round13 测试工具，非游戏实现）
# 与 npc_fake_server/npc_fake_chat 的区别：完整记录请求头与请求体
# （含 Authorization 头与 body 的 model 字段），供 round13 断言：
#   1) 请求头 Authorization: Bearer <key>
#   2) 请求体含 "model":"glm-4.7-flash"
# 响应 {"reply":"AI在线回话"}（房内聊天路径）或 {"action":"VOTE|3"}
# （游戏内决策路径），按请求体含 "reply" 提示词自动选择——实际上
# NpcOnlineRoomChat 只认 "reply" 键，NpcOnlineDecide 只认 "action" 键，
# 返回两者都含的体可兼容两个路径。
# 退出策略：无新连接 60 秒退出（建房/入房流程约 8-12 秒 + PowerShell 子
# 进程冷启动约 15 秒），总寿命上限 120 秒。写 PID 文件 npc_fake_ai.pid。
$listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 18080)
$listener.Start()
[void][System.IO.File]::WriteAllText("$PSScriptRoot\..\npc_fake_ai.pid",
    ([System.Diagnostics.Process]::GetCurrentProcess().Id.ToString()),
    [System.Text.Encoding]::ASCII)
try {
    $deadline = [DateTime]::Now.AddSeconds(120)
    $idleSince = [DateTime]::Now
    while ([DateTime]::Now -lt $deadline) {
        if (-not $listener.Pending) {
            Start-Sleep -Milliseconds 200
            if (([DateTime]::Now - $idleSince).TotalSeconds -gt 60) { break }
            continue
        }
        $idleSince = [DateTime]::Now
        $client = $listener.AcceptTcpClient()
        [void][System.IO.File]::AppendAllText("$PSScriptRoot\..\fake_ai_log.txt",
            ("ACCEPT: " + $client.Client.RemoteEndPoint.ToString() + "`r`n"),
            [System.Text.Encoding]::UTF8)
        try {
            $client.ReceiveTimeout = 5000
            $stream = $client.GetStream()
            $bytes = New-Object byte[] 8192
            $total = New-Object System.Text.StringBuilder
            do {
                $n = 0
                try {
                    $n = $stream.Read($bytes, 0, $bytes.Length)
                } catch { $n = -1 }
                if ($n -le 0) { break }
                [void]$total.Append([System.Text.Encoding]::ASCII.GetString($bytes, 0, $n))
            } while (-not $total.ToString().Contains("`r`n`r`n"))
            $raw = $total.ToString()
            $reqLines = $raw -split "`r`n"
            $reqHead = $reqLines[0]
            $authLine = ''
            foreach ($rl in $reqLines) {
                if ($rl -match '^Authorization:\s*Bearer ') { $authLine = $rl }
            }
            $body = ''
            $idx = $raw.IndexOf("`r`n`r`n")
            if ($idx -ge 0) { $body = $raw.Substring($idx + 4) }
            [void][System.IO.File]::AppendAllText("$PSScriptRoot\..\fake_ai_log.txt",
                ("REQ: " + $reqHead + "`r`n" + "AUTH: " + $authLine + "`r`n" + "BODY: " + $body + "`r`n"),
                [System.Text.Encoding]::UTF8)
            $bodyResp = '{"reply":"AI在线回话","action":"VOTE|3"}'
            $bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($bodyResp)
            $resp = "HTTP/1.1 200 OK`r`nContent-Type: application/json`r`nContent-Length: $($bodyBytes.Length)`r`nConnection: close`r`n`r`n"
            $respBytes = [System.Text.Encoding]::ASCII.GetBytes($resp)
            try {
                $stream.Write($respBytes, 0, $respBytes.Length)
                $stream.Write($bodyBytes, 0, $bodyBytes.Length)
                $stream.Flush()
            } catch { }
        } catch {
            [void][System.IO.File]::AppendAllText("$PSScriptRoot\..\fake_ai_log.txt",
                ("CONN_ERR: " + $_.Exception.Message + "`r`n"), [System.Text.Encoding]::UTF8)
        } finally {
            try { $client.Close() } catch { }
        }
    }
} finally {
    $listener.Stop()
    Remove-Item "$PSScriptRoot\..\npc_fake_ai.pid" -ErrorAction SilentlyContinue
}