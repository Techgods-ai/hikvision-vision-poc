#!/usr/bin/env python3
"""Service d'analyse IA — détection de personnes sur les snapshots des caméras.

Poll les snapshots du service SDK (port 8090), détecte les personnes avec
YOLO, et expose les événements détectés en JSON sur le port 8091.

Endpoints:
  GET /events              -> derniers événements (JSON)
  GET /events/<id>/image   -> snapshot annoté de l'événement (JPEG)
  GET /stats               -> compteurs (précision, faux positifs)
  POST /events/<id>/label  -> validation humaine {"label":"ok"|"fp"}
  GET /health              -> état du service

Lancement:
  env -u PYTHONPATH python3 analyze.py [--interval 5] [--model yolov8n]
"""
import argparse
import base64
import json
import os
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from collections import deque

import numpy as np
from ultralytics import YOLO

SDK = os.environ.get("SDK_URL", "http://localhost:8090")
PORT = int(os.environ.get("ANALYZE_PORT", "8091"))

# Types d'événements pertinents pour la surveillance resto (classes COCO)
CLASSES = {0: "personne"}   # on flagge les personnes ; extensible plus tard

class Store:
    def __init__(self, cap=200):
        self.events = deque(maxlen=cap)
        self.stats = {"detected": 0, "validated": 0, "false_positive": 0}
        self.lock = threading.Lock()

    def add(self, ev):
        with self.lock:
            ev["id"] = len(self.events) + 1 if not hasattr(self, "_seq") else None
            self.events.append(ev)
            self.stats["detected"] += 1
            return list(self.events)[-1]["id"]

    def get(self, evid):
        with self.lock:
            for e in self.events:
                if e["id"] == evid:
                    return e
        return None

    def label(self, evid, label):
        with self.lock:
            for e in self.events:
                if e["id"] == evid:
                    e["label"] = label
                    self.stats["validated" if label == "ok" else "false_positive"] += 1
                    return e
        return None

    def list(self, status=None):
        with self.lock:
            evs = list(self.events)
            if status:
                evs = [e for e in evs if e.get("label", "pending") == status]
            return evs

STORE = Store()

def annotate(frame, boxes, names):
    """Dessine les boîtes de détection. frame est un ndarray BGR."""
    out = frame.copy()
    for box in boxes:
        x1, y1, x2, y2 = map(int, box.xyxy[0].tolist())
        conf = float(box.conf[0])
        cls = int(box.cls[0])
        label = f"{names.get(cls, cls)} {conf:.2f}"
        cv2 = __import__("cv2")
        cv2.rectangle(out, (x1, y1), (x2, y2), (0, 165, 255), 2)
        cv2.putText(out, label, (x1, y1 - 6), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 165, 255), 2)
    return out

def fetch_snapshot(nvr, chan):
    url = f"{SDK}/snapshot/{nvr}/{chan}"
    req = urllib.request.Request(url)
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()

def analyze_once(model, nvr, chan, names):
    raw = fetch_snapshot(nvr, chan)
    img = np.frombuffer(raw, dtype=np.uint8)
    cv2 = __import__("cv2")
    frame = cv2.imdecode(img, cv2.IMREAD_COLOR)
    if frame is None:
        return None
    results = model(frame, verbose=False)
    r = results[0]
    persons = [b for b in r.boxes if int(b.cls[0]) == 0 and float(b.conf[0]) >= 0.4]
    if not persons:
        return None
    top = max(persons, key=lambda b: float(b.conf[0]))
    annotated = annotate(frame, persons, names)
    ok, buf = cv2.imencode(".jpg", annotated)
    jpeg = buf.tobytes() if ok else None
    return {
        "nvr": nvr, "chan": chan,
        "ts": time.strftime("%H:%M:%S"),
        "type": "Personne détectée",
        "conf": round(float(top.conf[0]) * 100, 1),
        "count": len(persons),
        "label": "pending",
        "image": base64.b64encode(jpeg).decode() if jpeg else None,
    }

def worker(model, nvrs, interval, names):
    seq = 0
    while True:
        for n in nvrs:
            nid, chans = n["id"], n["channels"]
            # un canal à la fois (le SDK sérialise les captures)
            for ch in [n["firstChannel"] + i for i in range(min(chans, 4))]:
                try:
                    ev = analyze_once(model, nid, ch, names)
                    if ev:
                        seq += 1
                        ev["id"] = seq
                        with STORE.lock:
                            STORE.events.append(ev)
                            STORE.stats["detected"] += 1
                except Exception:
                    pass
                time.sleep(1)   # ~1 capture/sec/canal pour ne pas saturer le NVR
        time.sleep(interval)

class Handler(BaseHTTPRequestHandler):
    def _json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _img(self, data, code=200):
        self.send_response(code)
        self.send_header("Content-Type", "image/jpeg")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/health":
            self._json({"status": "ok", "stats": STORE.stats})
        elif path == "/stats":
            self._json(STORE.stats)
        elif path == "/events":
            status = None
            if "?" in self.path:
                q = dict(p.split("=") for p in self.path.split("?")[1].split("&") if "=" in p)
                status = q.get("status")
            evs = STORE.list(status)
            self._json([{k: v for k, v in e.items() if k != "image"} for e in evs])
        elif path.startswith("/events/") and path.endswith("/image"):
            evid = int(path.split("/")[2])
            ev = STORE.get(evid)
            if ev and ev.get("image"):
                self._img(base64.b64decode(ev["image"]))
            else:
                self._json({"error": "pas d'image"}, 404)
        else:
            self._json({"error": "not found"}, 404)

    def do_POST(self):
        path = self.path.split("?")[0]
        if path.startswith("/events/") and path.endswith("/label"):
            evid = int(path.split("/")[2])
            n = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(n) or b"{}")
            ev = STORE.label(evid, body.get("label", "ok"))
            self._json({"ok": bool(ev), "stats": STORE.stats})
        else:
            self._json({"error": "not found"}, 404)

    def log_message(self, *a):
        pass

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--interval", type=int, default=5)
    ap.add_argument("--model", default="yolov8n.pt")
    args = ap.parse_args()

    model = YOLO(args.model)
    names = model.names

    # inventaire des NVR via le SDK
    with urllib.request.urlopen(f"{SDK}/api/nvrs", timeout=10) as r:
        nvrs = json.load(r)["nvrs"]

    t = threading.Thread(target=worker, args=(model, nvrs, args.interval, names), daemon=True)
    t.start()

    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print(f"Analyse IA prête sur http://localhost:{PORT}  (modèle {args.model})", flush=True)
    srv.serve_forever()

if __name__ == "__main__":
    main()
