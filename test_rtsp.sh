#!/bin/bash
# Teste la connectivité d'un flux RTSP avec ffprobe (sans rien lire d'autre).
# Usage : ./test_rtsp.sh "rtsp://user:pass@IP:554/Streaming/Channels/101"
URL="$1"
if [ -z "$URL" ]; then
  echo "Usage : ./test_rtsp.sh <rtsp_url>"
  exit 1
fi
ffprobe -v error -rtsp_transport tcp \
  -show_entries stream=codec_type,codec_name,width,height \
  -of default=noprint_wrappers=1 "$URL"
