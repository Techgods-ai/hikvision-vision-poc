# Hikvision Vision POC

POC local de connexion aux NVR Hikvision Bossini Ste-Foy et de capture d'images via le Device Network SDK officiel.

## État actuel

Validé sur Mac Apple Silicon avec Colima/Docker et émulation `linux/amd64` :

- login SDK Hikvision sur les trois NVR;
- capture JPEG du canal IP 33;
- inventaire des appareils détectés.

| Port | Modèle | Canaux IP |
|---:|---|---:|
| 8001 | DS-7604NXI-K1/4P | 4, canaux 33 à 36 |
| 8000 | DS-7616NI-Q2/16P | 16, canaux 33 à 48 |
| 7000 | DS-7616NXI-K2/16P | 16, canaux 33 à 48 |

## Accéder à l'interface actuelle

Le viewer web RTSP existant se lance avec :

```bash
cd ~/camviewer
python3 viewer.py 8080
```

Puis ouvrir : http://localhost:8080

Important : cette interface attend des URLs RTSP. Les NVR distants de ce POC exposent actuellement le SDK Hikvision sur les ports 8001, 8000 et 7000. Le viewer web n'affichera donc pas encore les NVR SDK.

## Test SDK

```bash
cd ~/camviewer
colima start --cpu 2 --memory 4
cd docker
docker build --platform linux/amd64 -t hik-sdk-test .
mkdir -p out
docker run --rm --platform linux/amd64 -v "$PWD/out:/out" hik-sdk-test ./test_sdk bstf-rtr-gw.gbm10.com 8000
```

Ne jamais committer de mot de passe ou de secret. Avant un déploiement partagé, remplacer les valeurs de test par des variables d'environnement ou Docker secrets.

## Architecture cible

```text
NVR Hikvision distant
  | SDK privé Hikvision, port 8000/8001/7000
  v
Service capture local Docker amd64
  | JPEG / frames / événements
  v
API localhost + interface web
  | clips, validation humaine, métriques
  v
Moteur d'analyse sous licence commerciale
```

## Prochaines étapes

1. Remplacer le binaire de test par un service persistant SDK.
2. Ajouter les canaux dans une configuration non secrète.
3. Exposer une API locale de snapshots et de clips.
4. Construire la grille web SDK, avec statut de connexion et reconnexion.
5. Brancher le modèle sous licence commerciale après validation des cas d'usage.
6. Ajouter conservation limitée, journal d'accès et validation humaine.

## Sécurité et licence

Le dépôt doit rester privé pendant le POC. Une rotation du mot de passe est recommandée, car il a été partagé dans la conversation et utilisé dans des tests.

Le SDK Hikvision provient du téléchargement officiel `EN-HCNetSDKV6.1.9.48_build20230410_linux64.zip`. Vérifier les conditions Hikvision avant toute redistribution du SDK ou publication d'une image Docker contenant ses bibliothèques. Ce dépôt ne doit pas publier les bibliothèques propriétaires sans validation juridique.
