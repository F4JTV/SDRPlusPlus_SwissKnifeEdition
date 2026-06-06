"""
Point d'entrée ASGI : HTTP classique (Django) + WebSocket (Channels).
"""
import os

from django.core.asgi import get_asgi_application

os.environ.setdefault("DJANGO_SETTINGS_MODULE", "sdr_map.settings")

# IMPORTANT : initialiser l'application Django AVANT d'importer les modules
# qui touchent aux modèles / routing.
django_asgi_app = get_asgi_application()

from channels.routing import ProtocolTypeRouter, URLRouter  # noqa: E402
from channels.security.websocket import AllowedHostsOriginValidator  # noqa: E402

import map.routing  # noqa: E402

application = ProtocolTypeRouter(
    {
        "http": django_asgi_app,
        "websocket": AllowedHostsOriginValidator(
            URLRouter(map.routing.websocket_urlpatterns)
        ),
    }
)
