"""
tasks_server - Task list web page for the ESP32 dashboard
---------------------------------------------------------
Serves a small web page where you manage your pending tasks, keeps them in
pc/data/tasks.json, and pushes them to the board in every one of its WiFi windows
(POST /tasks, format in src/model/tasks.h). Tasks finished or deleted with the BOOT
button on the board come back in the answer to that push and are applied here.

Runs forever; install_tasks.ps1 starts it at sign-in. Open the page at
http://localhost:8765, or from the phone at http://<this PC's IP>:8765.

Usage (from the pc/ folder):
    python -m tasks_server              # Serve the page and push to the board
    python -m tasks_server --no-push    # Only serve the page (no board needed)

Settings live in pc/config.ini (see config.example.ini).
"""

import argparse
import logging
import sys
import threading
from http.server import ThreadingHTTPServer

from rlcd.config import DATA_DIR, load_config
from rlcd.logs import setup_logging

from .delivery import BoardStatus, push_loop
from .store import TaskStore
from .web import Handler

log = logging.getLogger("tasks_server")


def main() -> None:
    parser = argparse.ArgumentParser(prog="tasks_server", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--no-push", action="store_true", help="only serve the page, do not talk to the board")
    parser.add_argument("--debug", action="store_true", help="log every request")
    args = parser.parse_args()

    setup_logging("tasks_server", debug=args.debug)
    config = load_config()
    tasks_cfg = config["tasks"]
    esp_cfg = config["esp32"]
    token = tasks_cfg.get("token", "").strip()
    if not args.no_push:
        if not token:
            sys.exit("[tasks] token is not set in config.ini: it must match TASKS_TOKEN in the board's secrets.h.")
        if not esp_cfg.get("host"):
            sys.exit("[esp32] host is not set in config.ini.")

    store = TaskStore(DATA_DIR / "tasks.json")
    status = BoardStatus(enabled=not args.no_push)
    Handler.store = store
    Handler.status = status
    Handler.web_token = tasks_cfg.get("web_token", "").strip()

    bind = tasks_cfg.get("bind", "0.0.0.0")
    port = int(tasks_cfg.get("port", "8765"))
    try:
        server = ThreadingHTTPServer((bind, port), Handler)
    except OSError as e:
        sys.exit(f"Cannot listen on {bind}:{port}: {e}")
    log.info("Task page on http://%s:%d (%d tasks)", "localhost" if bind == "0.0.0.0" else bind, port,
             len(store.all()))
    if not Handler.web_token and bind != "127.0.0.1":
        log.warning("No [tasks] web_token: anyone on the network can edit the tasks")

    stop = threading.Event()
    if not args.no_push:
        threading.Thread(target=push_loop, args=(store, esp_cfg, token, status, stop), daemon=True).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        server.server_close()
