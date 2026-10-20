"""
events.py - A meeting, and the body the board receives (format in src/model/calendar.h)
"""

import logging
import time
import zlib
from dataclasses import dataclass
from datetime import datetime

log = logging.getLogger(__name__)


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


def build_body(events: list[Event]) -> str:
    """The text POSTed to the board's /calendar."""
    lines = [f"v1\t{int(time.time())}"]
    lines += [f"{e.id}\t{e.start}\t{e.end}\t{e.subject}\t{e.location}" for e in events]
    return "\n".join(lines) + "\n"
