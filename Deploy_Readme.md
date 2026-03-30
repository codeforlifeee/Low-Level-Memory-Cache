# Deploy Readme: Step-by-Step EC2 Deployment Guide

This guide is written so you can deploy the project on AWS EC2 with minimum confusion.

Deployment model used:
- GitHub Actions builds/tests the project.
- GitHub Actions builds and pushes a Docker image to GHCR.
- GitHub Actions copies a deployment bundle to EC2.
- EC2 runs a rollback-safe deployment script.
- Nginx exposes the API on port 80.

---

## 0. What You Will Deploy

Services deployed on EC2:
1. `cache_http` container (your C++ HTTP cache server)
2. `nginx` container (reverse proxy)

Endpoints after deployment:
- `GET /health`
- `POST /set?key=<k>&value=<v>&ttl_ms=<n>`
- `GET /get?key=<k>`
- `DELETE /del?key=<k>`
- `GET /stats`
- `POST /cmd` (Redis-like command in body)

---

## 1. Prerequisites Checklist

Before you start, confirm all of these:

1. GitHub repository contains this project.
2. You can push to `main` (or `master`) branch.
3. AWS account is active.
4. You have an SSH key pair (`.pem`) for EC2 login.
5. Your repository already contains these files:
   - `Dockerfile`
   - `docker-compose.yml`
   - `.github/workflows/deploy-ec2.yml`
   - `deploy/ec2/bootstrap.sh`
   - `deploy/ec2/deploy.sh`
   - `deploy/nginx/default.conf`
   - `deploy/systemd/mini-redis.service`

---

## 2. Create EC2 Instance (AWS Console)

1. Open AWS Console -> EC2 -> Instances -> Launch Instance.
2. Name: `mini-redis-cache-prod`.
3. AMI: Ubuntu Server 22.04 LTS.
4. Instance type: `t3.small` (good start) or `t3.medium` (better for benchmarking).
5. Key pair:
   - Select existing key pair, or create new key pair and download `.pem`.
6. Network settings:
   - Allow SSH (port 22) from your IP for local terminal access.
   - If you use GitHub-hosted runners for deploy, port 22 must also be reachable from the runner. For first deploy, temporarily allow `0.0.0.0/0` and keep key-based auth only.
   - Safer long-term option: use a self-hosted runner (inside your VPC) and then lock SSH back to trusted sources.
   - Add HTTP (port 80) from Anywhere (`0.0.0.0/0`).
   - Do not open port 8080 publicly (not required).
7. Storage: 20 GB gp3 is enough.
8. Launch instance.

After launch:
1. Allocate an Elastic IP.
2. Associate Elastic IP to this instance.
3. Copy the Elastic IP; you need it later.

---

## 3. Connect to EC2

From your local terminal:

```bash
chmod 400 /path/to/your-key.pem
ssh -i /path/to/your-key.pem ubuntu@<EC2_PUBLIC_IP>
```

Replace `<EC2_PUBLIC_IP>` with your Elastic IP.

---

## 4. Bootstrap EC2 (Install Docker + Dependencies)

You need project files on EC2 first. Use one of these:

Option A (recommended): clone repo
```bash
git clone https://github.com/<your-user>/<your-repo>.git
cd <your-repo>
```

Option B: upload folder manually.

Then run bootstrap:

```bash
chmod +x deploy/ec2/bootstrap.sh
./deploy/ec2/bootstrap.sh
```

Important:
- Log out and log in again so docker group permissions apply.

Reconnect:

```bash
ssh -i /path/to/your-key.pem ubuntu@<EC2_PUBLIC_IP>
cd <your-repo>
```

---

## 5. Install systemd Service (Auto Start on Reboot)

Run on EC2:

```bash
sudo cp deploy/systemd/mini-redis.service /etc/systemd/system/mini-redis.service
sudo systemctl daemon-reload
sudo systemctl enable mini-redis.service
```

