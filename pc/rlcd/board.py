"""
board.py - Talking to the board: its WiFi windows, its status and the pushes
----------------------------------------------------------------------------
Shared by calendar_bridge (POST /calendar) and tasks_server (POST /tasks).

Every push body starts with a header line (format in src/model/push_body.h):
    v2  <pc_now_epoch>  <content_hash>  [<extra fields>]
The board keeps the hash of what it holds and reports it in GET /status, so the PC
compares hashes instead of remembering what it sent: a board that restarted (and lost
everything, it lives in RAM) or a push whose answer got lost are noticed and fixed in
the next check.
"""

import configparser
import hashlib
import json
import logging
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta
from pathlib import Path

from .config import DATA_DIR

log = logging.getLogger(__name__)

# ---------------- Board limits (must match config.h and src/model/*.h) ----------------
# The board keeps at most this much; anything more is wasted bytes on the air (and the
# body must stay under MAX_BODY_BYTES in src/net/push_server.cpp).
TASKS_MAX_ITEMS = 20         # TASKS_MAX_ITEMS in config.h
TASK_TEXT_BYTES = 63         # Task::text[64] in src/model/tasks.h
CALENDAR_MAX_EVENTS = 16     # CALENDAR_MAX_EVENTS in config.h
EVENT_SUBJECT_BYTES = 63     # CalendarEvent::subject[64] in src/model/calendar.h
EVENT_LOCATION_BYTES = 31    # CalendarEvent::location[32]

# ---------------- WiFi windows ----------------
# The board only has WiFi on in short windows (WIFI_WINDOW_* in config.h): it opens
# OPEN_BEFORE_MIN before each multiple of WINDOW_PERIOD_MIN (:59, :14, :29, :44)
# and closes CLOSE_AFTER_MIN after it (:01, :16, :31, :46), for the slots from
# ACTIVE_FROM_HOUR:00 up to (not including) ACTIVE_UNTIL_HOUR:00 (WIFI_ACTIVE_* in config.h).
# These are the defaults: once the board has answered GET /status, the values it reports
# are remembered in data/board_settings.json and used instead, so config.h is the only
# place to change them.
DEFAULT_WINDOWS = {"period": 15, "before": 1, "after": 1, "from_hour": 8, "until_hour": 17}
CLOSE_MARGIN_S = 5  # Stop trying this long before the board closes

# With power saving off (POWER_SAVING in config.h, power_saving in the [esp32] section
# of config.ini) the board keeps WiFi on 24/7: no windows. The PC checks every minute
# (and the tasks server right after each change) and pushes only what the board lacks.
ALWAYS_ON_PERIOD_S = 60
ALWAYS_ON_RETRY_FOR_S = 50  # Keep retrying a push this long, then wait for the next minute

# push_if_changed's answer when the board already holds the same content
UNCHANGED = "(unchanged, not sent)"

SETTINGS_PATH = DATA_DIR / "board_settings.json"


class BoardRefused(Exception):
    """The board answered, but refused the request (bad token, malformed body...).

    Not worth retrying: the same request gets the same answer.
    """

    def __init__(self, code: int, reason: str, hint: str = ""):
        self.code = code
        self.reason = reason
        self.hint = hint
        super().__init__(f"the board answered {code}: {reason}" + (f" ({hint})" if hint else ""))


def power_saving(cfg: configparser.SectionProxy) -> bool:
    """Whether the board uses WiFi windows ([esp32] power_saving, default true)."""
    return cfg.getboolean("power_saving", True)


# ============================================================
#                         BODIES
# ============================================================
def fit_utf8(text: str, max_bytes: int) -> str:
    """`text` cut to at most `max_bytes` bytes of UTF-8, never in the middle of a character."""
    data = text.encode("utf-8")
    if len(data) <= max_bytes:
        return text
    return data[:max_bytes].decode("utf-8", errors="ignore")


def content_hash(content: str) -> str:
    return hashlib.sha256(content.encode("utf-8")).hexdigest()[:16]


