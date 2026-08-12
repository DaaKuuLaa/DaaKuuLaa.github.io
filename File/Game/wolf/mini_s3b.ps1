# mini S3 repro: online NPC -> fake server. All-English comments to survive PS5.1 GBK pre-read.
$ErrorActionPreference = 'Continue'
$wolf = Split-Path -Parent $MyInvocation.MyCommand.Path

function Connect-Retry($port) {
    for ($i = 0; $i -lt 20; $i++) {
        try { return [Net.Sockets.TcpClient]::new('127.0.0.1', $port) } catch { Start-Sleep -Milliseconds 250 }
    }
    return $null
}
function Recv-Until($s, $needle, $ms) {
    $buf = ''
    $dl = [DateTime]::Now.AddMilliseconds($ms)
    while ([DateTime]::Now -lt $dl) {
        if ($s.DataAvailable) {
            $b = $s.ReadByte()
            if ($b -lt 0) { break }
            $buf += [char]$b
            if ($buf -match $needle) { return $buf }
        } else { Start-Sleep -Milliseconds 20 }
    }
    return $buf
}
function SendLine($c, $line) {
    $c.w.WriteLine($line)
}
function New-Room4($port) {
    $room = @()
    for ($k = 1; $k -le 4; $k++) {
        $c = Connect-Retry $port
        if (-not $c) { Write-Output 'CONNECT FAIL'; exit 1 }
        $s = $c.GetStream()
        $w = New-Object IO.StreamWriter($s, [System.Text.UTF8Encoding]::new($false))
        $w.NewLine = "`n"
        $w.AutoFlush = $true
        $w.WriteLine('HELLO|' + ('Alice','Bob','Cathy','Dave')[$k-1] + '|zh')
        $null = Recv-Until $s 'WELCOME|' 4000
        $room += @{ c = $c; s = $s; w = $w }
    }
    $null = Recv-Until $room[0].s 'ROOM_STATUS\|' 3000
    return $room
}
function New-Bot($k, $port) {
    $c = Connect-Retry $port
    if (-not $c) { Write-Output 'BOT CONNECT FAIL'; exit 1 }
    $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s, [System.Text.UTF8Encoding]::new($false))
    $w.NewLine = "`n"
    $w.AutoFlush = $true
    $w.WriteLine('PLAYER_ID|' + $k)
    return @{ k = $k; c = $c; s = $s; w = $w; lines = [System.Collections.ArrayList]::new(); queue = [System.Collections.Queue]::new(); bytes = [System.Collections.Generic.List[byte]]::new() }
}
function Pump($bots) {
    foreach ($cl in $bots) {
        try {
            while ($cl.s.DataAvailable) {
                $b = $cl.s.ReadByte()
                if ($b -lt 0) { break }
                if ($b -eq 10) {
                    $raw = $cl.bytes.ToArray()
                    $cl.bytes.Clear()
                    $line = [System.Text.Encoding]::UTF8.GetString($raw).TrimEnd("`r")
                    if ($line.Length -gt 0) { $cl.lines.Add($line) | Out-Null; $cl.queue.Enqueue($line) }
                } else { $cl.bytes.Add([byte]$b) }
            }
        } catch { }
    }
}

Get-Process -Name Start,Server,Client,Client_en -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item "$wolf\npc_key.bin" -ErrorAction SilentlyContinue
Remove-Item "$wolf\fake_out.txt" -ErrorAction SilentlyContinue
$env:WOLF_VOTE_TIMEOUT_SECONDS = '6'
$env:WOLF_NPC_API_KEY = 'testkey-12345'
$env:WOLF_NPC_API_URL = 'http://127.0.0.1:18080/chat'
$env:WOLF_NPC_TIMEOUT_SECONDS = '3'

$fake = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File', "$wolf\tests\npc_fake_server.ps1") -WindowStyle Hidden -PassThru -RedirectStandardOutput "$wolf\fake_out.txt"
Start-Sleep -Milliseconds 800

$pStart = Start-Process -FilePath "$wolf\Start.exe" -ArgumentList '8888' -WindowStyle Hidden -PassThru -RedirectStandardOutput "$wolf\start_env_mini.txt"
Start-Sleep -Milliseconds 800

$room = New-Room4 8888
SendLine $room[0] 'ADD NPC NpcOn on'
$null = Recv-Until $room[0].s '已添加' 3000
SendLine $room[0] 'ADD NPC NpcOff off'
$null = Recv-Until $room[0].s '已添加' 3000
SendLine $room[0] 'READY'
SendLine $room[1] 'READY'
SendLine $room[2] 'READY'
SendLine $room[3] 'READY'
Start-Sleep -Milliseconds 600
SendLine $room[0] 'START'
$gps = @()
foreach ($cl in $room) { $gps += Recv-Until $cl.s 'GAME_PREPARE\|' 6000 }
foreach ($cl in $room) { try { $cl.c.Close() } catch {} }
$bots = @()
for ($k = 1; $k -le 4; $k++) { $bots += New-Bot $k 8888 }

$waitHinted = $false
$s3over = $false
$deadline = [DateTime]::Now.AddSeconds(30)
$lastPing = [DateTime]::Now
while ([DateTime]::Now -lt $deadline) {
    if (([DateTime]::Now - $lastPing).TotalSeconds -ge 1) {
        foreach ($b in $bots) { try { $b.w.WriteLine('PING') } catch {} }
        $lastPing = [DateTime]::Now
    }
    Pump $bots
    foreach ($b in $bots) {
        while ($b.queue.Count -gt 0) {
            $line = $b.queue.Dequeue()
            if ($line -match '__GAME_OVER__') { $s3over = $true }
        }
    }
    if (-not $waitHinted) {
        $waitHinted = (@($bots | Where-Object { $_.lines -match 'AI 分析中' }).Count -gt 0)
    }
    if ($s3over) { break }
    Start-Sleep -Milliseconds 50
}

$keyFile = Test-Path "$wolf\npc_key.bin"
$fakeOut = ''
if (Test-Path "$wolf\fake_out.txt") {
    try {
        $fs = [System.IO.File]::Open("$wolf\fake_out.txt", 'Open', 'Read', 'ReadWrite')
        $buf = New-Object byte[] $fs.Length
        $null = $fs.Read($buf, 0, $buf.Length)
        $fs.Close()
        $fakeOut = [System.Text.Encoding]::Unicode.GetString($buf)
    } catch {
        $fakeOut = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes("$wolf\fake_out.txt"))
    }
}
Write-Output ("RESULT waitHinted=" + $waitHinted + " keyFile=" + $keyFile + " s3over=" + $s3over)
Write-Output ("FAKE_OUT=" + $fakeOut)
if (Test-Path "$wolf\start_env_mini.txt") {
    Write-Output ("START_OUT=" + [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes("$wolf\start_env_mini.txt")))
}
Get-Process -Name Start,Server -ErrorAction SilentlyContinue | Stop-Process -Force
Stop-Process -Name 'npc_fake_server' -Force -ErrorAction SilentlyContinue