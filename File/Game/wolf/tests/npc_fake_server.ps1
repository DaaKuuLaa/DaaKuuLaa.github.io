$listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 18080)
$listener.Start()
try {
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
    $body = '{"action":"VOTE|3"}'
    $bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($body)
    $resp = "HTTP/1.1 200 OK`r`nContent-Type: application/json`r`nContent-Length: $($bodyBytes.Length)`r`nConnection: close`r`n`r`n"
    $respBytes = [System.Text.Encoding]::ASCII.GetBytes($resp)
    $stream.Write($respBytes, 0, $respBytes.Length)
    $stream.Write($bodyBytes, 0, $bodyBytes.Length)
    $stream.Flush()
    Start-Sleep -Milliseconds 300
    $client.Close()
} finally {
    $listener.Stop()
}
