# LRRP → carte Django : intégration côté serveur

Le module `dsd_decoder` envoie chaque position LRRP/GPS décodée comme **une
ligne JSON** (terminée par `\n`) vers le **même serveur TCP** que les modules
AIS / ADS-B / APRS — par défaut `IP:10100`.

## Format émis

```json
{"name":"RID 2081371","date":"2026-05-28","time":"20:30:05",
 "lat":43.5123,"lon":7.0211,"type":"lrrp","speed":null,
 "source":"2081371","info":"DMRA LRRP lat=43.5123 lon=7.0211 RID=2081371"}
```

Champs :

| Champ    | Contenu                                                            |
|----------|-------------------------------------------------------------------|
| `name`   | `RID <source>` si la source est connue, sinon `LRRP-<instance>`   |
| `date`   | `YYYY-MM-DD` (heure locale du PC SDR++)                            |
| `time`   | `HH:MM:SS`                                                         |
| `lat`    | latitude décimale (nombre)                                        |
| `lon`    | longitude décimale (nombre)                                       |
| `type`   | toujours `"lrrp"`                                                  |
| `speed`  | `null` (LRRP ne fournit pas de vitesse)                           |
| `source` | RID DMR émetteur (présent seulement si décodé)                    |
| `info`   | ligne brute décodée par dsd-fme (pour debug / affichage)          |

Seules les positions **avec lat ET lon** sont envoyées.

## Dispatch dans `listen_sdr`

Si ton collecteur range déjà les objets par `type` (comme convenu pour AIS /
ADSB / APRS), il suffit d'ajouter `"lrrp"` à la liste des types acceptés. Le
schéma de base étant identique, aucune logique spéciale n'est nécessaire :

```python
# management/commands/listen_sdr.py  (extrait)

KNOWN_TYPES = {"AIS", "ADSB", "APRS", "APRS Meteo", "lrrp"}

def handle_object(obj):
    typ = obj.get("type")
    if typ not in KNOWN_TYPES:
        return  # type inconnu, ignoré

    # Identité stable : pour le LRRP on prend la source RID si dispo,
    # sinon le name. (AIS=mmsi, ADSB=icao, APRS=name, lrrp=source/name.)
    if typ == "lrrp":
        ident = obj.get("source") or obj.get("name")
    elif typ == "AIS":
        ident = obj.get("mmsi")
    elif typ == "ADSB":
        ident = obj.get("icao")
    else:
        ident = obj.get("name")

    SdrObject.objects.update_or_create(
        obj_type=typ,
        ident=str(ident),
        defaults={
            "name": obj.get("name", ""),
            "lat": obj["lat"],
            "lon": obj["lon"],
            "speed": obj.get("speed"),
            "info": obj.get("info", ""),
            "last_seen": timezone.now(),
        },
    )
    # ... puis group_send WebSocket comme pour les autres types.
```

## Côté carte (Leaflet)

Ajoute un calque / une icône pour `type === "lrrp"` (ex. un marqueur balise).
Le `name` (`RID …`) sert d'étiquette, et `info` peut alimenter le popup.

```js
const LRRP_ICON = L.divIcon({className: 'lrrp-marker', html: '📟'});

function styleFor(type) {
    if (type === 'lrrp') return { icon: LRRP_ICON, label: 'LRRP / GPS DMR' };
    // ... AIS / ADSB / APRS existants
}
```

## Test sans radio

```bash
printf '%s\n' '{"name":"RID 2081371","date":"2026-05-28","time":"20:30:05","lat":43.5123,"lon":7.0211,"type":"lrrp","speed":null,"source":"2081371","info":"test"}' \
  | nc 127.0.0.1 10100
```

La balise doit apparaître sur la carte et se mettre à jour si tu renvoies la
même `source` avec une nouvelle position.
