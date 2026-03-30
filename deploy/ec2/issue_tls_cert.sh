#!/usr/bin/env bash
set -euo pipefail

APP_NAME="mini-redis-cache"
BASE_DIR="/opt/${APP_NAME}"
CURRENT_LINK="${BASE_DIR}/current"

if [[ ! -d "${CURRENT_LINK}" ]]; then
  echo "Current release directory not found at ${CURRENT_LINK}"
  exit 1
fi

cd "${CURRENT_LINK}"

if [[ -f ".env" ]]; then
  set -a
  # shellcheck disable=SC1091
  source .env
  set +a
fi

TLS_DOMAIN="${TLS_DOMAIN:-${1:-}}"
TLS_EMAIL="${TLS_EMAIL:-${2:-}}"
CERTBOT_CONF_DIR="${CERTBOT_CONF_DIR:-/opt/${APP_NAME}/shared/certbot/conf}"
CERTBOT_WWW_DIR="${CERTBOT_WWW_DIR:-/opt/${APP_NAME}/shared/certbot/www}"

if [[ -z "${TLS_DOMAIN}" ]]; then
  echo "Usage: ./deploy/ec2/issue_tls_cert.sh <domain> <email>"
  echo "You can also set TLS_DOMAIN and TLS_EMAIL in environment or .env"
  exit 1
fi

if [[ -z "${TLS_EMAIL}" ]]; then
  echo "Email is required for Let's Encrypt registration"
  echo "Usage: ./deploy/ec2/issue_tls_cert.sh <domain> <email>"
  exit 1
fi

mkdir -p "${CERTBOT_CONF_DIR}" "${CERTBOT_WWW_DIR}"

echo "Starting nginx on HTTP for ACME challenge"
docker compose up -d cache_http nginx

echo "Requesting certificate for ${TLS_DOMAIN}"
docker run --rm \
  -v "${CERTBOT_CONF_DIR}:/etc/letsencrypt" \
  -v "${CERTBOT_WWW_DIR}:/var/www/certbot" \
  certbot/certbot:v2.11.0 certonly \
  --webroot -w /var/www/certbot \
  -d "${TLS_DOMAIN}" \
  --email "${TLS_EMAIL}" \
  --agree-tos \
  --no-eff-email \
  --non-interactive \
  --keep-until-expiring

if [[ -f "deploy/nginx/default.https.conf.template" ]]; then
  sed "s|__TLS_DOMAIN__|${TLS_DOMAIN}|g" deploy/nginx/default.https.conf.template > deploy/nginx/default.conf
fi

echo "Reloading nginx with HTTPS config"
docker compose up -d nginx

echo "Certificate setup complete"
echo "Verify with:"
echo "  curl -fsS http://${TLS_DOMAIN}/health"
echo "  curl -fsS https://${TLS_DOMAIN}/health"
