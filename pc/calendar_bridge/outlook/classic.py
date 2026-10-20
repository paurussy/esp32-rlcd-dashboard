"""
classic.py - Classic Outlook desktop app (COM automation)
---------------------------------------------------------
"""

import logging
import re
import sys
import winreg
from datetime import datetime, timezone

try:  # pywin32 is only needed for source = classic
    import pythoncom
    import pywintypes
    import win32com.client
except ImportError:
    pywintypes = None

from ..errors import BridgeError
from ..events import Event, make_event

log = logging.getLogger(__name__)

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


def skip_reason(item, start: int, end: int, day: tuple[int, int]) -> str | None:
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
            reason = skip_reason(item, start, end, day)
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


def read_events(day_start: datetime, day_end: datetime) -> list[Event]:
    """Today's meetings, between `day_start` and `day_end`."""
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
