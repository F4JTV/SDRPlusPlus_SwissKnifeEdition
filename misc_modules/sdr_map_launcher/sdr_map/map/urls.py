from django.urls import path

from . import views

urlpatterns = [
    path("", views.index, name="index"),
    path("api/objects/", views.api_objects, name="api_objects"),
    path("api/track/", views.api_track, name="api_track"),
    path("api/retention/", views.api_retention, name="api_retention"),
    # Aircraft enrichment database: report status and trigger Mictronics
    # refresh from the side panel's "Aircraft DB" section.
    path("api/aircraft_db/status", views.api_aircraft_db_status,
         name="api_aircraft_db_status"),
    path("api/aircraft_db/update", views.api_aircraft_db_update,
         name="api_aircraft_db_update"),
    # Service worker servi à la racine pour avoir le scope '/'.
    path("sw.js", views.service_worker, name="service_worker"),
]
