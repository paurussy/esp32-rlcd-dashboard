"""
teams_bridge.py - Pushes today's Outlook (Teams) meetings to the ESP32 dashboard
--------------------------------------------------------------------------------
Reads today's meetings on this PC, with no Azure app, OAuth or admin consent,
and POSTs them to the board in the format described in src/calendar.h.
install_task.ps1 runs it with --once every 15 minutes.

Sources ([outlook] source in config.ini):
  new      The new Outlook for Windows (default). Reads its offline cache, a
           Chromium IndexedDB database, read-only. The cache is refreshed while
           the new Outlook is running.
  classic  The classic Outlook desktop app, through COM.

Usage:
    python teams_bridge.py --once       # Push once and exit (scheduled task)
    python teams_bridge.py              # Run forever, every `interval_s` seconds
    python teams_bridge.py --dry-run    # Print today's events, send nothing

Settings live in config.ini next to this script (see config.example.ini).
"""

import argparse
import configparser
import logging
import logging.handlers
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
import winreg
import zlib
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path

try:  # Only needed for source = classic
    import pythoncom
    import pywintypes
    import win32com.client
except ImportError:
    pywintypes = None

HERE = Path(__file__).resolve().parent
CONFIG_PATH = HERE / "config.ini"
LOG_PATH = HERE / "teams_bridge.log"

log = logging.getLogger("teams_bridge")


class BridgeError(Exception):
    """The calendar could not be read; the message says what to check."""


@dataclass
class Event:
    id: int
    start: int  # UTC epoch seconds
    end: int
    subject: str
    location: str
    modified: int = 0  # Last modification (epoch seconds); only used to pick between duplicates


def clean(text: str, limit: int) -> str:
    """Single line, no tabs, bounded length (the board truncates further)."""
    return " ".join((text or "").split())[:limit]


def make_event(uid: str, start: int, end: int, subject: str, location: str, modified: int = 0) -> Event:
    return Event(
        id=zlib.crc32(f"{uid}|{start}".encode()),  # Stable across pushes
        start=start,
        end=end,
        subject=clean(subject, 120),
        location=clean(location, 60),
        modified=modified,
    )


def keep_latest_per_subject(events: list[Event]) -> list[Event]:
    """One event per subject: the most recently modified one wins.

    A meeting that was deleted and created again, or whose old copy is still in
    a stale cache, shows up twice with the same name; only the latest is kept.
    Events without a subject are never merged.
    """
    latest: dict[str, Event] = {}
    for e in events:
        key = " ".join(e.subject.split()).casefold() or f"\0{e.id}"
        kept = latest.get(key)
        if kept is None or (e.modified, e.start) > (kept.modified, kept.start):
            if kept is not None:
                log.debug("  duplicate '%s': dropping the copy at %s", e.subject,
                          datetime.fromtimestamp(kept.start).strftime("%H:%M"))
            latest[key] = e
        else:
            log.debug("  duplicate '%s': dropping the copy at %s", e.subject,
                      datetime.fromtimestamp(e.start).strftime("%H:%M"))
    return sorted(latest.values(), key=lambda e: e.start)


# ============================================================
#                     NEW OUTLOOK (IndexedDB)
# ============================================================
# The new Outlook is a WebView2 app: its offline data lives in the IndexedDB of
# outlook.office.com. Each mailbox has an "owa-offline-data-<id>" database whose
# "events" store holds calendar items, recurring meetings already expanded into
# occurrences. Unofficial format: an Outlook update may change it.
OLK_INDEXEDDB_DIR = Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft/Olk/EBWebView/Default/IndexedDB"
OLK_ORIGIN = "https_outlook.office.com_0.indexeddb"
OLK_DATA_DB_PREFIX = "owa-offline-data-"
# Outlook refreshes the cached calendar much less often than mail (hours apart
# have been seen): meetings deleted or moved since then are still in the cache.
OLK_CACHE_STALE_MIN = 30


def js_text(value) -> str:
    """V8 one-byte strings may come back as Latin-1 bytes."""
    if isinstance(value, bytes):
        return value.decode("latin-1")
    return value if isinstance(value, str) else ""


def iso_utc_epoch(value) -> int:
    """'2026-10-20T07:00:00.000Z' -> epoch seconds."""
    return int(datetime.fromisoformat(js_text(value).replace("Z", "+00:00")).timestamp())


def iso_utc_epoch_or_zero(value) -> int:
    try:
        return iso_utc_epoch(value)
    except ValueError:
        return 0


