"""
events.py - A meeting, and the body the board receives (format in src/model/calendar.h)
"""

import logging
import time
import zlib
from dataclasses import dataclass
from datetime import datetime

from rlcd.board import CALENDAR_MAX_EVENTS, EVENT_LOCATION_BYTES, EVENT_SUBJECT_BYTES, fit_utf8, make_body

log = logging.getLogger(__name__)


@dataclass
class Event:
    id: int
    start: int  # UTC epoch seconds
    end: int
    subject: str
    location: str
    modified: int = 0  # Last modification (epoch seconds); only used to pick between duplicates
    uid: str = ""      # Same for every copy (and every occurrence) of one meeting


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
        uid=uid or "",
    )


def _same_meeting(a: Event, b: Event) -> bool:
    """Two copies of one meeting: same subject, and either the same meeting id (moved, with the old
    time still in a stale cache) or overlapping times (deleted and created again)."""
    if " ".join(a.subject.split()).casefold() != " ".join(b.subject.split()).casefold() or not a.subject.strip():
        return False
    return (a.uid and a.uid == b.uid) or (a.start < b.end and b.start < a.end)


def keep_latest_per_subject(events: list[Event]) -> list[Event]:
    """Drops duplicate copies of a meeting: the most recently modified one wins.

    A meeting that was moved, or deleted and created again, may still show up twice in a
    stale cache. Only copies with the same subject that are the same meeting or overlap
    are merged: two separate meetings with the same name ("Focus time" in the morning and
    in the afternoon) are both kept. Events without a subject are never merged.
    """
    kept: list[Event] = []
    for e in sorted(events, key=lambda e: (e.modified, e.start), reverse=True):  # Latest first
        twin = next((k for k in kept if _same_meeting(k, e)), None)
        if twin is not None:
            log.debug("  duplicate '%s': dropping the copy at %s (keeping %s)", e.subject,
                      datetime.fromtimestamp(e.start).strftime("%H:%M"),
                      datetime.fromtimestamp(twin.start).strftime("%H:%M"))
            continue
        kept.append(e)
    return sorted(kept, key=lambda e: e.start)


def build_body(events: list[Event], now: float | None = None) -> str:
    """The text POSTed to the board's /calendar.

    Meetings already over are left out (they would only take the board's slots), at most
    CALENDAR_MAX_EVENTS are sent, and each text is cut to what the board stores.
    """
    now = time.time() if now is None else now
    upcoming = sorted((e for e in events if e.end > now), key=lambda e: e.start)
    if len(upcoming) > CALENDAR_MAX_EVENTS:
        log.warning("%d meetings left today: the board keeps the first %d", len(upcoming), CALENDAR_MAX_EVENTS)
    lines = [f"{e.id}\t{e.start}\t{e.end}\t{fit_utf8(e.subject, EVENT_SUBJECT_BYTES)}\t"
             f"{fit_utf8(e.location, EVENT_LOCATION_BYTES)}" for e in upcoming[:CALENDAR_MAX_EVENTS]]
    return make_body(lines)
