#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Visualiseur local de caméras RTSP (DVR/NVR) — stdlib uniquement, aucun pip.
Lit cameras.json, transcode chaque flux en MJPEG via ffmpeg, sert une grille web.

Usage :
    python3 viewer.py            -> http://localhost:8080
    python3 viewer.py 9090       -> autre port
"""

import http.server
import json
import os
import subprocess
import sys
import urllib.parse

BASE = os.path.dirname(os.path.abspath(__file__))
CONFIG = os.path.join(BASE, "cameras.json")
INDEX = os.path.join(BASE, "index.html")


def load_cameras():
    with open(CONFIG, "r", encoding="utf-8") as f:
        return json.load(f).get("cameras", [])


def find_camera(cam_id):
    for c in load_cameras():
        if c["id"] == cam_id:
            return c
    return None


class ViewerHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # silencieux

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        if path in ("/", "/index.html"):
            self.serve_index()
        elif path == "/api/cameras":
            self.serve_api()
        elif path.startswith("/stream/"):
            self.serve_stream(urllib.parse.unquote(path.split("/")[-1]))
        elif path.startswith("/snapshot/"):
            self.serve_snapshot(urllib.parse.unquote(path.split("/")[-1]))
        else:
            self.send_error(404)

    def serve_index(self):
        with open(INDEX, "rb") as f:
            body = f.read()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def serve_api(self):
        body = json.dumps({"cameras": load_cameras()}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _ffmpeg_cmd(self, rtsp, snapshot=False):
        cmd = [
            "ffmpeg", "-loglevel", "error",
            "-rtsp_transport", "tcp",
            "-i", rtsp,
            "-an",
            "-vf", "fps=10,scale=-2:720",
            "-c:v", "mjpeg",
            "-q:v", "6",
        ]
        if snapshot:
            cmd += ["-frames:v", "1", "-f", "image2", "pipe:1"]
        else:
            cmd += ["-f", "image2pipe", "pipe:1"]
        return cmd

    def serve_snapshot(self, cam_id):
        cam = find_camera(cam_id)
        if not cam:
            self.send_error(404)
            return
        proc = subprocess.Popen(
            self._ffmpeg_cmd(cam["rtsp"], snapshot=True),
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        )
        try:
            out, _ = proc.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
            self.send_error(504)
            return
        if proc.returncode != 0 or not out:
            self.send_error(502)
            return
        self.send_response(200)
        self.send_header("Content-Type", "image/jpeg")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def serve_stream(self, cam_id):
        cam = find_camera(cam_id)
        if not cam:
            self.send_error(404)
            return
        rtsp = cam.get("rtsp", "")
        if not rtsp or "user:pass@" in rtsp:
            # configuration non remplie
            self.send_error(503)
            return
        proc = subprocess.Popen(
            self._ffmpeg_cmd(rtsp),
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        )
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        try:
            buf = b""
            while True:
                data = proc.stdout.read(4096)
                if not data:
                    break
                buf += data
                while True:
                    start = buf.find(b"\xff\xd8")
                    if start == -1:
                        break
                    end = buf.find(b"\xff\xd9", start + 2)
                    if end == -1:
                        break
                    jpeg = buf[start:end + 2]
                    buf = buf[end + 2:]
                    self.wfile.write(
                        b"--frame\r\n"
                        b"Content-Type: image/jpeg\r\n"
                        b"Content-Length: " + str(len(jpeg)).encode() + b"\r\n\r\n"
                        + jpeg + b"\r\n"
                    )
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            proc.kill()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    server = http.server.ThreadingHTTPServer(("0.0.0.0", port), ViewerHandler)
    print(f"Visualiseur démarré : http://localhost:{port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nArrêt.")


if __name__ == "__main__":
    main()
