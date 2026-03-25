param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$ServerExe = "build/Release/cache_tcp_server.exe",
    [int]$Port = 6379,
    [int]$Capacity = 50000,
    [int]$AofWaitSeconds = 2,
    [int]$SnapshotWaitSeconds = 35,
    [int]$KeyTtlMs = 300000
)

$ErrorActionPreference = "Stop"

$result = [ordered]@{
    ServerStart = $false
    AofFileExists = $false
    AofContent = $false
    SnapshotFileExists = $false
    SnapshotContent = $false
    RecoveryLiveKey = $false
    RecoveryDeletedKey = $false
}

$serverProc = $null
$global:tcpClient = $null

function Write-Pass([string]$Message) {
    Write-Host "PASS: $Message" -ForegroundColor Green
}

function Write-Fail([string]$Message) {
    Write-Host "FAIL: $Message" -ForegroundColor Red
}

function Wait-ForServer([string]$HostName, [int]$ServerPort, [int]$TimeoutSec) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        try {
            $probe = New-Object System.Net.Sockets.TcpClient
            $async = $probe.BeginConnect($HostName, $ServerPort, $null, $null)
            $ok = $async.AsyncWaitHandle.WaitOne(300)
            if ($ok -and $probe.Connected) {
                $probe.EndConnect($async)
                $probe.Close()
                return $true
            }
            $probe.Close()
        } catch {
            # keep retrying until timeout
        }
        Start-Sleep -Milliseconds 200
    }
    return $false
}

function Open-Session([string]$HostName, [int]$ServerPort) {
    $client = New-Object System.Net.Sockets.TcpClient($HostName, $ServerPort)
    $stream = $client.GetStream()

    $writer = New-Object System.IO.StreamWriter($stream)
    $writer.NewLine = "`n"
    $writer.AutoFlush = $true

    $reader = New-Object System.IO.StreamReader($stream)
    $greeting = $reader.ReadLine()

    return [pscustomobject]@{
        Client = $client
        Reader = $reader
        Writer = $writer
        Greeting = $greeting
    }
}

function Send-Line($session, [string]$line) {
    $session.Writer.WriteLine($line)
    return $session.Reader.ReadLine()
}

function Stop-ServerProcess() {
    if ($null -ne $serverProc -and -not $serverProc.HasExited) {
        try {
            Stop-Process -Id $serverProc.Id -Force
        } catch {
            # ignore cleanup failures
        }
    }
}

function Start-ServerProcess([string]$ExePath, [string]$WorkingDir, [int]$ServerPort, [int]$MaxKeys) {
    $args = "$ServerPort $MaxKeys"
    return Start-Process -FilePath $ExePath -ArgumentList $args -WorkingDirectory $WorkingDir -PassThru
}

