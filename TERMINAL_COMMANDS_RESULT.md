# Terminal Commands Result: End-to-End Command Guide

This file gives you exact commands to run, where to run them, and what output to collect.

## 1. Where To Run Commands

Use these terminal contexts correctly:

1. Local terminal (VS Code terminal on your laptop):
- Path: project root folder
- Example path: `C:\Users\LENOVO\Desktop\LLMC`

2. EC2 terminal (SSH session inside AWS instance):
- Example user/path: `ubuntu@<EC2_IP>` and `/opt/mini-redis-cache/current`

If you run commands in the wrong place, results will fail.

---

## 2. Local Setup Commands (Windows, VS Code Terminal)

### 2.1 Go to project root

```powershell
cd C:\Users\LENOVO\Desktop\LLMC
```

### 2.2 Verify core tools are visible

```powershell
cmake --version
```

```powershell
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
& $vswhere -all -products * -format json
```

Expected:
- CMake version shown
- Visual Studio instance shown in JSON

### 2.3 Run benchmark script (auto-detects correct generator)

```powershell
./scripts/run_benchmark.ps1
```

Expected:
- It tries generators
- Selects Visual Studio 18 2026 on your machine
- Builds and runs benchmark
- Prints sections for balanced, read_heavy, write_heavy

---

## 3. Local Build/Test Commands (Manual, No Script)

If you want full manual control:

### 3.1 Configure (Visual Studio 2026)

```powershell
cmake -S . -B build -G "Visual Studio 18 2026"
```

### 3.2 Build tests + benchmark (Release)

```powershell
cmake --build build --config Release --parallel
```

### 3.3 Run tests

```powershell
ctest --test-dir build -C Release --output-on-failure
```

### 3.4 Run benchmark

```powershell
./build/Release/cache_benchmark.exe
```

---

## 4. Capture Benchmark Output Properly

### 4.1 Save full benchmark output to file

```powershell
./build/Release/cache_benchmark.exe | Tee-Object -FilePath .\benchmark_output.txt
```

This creates `benchmark_output.txt` in project root.

### 4.2 What numbers to extract

For each profile (balanced/read_heavy/write_heavy), collect:
1. single_lock throughput and p99
2. best shard throughput and p99
3. printed "Resume bullet" line

### 4.3 Optional quick grep for resume lines

```powershell
Select-String -Path .\benchmark_output.txt -Pattern "Resume bullet"
```

---

## 5. Docker Image Commands (Local)

Run from project root.

### 5.1 Build image

```powershell
docker build -t mini-redis-cache:local .
```

### 5.2 Run image directly

```powershell
docker run --rm -p 8080:8080 mini-redis-cache:local
```

### 5.3 Test endpoints from another terminal

```powershell
curl.exe "http://127.0.0.1:8080/health"
```

```powershell
curl.exe -X POST "http://127.0.0.1:8080/set?key=user:1&value=alice&ttl_ms=5000"
```

```powershell
curl.exe "http://127.0.0.1:8080/get?key=user:1"
```

---

## 6. EC2 First-Time Provision Commands

Run these in EC2 terminal after SSH login.

### 6.1 Connect to EC2

```bash
ssh -i /path/to/key.pem ubuntu@<EC2_PUBLIC_IP>
```

### 6.2 Clone repo

```bash
git clone https://github.com/<your-user>/<your-repo>.git
cd <your-repo>
```

### 6.3 Bootstrap Docker + dependencies

```bash
chmod +x deploy/ec2/bootstrap.sh
./deploy/ec2/bootstrap.sh
```

Logout and login again:

```bash
exit
ssh -i /path/to/key.pem ubuntu@<EC2_PUBLIC_IP>
```

### 6.4 Install systemd unit

```bash
cd <your-repo>
sudo cp deploy/systemd/mini-redis.service /etc/systemd/system/mini-redis.service
sudo systemctl daemon-reload
sudo systemctl enable mini-redis.service
```

---

## 7. EC2 Manual Deploy Commands (Without GitHub Actions)

Run in EC2 terminal.

### 7.1 Prepare runtime env file

```bash
cat > .env <<EOF
GHCR_OWNER=<your-ghcr-owner>
GHCR_IMAGE=mini-redis-cache
IMAGE_TAG=<image-tag>
EOF
```

### 7.2 Create deployment package

