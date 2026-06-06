from django.urls import path

from . import views

urlpatterns = [
    path("", views.index, name="index"),
    path("api/objects/", views.api_objects, name="api_objects"),
    path("api/track/", views.api_track, name="api_track"),
    path("api/retention/", views.api_retention, name="api_retention"),
    # Service worker servi à la racine pour avoir le scope '/'.
    path("sw.js", views.service_worker, name="service_worker"),
]
