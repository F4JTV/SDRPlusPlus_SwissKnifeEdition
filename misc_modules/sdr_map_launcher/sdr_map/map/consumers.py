"""Consumer WebSocket : diffuse en direct les objets SDR vers les cartes ouvertes."""
import json

from channels.generic.websocket import AsyncWebsocketConsumer

GROUP_NAME = "sdr_objects"


class SdrConsumer(AsyncWebsocketConsumer):
    async def connect(self):
        await self.channel_layer.group_add(GROUP_NAME, self.channel_name)
        await self.accept()

    async def disconnect(self, code):
        await self.channel_layer.group_discard(GROUP_NAME, self.channel_name)

    # Réception d'un message côté client (non utilisé : flux descendant seul).
    async def receive(self, text_data=None, bytes_data=None):
        pass

    # Handler appelé par group_send(type="sdr.object", ...) depuis le listener.
    async def sdr_object(self, event):
        await self.send(text_data=json.dumps(
            {"event": "object", "object": event["object"]},
            ensure_ascii=False,
        ))

    # Handler de suppression (objet expiré / purgé).
    async def sdr_remove(self, event):
        await self.send(text_data=json.dumps(
            {"event": "remove", "id": event["id"]},
            ensure_ascii=False,
        ))
