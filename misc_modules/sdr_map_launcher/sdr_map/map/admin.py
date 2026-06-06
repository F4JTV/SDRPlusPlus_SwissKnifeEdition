from django.contrib import admin

from .models import SdrObject, SdrObjectTrack, RetentionSetting


@admin.register(SdrObject)
class SdrObjectAdmin(admin.ModelAdmin):
    list_display = (
        "obj_type", "ident", "name", "lat", "lon",
        "speed", "altitude_ft", "last_seen",
    )
    list_filter = ("obj_type",)
    search_fields = ("ident", "name", "icao", "mmsi", "info")
    readonly_fields = ("first_seen", "last_seen")
    ordering = ("-last_seen",)


@admin.register(SdrObjectTrack)
class SdrObjectTrackAdmin(admin.ModelAdmin):
    list_display = ("obj_type", "ident", "lat", "lon", "altitude_ft", "timestamp")
    list_filter = ("obj_type",)
    search_fields = ("ident",)
    ordering = ("-timestamp",)


@admin.register(RetentionSetting)
class RetentionSettingAdmin(admin.ModelAdmin):
    list_display = ("obj_type", "minutes", "updated_at")
    list_editable = ("minutes",)
    ordering = ("obj_type",)
