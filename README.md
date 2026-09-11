# Vision POC — NVR Hikvision

Connexion aux NVR Hikvision via le Device Network SDK officiel, capture d'images pleine résolution et interface web multi-caméras. Base technique pour un pilote de détection d'événements en restaurant.

## État

Validé sur macOS Apple Silicon (Colima + Docker, émulation `linux/amd64`) :

- login SDK sur les 3 NVR du site, avec reconnexion automatique ;
- capture JPEG **1920x1080** (résolution native du flux) ;
- API HTTP locale de snapshots, clips et flux live ;
- interface web multi-vues (caméras, événements, clips, analytics) ;
- **service d'analyse IA** : détection de personnes (YOLO) sur les snapshots,
  avec validation humaine et compteurs de faux positifs.

| Port SDK | Modèle | Canaux |
|---:|---|---:|
| 8001 | DS-7604NXI-K1/4P | 4 (33 à 36) |
| 8000 | DS-7616NI-Q2/16P | 16 (33 à 48) |
| 7000 | DS-7616NXI-K2/16P | 16 (33 à 48) |

## Prérequis

- macOS ou Linux, Docker (via Colima sur Mac) ;
- le SDK Hikvision officiel `EN-HCNetSDKV6.1.9.48_build20230410_linux64.zip`, non inclus dans ce dépôt.

Installation du SDK :

```bash
unzip EN-HCNetSDKV6.1.9.48_build20230410_linux64.zip -d /tmp/hiksdk
SDK=/tmp/hiksdk/EN-HCNetSDKV6.1.9.48_build20230410_linux64
mkdir -p docker/libs
cp -R "$SDK/lib/." docker/libs/
cp "$SDK/incEn/HCNetSDK.h" docker/
```

## Démarrage

```bash
cp .env.example .env    # renseigner HIK_HOST, HIK_USER, HIK_PASS
./start.sh
```

Interface : http://localhost:8080

Arrêt : `./stop.sh`

## Service d'analyse IA

Le service d'analyse (Python, `analyze.py`) détecte les objets sur les
snapshots et applique un **moteur de règles** (config `rules.json`) pour
flager les événements métier. Il expose les événements sur le port **8091**.

```bash
# Attention : vider PYTHONPATH (le venv du Hermes agent contamine Python)
env -u PYTHONPATH python3 analyze.py --interval 5 --model yolov8n.pt --rules rules.json
```

Prérequis : `pip install ultralytics` (installe torch, opencv, pillow).

### Moteur de règles

`rules.json` décrit chaque cas d'usage sans toucher au code :

