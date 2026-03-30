param(
    [string]$BaseUrl = "http://13.205.178.239",
    [int]$TimeoutSec = 15,
    [int]$TtlMs = 4000
)

$ErrorActionPreference = "Stop"

$results = [System.Collections.Generic.List[object]]::new()

function Write-Pass([string]$Message) {
    Write-Host "PASS: $Message" -ForegroundColor Green
}

function Write-Fail([string]$Message) {
    Write-Host "FAIL: $Message" -ForegroundColor Red
}

function Read-ErrorBody($Exception) {
    try {
        if ($null -ne $Exception.Response -and $null -ne $Exception.Response.GetResponseStream()) {
            $reader = New-Object System.IO.StreamReader($Exception.Response.GetResponseStream())
            $body = $reader.ReadToEnd()
            $reader.Close()
            return $body
        }
    } catch {
        return ""
    }
    return ""
}

function Invoke-Case {
    param(
        [string]$Name,
        [string]$Method,
        [string]$Path,
        [int]$ExpectedStatus,
        [string]$ExpectedContains = "",
        [string]$Body = "",
        [string]$ContentType = "text/plain"
    )

    $uri = ($BaseUrl.TrimEnd('/')) + $Path
    $status = -1
    $content = ""

    try {
        if ($Method -eq "GET") {
            $resp = Invoke-WebRequest -Uri $uri -Method Get -TimeoutSec $TimeoutSec -UseBasicParsing
        } elseif ($Method -eq "POST") {
            if ($Body -ne "") {
                $resp = Invoke-WebRequest -Uri $uri -Method Post -Body $Body -ContentType $ContentType -TimeoutSec $TimeoutSec -UseBasicParsing
            } else {
                $resp = Invoke-WebRequest -Uri $uri -Method Post -TimeoutSec $TimeoutSec -UseBasicParsing
            }
        } elseif ($Method -eq "DELETE") {
            $resp = Invoke-WebRequest -Uri $uri -Method Delete -TimeoutSec $TimeoutSec -UseBasicParsing
        } else {
            throw "Unsupported method: $Method"
        }

        $status = [int]$resp.StatusCode
        $content = [string]$resp.Content
    } catch {
        if ($null -ne $_.Exception.Response) {
            $status = [int]$_.Exception.Response.StatusCode
            $content = Read-ErrorBody $_.Exception
        } else {
            $content = $_.Exception.Message
        }
    }

    $pass = ($status -eq $ExpectedStatus)
    if ($pass -and $ExpectedContains -ne "") {
        $pass = $content.Contains($ExpectedContains)
    }

    $results.Add([pscustomobject]@{
        Name = $Name
        Method = $Method
        Path = $Path
        Expected = $ExpectedStatus
        Actual = $status
        Pass = $pass
        Body = ($content -replace "`r|`n", " ")
    })

    if ($pass) {
        Write-Pass "$Name ($Method $Path -> $status)"
    } else {
        Write-Fail "$Name ($Method $Path -> expected $ExpectedStatus, got $status)"
        if ($content) {
            Write-Host "  Body: $content"
        }
    }
}

$key = "instance-check-" + [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()

Invoke-Case -Name "Health" -Method "GET" -Path "/health" -ExpectedStatus 200 -ExpectedContains '"ok":true'
Invoke-Case -Name "Stats" -Method "GET" -Path "/stats" -ExpectedStatus 200 -ExpectedContains '"ok":true'
Invoke-Case -Name "Set persistent key" -Method "POST" -Path "/set?key=$key&value=v1" -ExpectedStatus 200 -ExpectedContains '"message":"OK"'
Invoke-Case -Name "Get persistent key" -Method "GET" -Path "/get?key=$key" -ExpectedStatus 200 -ExpectedContains '"value":"v1"'
Invoke-Case -Name "Delete key" -Method "DELETE" -Path "/del?key=$key" -ExpectedStatus 200 -ExpectedContains '"deleted":true'
Invoke-Case -Name "Get deleted key" -Method "GET" -Path "/get?key=$key" -ExpectedStatus 404
Invoke-Case -Name "Bad GET missing key" -Method "GET" -Path "/get" -ExpectedStatus 400
Invoke-Case -Name "Bad SET missing value" -Method "POST" -Path "/set?key=only" -ExpectedStatus 400
Invoke-Case -Name "CMD PING" -Method "POST" -Path "/cmd" -ExpectedStatus 200 -ExpectedContains "PONG" -Body "PING"

$ttlKey = "$key-ttl"
Invoke-Case -Name "Set TTL key" -Method "POST" -Path "/set?key=$ttlKey&value=temp&ttl_ms=$TtlMs" -ExpectedStatus 200
Invoke-Case -Name "Get TTL key before expiry" -Method "GET" -Path "/get?key=$ttlKey" -ExpectedStatus 200 -ExpectedContains '"value":"temp"'
Start-Sleep -Milliseconds ($TtlMs + 1000)
Invoke-Case -Name "Get TTL key after expiry" -Method "GET" -Path "/get?key=$ttlKey" -ExpectedStatus 404

Write-Host ""
Write-Host "========== HTTP Instance Validation Summary =========="
$results | Format-Table -AutoSize

$total = $results.Count
$passed = ($results | Where-Object { $_.Pass }).Count
Write-Host ""
Write-Host "Passed: $passed/$total"

if ($passed -eq $total) {
    Write-Host "OVERALL: PASS" -ForegroundColor Green
    exit 0
}

Write-Host "OVERALL: FAIL" -ForegroundColor Red
exit 1