def make_body(lines: list[str], extra: tuple = ()) -> str:
    """A push body: the v2 header (PC time, hash of the content, `extra` fields) and `lines`.

    The hash covers the extra fields too (the board shows them: the number of pending tasks).
    """
    content = "".join(line + "\n" for line in lines)
    extra = [str(e) for e in extra]
    key = content_hash("\t".join(extra) + "\n" + content if extra else content)
    header = "\t".join(["v2", str(int(time.time())), key, *extra])
    return header + "\n" + content


def body_hash(body: str) -> str:
    """The content hash of a body: the one in its v2 header, else computed."""
    header, _, content = body.partition("\n")
    fields = header.split("\t")
    if fields[0] == "v2" and len(fields) > 2:
        return fields[2]
    return content_hash(content)


def _restamp(body: str) -> str:
    """The body with the PC time of its header set to now (the board may set its clock from it)."""
    header, sep, content = body.partition("\n")
    fields = header.split("\t")
    if len(fields) > 1 and fields[0] in ("v1", "v2"):
        fields[1] = str(int(time.time()))
    return "\t".join(fields) + sep + content


# ============================================================
#                       HTTP REQUESTS
# ============================================================
def _url(cfg: configparser.SectionProxy, path: str) -> str:
    return f"http://{cfg['host']}:{cfg.getint('port', 80)}{path}"


def _refusal(e: urllib.error.HTTPError, path: str) -> BoardRefused:
    reason = e.read().decode(errors="replace").strip()
    hint = ""
    if e.code == 401:
        hint = "the token in config.ini does not match the one in the board's secrets.h"
    elif e.code == 404:
        hint = f"the board has no {path}: is its token set in secrets.h, and the firmware up to date?"
    elif e.code == 413:
        hint = "the body is larger than the board accepts"
    return BoardRefused(e.code, reason, hint)


def as_v1(body: str) -> str:
    """The body with the v1 header of older board firmware (no hash, no extra fields)."""
    header, sep, content = body.partition("\n")
    fields = header.split("\t")
    if fields[0] != "v2":
        return body
    return f"v1\t{fields[1] if len(fields) > 1 else int(time.time())}" + sep + content


# Hosts whose firmware refused a v2 body: sent v1 from then on (until this process restarts,
# so a reflashed board gets v2 again)
_v1_hosts: set[str] = set()


def push(cfg: configparser.SectionProxy, body: str, path: str, token: str) -> str:
    """POSTs `body` to the board (settings in the [esp32] section) and returns its answer.

    A board with an older firmware refuses the v2 header (400): the body is sent again with
    a v1 header. Raises BoardRefused when the board answers with an error, OSError/URLError
    when it cannot be reached.
    """
    host = cfg["host"]
    if host in _v1_hosts:
        return _post(cfg, as_v1(body), path, token)
    try:
        return _post(cfg, body, path, token)
    except BoardRefused as e:
        if e.code != 400 or as_v1(body) == body:
            raise
        log.warning("The board refused the v2 format: its firmware is older, sending v1 (flash the new "
                    "firmware to get the hash checks)")
        answer = _post(cfg, as_v1(body), path, token)
        _v1_hosts.add(host)
        return answer


def _post(cfg: configparser.SectionProxy, body: str, path: str, token: str) -> str:
    url = _url(cfg, path)
    request = urllib.request.Request(
        url,
        data=_restamp(body).encode("utf-8"),
        method="POST",
        headers={"X-Token": token, "Content-Type": "text/plain; charset=utf-8"},
    )
    try:
        with urllib.request.urlopen(request, timeout=cfg.getint("timeout_s", 4)) as response:
            answer = response.read().decode(errors="replace").strip()
    except urllib.error.HTTPError as e:  # The board answered, but refused: say why
        if e.code >= 500:
            raise OSError(f"the board answered {e.code}") from e
        raise _refusal(e, path) from e
    log.info("Pushed to %s: %s", url, answer.splitlines()[0] if answer else "")
    return answer