```bash
tar -czf /tmp/mini-redis-release.tar.gz docker-compose.yml deploy .env
```

### 7.3 Execute deploy script

```bash
chmod +x deploy/ec2/deploy.sh
HEALTH_URL=http://127.0.0.1/health ./deploy/ec2/deploy.sh /tmp/mini-redis-release.tar.gz
```

Expected:
- containers pulled and started
- health check passed
- release activated

---

## 8. EC2 Runtime Verification Commands

Run in EC2 terminal:

```bash
curl -fsS http://127.0.0.1/health
```

```bash
curl -X POST "http://127.0.0.1/set?key=demo&value=ok&ttl_ms=5000"
```

```bash
curl "http://127.0.0.1/get?key=demo"
```

From your local machine (public check):

```bash
curl -fsS http://<EC2_PUBLIC_IP>/health
```

---

## 9. Useful Operational Commands (EC2)

### 9.1 Check containers

```bash
docker ps
```

### 9.2 Check compose status

```bash
cd /opt/mini-redis-cache/current
docker compose ps
```

### 9.3 Tail logs

```bash
cd /opt/mini-redis-cache/current
docker compose logs --tail=200
```

### 9.4 Restart stack

```bash
cd /opt/mini-redis-cache/current
docker compose up -d --remove-orphans
```

### 9.5 Check systemd status

```bash
sudo systemctl status mini-redis.service
```

---

## 10. TCP Server (Redis-like Commands over Raw TCP)

This section is for your new TCP server executable.

### 10.1 Build TCP server target (Local, Windows)

Run in local VS Code terminal at project root:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026"
cmake --build build --config Release --target cache_tcp_server
```

Expected:
- `cache_tcp_server.exe` created in `build\Release\`

### 10.2 Start TCP server

```powershell
.\build\Release\cache_tcp_server.exe 6379 50000
```

Arguments:
1. `6379` = TCP port
2. `50000` = cache capacity

Expected startup output:
- `Mini Redis TCP server listening on port 6379`
- `Supported commands: SET key value [ttl_ms], GET key, DEL key, STATS, PING, QUIT`

Keep this terminal open while testing clients.

### 10.3 Quick Test with PowerShell TCP client (Copy-Paste)

Open a second local terminal and run:

```powershell
$client = New-Object System.Net.Sockets.TcpClient("127.0.0.1", 6379)
$stream = $client.GetStream()
$writer = New-Object System.IO.StreamWriter($stream)
$writer.NewLine = "`n"
$writer.AutoFlush = $true
$reader = New-Object System.IO.StreamReader($stream)

# Greeting
$reader.ReadLine()

# Commands
$writer.WriteLine("SET user:1 Alice 3000")
$reader.ReadLine()

$writer.WriteLine("GET user:1")
$reader.ReadLine()

$writer.WriteLine("DEL user:1")
$reader.ReadLine()

$writer.WriteLine("GET user:1")
$reader.ReadLine()

$writer.WriteLine("STATS")
$reader.ReadLine()

$writer.WriteLine("QUIT")
$reader.ReadLine()

$client.Close()
```

Expected response pattern:
1. greeting: `OK mini-redis tcp server ready`
2. `SET ...` -> `OK`
3. `GET ...` -> `Alice`
4. `DEL ...` -> `OK`
5. `GET ...` after delete -> `(nil)`
6. `STATS` -> starts with `OK hits=...`
7. `QUIT` -> `OK`

### 10.4 Interactive Manual Test (Telnet)

If Telnet is installed:

```powershell
telnet 127.0.0.1 6379
```

Then type one command per line:

```text
PING
SET user:2 Bob 5000
GET user:2
STATS
QUIT
```

### 10.5 Interactive Manual Test (Netcat)

If `nc`/`ncat` is available:

```bash
nc 127.0.0.1 6379
```

Then type same commands as above.

### 10.6 Command Format Reference

Supported commands:

1. `PING`
2. `SET <key> <value> [ttl_ms]`
3. `GET <key>`
4. `DEL <key>`
5. `STATS`
6. `HELP`
7. `QUIT` or `EXIT`

Simple response format:

1. success: `OK`
2. fetch hit: `<value>`
3. fetch miss: `(nil)`
4. invalid input: `ERROR ...`

### 10.7 Error Case Tests (Recommended)

Run these in PowerShell client or telnet:

```text
SET
GET
DEL
SET k v -1
SET k v not_number
UNKNOWN_CMD
```

Expected:
- each returns `ERROR ...` with helpful usage/message.

### 10.8 TTL Behavior Test

Use this PowerShell client snippet:

```powershell
$client = New-Object System.Net.Sockets.TcpClient("127.0.0.1", 6379)
$stream = $client.GetStream()
$writer = New-Object System.IO.StreamWriter($stream)
$writer.NewLine = "`n"
$writer.AutoFlush = $true
$reader = New-Object System.IO.StreamReader($stream)
$reader.ReadLine() | Out-Null