def new_outlook_skip_reason(item: dict, start: int, end: int, day: tuple[int, int],
                            mailbox: str, item_mailbox: str) -> str | None:
    if mailbox and item_mailbox.lower() != mailbox.lower():
        return f"other mailbox ({item_mailbox})"
    if not (start < day[1] and end > day[0]):
        return "not today"
    if item.get("IsAllDayEvent"):
        return "all-day"
    if item.get("IsCancelled"):
        return "cancelled"
    if js_text(item.get("ResponseType")) == "Decline":
        return "declined"
    if item.get("IsDraft"):
        return "draft"
    return None


def new_outlook_running() -> bool:
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq olk.exe", "/NH"], capture_output=True,
                             text=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW).stdout
    except (OSError, subprocess.SubprocessError):
        return True  # Unknown: don't warn
    return "olk.exe" in out.lower()


def events_synced_at(db) -> datetime | None:
    """When Outlook last refreshed this mailbox's cached calendar."""
    for record in db["syncState"].iterate_records(live_only=True):
        if record.key.value == "events" and isinstance(record.value, dict):
            ms = record.value.get("lastSuccessfulSyncTime")
            if isinstance(ms, (int, float)):
                return datetime.fromtimestamp(ms / 1000)
    return None


def log_cache_age(mailbox_db: str, synced_at: datetime | None) -> None:
    if synced_at is None:
        log.warning("%s: unknown calendar cache age", mailbox_db)
        return
    age_min = (datetime.now() - synced_at).total_seconds() / 60
    if age_min > OLK_CACHE_STALE_MIN:
        log.warning("Outlook last refreshed its cached calendar at %s (%d min ago): meetings "
                    "deleted or moved since then may still show", synced_at.strftime("%H:%M"), age_min)
    else:
        log.info("Outlook calendar cache refreshed at %s", synced_at.strftime("%H:%M"))


def read_new_outlook(day: tuple[int, int], indexeddb_dir: Path, mailbox: str) -> list[Event]:
    try:
        from ccl_chromium_reader import ccl_chromium_indexeddb
    except ImportError as e:
        raise BridgeError(f"ccl_chromium_reader is missing for {sys.executable}: "
                          "run python -m pip install -r requirements.txt") from e

    leveldb_dir = indexeddb_dir / f"{OLK_ORIGIN}.leveldb"
    blob_dir = indexeddb_dir / f"{OLK_ORIGIN}.blob"
    if not leveldb_dir.is_dir():
        raise BridgeError(f"New Outlook data not found in {leveldb_dir}: open the new Outlook once")
    if not new_outlook_running():
        log.warning("The new Outlook is not running: its cached calendar may be out of date")

    events = {}
    try:
        # Opens the files read-only; Outlook can keep running
        idb = ccl_chromium_indexeddb.WrappedIndexDB(str(leveldb_dir), str(blob_dir) if blob_dir.is_dir() else None)
        for db_id in idb.database_ids:
            if not db_id.name.startswith(OLK_DATA_DB_PREFIX):
                continue
            db = idb[db_id.dbid_no]
            if "events" not in db.object_store_names:
                continue
            records = list(db["events"].iterate_records(live_only=True))
            if not records:
                continue  # Mailbox without a cached calendar
            log_cache_age(db_id.name, events_synced_at(db))
            for record in records:
                item = record.value
                if not isinstance(item, dict) or "Start" not in item:
                    continue
                try:
                    start, end = iso_utc_epoch(item["Start"]), iso_utc_epoch(item["End"])
                except (KeyError, ValueError):
                    continue
                item_mailbox = js_text(item.get("ItemId", {}).get("mailboxInfo", {}).get("mailboxSmtpAddress"))
                reason = new_outlook_skip_reason(item, start, end, day, mailbox, item_mailbox)
                if reason != "not today":
                    log.debug("  %s  %s  %s -> %s", "skip" if reason else "keep",
                              js_text(item.get("Subject")), js_text(item["Start"]), reason or "ok")
                if reason is None:
                    event = make_event(js_text(item.get("UID")) or js_text(item.get("id")), start, end,
                                       js_text(item.get("Subject")), js_text(item.get("Location")),
                                       iso_utc_epoch_or_zero(item.get("LastModifiedTime")))
                    events[event.id] = event
    except BridgeError:
        raise
    except Exception as e:  # noqa: BLE001 - unofficial format, or a file mid-write
        raise BridgeError(f"Could not read the new Outlook data ({type(e).__name__}: {e})") from e

    return sorted(events.values(), key=lambda e: e.start)


# ============================================================
#                     CLASSIC OUTLOOK (COM)
# ============================================================
OL_FOLDER_CALENDAR = 9
OL_MEETING_CANCELED = (5, 7)  # olMeetingCanceled, olMeetingReceivedAndCanceled
OL_RESPONSE_DECLINED = 4

