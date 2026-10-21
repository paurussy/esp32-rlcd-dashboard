"""
board.py - Talking to the board: its WiFi windows and the pushes
----------------------------------------------------------------
Shared by calendar_bridge (POST /calendar) and tasks_server (POST /tasks).
"""

import configparser
import logging
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta

log = logging.getLogger(__name__)

# The board only has WiFi on in short windows (WIFI_WINDOW_* in config.h): it opens
# OPEN_BEFORE_MIN before each multiple of WINDOW_PERIOD_MIN (:59, :14, :29, :44)
# and closes CLOSE_AFTER_MIN after it (:01, :16, :31, :46).
WINDOW_PERIOD_MIN = 15
OPEN_BEFORE_MIN = 1
CLOSE_AFTER_MIN = 1
CLOSE_MARGIN_S = 5  # Stop trying this long before the board closes
# Only slots from ACTIVE_FROM_HOUR:00 up to (not including) ACTIVE_UNTIL_HOUR:00 have a
# window (WIFI_ACTIVE_* in config.h): the board keeps WiFi off the rest of the day.
ACTIVE_FROM_HOUR = 8
ACTIVE_UNTIL_HOUR = 17


def push(cfg: configparser.SectionProxy, body: str, path: str, token: str) -> str:
    """POSTs `body` to the board (settings in the [esp32] section) and returns its answer."""
    url = f"http://{cfg['host']}:{cfg.getint('port', 80)}{path}"
    request = urllib.request.Request(
        url,
        data=body.encode("utf-8"),
        method="POST",
        headers={"X-Token": token, "Content-Type": "text/plain; charset=utf-8"},
    )
    try:
        with urllib.request.urlopen(request, timeout=cfg.getint("timeout_s", 4)) as response:
            answer = response.read().decode(errors="replace").strip()
    except urllib.error.HTTPError as e:  # The board answered, but refused: say why
        reason = e.read().decode(errors="replace").strip()
        raise OSError(f"the board answered {e.code}: {reason}") from e
    log.info("Pushed to %s: %s", url, answer.splitlines()[0] if answer else "")
    return answer


def in_active_hours(now: datetime) -> bool:
    """Whether the board may have WiFi on at all at this time of day."""
    return ACTIVE_FROM_HOUR <= now.hour < ACTIVE_UNTIL_HOUR


def window_bounds(now: datetime) -> tuple[datetime, datetime]:
    """Start and end of the window closest to `now` (the current one, else the next active one)."""
    slot = now.replace(minute=0, second=0, microsecond=0) - timedelta(hours=1)
    while True:
        slot += timedelta(minutes=WINDOW_PERIOD_MIN)
        if not ACTIVE_FROM_HOUR <= slot.hour < ACTIVE_UNTIL_HOUR:
            continue
        opens = slot - timedelta(minutes=OPEN_BEFORE_MIN)
        closes = slot + timedelta(minutes=CLOSE_AFTER_MIN)
        if now < closes:
            return opens, closes


def push_in_window(cfg: configparser.SectionProxy, body: str, path: str, token: str) -> str | None:
    """POST until the board answers or its WiFi window closes. Returns its answer, None if not delivered."""
    retry_s = cfg.getint("retry_every_s", 5)
    opens, closes = window_bounds(datetime.now())
    deadline = closes - timedelta(seconds=CLOSE_MARGIN_S)

    if 0 < (opens - datetime.now()).total_seconds() <= 60:  # Early by a few seconds
        time.sleep((opens - datetime.now()).total_seconds())

    if datetime.now() < opens:  # Manual run: the board is asleep, so one attempt only
        log.warning("Outside the board's WiFi window (next one %s-%s): trying once",
                    opens.strftime("%H:%M"), closes.strftime("%H:%M"))
        try:
            return push(cfg, body, path, token)
        except (urllib.error.URLError, OSError) as e:
            log.error("Could not reach the board: %s", e)
            return None

    last_error = None
    while datetime.now() < deadline:
        try:
            return push(cfg, body, path, token)
        except (urllib.error.URLError, OSError) as e:
            last_error = e
            time.sleep(retry_s)
    log.error("Could not reach the board before its WiFi window closed at %s: %s",
              closes.strftime("%H:%M:%S"), last_error)
    return None
