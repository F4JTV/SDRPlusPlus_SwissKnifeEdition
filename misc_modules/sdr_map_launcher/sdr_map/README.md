# SDR Map — ADRASEC 06

Carte web **temps réel** des objets décodés par les modules SDR++
(**AIS**, **ADS-B**, **APRS**) que nous avons développés. Tous les modules se
connectent en **TCP sur un seul et même port** ; le serveur range les objets
selon leur champ `type`, les enregistre et les diffuse au navigateur par
**WebSocket**. La carte est **consultable hors-ligne** grâce à un cache de
tuiles dans le navigateur (service worker).

---

## 1. Architecture

```
   SDR++ (plusieurs modules, tous clients TCP)
   ┌──────────┐ ┌───────────┐ ┌───────────┐ ┌─────────────────┐
   │ais_decoder│ │adsb_decoder│ │aprs_decoder│ │aprs (météo)    │
   └────┬─────┘ └─────┬─────┘ └─────┬─────┘ └────────┬────────┘
        │ JSON\n      │ JSON\n      │ JSON\n         │ JSON\n
        └─────────────┴──────┬──────┴────────────────┘
                             ▼   un SEUL port TCP (10100)
                 ┌───────────────────────────┐
                 │  manage.py listen_sdr      │  serveur TCP multi-clients
                 │  → dispatch par "type"     │  → update_or_create
                 │  → purge TTL               │  → group_send (Channels)
                 └────────────┬──────────────┘
                              │ WebSocket (Channels/Daphne)
                              ▼
                 ┌───────────────────────────┐
                 │  Navigateur — Leaflet      │  fonds multiples + filtres
                 │  + Service Worker (cache)  │  + tuiles hors-ligne
                 └───────────────────────────┘
```

- **Un seul port** : chaque instance de module SDR++ ouvre sa propre connexion
  TCP vers le même `IP:10100`. Un serveur `ThreadingTCPServer` accepte autant
  de connexions simultanées que de modules.
- **Dispatch par `type`** : `AIS`, `ADSB`, `APRS`, `APRS Meteo`, `lrrp`,
  `radiosonde`.
- **Identité stable** : MMSI (AIS), ICAO (ADS-B), indicatif/nom (APRS),
  RID DMR `source` (LRRP/DSD-FME), numéro de série (radiosonde). Un objet qui
  ré-émet est **mis à jour** au lieu d'être dupliqué.
- **TTL** : les objets non rafraîchis depuis `SDR_OBJECT_TTL_MINUTES` (30 par
  défaut) sont purgés et retirés de la carte.

---

## 2. Format attendu (identique à la sortie des modules)

Une ligne **JSON par objet**, terminée par `\n`. Schéma de base commun
(calqué sur le module AIS) :

```json
{"name":"...", "date":"YYYY-MM-DD", "time":"HH:MM:SS",
 "lat":43.30, "lon":7.36, "type":"AIS|ADSB|APRS|APRS Meteo",
 "speed":10.0, "info":"texte libre"}
```

Champs additionnels reconnus selon le type :

| Type         | Champs en plus                                                  |
|--------------|-----------------------------------------------------------------|
| `AIS`        | `mmsi`                                                          |
| `ADSB`       | `icao` ; `info` contient `alt=…ft`, `hdg=…`, `vrate=…fpm`       |
| `APRS`       | —                                                              |
| `APRS Meteo` | `temperature`/`humidity`/`wind_dir`/`wind_speed`/`wind_gust`/`pressure`/`rain` (variantes acceptées) |
| `lrrp`       | `source` (RID DMR émetteur) — positions GPS LRRP via DSD-FME    |
| `radiosonde` | `name` = n° de série ; `info` = `alt=` (m), `hdg=`, `climb=` (m/s), `temp=`, `rh=`, `p=` ; `speed` en m/s |

> L'altitude et le cap des aéronefs sont extraits automatiquement du champ
> `info` s'ils ne sont pas fournis en clair. Tout champ non reconnu est
> conservé dans `extra` (robustesse aux évolutions des modules).

---

## 3. Installation

```bash
cd sdr_map
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python manage.py migrate
python manage.py createsuperuser     # optionnel (accès /admin)
```

## 4. Lancement

### Le plus simple (recommandé pour un poste opérateur) — tout-en-un

