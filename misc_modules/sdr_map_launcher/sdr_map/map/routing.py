from django.urls import path

from . import consumers

websocket_urlpatterns = [
    path("ws/objects/", consumers.SdrConsumer.as_asgi()),
]
