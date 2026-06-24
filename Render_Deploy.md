# Render Deployment Guide

This project supports automated deployment to [Render](https://render.com/) via the provided `render.yaml` Blueprint.
Deploying to Render is much simpler than AWS EC2 and doesn't require manual SSH, Nginx setup, or GitHub Actions for deployment, while providing a free/cheap tier.

## Deployment Steps

1. Push your code to a GitHub repository on the `render-deployment` branch (or merge it into `main`).
2. Log in to [Render's Dashboard](https://dashboard.render.com/).
3. Click **New** -> **Blueprint**.
4. Connect your GitHub account and select your repository.
5. Render will automatically detect the `render.yaml` file at the root of the repository.
6. Review the proposed service (named `low-level-memory-cache`) and click **Apply**.

## How it Works

- **Docker Built-in**: Render will automatically build the `Dockerfile` in the root of your project.
- **Port Binding**: The `render.yaml` specifies `PORT=8080`, and the `Dockerfile` exposes `8080`. The C++ application listens on `0.0.0.0:8080`. Render handles routing incoming HTTP traffic on port `80` or `443` down to your container's port `8080`.
- **Health Checks**: Render uses the `/health` endpoint configured in `render.yaml` for zero-downtime deployments.

## Testing your Deployment

Once the deployment finishes and the service status is **Live**, Render will provide you with a URL like `https://low-level-memory-cache-xxxx.onrender.com`.

You can test it just like the local environment:

```bash
# Check Health
curl -fsS https://low-level-memory-cache-xxxx.onrender.com/health

# Set a Value
curl -X POST "https://low-level-memory-cache-xxxx.onrender.com/set?key=user:1&value=alice&ttl_ms=5000"

# Get a Value
curl "https://low-level-memory-cache-xxxx.onrender.com/get?key=user:1"
```
