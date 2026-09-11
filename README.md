# Vision POC — NVR Hikvision

Connexion aux NVR Hikvision via le Device Network SDK officiel, capture d'images pleine résolution et interface web multi-caméras. Base technique pour un pilote de détection d'événements en restaurant.

## État

Validé sur macOS Apple Silicon (Colima + Docker, émulation `linux/amd64`) :

- login SDK sur les 3 NVR du site, avec reconnexion automatique ;
- capture JPEG **1920x1080** (résolution native du flux) ;
- API HTTP locale de snapshots ;
- interface web avec onglets par NVR, rafraîchissement réglable et plein écran.

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

## Prochaines étapes

1. Confirmer l'inventaire complet des canaux actifs sur les 3 NVR.
2. Ajouter l'export de clips (`NET_DVR_PlayBackByTime`) pour constituer un jeu d'évaluation.
3. Brancher le moteur d'analyse sous licence commerciale sur le flux de snapshots.
4. Ajouter la file de validation humaine et les métriques de précision.
5. Encadrer la conformité : ÉFVP, durée de conservation, journal d'accès, zones exclues.
