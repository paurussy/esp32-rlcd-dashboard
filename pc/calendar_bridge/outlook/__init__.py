"""
Where today's meetings come from, picked with `source` in the [outlook] section of config.ini:
  new      The new Outlook for Windows (default): its offline cache (new.py).
  classic  The classic Outlook desktop app, through COM (classic.py).
"""

import configparser
from datetime import datetime, timedelta
from pathlib import Path

from ..errors import BridgeError
from ..events import Event, keep_latest_per_subject


def source_fingerprint(outlook_cfg: configparser.SectionProxy) -> str | None:
    """Something that changes whenever the meetings may have changed, cheap to compute: the
    names, sizes and times of the new Outlook's cache files. None when there is no such thing
    (classic Outlook): read every time."""
    if outlook_cfg.get("source", "new").strip().lower() != "new":
        return None
    from . import new
    indexeddb_dir = Path(outlook_cfg.get("indexeddb_dir", "").strip() or new.INDEXEDDB_DIR)
    parts = []
    for folder in (indexeddb_dir / f"{new.ORIGIN}.leveldb", indexeddb_dir / f"{new.ORIGIN}.blob"):
        try:
            for path in sorted(folder.rglob("*")):
                stat = path.stat()
                parts.append(f"{path.name}:{stat.st_size}:{stat.st_mtime_ns}")
        except OSError:
            return None
    return f"{datetime.now():%Y-%m-%d}|" + "|".join(parts)  # A new day reads again (another "today")


def read_today_events(outlook_cfg: configparser.SectionProxy) -> list[Event]:
    day_start = datetime.now().replace(hour=0, minute=0, second=0, microsecond=0)
    day_end = day_start + timedelta(days=1)

    source = outlook_cfg.get("source", "new").strip().lower()
    if source == "classic":
        from . import classic  # Imports Windows-only modules, and pywin32 if installed
        events = classic.read_events(day_start, day_end)
    elif source == "new":
        from . import new
        indexeddb_dir = Path(outlook_cfg.get("indexeddb_dir", "").strip() or new.INDEXEDDB_DIR)
        day = (int(day_start.timestamp()), int(day_end.timestamp()))
        events = new.read_events(day, indexeddb_dir, outlook_cfg.get("mailbox", "").strip())
    else:
        raise BridgeError(f"Unknown [outlook] source '{source}': use 'new' or 'classic'")
    return keep_latest_per_subject(events)