Do not start now manually; deployment script will handle compose startup.

---

## 6. Configure GitHub Secrets (Critical)

Go to GitHub -> Your Repo -> Settings -> Secrets and variables -> Actions -> New repository secret.

Add these required secrets:

1. `EC2_HOST`
   - Value: your EC2 public IP or public DNS only
   - Do not include protocol like `http://` or `https://`
   - Do not include path or trailing slash
2. `EC2_USER`
   - Value: `ubuntu`
3. `EC2_SSH_KEY`
   - Value: full private key content (the `.pem` content)

Optional:
4. `EC2_PORT`
   - Value: `22` (or your custom SSH port)

Add these optional secrets (needed if GHCR package is private):

5. `GHCR_USERNAME`
   - Your GitHub username
6. `GHCR_TOKEN`
   - A token with package read permission

If image pull fails on EC2, set optional GHCR secrets immediately.

---

## 7. Verify GitHub Actions Permissions

In your repo:
1. Settings -> Actions -> General
2. Workflow permissions: choose `Read and write permissions`
3. Save

This is needed for image push and artifact handling.

---

## 8. Trigger First Deployment

Method A: push to `main`

```bash
git add .
git commit -m "Setup EC2 deployment"
git push origin main
```

Method B: manual run
1. GitHub -> Actions
2. Select workflow `deploy-ec2`
3. Click `Run workflow`

---

## 9. Monitor Deployment Workflow

In GitHub Actions, verify these jobs pass:

1. `build-test-package`
   - configure
   - build
   - tests
   - docker image build+push
   - artifact upload

2. `deploy`
   - artifact download
   - copy bundle to EC2
   - run remote deploy script
   - verify endpoint

If any step fails, open logs and follow troubleshooting section below.

---

## 10. Validate Deployed Service

From your local machine:

```bash
curl -fsS http://<EC2_PUBLIC_IP>/health
curl -X POST "http://<EC2_PUBLIC_IP>/set?key=user:1&value=alice&ttl_ms=5000"
curl "http://<EC2_PUBLIC_IP>/get?key=user:1"
curl "http://<EC2_PUBLIC_IP>/stats"
```

Expected behavior:
- `/health` returns success JSON.
- `/set` returns OK.
- `/get` returns stored value.
- `/stats` returns hit/miss and memory counters.

---

## 11. Understand Rollback (Already Built-In)

The script `deploy/ec2/deploy.sh` does this automatically:

1. Creates a new release folder: `/opt/mini-redis-cache/releases/<timestamp>`
2. Updates `current` symlink to new release
3. Pulls and starts containers
4. Runs health check
5. If health check fails:
   - Reverts `current` symlink to previous release
   - Restarts previous containers

So rollback is automatic for failed deploys.

---

## 12. Useful EC2 Operations

Check running containers:

```bash
docker ps
```

See compose status:

```bash
cd /opt/mini-redis-cache/current
docker compose ps
```

View logs:

```bash
cd /opt/mini-redis-cache/current
docker compose logs --tail=200
```

Restart stack:

```bash
cd /opt/mini-redis-cache/current
docker compose up -d --remove-orphans
```

Check service status:

```bash
sudo systemctl status mini-redis.service
```

---

## 13. Troubleshooting Guide

### Problem A: SSH connection fails

Checks:
1. Security group allows port 22 from your IP.
2. Correct username (`ubuntu` for Ubuntu AMI).
3. Correct `.pem` key used.
4. `chmod 400` set on key file.

### Problem B: GitHub deploy step cannot connect to EC2

Checks:
1. `EC2_HOST` secret correct.
   - Must be host only (for example `1.2.3.4` or `ec2-xx-xx-xx-xx.compute-1.amazonaws.com`), not a URL.
