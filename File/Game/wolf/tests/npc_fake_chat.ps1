# 在线 NPC 房内对话假 API 服务器（测试工具，非游戏实现）
# 与 npc_fake_server.ps1（游戏内决策）不同：这是房内闲聊路径（§22），
# 模型返回 {"reply":"..."} 纯文本，NpcExtractText 提取 "reply" 键当发言。
# 退出策略：无新连接 60 秒即自行退出（必须大于建房/入房流程耗时——
# 房内测试从起 server 到第一条 @ 要约 8-12 秒，且本机 PowerShell 子进程
# 冷启动到监听生效约 15 秒，8/25 秒空闲窗口都可能提前退出导致在线请求
# 连不上、回退离线假 PASS），总寿命上限 120 秒防残留。
# 写 PID 文件 fake_chat.pid 供脚本按文件清理（Get-NetTCPConnection 对
# Loopback 监听不可靠，按端口查不到就杀不掉残留）。
$listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 18080)
$listener.Start()
[void][System.IO.File]::WriteAllText("$PSScriptRoot\..\fake_chat.pid",
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
        [void][System.IO.File]::AppendAllText("$PSScriptRoot\..\fake_chat_log.txt",
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
            $reqHead = $total.ToString().Split("`r`n")[0]
            Write-Output ("REQ: " + $reqHead)
            [void][System.IO.File]::AppendAllText("$PSScriptRoot\..\fake_chat_log.txt",
                ("REQ: " + $reqHead + "`r`n"), [System.Text.Encoding]::UTF8)
            $body = '{"reply":"AI房内回话"}'
            $bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($body)
            $resp = "HTTP/1.1 200 OK`r`nContent-Type: application/json`r`nContent-Length: $($bodyBytes.Length)`r`nConnection: close`r`n`r`n"
            $respBytes = [System.Text.Encoding]::ASCII.GetBytes($resp)
            try {
                $stream.Write($respBytes, 0, $respBytes.Length)
                $stream.Write($bodyBytes, 0, $bodyBytes.Length)
                $stream.Flush()
            } catch { }
        } catch {
            [void][System.IO.File]::AppendAllText("$PSScriptRoot\..\fake_chat_log.txt",
                ("CONN_ERR: " + $_.Exception.Message + "`r`n"), [System.Text.Encoding]::UTF8)
        } finally {
            try { $client.Close() } catch { }
        }
    }
} finally {
    $listener.Stop()
    Remove-Item "$PSScriptRoot\..\fake_chat.pid" -ErrorAction SilentlyContinue
}
