#!/usr/bin/env bash
set -euo pipefail

APP_NAME="mini-redis-cache"
BASE_DIR="/opt/${APP_NAME}"
RELEASES_DIR="${BASE_DIR}/releases"
CURRENT_LINK="${BASE_DIR}/current"
BACKUP_LINK="${BASE_DIR}/previous"
TIMESTAMP="$(date +%Y%m%d%H%M%S)"
NEW_RELEASE_DIR="${RELEASES_DIR}/${TIMESTAMP}"
PACKAGE_PATH="${1:-/tmp/mini-redis-release.tar.gz}"
HEALTH_URL="${HEALTH_URL:-http://127.0.0.1/health}"
HEALTH_RETRIES="${HEALTH_RETRIES:-15}"
TLS_DOMAIN="${TLS_DOMAIN:-}"
CERTBOT_CONF_DIR="${CERTBOT_CONF_DIR:-/opt/${APP_NAME}/shared/certbot/conf}"
CERTBOT_WWW_DIR="${CERTBOT_WWW_DIR:-/opt/${APP_NAME}/shared/certbot/www}"

is_docker_port_80_listener_only() {
  local listeners="${1:-}"

  if [[ -z "${listeners}" ]]; then
    return 1
  fi

  if ! echo "${listeners}" | grep -qiE 'docker-proxy|docker-pr'; then
    return 1
  fi

  if echo "${listeners}" | grep -qviE 'docker-proxy|docker-pr'; then
    return 1
  fi

  return 0
}

ensure_port_80_available() {
  local listeners=""

  if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet nginx; then
    echo "Host nginx is active on this machine. Stopping it so containerized nginx can use port 80."
    sudo systemctl stop nginx || true
    sudo systemctl disable nginx || true
  fi

  if command -v lsof >/dev/null 2>&1; then
    listeners="$(lsof -iTCP:80 -sTCP:LISTEN -n -P 2>/dev/null || true)"
    if [[ -n "${listeners}" ]]; then
      if is_docker_port_80_listener_only "${listeners}"; then
        echo "Port 80 is currently owned by an existing Docker proxy; proceeding with rolling compose update."
        return 0
      fi

      echo "Port 80 is already in use by a non-Docker process."
      echo "${listeners}"
      return 1
    fi
  elif command -v ss >/dev/null 2>&1; then
    listeners="$(ss -ltnp sport = :80 2>/dev/null | tail -n +2 || true)"
    if [[ -n "${listeners}" ]]; then
      if is_docker_port_80_listener_only "${listeners}"; then
        echo "Port 80 is currently owned by an existing Docker proxy; proceeding with rolling compose update."
        return 0
      fi

      echo "Port 80 is already in use by a non-Docker process."
      ss -ltnp sport = :80 || true
      return 1
    fi
  fi
}

render_nginx_config() {
  local nginx_dir="${CURRENT_LINK}/deploy/nginx"
  local active_conf="${nginx_dir}/default.conf"
  local http_conf="${nginx_dir}/default.http.conf"
  local https_template="${nginx_dir}/default.https.conf.template"

  mkdir -p "${CERTBOT_CONF_DIR}" "${CERTBOT_WWW_DIR}"

  if [[ -n "${TLS_DOMAIN}" ]] && \
     [[ -f "${CERTBOT_CONF_DIR}/live/${TLS_DOMAIN}/fullchain.pem" ]] && \
     [[ -f "${CERTBOT_CONF_DIR}/live/${TLS_DOMAIN}/privkey.pem" ]]; then
    echo "TLS certificate detected for ${TLS_DOMAIN}; enabling HTTPS nginx config"
    sed "s|__TLS_DOMAIN__|${TLS_DOMAIN}|g" "${https_template}" > "${active_conf}"
    return 0
  fi

  if [[ -n "${TLS_DOMAIN}" ]]; then
    echo "TLS_DOMAIN is set but certificate files were not found for ${TLS_DOMAIN}; serving HTTP until certs exist"
  fi

  cp "${http_conf}" "${active_conf}"
}

rollback() {
  echo "Deployment failed. Rolling back..."

  if [[ -L "${BACKUP_LINK}" ]]; then
    PREVIOUS_TARGET="$(readlink -f "${BACKUP_LINK}")"
    ln -sfn "${PREVIOUS_TARGET}" "${CURRENT_LINK}"
    cd "${CURRENT_LINK}"
    docker compose pull || true
    docker compose up -d || true
    echo "Rollback complete: ${PREVIOUS_TARGET}"
  else
    echo "No previous release found. Manual recovery required."
  fi
}

trap rollback ERR

sudo mkdir -p "${RELEASES_DIR}"
sudo chown -R "$USER:$USER" "${BASE_DIR}"

mkdir -p "${NEW_RELEASE_DIR}"
tar -xzf "${PACKAGE_PATH}" -C "${NEW_RELEASE_DIR}"

if [[ -L "${CURRENT_LINK}" ]]; then
  PREV_TARGET="$(readlink -f "${CURRENT_LINK}")"
  ln -sfn "${PREV_TARGET}" "${BACKUP_LINK}"
fi

ln -sfn "${NEW_RELEASE_DIR}" "${CURRENT_LINK}"

cd "${CURRENT_LINK}"

if [[ -f ".env" ]]; then
  set -a
  # shellcheck disable=SC1091
  source .env
  set +a
fi

render_nginx_config

ensure_port_80_available

docker compose pull

docker compose up -d --remove-orphans

echo "Checking health at ${HEALTH_URL}"
for _ in $(seq 1 "${HEALTH_RETRIES}"); do
  if curl -fsS "${HEALTH_URL}" >/dev/null; then
    echo "Deployment successful"
    trap - ERR
    exit 0
  fi
  sleep 2
done

echo "Health check failed"
exit 1