$writer.WriteLine("SET session token123 1000")
$reader.ReadLine()

$writer.WriteLine("GET session")
$reader.ReadLine()

Start-Sleep -Seconds 2

$writer.WriteLine("GET session")
$reader.ReadLine()

$writer.WriteLine("QUIT")
$reader.ReadLine() | Out-Null
$client.Close()
```

Expected:
1. first GET -> `token123`
2. second GET after wait -> `(nil)`

### 10.9 Save TCP Test Transcript for Resume Proof

Run this in a new terminal while server is running:

```powershell
$outFile = ".\tcp_test_output.txt"
"=== TCP Test Start $(Get-Date) ===" | Out-File -FilePath $outFile -Encoding utf8

$client = New-Object System.Net.Sockets.TcpClient("127.0.0.1", 6379)
$stream = $client.GetStream()
$writer = New-Object System.IO.StreamWriter($stream)
$writer.NewLine = "`n"
$writer.AutoFlush = $true
$reader = New-Object System.IO.StreamReader($stream)

$greet = $reader.ReadLine(); "GREETING: $greet" | Out-File -Append $outFile

$commands = @(
	"PING",
	"SET user:proof Alice 5000",
	"GET user:proof",
	"STATS",
	"DEL user:proof",
	"GET user:proof",
	"QUIT"
)

foreach($cmd in $commands){
	"CMD: $cmd" | Out-File -Append $outFile
	$writer.WriteLine($cmd)
	$resp = $reader.ReadLine()
	"RESP: $resp" | Out-File -Append $outFile
}

$client.Close()
"=== TCP Test End $(Get-Date) ===" | Out-File -Append $outFile

Get-Content $outFile
```

Generated file:
- `tcp_test_output.txt`

Use this file as implementation proof in your portfolio/repo docs.

### 10.10 Python Load Benchmark (1k / 10k / 100k)

You now have `benchmark.py` in project root to simulate concurrent clients and measure:

1. throughput (ops/sec)
2. average latency
3. P50, P95, P99 latency (microseconds)

Start TCP server first (Terminal A):

```powershell
.\build\Release\cache_tcp_server.exe 6379 50000
```

Run benchmark commands in another terminal (Terminal B):

1. 1,000 requests:

```powershell
python .\benchmark.py --host 127.0.0.1 --port 6379 --requests 1000 --concurrency 10 --mode ping
```

2. 10,000 requests:

```powershell
python .\benchmark.py --host 127.0.0.1 --port 6379 --requests 10000 --concurrency 20 --mode ping
```

3. 100,000 requests:

```powershell
python .\benchmark.py --host 127.0.0.1 --port 6379 --requests 100000 --concurrency 50 --mode ping
```

### 10.11 Realistic Mixed Workload (GET/SET/DEL)

Use 70% GET, 25% SET, 5% DEL:

```powershell
python .\benchmark.py --host 127.0.0.1 --port 6379 --requests 100000 --concurrency 50 --mode mixed --get-ratio 0.70 --set-ratio 0.25 --key-space 20000
```

Explanation:
1. Remaining 5% automatically becomes DEL.
2. `key-space` controls key cardinality (higher = more realistic cache behavior).

### 10.12 Save Benchmark Results to File

```powershell
python .\benchmark.py --host 127.0.0.1 --port 6379 --requests 100000 --concurrency 50 --mode mixed --get-ratio 0.70 --set-ratio 0.25 --key-space 20000 | Tee-Object -FilePath .\tcp_benchmark_output.txt
```

Generated file:
- `tcp_benchmark_output.txt`

Use this as performance proof in resume/project report.

---

## 11. GitHub Actions Trigger Commands

Local terminal:

```powershell
git add .
git commit -m "deploy setup"
git push origin main
```

This triggers:
- CI workflow
- Deploy workflow (if configured)

---

## 12. Required GitHub Secrets (No Terminal, GitHub UI)

Add in repository secrets:
1. `EC2_HOST`
2. `EC2_USER`
3. `EC2_SSH_KEY`
4. `GHCR_USERNAME` (optional if image private)
5. `GHCR_TOKEN` (optional if image private)

---

## 13. How To Obtain Resume Proof Properly

Collect these artifacts:

1. Benchmark file:
- command used: `./build/Release/cache_benchmark.exe | Tee-Object -FilePath .\benchmark_output.txt`
- output file: `benchmark_output.txt`

2. TCP feature proof:
- run transcript command from section 10.9
- output file: `tcp_test_output.txt`

3. Deployment proof:
- `curl http://<EC2_PUBLIC_IP>/health` output
- screenshot of successful GitHub Actions deploy run

