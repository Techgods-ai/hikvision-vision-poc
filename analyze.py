#!/usr/bin/env python3
"""Service d'analyse IA — moteur de règles de détection sur les snapshots.

Poll les snapshots du service SDK (port 8090), détecte les objets avec YOLO,
et applique un ensemble de règles (règles.json) pour flager des événements
métier : sac en zone restreinte, présence prolongée, file d'attente, etc.

Endpoints:
  GET /events              -> derniers événements (JSON)
  GET /events/<id>/image   -> snapshot annoté de l'événement (JPEG)
  GET /stats               -> compteurs (détectés / validés / faux positifs)
  POST /events/<id>/label  -> validation humaine {"label":"ok"|"fp"}
  GET /rules               -> règles chargées
  GET /health              -> état du service

Lancement:
  env -u PYTHONPATH python3 analyze.py [--interval 5] [--model yolov8n.pt] [--rules rules.json]
"""
import argparse
import base64
import json
import os
import threading
import time
import urllib.request
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import numpy as np
from ultralytics import YOLO

SDK = os.environ.get("SDK_URL", "http://localhost:8090")
PORT = int(os.environ.get("ANALYZE_PORT", "8091"))

# Classes COCO pertinentes
COCO = {
    0: "personne",
    24: "sac à dos",
    26: "sac à main",
    28: "valise",
    39: "bouteille",
    40: "verre",
}

class Store:
    def __init__(self, cap=500):
        self.events = deque(maxlen=cap)
        self.stats = {"detected": 0, "validated": 0, "false_positive": 0}
        self.lock = threading.Lock()

    def add(self, ev):
        with self.lock:
            self.events.append(ev)
            self.stats["detected"] += 1

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

def load_rules(path):
    with open(path) as f:
        cfg = json.load(f)
    return cfg["rules"]

def annotate(frame, boxes, names):
    out = frame.copy()
    cv2 = __import__("cv2")
    for box in boxes:
        x1, y1, x2, y2 = map(int, box.xyxy[0].tolist())
        conf = float(box.conf[0])
        cls = int(box.cls[0])
        label = f"{names.get(cls, cls)} {conf:.2f}"
        cv2.rectangle(out, (x1, y1), (x2, y2), (0, 165, 255), 2)
        cv2.putText(out, label, (x1, y1 - 6), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 165, 255), 2)
    return out

def fetch_snapshot(nvr, chan):
    url = f"{SDK}/snapshot/{nvr}/{chan}"
    req = urllib.request.Request(url)
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()

def box_center(box):
    x1, y1, x2, y2 = box.xyxy[0].tolist()
    return ((x1 + x2) / 2, (y1 + y2) / 2)

def normalized_center(box, w, h):
    cx, cy = box_center(box)
    return (cx / w, cy / h)

def cluster_size(centers, radius):
    """Taille du plus grand cluster de points dans un rayon donné (normalisé)."""
    n = len(centers)
    best = 0
    used = [False] * n
    for i in range(n):
        if used[i]:
            continue
        group = [i]
        for j in range(i + 1, n):
            if used[j]:
                continue
            if any((centers[j][0] - centers[k][0]) ** 2 + (centers[j][1] - centers[k][1]) ** 2 <= radius ** 2 for k in group):
                group.append(j)
        for k in group:
            used[k] = True
        best = max(best, len(group))
    return best

# État de présence prolongée par (nvr, chan, rule_id)
PRESENCE = {}

def evaluate_rules(frame, detections, w, h, rules, nvr_id, chan):
    """Retourne la liste des événements déclenchés."""
    fired = []
    for rule in rules:
        rule_id = rule["id"]
        classes = set(rule["classes"])
        min_conf = rule.get("min_conf", 0.35)
        min_count = rule.get("min_count", 1)

        hits = [b for b in detections if int(b.cls[0]) in classes and float(b.conf[0]) >= min_conf]
        # présence est suivie par (règle, nvr, canal) — pas globalement
        pkey = (rule_id, nvr_id, chan)

        if len(hits) < min_count:
            PRESENCE.pop(pkey, None)
            continue

        # Cas file d'attente : clustering spatial
        if rule.get("cluster_radius"):
            centers = [normalized_center(b, w, h) for b in hits]
            cs = cluster_size(centers, rule["cluster_radius"])
            if cs >= min_count:
                top = max(hits, key=lambda b: float(b.conf[0]))
                fired.append(_make_event(rule, top, len(hits), cs))
            continue

        # Cas présence prolongée
        if rule.get("min_duration"):
            now = time.time()
            if pkey not in PRESENCE:
                PRESENCE[pkey] = now
            elif now - PRESENCE[pkey] >= rule["min_duration"]:
                PRESENCE.pop(pkey)
                top = max(hits, key=lambda b: float(b.conf[0]))
                fired.append(_make_event(rule, top, len(hits)))
            continue

        # Cas simple : présence d'objets interdits
        top = max(hits, key=lambda b: float(b.conf[0]))
        fired.append(_make_event(rule, top, len(hits)))

    return fired

def _make_event(rule, top_box, count, cluster=None):
    conf = round(float(top_box.conf[0]) * 100, 1)
    cls = int(top_box.cls[0])
    ev = {
        "type": rule["label"],
        "rule": rule["id"],
        "cls": COCO.get(cls, cls),
        "conf": conf,
        "count": count,
        "label": "pending",
    }
    if cluster:
        ev["cluster"] = cluster
    return ev

def worker(model, nvrs, interval, rules):
    seq = 0
    while True:
        for n in nvrs:
            nid = n["id"]
            for ch in [n["firstChannel"] + i for i in range(min(n["channels"], 4))]:
                try:
                    raw = fetch_snapshot(nid, ch)
                    img = np.frombuffer(raw, dtype=np.uint8)
                    cv2 = __import__("cv2")
                    frame = cv2.imdecode(img, cv2.IMREAD_COLOR)
                    if frame is None:
                        time.sleep(1)
                        continue
                    results = model(frame, verbose=False)
                    detections = results[0].boxes
                    h, w = frame.shape[:2]
                    fired = evaluate_rules(frame, detections, w, h, rules, nid, ch)
                    for ev in fired:
                        seq += 1
                        ev.update({"id": seq, "nvr": nid, "chan": ch, "ts": time.strftime("%H:%M:%S")})
                        ann = annotate(frame, detections, COCO)
                        ok, buf = cv2.imencode(".jpg", ann)
                        if ok:
                            ev["image"] = base64.b64encode(buf.tobytes()).decode()
                        STORE.add(ev)
                except Exception:
                    pass
                time.sleep(1)
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
        elif path == "/rules":
            self._json(RULES)
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
    global RULES
    ap = argparse.ArgumentParser()
    ap.add_argument("--interval", type=int, default=5)
    ap.add_argument("--model", default="yolov8n.pt")
    ap.add_argument("--rules", default="rules.json")
    args = ap.parse_args()

    RULES = load_rules(args.rules)
    model = YOLO(args.model)

    with urllib.request.urlopen(f"{SDK}/api/nvrs", timeout=10) as r:
        nvrs = json.load(r)["nvrs"]

    t = threading.Thread(target=worker, args=(model, nvrs, args.interval, RULES), daemon=True)
    t.start()

    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print(f"Analyse IA prête sur http://localhost:{PORT}  (modèle {args.model}, {len(RULES)} règles)", flush=True)
    srv.serve_forever()

if __name__ == "__main__":
    main()