Une seule commande lance le serveur web **et** le collecteur TCP dans le même
process, ce qui active le **temps réel sans Redis** (voir l'encadré ci-dessous) :

```bash
python manage.py runserver_sdr
# options : --web-host 0.0.0.0 --web-port 8000 --tcp-host 0.0.0.0 --tcp-port 10100
```

Ouvrez **http://IP_DU_SERVEUR:8000/** et pointez le « TCP output » de chaque
module SDR++ vers le port **10100**.

> **Pourquoi cette commande ?** La couche Channels par défaut
> (`InMemoryChannelLayer`) n'est **pas partagée entre processus**. Si le serveur
> web (`daphne`) et le collecteur (`listen_sdr`) tournent séparément, les
> positions reçues en TCP n'atteignent jamais le navigateur : les objets ne se
> déplacent pas en direct. `runserver_sdr` exécute les deux dans le même process
> → la diffusion temps réel fonctionne sans rien installer de plus.

### En deux processus (production, avec Redis)

Pour séparer web et collecteur (plusieurs workers, montée en charge), il faut
une couche partagée → **Redis** :

```bash
pip install channels-redis
export SDR_MAP_REDIS_URL=redis://127.0.0.1:6379/0

# a) serveur web (sert carte + WebSocket + statiques via WhiteNoise)
daphne -b 0.0.0.0 -p 8000 sdr_map.asgi:application
# b) collecteur TCP unique
python manage.py listen_sdr --host 0.0.0.0 --port 10100
```

> Sans `SDR_MAP_REDIS_URL`, ces deux commandes séparées **n'auront pas** de
> temps réel (chacune sa mémoire). Utilisez alors `runserver_sdr`.

Ouvrez ensuite **http://IP_DU_SERVEUR:8000/**.

**b) Le collecteur TCP unique** (dans un autre terminal) :

```bash
python manage.py listen_sdr --host 0.0.0.0 --port 10100
```

### Test sans matériel

Avec `runserver_sdr` lancé, dans un autre terminal :

```bash
python manage.py send_test_objects --port 10100
```

Des objets mobiles (un par type, dont un avion qui change de cap et une
radiosonde qui monte puis redescend) apparaissent et **se déplacent en direct**
sur la carte.

---

## 5. Configuration côté SDR++

Pour **chaque** module (AIS, ADS-B, APRS…), dans son panneau « TCP output » :

- **Adresse / IP** : celle du serveur Django.
- **Port** : **10100** (le même pour tous).
- Cochez l'envoi des positions/contacts.

Comme l'AIS émet sur deux canaux, lancez deux instances `ais_decoder` : les
deux pointent vers `IP:10100`, sans conflit.

---

## 6. La carte

- **Plusieurs fonds** (sélecteur en haut à droite) : Plan OSM, Humanitaire HOT,
  Relief OpenTopo, Satellite Esri, Clair Carto, Sombre Carto.
- **Filtres** (panneau gauche) : afficher/masquer chaque type, ou
  « Tout afficher ».
- **Marqueurs** colorés par type ; les aéronefs sont orientés selon leur cap.
  Les **radiosondes** (ballon jaune) affichent l'altitude en mètres, la vitesse
  verticale (↑ montée / ↓ descente) et la PTU (température, humidité, pression)
  extraites de `info` ; leur trajet GPX porte l'élévation (profil d'altitude).
- **Traces de déplacement** : une polyligne relie les positions successives
  d'un objet mobile (ex. avion de A vers B), de la couleur du type, l'icône
  restant sur la dernière position. Case **« Afficher les traces »** pour les
  activer/désactiver. Les objets fixes (stations météo) ne tracent rien.
- **Historique persistant** : chaque position distincte est horodatée en base
  (`SdrObjectTrack`). Les traces **survivent au rechargement** (la carte se
  charge avec les `SDR_TRACK_DISPLAY_POINTS` dernières positions par objet) et
  restent exploitables même après la purge TTL de l'objet vivant.