4. Runtime proof:
- `docker compose ps`
- `docker compose logs --tail=100`

5. Optional stress proof:
- rerun benchmark on EC2 and save output to `ec2_benchmark_output.txt`

---

## 14. Fast Command Flow (Copy-Paste Sequence)

Local:

```powershell
cd C:\Users\LENOVO\Desktop\LLMC
cmake -S . -B build -G "Visual Studio 18 2026"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
./build/Release/cache_benchmark.exe | Tee-Object -FilePath .\benchmark_output.txt
```

EC2:

```bash
ssh -i /path/to/key.pem ubuntu@<EC2_PUBLIC_IP>
git clone https://github.com/<your-user>/<your-repo>.git
cd <your-repo>
chmod +x deploy/ec2/bootstrap.sh && ./deploy/ec2/bootstrap.sh
# relogin
sudo cp deploy/systemd/mini-redis.service /etc/systemd/system/mini-redis.service
sudo systemctl daemon-reload
sudo systemctl enable mini-redis.service
```

Then push to `main` to deploy via Actions.

---

## 15. 30-Second Interview Demo Script

Use this when interviewer asks: "Can you show it quickly?"

### 15.1 Terminal A (start server)

```powershell
cd C:\Users\LENOVO\Desktop\LLMC
cmake --build build --config Release --target cache_tcp_server
.\build\Release\cache_tcp_server.exe 6379 50000
```

### 15.2 Terminal B (8 command live demo)

```powershell
$c=New-Object System.Net.Sockets.TcpClient("127.0.0.1",6379);$s=$c.GetStream();$w=New-Object System.IO.StreamWriter($s);$w.NewLine="`n";$w.AutoFlush=$true;$r=New-Object System.IO.StreamReader($s)
$r.ReadLine()
$w.WriteLine("PING");$r.ReadLine()
$w.WriteLine("SET user:live Alice 3000");$r.ReadLine()
$w.WriteLine("GET user:live");$r.ReadLine()
Start-Sleep -Seconds 4
$w.WriteLine("GET user:live");$r.ReadLine()
$w.WriteLine("STATS");$r.ReadLine()
$w.WriteLine("QUIT");$r.ReadLine();$c.Close()
```

### 15.3 What to say while running

1. "PING confirms server responsiveness."
2. "SET stores key with 3-second TTL."
3. "GET returns value before expiry."
4. "After wait, GET returns (nil), proving TTL expiration."
5. "STATS confirms hit/miss tracking."

### 15.4 Expected key outputs

1. `OK mini-redis tcp server ready`
2. `OK`
3. `Alice`
4. `(nil)` after sleep
5. `OK hits=... misses=...`

---

## 16. Troubleshooting (Most Common)

### 14.1 Docker run error: `GLIBCXX_3.4.32 not found`

Meaning:
- Binary was built with newer GCC/libstdc++ than runtime image provides.

Fix used in this project:
- Runtime image aligned to `gcc:13-bookworm` in `Dockerfile`.

After pulling latest code, rebuild image:

```powershell
docker build -t mini-redis-cache:local .
docker run --rm -p 8080:8080 mini-redis-cache:local
```

### 14.2 PowerShell `curl` asks script-execution warning

Meaning:
- In PowerShell, `curl` can map to `Invoke-WebRequest` alias.

Fix:
- Use `curl.exe` explicitly in Windows terminal commands.

---

You now have a complete terminal-only command map for build, benchmark, deploy, verify, and proof collection.