try {
    $resolvedProjectRoot = (Resolve-Path $ProjectRoot).Path
    $resolvedServerExe = if ([System.IO.Path]::IsPathRooted($ServerExe)) {
        $ServerExe
    } else {
        Join-Path $resolvedProjectRoot $ServerExe
    }

    if (-not (Test-Path $resolvedServerExe)) {
        throw "Server executable not found: $resolvedServerExe"
    }

    $dataDir = Join-Path $resolvedProjectRoot "data"
    New-Item -ItemType Directory -Force -Path $dataDir | Out-Null

    $aofPath = Join-Path $dataDir "appendonly.aof"
    $snapshotPath = Join-Path $dataDir "dump.rdb"

    Remove-Item $aofPath -ErrorAction SilentlyContinue
    Remove-Item $snapshotPath -ErrorAction SilentlyContinue

    $suffix = [System.Guid]::NewGuid().ToString("N").Substring(0, 8)
    $liveKey = "persist:live:$suffix"
    $deleteKey = "persist:delete:$suffix"

    Write-Host "Starting server..."
    $serverProc = Start-ServerProcess -ExePath $resolvedServerExe -WorkingDir $resolvedProjectRoot -ServerPort $Port -MaxKeys $Capacity

    if (-not (Wait-ForServer -HostName "127.0.0.1" -ServerPort $Port -TimeoutSec 10)) {
        throw "Server did not become ready on port $Port"
    }
    $result.ServerStart = $true
    Write-Pass "Server started and accepted TCP connections"

    $session = Open-Session -HostName "127.0.0.1" -ServerPort $Port
    if ($session.Greeting -like "OK*") {
        Write-Pass "Server greeting received"
    } else {
        throw "Unexpected greeting: $($session.Greeting)"
    }

    $resp1 = Send-Line $session "SET $liveKey live_value $KeyTtlMs"
    $resp2 = Send-Line $session "SET $deleteKey delete_me"
    $resp3 = Send-Line $session "DEL $deleteKey"

    if ($resp1 -ne "OK") { throw "SET live key failed: $resp1" }
    if ($resp2 -ne "OK") { throw "SET delete key failed: $resp2" }
    if ($resp3 -ne "1") { throw "DEL key failed: $resp3" }
    Write-Pass "Write commands succeeded"

    [void](Send-Line $session "QUIT")
    $session.Client.Close()

    Start-Sleep -Seconds $AofWaitSeconds

    if (Test-Path $aofPath) {
        $result.AofFileExists = $true
        Write-Pass "AOF file exists"
    } else {
        Write-Fail "AOF file not found at $aofPath"
    }

    if ($result.AofFileExists) {
        $aofText = Get-Content $aofPath -Raw
        if ($aofText.Contains("SET $liveKey live_value $KeyTtlMs") -and
            $aofText.Contains("SET $deleteKey delete_me") -and
            $aofText.Contains("DEL $deleteKey")) {
            $result.AofContent = $true
            Write-Pass "AOF contains expected write commands"
        } else {
            Write-Fail "AOF content missing expected commands"
        }
    }

    Start-Sleep -Seconds $SnapshotWaitSeconds

    if (Test-Path $snapshotPath) {
        $result.SnapshotFileExists = $true
        Write-Pass "Snapshot file exists"
    } else {
        Write-Fail "Snapshot file not found at $snapshotPath"
    }

    if ($result.SnapshotFileExists) {
        $snapText = Get-Content $snapshotPath -Raw
        if ($snapText.Contains("# LLMC_RDB_V1") -and $snapText.Contains("SET $liveKey live_value")) {
            $result.SnapshotContent = $true
            Write-Pass "Snapshot contains header and live key"
        } else {
            Write-Fail "Snapshot content missing expected header or live key"
        }
    }

    Write-Host "Restarting server for recovery check..."
    Stop-ServerProcess
    Start-Sleep -Seconds 1

    $serverProc = Start-ServerProcess -ExePath $resolvedServerExe -WorkingDir $resolvedProjectRoot -ServerPort $Port -MaxKeys $Capacity
    if (-not (Wait-ForServer -HostName "127.0.0.1" -ServerPort $Port -TimeoutSec 10)) {
        throw "Server did not restart on port $Port"
    }

    $session2 = Open-Session -HostName "127.0.0.1" -ServerPort $Port
    $getLive = Send-Line $session2 "GET $liveKey"
    $getDeleted = Send-Line $session2 "GET $deleteKey"
    [void](Send-Line $session2 "QUIT")
    $session2.Client.Close()

    if ($getLive -eq "live_value") {
        $result.RecoveryLiveKey = $true
        Write-Pass "Recovery restored live key"
    } else {
        Write-Fail "Recovery live key mismatch: $getLive"
    }

    if ($getDeleted -eq "(nil)") {
        $result.RecoveryDeletedKey = $true
        Write-Pass "Recovery preserved delete operation"
    } else {
        Write-Fail "Deleted key unexpectedly present after recovery: $getDeleted"
    }
}
catch {
    Write-Fail $_.Exception.Message
}
finally {
    Stop-ServerProcess
}

Write-Host ""
Write-Host "========== Persistence Validation Summary =========="
$result.GetEnumerator() | ForEach-Object {
    if ($_.Value) {
        Write-Host ("PASS: {0}" -f $_.Key) -ForegroundColor Green
    } else {
        Write-Host ("FAIL: {0}" -f $_.Key) -ForegroundColor Red
    }
}

$allPassed = $true
foreach ($kv in $result.GetEnumerator()) {
    if (-not $kv.Value) {
        $allPassed = $false
        break
    }
}

if ($allPassed) {
    Write-Host "OVERALL: PASS" -ForegroundColor Green
    exit 0
}

Write-Host "OVERALL: FAIL" -ForegroundColor Red
exit 1
