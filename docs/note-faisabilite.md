# Note de faisabilité — Monitoring vidéo par IA

## Objet

Confronter les cas d'usage identifiés (classification du groupe) à la réalité
technique des modèles de vision disponibles, pour éclairer la décision de
direction : faire le projet ou non, et sur quel périmètre.

---

## 1. Où on en est (preuve technique)

Un pilote local fonctionne déjà de bout en bout sur le site Bossini Ste-Foy :

- connexion aux 3 NVR Hikvision (36 canaux) via le SDK officiel ;
- capture d'images 1080p, flux vidéo continu, export de clips ;
- **détection automatique de personnes** (modèle YOLO) avec validation humaine ;
- 57+ événements détectés en conditions réelles lors des premiers essais.

Le moteur de règles vient d'être généralisé pour couvrir les cas d'usage
métier : `rules.json` décrit chaque règle (classes, seuil, durée, zone,
clustering), sans toucher au code.

---

## 2. Lecture des cas d'usage (classification du groupe)

### ✅ Réalistes immédiatement (détection d'objets/zones — cœur de YOLO)

| Cas d'usage | Principe | Effort |
|---|---|---|
| Sacs en zones restreintes | zone + détection sac (dos/main/valise) | faible |
| Cellier : contrôle des vins | zone + présence prolongée | faible |
| Clients / file d'attente | comptage de personnes groupées | faible |

Ces trois cas reposent sur de la **détection d'objets et de zones**, la
fonction la plus mature de la vision par ordinateur. Ils sont couverts par le
pilote actuel.

### ⚠️ Demandent un modèle spécialisé (classification fine, pas YOLO générique)

| Cas d'usage | Pourquoi c'est plus dur |
|---|---|
| Employé sans uniforme | classification vestimentaire — il faut entraîner un modèle sur les uniformes réels |
| Filet cheveux / barbe (hygiène) | classification très fine — nécessite des images annotées du personnel |

Ces cas ne sont pas de la détection d'objet, mais de la **classification
d'apparence**. Ils exigent un jeu de données annoté (photos des employés en
uniforme, avec/sans filet) et un entraînement sur mesure. C'est faisable mais
c'est un chantier de données, pas un branchement de modèle.

### ⚙️ Hors vision (intégration API, plus simple et plus fiable)

| Cas d'usage | Nature |
|---|---|
| Vérification des punchs in/out | intégration au système de poinçonnage/paie existant |

Ce n'est pas de la vidéo. C'est une intégration API au système de pointage.
C'est le plus fiable des cas listés, mais hors périmètre du monitoring.

### ❌ À écarter (décision déjà prise, confirmée techniquement)

| Cas d'usage | Raison |
|---|---|
| **Reconnaissance faciale** | risque **Loi 25 critique** — donnée biométrique très encadrée |
| Marchandise dans sac | très difficile, valeur faible |
| Consommation d'alcool | détection ambiguë, non fiable |

---

## 3. Le point de décision central : la licence

Le pilote utilise YOLO (Ultralytics), sous **licence AGPL-3.0** (copyleft).
En usage propriétaire, ça impose soit d'ouvrir tout le code, soit d'acheter
une licence commerciale.

Deux issues propres :

| Option | Licence | Coût | Obligation d'open-source |
|---|---|---|---|
| Ultralytics Enterprise | commerciale | à chiffrer | aucune |
| RF-DETR / YOLOX | Apache 2.0 | gratuit | aucune |
| LibreYOLO | MIT | gratuit | aucune |

**Recommandation** : basculer sur **RF-DETR ou YOLOX (Apache 2.0)** — même
pipeline technique (on change le modèle, pas le code), aucune obligation
d'open-source, aucun coût de licence. C'est une décision juridique/coût, pas
technique : le pilote a déjà prouvé que le pipeline marche.

---

## 4. Coût et modèle de déploiement

Le pilote tourne **localement** (pas de cloud), ce qui :
- élimine le coût récurrent d'une API vision ;
- répond à la contrainte de souveraineté des données (Loi 25) ;
- reste réaliste : YOLO/nano détecte sur un Mac M4, sans GPU serveur.

Le modèle final pourra tourner sur un serveur local du groupe (ou sur chaque
site si on veut zéro transit réseau).

---

## 5. Points pour / contre (réunion de direction)

### Points POUR faire le projet

1. **Le noyau dur est déjà prouvé** — connexion NVR, détection, validation
   humaine : ça fonctionne en conditions réelles.
2. **3 cas d'usage à forte valeur sont immédiatement couverts** (zones
   restreintes, cellier, file d'attente) avec un effort faible.
3. **Coût marginal très bas** — modèles gratuits (Apache 2.0), exécution
   locale, pas d'abonnement cloud.
4. **Souveraineté et conformité** — données restent chez nous, pas de
   fournisseur US (contrairement à spot.ai), aligné Loi 25.
5. **Ne dépend pas d'un fournisseur** — pas de changement de caméras, pas de
   contrat 1 an, on garde le matériel existant.

### Points CONTRE (à lever avant de s'engager)

1. **Les cas les plus « métier » (uniforme, hygiène) demandent un entraînement
   sur mesure** — un chantier de collecte et d'annotation de données, pas un
   simple branchement.
2. **La précision réelle n'est pas encore mesurée** — on sait détecter, on ne
   sait pas encore le taux de faux positifs sur une semaine d'exploitation.
3. **Le punch in/out, cas le plus rentable, n'est pas de la vision** — c'est un
   projet d'intégration API séparé.
4. **Effort d'équipe** — il faut du temps de développement (Christian) et de la
   collecte de données terrain, à planifier avec les autres chantiers.

---

## 6. Recommandation

**Faire le projet, sur un périmètre restreint et mesurable :**

1. Pilote sur les **3 cas d'usage réalistes** (zones restreintes, cellier,
   file d'attente) pendant 2 semaines, pour **mesurer la précision réelle et
   le taux de faux positifs** — c'est la donnée qui manque.
2. Trancher la **licence** (basculer sur RF-DETR/YOLOX Apache 2.0, ou acheter
   Ultralytics Enterprise si on veut rester sur YOLO).
3. Évaluer séparément le **punch in/out** comme projet d'intégration API.
4. Repousser uniforme/hygiène à une phase 2 avec budget de collecte de données.
5. Confirmer l'écart définitif de la **reconnaissance faciale** (Loi 25).

Le pilote actuel fournit déjà la démonstration technique nécessaire à la
décision. Il reste à produire les chiffres de précision/coût sur la durée.