- **Rejeu & export** (depuis le popup d'un objet) :
  - **▷ Rejouer** : animation du trajet enregistré, avec un curseur rouge et
    un bouton « ⏹ Arrêter le rejeu ».
  - **⤓ GPX** : téléchargement du trajet au format **GPX 1.1** (avec altitude),
    ouvrable dans QGIS, GPXSee, Google Earth, etc.
- **Popups** détaillés (position, vitesse, altitude, MMSI/ICAO, météo, info).

### Hors-ligne

- Le **service worker** met en cache l'application et **toutes les tuiles
  consultées** au fil de la navigation.
- Bouton **« Mettre la zone en cache »** : précharge les tuiles du fond actif
  sur la zone visible + 2 niveaux de zoom, pour une consultation **sans
  réseau**. Un badge `HORS-LIGNE` s'affiche quand la connexion est perdue ;
  les tuiles déjà en cache restent affichées.
- Bouton **« Vider »** : efface le cache de tuiles.

> Premier passage **en ligne** obligatoire pour amorcer le cache (chargement de
> l'app + tuiles). Ensuite la carte se charge et s'affiche hors-ligne.

---

## 7. Intégration dans la plateforme ADRASEC 06 existante

Ce projet est autonome, mais peut devenir une **app** de votre projet Django :

1. Copiez le dossier `map/` dans votre projet.
2. Ajoutez `"channels"`, `"daphne"` et `"map"` à `INSTALLED_APPS`, définissez
   `ASGI_APPLICATION` et `CHANNEL_LAYERS` (voir `sdr_map/settings.py`).
3. Branchez `map.urls` dans vos `urls` et `map.routing` dans votre `asgi.py`.
4. La management command devient `python manage.py listen_sdr` dans votre projet.

Le style suit la même logique que vos commandes `listen_pocsag` / `listen_ais`.

---

## 8. Production

- Servez derrière Nginx (reverse proxy vers Daphne ; pensez au `proxy_pass`
  WebSocket sur `/ws/`).
- Lancez `python manage.py collectstatic` : WhiteNoise servira alors les
  statiques (compressés) depuis `STATIC_ROOT`. Vous pouvez aussi les faire
  servir directement par Nginx si vous préférez.
- Pour plusieurs workers/process, utilisez Redis comme couche Channels :
  `pip install channels-redis` puis exportez `SDR_MAP_REDIS_URL=redis://127.0.0.1:6379/0`.
- `SDR_MAP_DEBUG=0`, renseignez `SDR_MAP_ALLOWED_HOSTS` et `SDR_MAP_SECRET_KEY`.

### Base de données et écritures concurrentes

Plusieurs modules SDR++ écrivent en parallèle (un thread par connexion). SQLite
n'autorisant qu'un écrivain à la fois, le projet active le **mode WAL** + un
**délai d'attente sur verrou** (`busy_timeout`) et **sérialise** les écritures
côté collecteur (verrou + transaction atomique). C'est ce qui évite l'erreur
`database is locked` (et les déconnexions de modules qu'elle provoquait). Des
fichiers `db.sqlite3-wal` / `-shm` peuvent apparaître : c'est normal. Pour un
très grand nombre de modules / un débit très élevé, passez à **PostgreSQL**
(changez `DATABASES['default']`), qui gère nativement la concurrence.

### Variables d'environnement utiles

| Variable | Rôle | Défaut |
|---|---|---|
| `SDR_MAP_TTL_MINUTES` | durée de vie des objets | `30` |
| `SDR_MAP_TRACK_HOURS` | rétention de l'historique des trajets (h) | `24` |
| `SDR_MAP_TRACK_POINTS` | positions par objet renvoyées au chargement | `80` |
| `SDR_MAP_CENTER_LAT/LON` | centre de la carte | `43.70 / 7.25` |
| `SDR_MAP_ZOOM` | zoom initial | `9` |
| `SDR_MAP_ALLOWED_HOSTS` | hôtes autorisés | `localhost,127.0.0.1,…` |
| `SDR_MAP_REDIS_URL` | couche Channels Redis | (InMemory) |

### Endpoints HTTP

| URL | Rôle |
|---|---|
| `/` | la carte |
| `/api/objects/[?types=AIS,APRS]` | objets vivants + trace récente |
| `/api/track/?type=…&ident=…[&format=gpx][&limit=N]` | trajet (JSON ou GPX) |
| `/sw.js` | service worker (cache hors-ligne) |
| `/ws/objects/` | WebSocket temps réel |
```
