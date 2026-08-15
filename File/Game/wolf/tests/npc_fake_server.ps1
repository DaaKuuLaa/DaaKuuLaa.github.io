# 在线 NPC 假 API 服务器（测试工具，非游戏实现）
# 循环接受多个连接（online NPC 每阶段决策都会调一次），每个连接响应一个
# 固定动作 {"action":"VOTE|3"} 并记录 REQ: 行供脚本断言（F3）。
# 用端口探测 + 空闲等待退出：无新连接 8 秒即自行退出，避免残留占端口。
# 写 PID 文件 npc_fake_server.pid 供脚本按文件清理（Get-NetTCPConnection
# 对 Loopback 监听不可靠，按端口查不到就杀不掉残留）。
$listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 18080)
$listener.Start()
[void][System.IO.File]::WriteAllText("$PSScriptRoot\..\npc_fake_server.pid",
    ([System.Diagnostics.Process]::GetCurrentProcess().Id.ToString()),
    [System.Text.Encoding]::ASCII)
try {
    $deadline = [DateTime]::Now.AddSeconds(20)
    $idleSince = [DateTime]::Now
    while ([DateTime]::Now -lt $deadline) {
        if (-not $listener.Pending) {
            Start-Sleep -Milliseconds 200
            if (([DateTime]::Now - $idleSince).TotalSeconds -gt 8) { break }
            continue
        }
        $idleSince = [DateTime]::Now
        $client = $listener.AcceptTcpClient()
        $client.ReceiveTimeout = 5000
        $stream = $client.GetStream()
        $bytes = New-Object byte[] 8192
        $total = New-Object System.Text.StringBuilder
        do {
            $n = $stream.Read($bytes, 0, $bytes.Length)
            if ($n -le 0) { break }
            [void]$total.Append([System.Text.Encoding]::ASCII.GetString($bytes, 0, $n))
        } while (-not $total.ToString().Contains("`r`n`r`n"))
        $reqHead = $total.ToString().Split("`r`n")[0]
        Write-Output ("REQ: " + $reqHead)
        [void][System.IO.File]::AppendAllText("$PSScriptRoot\..\fake_server_log.txt",
            ("REQ: " + $reqHead + "`r`n"), [System.Text.Encoding]::UTF8)
        $body = '{"action":"VOTE|3"}'
        $bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($body)
        $resp = "HTTP/1.1 200 OK`r`nContent-Type: application/json`r`nContent-Length: $($bodyBytes.Length)`r`nConnection: close`r`n`r`n"
        $respBytes = [System.Text.Encoding]::ASCII.GetBytes($resp)
        $stream.Write($respBytes, 0, $respBytes.Length)
        $stream.Write($bodyBytes, 0, $bodyBytes.Length)
        $stream.Flush()
        Start-Sleep -Milliseconds 200
        $client.Close()
    }
} finally {
    $listener.Stop()
    Remove-Item "$PSScriptRoot\..\npc_fake_server.pid" -ErrorAction SilentlyContinue
}