2. `EC2_USER` is `ubuntu`.
3. `EC2_SSH_KEY` has full key content including BEGIN/END lines.
4. Security group inbound rule allows SSH from GitHub runner path (if hosted runner, `0.0.0.0/0` is the usual first-debug setting).
5. NACL/firewall is not blocking port 22.

Typical error:
- `dial tcp <host>:22: i/o timeout` means the runner cannot reach EC2 over network (not an SSH key format issue).

Fast fix path:
1. Temporarily open inbound 22 to `0.0.0.0/0`.
2. Re-run deploy workflow.
3. After success, move to self-hosted runner or tighten allowed sources for SSH.

### Problem C: Docker image pull fails on EC2

Cause:
- GHCR package is private and EC2 is not logged in.

Fix:
1. Add `GHCR_USERNAME` and `GHCR_TOKEN` secrets.
2. Rerun workflow.

### Problem D: Health check fails after deploy

Checks on EC2:

```bash
cd /opt/mini-redis-cache/current
docker compose logs --tail=200 cache_http
docker compose logs --tail=200 nginx
curl -v http://127.0.0.1/health
```

Then fix config and redeploy.

Important:
- Keep `deploy/nginx/default.conf` HTTP-first for CI health checks.
- Do not switch to HTTPS-only config before certificates are available under `/opt/mini-redis-cache/shared/certbot/conf/live/<domain>/`.

### Problem E: Deploy fails with `address already in use` on `0.0.0.0:80`

Cause:
- Host nginx (installed on EC2 OS) is already listening on port 80, so docker nginx cannot bind it.

One-time recovery on EC2:

```bash
sudo systemctl stop nginx
sudo systemctl disable nginx
sudo lsof -iTCP:80 -sTCP:LISTEN -n -P
```

Then rerun GitHub Actions deploy.

Notes:
- New bootstrap/deploy scripts now handle this automatically, but existing instances may need this one-time cleanup.

### Problem F: Port 80 not reachable publicly

Checks:
1. EC2 security group has inbound HTTP 80 from 0.0.0.0/0.
2. Nginx container is running.
3. You are calling the correct public IP.

---

## 14. Optional Domain + HTTPS (Later)

After HTTP deployment is stable:

1. Create an `A` record from your domain to EC2 Elastic IP.
2. Add GitHub secret `TLS_DOMAIN` with your domain.
3. Deploy once (keeps HTTP active).
4. SSH to EC2 and run:

```bash
cd /opt/mini-redis-cache/current
chmod +x deploy/ec2/issue_tls_cert.sh
./deploy/ec2/issue_tls_cert.sh <your-domain> <your-email>
```

5. Re-run GitHub Actions deploy. The release script auto-detects cert files and enables HTTPS config.

Persistent cert paths used by compose:
- `/opt/mini-redis-cache/shared/certbot/conf`
- `/opt/mini-redis-cache/shared/certbot/www`

---

## 15. Resume Proof Checklist

After deployment, collect these proofs:

1. Public endpoint screenshot (`/health`, `/stats`).
2. GitHub Actions successful workflow screenshot.
3. Benchmark output from cloud machine.
4. Short architecture diagram (GitHub -> GHCR -> EC2 -> Nginx -> Cache service).

Use quantified bullets in resume:
- deployed on EC2
- CI/CD automated
- rollback-safe releases
- measured throughput and P99 latency

---

## 16. Final Quick Runbook (Short Version)

1. Launch EC2 + Elastic IP + security group (80 public, and 22 reachable from wherever your deploy runner connects). 
2. SSH into EC2.
3. Clone repo.
4. Run `deploy/ec2/bootstrap.sh`.
5. Re-login.
6. Enable systemd service.
7. Add GitHub secrets (`EC2_HOST`, `EC2_USER`, `EC2_SSH_KEY`, optional GHCR secrets).
8. Trigger `deploy-ec2` workflow.
9. Verify with curl on public IP.
10. If failure occurs, inspect logs and rerun.

You are now deployed with CI/CD and automatic rollback.
