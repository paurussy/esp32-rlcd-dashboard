"""
calendar_bridge - Pushes today's Outlook (Teams) meetings to the ESP32 dashboard
--------------------------------------------------------------------------------
Reads today's meetings on this PC, with no Azure app, OAuth or admin consent,
and POSTs them to the board in the format described in src/model/calendar.h.
install_tasks.ps1 runs it with --once at :59, :14, :29 and :44, when the board's
WiFi window opens (the board keeps WiFi off the rest of the time).

Sources ([outlook] source in config.ini): the new Outlook for Windows (default,
reads its offline cache) or the classic Outlook (COM). See outlook/__init__.py.

Usage (from the pc/ folder):
    python -m calendar_bridge --once       # Push once and exit (scheduled task)
    python -m calendar_bridge              # Run forever, pushing in every WiFi window
    python -m calendar_bridge --dry-run    # Print today's events, send nothing

Settings live in pc/config.ini (see config.example.ini).
"""

import argparse
import logging
import sys
import time
from datetime import datetime, timedelta

from rlcd.board import push_in_window, window_bounds
from rlcd.config import load_config
from rlcd.logs import setup_logging

from .errors import BridgeError
from .events import Event, build_body
from .outlook import read_today_events

log = logging.getLogger("calendar_bridge")


def print_events(events: list[Event]) -> None:
    if not events:
        print("No meetings today.")
    for e in events:
        start = datetime.fromtimestamp(e.start).strftime("%H:%M")
        end = datetime.fromtimestamp(e.end).strftime("%H:%M")
        where = f"  [{e.location}]" if e.location else ""
        print(f"{start}-{end}  {e.subject}{where}")


def main() -> None:
    parser = argparse.ArgumentParser(prog="calendar_bridge", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--once", action="store_true", help="push once and exit")
    parser.add_argument("--dry-run", action="store_true", help="print today's events, send nothing")
    parser.add_argument("--debug", action="store_true", help="show what is read and why items are skipped")
    args = parser.parse_args()

    if args.dry_run:
        logging.basicConfig(format="%(message)s", level=logging.DEBUG if args.debug else logging.WARNING)
        sys.stdout.reconfigure(encoding="utf-8")  # Accents when piped (cp1252 otherwise)
        config = load_config(required=False)
        try:
            print_events(read_today_events(config["outlook"]))
        except BridgeError as e:
            sys.exit(str(e))
        return

    setup_logging("calendar_bridge", debug=args.debug)
    config = load_config()
    cfg = config["esp32"]
    token = cfg.get("token", "")
    if not token:
        sys.exit("[esp32] token is not set in config.ini: it must match CALENDAR_TOKEN in the board's secrets.h.")

    while True:
        if not args.once:  # Resident: wake up so that Outlook is read right when the window opens
            opens, _ = window_bounds(datetime.now())
            read_lead = timedelta(seconds=cfg.getint("read_lead_s", 40))  # Reading takes ~20 s
            wait = (opens - read_lead - datetime.now()).total_seconds()
            if wait > 0:
                time.sleep(wait)
        try:
            events = read_today_events(config["outlook"])
            push_in_window(cfg, build_body(events), "/calendar", token)
        except BridgeError as e:
            log.error("%s", e)

        if args.once:
            return
        _, closes = window_bounds(datetime.now())
        time.sleep(max((closes - datetime.now()).total_seconds(), 0) + 1)  # Past the end of this window