def get_status(cfg: configparser.SectionProxy, token: str) -> dict | None:
    """GET /status: what the board holds. None when the board cannot be reached, {} when its
    firmware predates /status. Raises BoardRefused for a bad token.

    The answer is one "key<TAB>value[<TAB>value...]" per line; values come back as strings
    (a single one) or lists (several).
    """
    request = urllib.request.Request(_url(cfg, "/status"), headers={"X-Token": token})
    try:
        with urllib.request.urlopen(request, timeout=cfg.getint("timeout_s", 4)) as response:
            text = response.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return {}
        if e.code >= 500:
            return None
        raise _refusal(e, "/status") from e
    except (urllib.error.URLError, OSError) as e:
        log.debug("Board status not available: %s", e)
        return None

    status = {}
    for line in text.splitlines():
        key, *values = line.split("\t")
        if key:
            status[key] = values[0] if len(values) == 1 else values
    if status and cfg["host"] in _v1_hosts:
        # It answers /status: it was flashed with the new firmware while this program ran
        log.info("The board's firmware now has /status: sending the v2 format again")
        _v1_hosts.discard(cfg["host"])
    _remember_settings(cfg, status)
    return status


# ============================================================
#                     WINDOW SETTINGS
# ============================================================
_settings: dict | None = None
_mismatch_warned = False


def _window_settings() -> dict:
    """The board's window settings: last reported by the board, else DEFAULT_WINDOWS."""
    global _settings
    if _settings is None:
        _settings = dict(DEFAULT_WINDOWS)
        try:
            saved = json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
            _settings.update({k: int(v) for k, v in saved.items() if k in DEFAULT_WINDOWS})
        except (OSError, ValueError, TypeError):
            pass
        if _settings["period"] <= 0:
            _settings = dict(DEFAULT_WINDOWS)
    return _settings


def _remember_settings(cfg: configparser.SectionProxy, status: dict) -> None:
    """Keeps the window settings the board reported; warns if its power saving differs from config.ini."""
    global _settings, _mismatch_warned
    windows = status.get("windows")
    if isinstance(windows, list) and len(windows) == len(DEFAULT_WINDOWS):
        try:
            reported = dict(zip(DEFAULT_WINDOWS, map(int, windows)))
        except ValueError:
            reported = None
        if reported and reported["period"] > 0 and reported != _window_settings():
            _settings = reported
            try:
                DATA_DIR.mkdir(parents=True, exist_ok=True)
                SETTINGS_PATH.write_text(json.dumps(reported), encoding="utf-8")
                log.info("Board WiFi windows: %s", reported)
            except OSError as e:
                log.warning("Could not save %s: %s", SETTINGS_PATH, e)

    board_saving = status.get("power_saving")
    if board_saving in ("0", "1") and (board_saving == "1") != power_saving(cfg) and not _mismatch_warned:
        _mismatch_warned = True
        log.warning("The board has POWER_SAVING %s but config.ini has power_saving = %s: make them match "
                    "(and run install_tasks.ps1 again)", "on" if board_saving == "1" else "off",
                    power_saving(cfg))


def in_active_hours(now: datetime, cfg: configparser.SectionProxy | None = None) -> bool:
    """Whether the board may have WiFi on at all at this time of day (always without power saving)."""
    if cfg is not None and not power_saving(cfg):
        return True
    s = _window_settings()
    return s["from_hour"] <= now.hour < s["until_hour"]


def window_bounds(now: datetime) -> tuple[datetime, datetime]:
    """Start and end of the window closest to `now` (the current one, else the next active one).

    Same rule as windowActiveAt() in the sketch: slots are the multiples of the period
    counted from midnight, within the active hours.
    """
    s = _window_settings()
    period, before, after = s["period"], s["before"], s["after"]
    midnight = now.replace(hour=0, minute=0, second=0, microsecond=0)
    for day in range(3):
        base = midnight + timedelta(days=day)
        for slot_min in range(0, 24 * 60, period):
            if not s["from_hour"] * 60 <= slot_min < s["until_hour"] * 60:
                continue
            slot = base + timedelta(minutes=slot_min)
            closes = slot + timedelta(minutes=after)
            if now < closes:
                return slot - timedelta(minutes=before), closes
    raise ValueError(f"No WiFi window in the board settings: {s}")


