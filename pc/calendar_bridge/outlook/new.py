"""
new.py - New Outlook for Windows (IndexedDB offline cache)
----------------------------------------------------------
The new Outlook is a WebView2 app: its offline data lives in the IndexedDB of
outlook.office.com. Each mailbox has an "owa-offline-data-<id>" database whose
"events" store holds calendar items, recurring meetings already expanded into
occurrences. Unofficial format: an Outlook update may change it.
"""

import logging
import os
import subprocess
import sys
from datetime import datetime
from pathlib import Path

from ..errors import BridgeError
from ..events import Event, make_event

log = logging.getLogger(__name__)

INDEXEDDB_DIR = Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft/Olk/EBWebView/Default/IndexedDB"
ORIGIN = "https_outlook.office.com_0.indexeddb"
DATA_DB_PREFIX = "owa-offline-data-"
# Outlook refreshes the cached calendar much less often than mail (hours apart
# have been seen): meetings deleted or moved since then are still in the cache.
CACHE_STALE_MIN = 30


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


def skip_reason(item: dict, start: int, end: int, day: tuple[int, int],
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


def is_running() -> bool:
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
    if age_min > CACHE_STALE_MIN:
        log.warning("Outlook last refreshed its cached calendar at %s (%d min ago): meetings "
                    "deleted or moved since then may still show", synced_at.strftime("%H:%M"), age_min)
    else:
        log.info("Outlook calendar cache refreshed at %s", synced_at.strftime("%H:%M"))


def read_events(day: tuple[int, int], indexeddb_dir: Path, mailbox: str) -> list[Event]:
    """Today's meetings. `day` is the start and end of today as epoch seconds."""
    try:
        from ccl_chromium_reader import ccl_chromium_indexeddb
    except ImportError as e:
        raise BridgeError(f"ccl_chromium_reader is missing for {sys.executable}: "
                          "run python -m pip install -r requirements.txt") from e

    leveldb_dir = indexeddb_dir / f"{ORIGIN}.leveldb"
    blob_dir = indexeddb_dir / f"{ORIGIN}.blob"
    if not leveldb_dir.is_dir():
        raise BridgeError(f"New Outlook data not found in {leveldb_dir}: open the new Outlook once")
    if not is_running():
        log.warning("The new Outlook is not running: its cached calendar may be out of date")

    events = {}
    try:
        # Opens the files read-only; Outlook can keep running
        idb = ccl_chromium_indexeddb.WrappedIndexDB(str(leveldb_dir), str(blob_dir) if blob_dir.is_dir() else None)
        for db_id in idb.database_ids:
            if not db_id.name.startswith(DATA_DB_PREFIX):
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
                reason = skip_reason(item, start, end, day, mailbox, item_mailbox)
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
