#!/usr/bin/env python
"""Utilitaire en ligne de commande Django pour le projet SDR Map."""
import os
import sys


def main():
    os.environ.setdefault("DJANGO_SETTINGS_MODULE", "sdr_map.settings")
    try:
        from django.core.management import execute_from_command_line
    except ImportError as exc:
        raise ImportError(
            "Django ne semble pas installé. Activez votre environnement "
            "virtuel puis : pip install -r requirements.txt"
        ) from exc
    execute_from_command_line(sys.argv)


if __name__ == "__main__":
    main()
