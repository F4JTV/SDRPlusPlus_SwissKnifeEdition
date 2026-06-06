"""
Configuration Django du projet SDR Map (ADRASEC 06).

Carte temps réel des objets décodés par les modules SDR++ (AIS, ADS-B, APRS),
reçus via un unique serveur TCP et diffusés au navigateur par WebSocket.
"""
from pathlib import Path
import os

BASE_DIR = Path(__file__).resolve().parent.parent

# --------------------------------------------------------------------------- #
#  Sécurité — à adapter en production
# --------------------------------------------------------------------------- #
SECRET_KEY = os.environ.get(
    "SDR_MAP_SECRET_KEY",
    "dev-key-CHANGEZ-MOI-en-production-0123456789abcdef",
)
DEBUG = os.environ.get("SDR_MAP_DEBUG", "1") == "1"

# Hôtes autorisés (ajoutez votre domaine, ex. adrasec06.ddns.net)
ALLOWED_HOSTS = os.environ.get(
    "SDR_MAP_ALLOWED_HOSTS", "localhost,127.0.0.1,0.0.0.0,[::1]"
).split(",")

CSRF_TRUSTED_ORIGINS = [
    o for o in os.environ.get("SDR_MAP_CSRF_TRUSTED", "").split(",") if o
]

# --------------------------------------------------------------------------- #
#  Applications
# --------------------------------------------------------------------------- #
INSTALLED_APPS = [
    "daphne",                 # serveur ASGI (doit précéder staticfiles)
    "django.contrib.admin",
    "django.contrib.auth",
    "django.contrib.contenttypes",
    "django.contrib.sessions",
    "django.contrib.messages",
    "django.contrib.staticfiles",
    "channels",               # WebSocket
    "map",                    # notre application carte
]

MIDDLEWARE = [
    "django.middleware.security.SecurityMiddleware",
    # WhiteNoise sert les fichiers statiques sous Daphne (serveur ASGI pur qui,
    # contrairement à runserver, ne les sert pas tout seul). Doit être placé
    # juste après SecurityMiddleware.
    "whitenoise.middleware.WhiteNoiseMiddleware",
    "django.contrib.sessions.middleware.SessionMiddleware",
    "django.middleware.common.CommonMiddleware",
    "django.middleware.csrf.CsrfViewMiddleware",
    "django.contrib.auth.middleware.AuthenticationMiddleware",
    "django.contrib.messages.middleware.MessageMiddleware",
    "django.middleware.clickjacking.XFrameOptionsMiddleware",
]

ROOT_URLCONF = "sdr_map.urls"

TEMPLATES = [
    {
        "BACKEND": "django.template.backends.django.DjangoTemplates",
        "DIRS": [],
        "APP_DIRS": True,
        "OPTIONS": {
            "context_processors": [
                "django.template.context_processors.debug",
                "django.template.context_processors.request",
                "django.contrib.auth.context_processors.auth",
                "django.contrib.messages.context_processors.messages",
            ],
        },
    },
]

# Django sert l'app en WSGI (admin) ET en ASGI (WebSocket via Channels).
WSGI_APPLICATION = "sdr_map.wsgi.application"
ASGI_APPLICATION = "sdr_map.asgi.application"

# --------------------------------------------------------------------------- #
#  Couche Channels (WebSocket)
#  Par défaut : InMemory (mono-processus, parfait pour un poste opérateur).
#  En production multi-process, utilisez Redis (channels_redis) :
#     CHANNEL_LAYERS = {"default": {
#         "BACKEND": "channels_redis.core.RedisChannelLayer",
#         "CONFIG": {"hosts": [("127.0.0.1", 6379)]}}}
# --------------------------------------------------------------------------- #
if os.environ.get("SDR_MAP_REDIS_URL"):
    CHANNEL_LAYERS = {
        "default": {
            "BACKEND": "channels_redis.core.RedisChannelLayer",
            "CONFIG": {"hosts": [os.environ["SDR_MAP_REDIS_URL"]]},
        }
    }
else:
    CHANNEL_LAYERS = {
        "default": {"BACKEND": "channels.layers.InMemoryChannelLayer"}
    }

# --------------------------------------------------------------------------- #
#  Base de données
# --------------------------------------------------------------------------- #
DATABASES = {
    "default": {
        "ENGINE": "django.db.backends.sqlite3",
        "NAME": BASE_DIR / "db.sqlite3",
        # SQLite n'autorise qu'un écrivain à la fois ; le collecteur TCP traite
        # chaque module dans un thread séparé. Sans réglage, deux écritures
        # simultanées lèvent « database is locked » (et déconnectent le module).
        "OPTIONS": {
            # Attendre jusqu'à 30 s qu'un verrou se libère au lieu d'échouer.
            "timeout": 30,
            "init_command": (
                "PRAGMA journal_mode=WAL;"      # lecteurs et écrivain concurrents
                "PRAGMA synchronous=NORMAL;"    # bon compromis vitesse/sûreté en WAL
                "PRAGMA busy_timeout=30000;"    # 30 s d'attente sur verrou
            ),
        },
    }
}

# --------------------------------------------------------------------------- #
#  Divers
# --------------------------------------------------------------------------- #
AUTH_PASSWORD_VALIDATORS = []

LANGUAGE_CODE = "fr-fr"
TIME_ZONE = "Europe/Paris"
USE_I18N = True
USE_TZ = True

STATIC_URL = "static/"
STATIC_ROOT = BASE_DIR / "staticfiles"

# WhiteNoise : sert les statiques directement depuis les apps (utile en dev,
# sans avoir à lancer collectstatic). En production, lancez tout de même
# `collectstatic` ; WhiteNoise servira alors depuis STATIC_ROOT.
WHITENOISE_USE_FINDERS = True

STORAGES = {
    "default": {
        "BACKEND": "django.core.files.storage.FileSystemStorage",
    },
    "staticfiles": {
        "BACKEND": "whitenoise.storage.CompressedStaticFilesStorage",
    },
}

DEFAULT_AUTO_FIELD = "django.db.models.BigAutoField"

# --------------------------------------------------------------------------- #
#  Paramètres applicatifs SDR Map
# --------------------------------------------------------------------------- #
# Durée (minutes) au-delà de laquelle un objet non rafraîchi est considéré
# obsolète et masqué de la carte / purgé.
SDR_OBJECT_TTL_MINUTES = int(os.environ.get("SDR_MAP_TTL_MINUTES", "30"))

# Centre et zoom par défaut de la carte (Côte d'Azur / ADRASEC 06).
SDR_MAP_CENTER_LAT = float(os.environ.get("SDR_MAP_CENTER_LAT", "43.70"))
SDR_MAP_CENTER_LON = float(os.environ.get("SDR_MAP_CENTER_LON", "7.25"))
SDR_MAP_DEFAULT_ZOOM = int(os.environ.get("SDR_MAP_ZOOM", "9"))

# Historique des trajets (table SdrObjectTrack).
# Rétention : durée (heures) au-delà de laquelle les points de trajet sont
# purgés, indépendamment du TTL des objets vivants.
SDR_TRACK_RETENTION_HOURS = int(os.environ.get("SDR_MAP_TRACK_HOURS", "24"))
# Nombre de positions récentes renvoyées par /api/objects/ pour redessiner la
# trace au chargement (le rejeu/GPX utilisent l'historique complet).
SDR_TRACK_DISPLAY_POINTS = int(os.environ.get("SDR_MAP_TRACK_POINTS", "80"))