- **règle simple** : `classes` + `min_conf` + `min_count` (ex. sac en zone restreinte) ;
- **règle durée** : + `min_duration` (ex. présence prolongée au cellier) ;
- **règle clustering** : + `cluster_radius` (ex. file d'attente : N personnes groupées).

Tester le moteur : `env -u PYTHONPATH python3 test_rules.py`.

| Route | Réponse |
|---|---|
| `GET /events` | derniers événements détectés |
| `GET /events/<id>/image` | snapshot annoté (boîtes de détection) |
| `GET /stats` | compteurs : détectés / validés / faux positifs |
| `POST /events/<id>/label` | validation humaine `{"label":"ok"\|"fp"}` |
| `GET /rules` | règles chargées |

### Pipeline complet

```text
NVR Hikvision → sdk_service (:8090) → snapshot JPEG 1080p
                                         ↓
                              analyze.py (:8091) → YOLO → événements
                                         ↓
                              interface web (:8080) → validation humaine
```

## API

| Route | Réponse |
|---|---|
| `GET /api/nvrs` | inventaire JSON : modèle, canaux, état de connexion |
| `GET /snapshot/<nvr>/<canal>` | JPEG pleine résolution |
| `GET /live/<nvr>/<canal>` | flux vidéo continu (MJPEG multipart, sous-flux) |
| `GET /clip/<nvr>/<canal>?start=YYYYMMDDHHMMSS&end=YYYYMMDDHHMMSS` | exporte un clip vidéo HEVC |
| `GET /health` | identique à `/api/nvrs` |

Exemple :

```bash
curl http://localhost:8090/api/nvrs
curl -o cam.jpg http://localhost:8090/snapshot/nvr8000/33
curl "http://localhost:8090/clip/nvr8000/33?start=$(date -v-5M +%Y%m%d%H%M%S)&end=$(date +%Y%m%d%H%M%S)"
```

L'identifiant de NVR est `nvr<port>`, par exemple `nvr8000`. Les clips sont écrits dans `clips/` (monté en volume, non versionné) et retournés en JSON avec leur chemin.

## Architecture

```text
NVR Hikvision
  | SDK privé Hikvision (ports 8001 / 8000 / 7000)
  v
sdk_service (C++, Docker linux/amd64)
  | API HTTP :8090, sessions persistantes, relogin automatique
  v
Interface web :8080
  | grille multi-caméras, snapshots, plein écran
  v
[à venir] moteur d'analyse sous licence commerciale
```

## Contraintes connues

**Snapshots, pas de vidéo fluide.** Une capture prend 2 à 3 s par canal via le SDK ; un balayage de 16 caméras demande environ 40 s. C'est suffisant pour de l'analyse d'images, mais ce n'est pas de la surveillance temps réel. Pour de la vidéo live il faudrait `NET_DVR_RealPlay_V30` et un décodage H.265.

**Captures sérialisées.** Le NVR ne répond pas à 16 requêtes simultanées ; l'interface charge les canaux un par un.

**RTSP indisponible.** Le port 554 n'est pas exposé sur le routeur du site ; seul le SDK est accessible à distance. `viewer.py` et `test_rtsp.sh` sont conservés pour un usage RTSP éventuel sur le réseau local.

## Pièges rencontrés

- Le SDK V6 chiffre le login via OpenSSL : sans `libcrypto.so.1.1` et `libssl.so.1.1`, le login échoue avec le code 11 (`NETWORK_ERRORDATA`). Copier `lib/` en entier, y compris `HCNetSDKCom/`.
- `HCNetSDK.h` utilise `extern "C"` : compiler en C++ (`g++`), pas en C.
- `wPicSize = 0xff` donne la résolution native ; les autres valeurs forcent des formats réduits (2 donne du 352x288).
- Les caméras IP d'un NVR commencent au canal 33, pas au canal 1.
- Colima ne monte pas `/var/folders` dans sa VM : les volumes Docker doivent pointer sous `$HOME`.
- **Timezone** : le conteneur Debian est en UTC par défaut alors que les NVR sont à l'heure locale (UTC-4). Une recherche d'enregistrements calculée en UTC vise le futur du NVR et ne trouve rien. Installer `tzdata` et `TZ=America/Toronto`.
- **Export de clip** : il faut `NET_DVR_GetFileByTime_V40` (pas l'ancienne `GetFileByTime`), avec `NET_DVR_PLAYCOND.byDownload = 1`, puis `PlayBackControl(PLAYSTART)` pour lancer le téléchargement. Sans le `PLAYSTART`, le SDK crée le fichier mais n'écrit rien (0 octet).
- **Flux live à distance** : le RTSP (`byProtoType=1`, port 554) n'est pas forwardé sur le routeur du site. Il faut le **protocole privé** (`byProtoType=0`, port SDK), comme iVMS-4200. Le flux privé commence par un en-tête propriétaire `IMKH` (29 octets) avant le MPEG-PS réel : le retirer avant de le passer à ffmpeg, sinon aucun décodage.

## Sécurité et licence

Dépôt **privé**. Les identifiants vivent dans `.env`, jamais dans Git.

Le mot de passe utilisé pendant le POC a circulé hors du dépôt : une rotation est recommandée.

Les bibliothèques Hikvision sont propriétaires et exclues du dépôt (`.gitignore`). Vérifier les conditions Hikvision avant toute redistribution ou publication d'une image Docker les contenant.

### Licence du modèle d'analyse (point critique avant mise en production)

Le POC utilise **YOLOv8n via Ultralytics**, sous licence **AGPL-3.0**. Cette
licence est **copyleft** : toute utilisation en contexte propriétaire (interne
ou commerciale) exige soit d'ouvrir tout le code source du projet sous AGPL,
soit d'acheter une **licence commerciale Enterprise** à Ultralytics.

Pour la production, deux voies :

1. **Licence commerciale Ultralytics** — déployer YOLO en source fermée sans
   obligation d'open-source. À chiffrer.
2. **Alternative à licence permissive** (aucune obligation d'open-source) :
   - **RF-DETR** — Apache 2.0 ;
   - **YOLOX** — Apache 2.0 ;
   - **LibreYOLO** — MIT (code).

Le boss a déjà tranché : **commercial sous licence**, pas d'open-source AGPL
gratuit. Donc avant tout déploiement, soit négocier la licence Ultralytics,
soit basculer sur RF-DETR / YOLOX (Apache 2.0) qui autorisent l'usage
propriétaire sans frais de licence ni obligation de publication.

Le POC prouve le pipeline ; le choix du modèle final est une décision
juridique/coût, pas technique.

## Prochaines étapes

1. Confirmer l'inventaire complet des canaux actifs sur les 3 NVR.
2. Tranchir la licence du modèle (Ultralytics Enterprise vs RF-DETR/YOLOX Apache-2.0).
3. Étendre la détection aux cas d'usage réels (zones, horaires, véhicules).
4. Brancher la file de validation humaine et les métriques de précision sur la durée.
5. Encadrer la conformité : ÉFVP, durée de conservation, journal d'accès, zones exclues.
