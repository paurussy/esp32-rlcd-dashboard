"""
calendar_bridge - Pushes today's Outlook (Teams) meetings to the ESP32 dashboard
--------------------------------------------------------------------------------
Reads today's meetings on this PC, with no Azure app, OAuth or admin consent,
and POSTs them to the board in the format described in src/model/calendar.h.
install_tasks.ps1 runs it with --once at :59, :14, :29 and :44, when the board's
WiFi window opens (the board keeps WiFi off the rest of the time). When the board's
power saving is off ([esp32] power_saving = false) it runs every minute at any hour,
asks the board what it holds and only pushes when that differs (a board that restarted
gets everything again); Outlook's cache is only read again when its files changed.

Sources ([outlook] source in config.ini): the new Outlook for Windows (default,
reads its offline cache) or the classic Outlook (COM). See outlook/__init__.py.

Usage (from the pc/ folder):
    python -m calendar_bridge --once       # Push once and exit (scheduled task)
    python -m calendar_bridge              # Run forever, pushing in every WiFi window (or on changes)
    python -m calendar_bridge --dry-run    # Print today's events, send nothing

Settings live in pc/config.ini (see config.example.ini).
"""

import argparse
import json
import logging
import sys
import time
from dataclasses import asdict
from datetime import datetime, timedelta

from rlcd.board import (ALWAYS_ON_PERIOD_S, BoardRefused, forget_last_push, in_active_hours, power_saving,
                        push_if_changed, push_in_window, window_bounds)
from rlcd.config import DATA_DIR, load_config
from rlcd.logs import setup_logging

from .errors import BridgeError
from .events import Event, build_body
from .outlook import read_today_events, source_fingerprint

log = logging.getLogger("calendar_bridge")

CACHE_PATH = DATA_DIR / "calendar_cache.json"
FORCE_READ_MIN = 15  # Read Outlook at least this often, even if its files look unchanged


def print_events(events: list[Event]) -> None:
    if not events:
        print("No meetings today.")
    for e in events:
        start = datetime.fromtimestamp(e.start).strftime("%H:%M")
        end = datetime.fromtimestamp(e.end).strftime("%H:%M")
        where = f"  [{e.location}]" if e.location else ""
        print(f"{start}-{end}  {e.subject}{where}")


def read_events_cached(outlook_cfg) -> list[Event]:
    """Today's meetings, from data/calendar_cache.json when Outlook's files have not changed since
    the last read (reading them takes ~20 s of CPU; without power saving this runs every minute)."""
    fingerprint = source_fingerprint(outlook_cfg)
    try:
        cache = json.loads(CACHE_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        cache = {}
    fresh = time.time() - cache.get("read_at", 0) < FORCE_READ_MIN * 60
    if fingerprint is not None and cache.get("fingerprint") == fingerprint and fresh:
        try:
            events = [Event(**e) for e in cache["events"]]
            log.debug("Outlook unchanged since %s: %d meetings from the cache",
                      datetime.fromtimestamp(cache["read_at"]).strftime("%H:%M"), len(events))
            return events
        except (KeyError, TypeError):
            pass

    events = read_today_events(outlook_cfg)
    try:
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        CACHE_PATH.write_text(json.dumps({"fingerprint": fingerprint, "read_at": int(time.time()),
                                          "events": [asdict(e) for e in events]}), encoding="utf-8")
    except OSError as e:
        log.warning("Could not save %s: %s", CACHE_PATH, e)
    return events


def push_changes(config, once: bool, token: str, resync: bool = False) -> None:
    """Board always on (power saving off): push if the board's meetings differ, now and, unless
    `once`, every minute at any hour."""
    cfg = config["esp32"]
    if resync:
        forget_last_push("/calendar")  # Only matters for a board firmware without /status
    while True:
        started = time.monotonic()
        try:
            events = read_events_cached(config["outlook"])
            if push_if_changed(cfg, build_body(events), "/calendar", token) is None:
                log.warning("The board did not answer")
        except BridgeError as e:
            log.error("%s", e)
        except BoardRefused as e:
            log.error("%s", e)
        if once:
            return
        time.sleep(max(ALWAYS_ON_PERIOD_S - (time.monotonic() - started), 1))


def main() -> None:
    parser = argparse.ArgumentParser(prog="calendar_bridge", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--once", action="store_true", help="push once and exit")
    parser.add_argument("--dry-run", action="store_true", help="print today's events, send nothing")
    parser.add_argument("--resync", action="store_true",
                        help="full sync: push even if unchanged (only needed with a board firmware "
                             "older than GET /status; newer ones report what they hold)")
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

    if not power_saving(cfg):
        push_changes(config, args.once, token, args.resync)
        return

    while True:
        if args.once:
            now = datetime.now()
            opens, _ = window_bounds(now)
            # Scheduled run at night: nobody is listening (the 07:58:40 run is just before the first window)
            if not in_active_hours(now) and (opens - now).total_seconds() > 60:
                log.info("Outside the board's active hours: nothing to push")
                return
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
        except BoardRefused as e:
            log.error("%s", e)

        if args.once:
            return
        _, closes = window_bounds(datetime.now())
        time.sleep(max((closes - datetime.now()).total_seconds(), 0) + 1)  # Past the end of this window