# Windows short date tokens -> strftime ("%#d" = day without leading zero)
DATE_TOKENS = {"yyyy": "%Y", "yy": "%y", "MM": "%m", "M": "%#m", "dd": "%d", "d": "%#d"}


def short_date_format() -> str:
    """Windows short date format as a strftime pattern.

    Items.Restrict parses dates with the user's locale, so "10/20/2026" only
    works on US systems; build the string the same way Outlook expects it.
    """
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Control Panel\International") as key:
            pattern, _ = winreg.QueryValueEx(key, "sShortDate")
    except OSError:
        return "%m/%d/%Y"
    return re.sub(r"yyyy|yy|MM|M|dd|d", lambda m: DATE_TOKENS[m.group()], pattern)


def utc_epoch(com_time) -> int:
    """pywin32 labels COM dates as UTC whatever they are; StartUTC/EndUTC really are UTC."""
    return int(datetime(com_time.year, com_time.month, com_time.day, com_time.hour,
                        com_time.minute, com_time.second, tzinfo=timezone.utc).timestamp())


def classic_skip_reason(item, start: int, end: int, day: tuple[int, int]) -> str | None:
    if item.AllDayEvent:
        return "all-day"
    if item.MeetingStatus in OL_MEETING_CANCELED:
        return "cancelled"
    if item.ResponseStatus == OL_RESPONSE_DECLINED:
        return "declined"
    if not (start < day[1] and end > day[0]):
        return "not today"  # The Restrict date string was read with another locale
    return None


def collect_today(found, day: tuple[int, int]) -> list[Event]:
    events = []
    # With IncludeRecurrences, Count is meaningless: walk with GetFirst/GetNext
    item = found.GetFirst()
    while item is not None:
        try:
            start, end = utc_epoch(item.StartUTC), utc_epoch(item.EndUTC)
            reason = classic_skip_reason(item, start, end, day)
            log.debug("  %s  %s  start=%s startUTC=%s -> %s", "skip" if reason else "keep",
                      item.Subject, item.Start, item.StartUTC, reason or "ok")
            if reason is None:
                try:
                    uid = item.GlobalAppointmentID
                except pywintypes.com_error:
                    uid = item.EntryID
                events.append(make_event(uid, start, end, item.Subject, item.Location,
                                         utc_epoch(item.LastModificationTime)))  # Only compared between items
        except (pywintypes.com_error, AttributeError) as e:
            log.warning("Skipping unreadable item: %s", e)
        item = found.GetNext()
    return events


def log_calendars(namespace, calendar) -> None:
    log.debug("Reading: %s (store %s)", calendar.FolderPath, calendar.Store.DisplayName)
    for store in namespace.Stores:
        try:
            folder = store.GetDefaultFolder(OL_FOLDER_CALENDAR)
            log.debug("  available calendar: %s (%d items)", folder.FolderPath, folder.Items.Count)
        except pywintypes.com_error:
            log.debug("  store without calendar: %s", store.DisplayName)


def read_classic_outlook(day_start: datetime, day_end: datetime) -> list[Event]:
    if pywintypes is None:
        raise BridgeError(f"pywin32 is missing for {sys.executable}: "
                          "run python -m pip install -r requirements.txt")
    day = (int(day_start.timestamp()), int(day_end.timestamp()))
    try:
        pythoncom.CoInitialize()
        outlook = win32com.client.Dispatch("Outlook.Application")
        namespace = outlook.GetNamespace("MAPI")
        calendar = namespace.GetDefaultFolder(OL_FOLDER_CALENDAR)
        log_calendars(namespace, calendar)

        items = calendar.Items
        # Order matters: sort on Start first, then ask for recurring occurrences
        items.Sort("[Start]")
        items.IncludeRecurrences = True

        # Restrict reads dates with the Windows locale, which is easy to get wrong.
        # Try the user's short date format first, then common ones; collect_today()
        # drops anything a misread date lets through.
        formats = dict.fromkeys([short_date_format(), "%m/%d/%Y", "%d/%m/%Y", "%Y-%m-%d"])
        for fmt in formats:
            fmt += " %H:%M"
            query = f"[Start] < '{day_end.strftime(fmt)}' AND [End] > '{day_start.strftime(fmt)}'"
            log.debug("Restrict: %s", query)
            try:
                events = collect_today(items.Restrict(query), day)
            except pywintypes.com_error as e:
                log.debug("  rejected by Outlook: %s", e)
                continue
            if events:
                return sorted(events, key=lambda e: e.start)
        return []
    except pywintypes.com_error as e:
        raise BridgeError(f"Classic Outlook error (is it installed and signed in?): {e}") from e


