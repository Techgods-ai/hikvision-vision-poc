#!/bin/bash
# Arrête le service SDK et l'interface web.
set -uo pipefail
WEB_PORT=${WEB_PORT:-8080}
docker rm -f hik-sdk >/dev/null 2>&1 && echo "service SDK arrêté" || echo "service SDK déjà arrêté"
pkill -f "http.server $WEB_PORT" 2>/dev/null && echo "interface web arrêtée" || echo "interface web déjà arrêtée"
