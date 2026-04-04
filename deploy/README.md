# EC2 Deployment Guide

This project ships with a Docker + Nginx + GitHub Actions deployment path for AWS EC2.

## 1) EC2 Prerequisites

- Ubuntu 22.04 instance
- Security group inbound rules:
  - 22 (SSH) from your IP
  - 80 (HTTP) from 0.0.0.0/0
- Optional domain + Route53 A record to EC2 Elastic IP

## 2) Bootstrap Server

Copy and run:

```bash
chmod +x deploy/ec2/bootstrap.sh
./deploy/ec2/bootstrap.sh
```

Log out and back in so docker group permissions apply.

## 3) Install systemd unit

```bash
sudo cp deploy/systemd/mini-redis.service /etc/systemd/system/mini-redis.service
sudo systemctl daemon-reload
sudo systemctl enable mini-redis.service
```

## 4) Local manual deploy test (without Actions)

Build and push an image to GHCR first, then create `.env`:

```bash
GHCR_OWNER=<your-ghcr-owner>
GHCR_IMAGE=mini-redis-cache
IMAGE_TAG=<tag>
```

Create bundle and deploy:

```bash
tar -czf /tmp/mini-redis-release.tar.gz docker-compose.yml deploy .env
chmod +x deploy/ec2/deploy.sh
HEALTH_URL=http://127.0.0.1/health ./deploy/ec2/deploy.sh /tmp/mini-redis-release.tar.gz
```

## 5) GitHub Actions Secrets Required

Add these repository secrets:

- `EC2_HOST`: EC2 public IP or DNS
- `EC2_USER`: SSH user, usually `ubuntu`
- `EC2_SSH_KEY`: private key content for the EC2 key pair
- `GHCR_USERNAME` (optional): username for GHCR pull auth on EC2
- `GHCR_TOKEN` (optional): GHCR token with package read permission
- `TLS_DOMAIN` (optional): domain to enable HTTPS automatically once cert files exist

## 6) CI/CD Flow

Workflow file: `.github/workflows/deploy-ec2.yml`

- Builds and tests project
- Builds and pushes Docker image to GHCR
- Packages deployment bundle
- Copies bundle to EC2
- Runs `deploy.sh` on EC2
- Auto-rolls back if health check fails

## 7) Verify Deployment

```bash
curl -fsS http://<ec2-ip>/health
curl -X POST "http://<ec2-ip>/set?key=user:1&value=alice&ttl_ms=5000"
curl "http://<ec2-ip>/get?key=user:1"
```

Cache persistence location (survives container recreation and deployment rollouts):
- `/opt/mini-redis-cache/shared/cache-data`

Quick persistence check:

```bash
curl -X POST "http://<ec2-ip>/set?key=1&value=A"
curl "http://<ec2-ip>/get?key=1"
```

After restarting EC2 or rerunning deploy, `GET key=1` should still return `A`.

## 8) Optional HTTPS

This repo now supports HTTPS with Let's Encrypt while keeping deployment health checks on HTTP.

1. Point your DNS `A` record to your EC2 Elastic IP.
2. Set `TLS_DOMAIN` in GitHub Actions secrets (for example `api.example.com`).
3. Deploy once so HTTP stack is live.
4. SSH into EC2 and run:

```bash
cd /opt/mini-redis-cache/current
chmod +x deploy/ec2/issue_tls_cert.sh
./deploy/ec2/issue_tls_cert.sh <your-domain> <your-email>
```

5. Re-run the deploy workflow. `deploy.sh` will detect cert files and auto-render HTTPS nginx config.

Certificate storage is persistent across releases:
- `/opt/mini-redis-cache/shared/certbot/conf`
- `/opt/mini-redis-cache/shared/certbot/www`

If certificate files are missing, deployment safely falls back to HTTP so rollback/health checks continue to work.