# ============================================================
#                           SOURCES
# ============================================================
def read_today_events(outlook_cfg: configparser.SectionProxy) -> list[Event]:
    day_start = datetime.now().replace(hour=0, minute=0, second=0, microsecond=0)
    day_end = day_start + timedelta(days=1)

    source = outlook_cfg.get("source", "new").strip().lower()
    if source == "classic":
        events = read_classic_outlook(day_start, day_end)
    elif source == "new":
        indexeddb_dir = Path(outlook_cfg.get("indexeddb_dir", "").strip() or OLK_INDEXEDDB_DIR)
        day = (int(day_start.timestamp()), int(day_end.timestamp()))
        events = read_new_outlook(day, indexeddb_dir, outlook_cfg.get("mailbox", "").strip())
    else:
        raise BridgeError(f"Unknown [outlook] source '{source}': use 'new' or 'classic'")
    return keep_latest_per_subject(events)


# ============================================================
#                           ESP32
# ============================================================
def build_body(events: list[Event]) -> str:
    lines = [f"v1\t{int(time.time())}"]
    lines += [f"{e.id}\t{e.start}\t{e.end}\t{e.subject}\t{e.location}" for e in events]
    return "\n".join(lines) + "\n"


def push(cfg: configparser.SectionProxy, body: str) -> None:
    url = f"http://{cfg['host']}:{cfg.getint('port', 80)}/calendar"
    request = urllib.request.Request(
        url,
        data=body.encode("utf-8"),
        method="POST",
        headers={"X-Token": cfg.get("token", ""), "Content-Type": "text/plain; charset=utf-8"},
    )
    with urllib.request.urlopen(request, timeout=cfg.getint("timeout_s", 8)) as response:
        log.info("Pushed to %s: %s", url, response.read().decode(errors="replace").strip())


# ============================================================
#                            MAIN
# ============================================================
def print_events(events: list[Event]) -> None:
    if not events:
        print("No meetings today.")
    for e in events:
        start = datetime.fromtimestamp(e.start).strftime("%H:%M")
        end = datetime.fromtimestamp(e.end).strftime("%H:%M")
        where = f"  [{e.location}]" if e.location else ""
        print(f"{start}-{end}  {e.subject}{where}")


def setup_logging() -> None:
    if log.level == logging.NOTSET:  # Not set to DEBUG by --debug
        log.setLevel(logging.INFO)
    fmt = logging.Formatter("%(asctime)s %(levelname)s %(message)s")
    file_handler = logging.handlers.RotatingFileHandler(LOG_PATH, maxBytes=512_000, backupCount=2,
                                                        encoding="utf-8")
    file_handler.setFormatter(fmt)
    log.addHandler(file_handler)
    if sys.stdout is not None:  # pythonw has no console
        console = logging.StreamHandler(sys.stdout)
        console.setFormatter(fmt)
        log.addHandler(console)


def load_config(required: bool) -> configparser.ConfigParser:
    config = configparser.ConfigParser()
    if not config.read(CONFIG_PATH, encoding="utf-8") and required:
        sys.exit(f"Missing {CONFIG_PATH}: copy config.example.ini and edit it.")
    for section in ("esp32", "outlook"):
        if not config.has_section(section):
            config.add_section(section)
    return config


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--once", action="store_true", help="push once and exit")
    parser.add_argument("--dry-run", action="store_true", help="print today's events, send nothing")
    parser.add_argument("--debug", action="store_true", help="show what is read and why items are skipped")
    args = parser.parse_args()

    if args.debug:
        log.setLevel(logging.DEBUG)
    if args.dry_run:
        logging.basicConfig(format="%(message)s")  # Console only, no log file
        sys.stdout.reconfigure(encoding="utf-8")  # Accents when piped (cp1252 otherwise)
        config = load_config(required=False)
        try:
            print_events(read_today_events(config["outlook"]))
        except BridgeError as e:
            sys.exit(str(e))
        return

    setup_logging()
    config = load_config(required=True)
    cfg = config["esp32"]
    interval = cfg.getint("interval_s", 60)
    keepalive = cfg.getint("keepalive_s", 300)

    last_events, last_push = None, 0.0
    while True:
        try:
            events = read_today_events(config["outlook"])
            # Push on any change, and now and then anyway so the board knows the PC is alive
            if events != last_events or time.monotonic() - last_push >= keepalive:
                push(cfg, build_body(events))
                last_events, last_push = events, time.monotonic()
        except BridgeError as e:
            log.error("%s", e)
        except (urllib.error.URLError, OSError) as e:
            log.error("Could not reach the board: %s", e)

        if args.once:
            return
        time.sleep(interval)


if __name__ == "__main__":
    main()
