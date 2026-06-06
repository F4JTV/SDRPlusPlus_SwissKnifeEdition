# map/management/commands/runserver_sdr.py
#
# All-in-one launcher: web server (Daphne) AND TCP collector in a SINGLE
# process. Web side: map + WebSocket + static files (via WhiteNoise). TCP side:
# the same listen_sdr collector.
#
# Why a single process? The default Channels layer (InMemoryChannelLayer) is
# NOT shared between processes. If the web server and the TCP collector run
# separately, positions received over TCP never reach the browser in real time.
# By running them in the same process, they share the same layer: aircraft
# move live, without having to install Redis.
#
#   python manage.py runserver_sdr
#   python manage.py runserver_sdr --web-host 0.0.0.0 --web-port 8000 \
#                                  --tcp-host 0.0.0.0 --tcp-port 10100
#
# For multi-process production, prefer Redis (SDR_MAP_REDIS_URL) with
# `daphne` + `listen_sdr` running separately.

import threading

from django.core.management.base import BaseCommand


class Command(BaseCommand):
    help = ("Run the web server (Daphne) AND the TCP collector in a single "
            "process for real-time updates without Redis.")

    def add_arguments(self, parser):
        parser.add_argument("--web-host", default="0.0.0.0",
                            help="Web listen address (default: 0.0.0.0)")
        parser.add_argument("--web-port", type=int, default=8000,
                            help="Web port (default: 8000)")
        parser.add_argument("--tcp-host", default="0.0.0.0",
                            help="TCP listen address for modules (default: 0.0.0.0)")
        parser.add_argument("--tcp-port", type=int, default=10100,
                            help="Single TCP port for modules (default: 10100)")
        parser.add_argument("--quiet", action="store_true",
                            help="Reduce collector verbosity")

    def handle(self, *args, **opts):
        # Start the TCP collector in a background thread (same process =>
        # same Channels layer as the web server).
        tcp_thread = threading.Thread(
            target=self._run_collector,
            kwargs={
                "host": opts["tcp_host"],
                "port": opts["tcp_port"],
                "quiet": opts["quiet"],
            },
            daemon=True,
            name="sdr-tcp-collector",
        )
        tcp_thread.start()

        self.stdout.write(self.style.SUCCESS(
            "\n+----------------------------------------------------------+"))
        self.stdout.write(self.style.SUCCESS(
            "|  SDR MAP - all-in-one (web + TCP collector, 1 process)   |"))
        self.stdout.write(self.style.SUCCESS(
            "+----------------------------------------------------------+"))
        self.stdout.write(
            f"  Map     : http://{opts['web_host']}:{opts['web_port']}/")
        self.stdout.write(
            f"  Modules : TCP {opts['tcp_host']}:{opts['tcp_port']} "
            "(point each SDR++ module's TCP output here)")
        self.stdout.write("  Real-time enabled (shared InMemory layer). Press Ctrl+C to stop.\n")

        # Launch Daphne (blocking) in the main thread.
        self._run_web(opts["web_host"], opts["web_port"])

    # ------------------------------------------------------------------ #
    def _run_collector(self, host, port, quiet):
        """Run the listen_sdr logic in this process."""
        from django.core.management import call_command
        try:
            call_command("listen_sdr", host=host, port=port, quiet=quiet)
        except Exception as exc:  # don't kill the web server if TCP dies
            self.stderr.write(self.style.ERROR(
                f"TCP collector stopped: {exc}"))

    def _run_web(self, host, port):
        """Start the Daphne ASGI server on the project's application."""
        try:
            from daphne.server import Server
            from daphne.endpoints import build_endpoint_description_strings
        except ImportError:
            raise SystemExit(
                "Daphne is not installed. Run: pip install -r requirements.txt")

        # Project's ASGI application (HTTP + WebSocket).
        from sdr_map.asgi import application

        endpoints = build_endpoint_description_strings(host=host, port=port)
        Server(
            application=application,
            endpoints=endpoints,
            signal_handlers=True,
            verbosity=1,
        ).run()