# ============================================================
#                          PUSHES
# ============================================================
def push_in_window(cfg: configparser.SectionProxy, body: str, path: str, token: str) -> str | None:
    """POST until the board answers or its WiFi window closes. Returns its answer, None if not delivered.

    Without power saving there is no window: retry for ALWAYS_ON_RETRY_FOR_S at most.
    Raises BoardRefused when the board refuses it (retrying would not help).
    """
    retry_s = cfg.getint("retry_every_s", 5)
    if not power_saving(cfg):
        return _push_retrying(cfg, body, path, token, retry_s,
                              datetime.now() + timedelta(seconds=ALWAYS_ON_RETRY_FOR_S))

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

    answer = _push_retrying(cfg, body, path, token, retry_s, deadline)
    if answer is None:
        log.error("The board's WiFi window closed at %s", closes.strftime("%H:%M:%S"))
    else:
        try:  # The board is up: refresh its window settings while at it
            get_status(cfg, token)
        except BoardRefused:
            pass
    return answer


def _push_retrying(cfg: configparser.SectionProxy, body: str, path: str, token: str, retry_s: int,
                   deadline: datetime) -> str | None:
    """POST every `retry_s` until the board answers or `deadline`. Returns its answer, None if not delivered."""
    last_error = None
    while datetime.now() < deadline:
        try:
            return push(cfg, body, path, token)
        except (urllib.error.URLError, OSError) as e:
            last_error = e
            time.sleep(min(retry_s, max((deadline - datetime.now()).total_seconds(), 0)))
    log.error("Could not reach the board: %s", last_error)
    return None


def _last_push_path(path: str) -> Path:
    # On disk, so the calendar bridge (a new process every minute) remembers it too
    return DATA_DIR / f"last_push_{path.strip('/').replace('/', '_')}.json"


def forget_last_push(path: str) -> None:
    """Forgets what was last delivered to `path` (only used with a board firmware without /status:
    newer ones report what they hold, so a full sync happens by itself)."""
    _last_push_path(path).unlink(missing_ok=True)
    log.info("Full sync of %s: the next push sends everything", path)


def push_if_changed(cfg: configparser.SectionProxy, body: str, path: str, token: str) -> str | None:
    """Pushes `body` (see push_in_window) only when the board does not already hold the same content.

    Returns UNCHANGED when nothing had to be sent, None when the board cannot be reached.
    Asks the board what it holds (GET /status); with an older firmware without it, falls
    back on remembering what was last delivered, and also resends when that is older than
    [esp32] resend_every_min (0 = never). Raises BoardRefused.
    """
    key = body_hash(body)
    status = get_status(cfg, token)
    if status is None:
        return None  # Unreachable: checked again next time (a change then is pushed then)
    if status:
        if status.get(path.strip("/")) == key:
            log.debug("%s already on the board: not sent", path)
            return UNCHANGED
        log.info("%s differs on the board (boot %s): pushing", path, status.get("boot_id", "?"))
        return push_in_window(cfg, body, path, token)

    # Firmware without /status
    state_path = _last_push_path(path)
    try:
        last = json.loads(state_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        last = {}
    resend_s = cfg.getint("resend_every_min", 0) * 60
    too_old = resend_s > 0 and time.time() - last.get("at", 0) >= resend_s
    if last.get("key") == key and not too_old:
        log.debug("%s unchanged since the last push: not sent", path)
        return UNCHANGED

    answer = push_in_window(cfg, body, path, token)
    if answer is not None:
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        state_path.write_text(json.dumps({"key": key, "at": int(time.time())}), encoding="utf-8")
    return answer
