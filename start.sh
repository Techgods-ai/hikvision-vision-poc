#!/bin/bash
# Démarre le service SDK Hikvision et l'interface web.
#   ./start.sh          -> lit les identifiants depuis .env
#   ./stop.sh           -> arrête tout
set -euo pipefail
cd "$(dirname "$0")"

SDK_PORT=${SDK_PORT:-8090}
WEB_PORT=${WEB_PORT:-8080}

[ -f .env ] || { echo "Manquant : .env (copier .env.example et remplir)"; exit 1; }
set -a; . ./.env; set +a
: "${HIK_HOST:?}" "${HIK_USER:?}" "${HIK_PASS:?}"
HIK_PORTS=${HIK_PORTS:-8001,8000,7000}

command -v docker >/dev/null || { echo "docker introuvable"; exit 1; }
docker info >/dev/null 2>&1 || { echo "Démarrage de colima..."; colima start --cpu 2 --memory 4; }

echo "Construction de l'image SDK..."
docker build --platform linux/amd64 -q -t hik-sdk ./docker >/dev/null

echo "Service SDK sur le port $SDK_PORT..."
docker rm -f hik-sdk >/dev/null 2>&1 || true
docker run -d --name hik-sdk --restart unless-stopped --platform linux/amd64 \
  -p "$SDK_PORT:8090" \
  -e HIK_HOST="$HIK_HOST" -e HIK_USER="$HIK_USER" -e HIK_PASS="$HIK_PASS" \
  -e HIK_PORTS="$HIK_PORTS" \
  hik-sdk >/dev/null

echo "Interface web sur le port $WEB_PORT..."
pkill -f "http.server $WEB_PORT" 2>/dev/null || true
sleep 1
nohup python3 -m http.server "$WEB_PORT" --bind 127.0.0.1 --directory web \
  >/tmp/hik-web.log 2>&1 </dev/null &
disown

for i in $(seq 20); do
  curl -sf -m 3 "http://localhost:$SDK_PORT/api/nvrs" >/dev/null 2>&1 && break
  sleep 1
done

echo
curl -s -m 10 "http://localhost:$SDK_PORT/api/nvrs" |
  python3 -c '
import json, sys
for n in json.load(sys.stdin)["nvrs"]:
    etat = "en ligne" if n["online"] else "hors ligne"
    print("  {:22} port {}  {:2} canaux  {}".format(n["model"], n["port"], n["channels"], etat))
' || echo "  (service SDK pas encore prêt, voir: docker logs hik-sdk)"

echo
echo "Interface : http://localhost:$WEB_PORT"
