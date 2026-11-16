"""
delivery.py - Pushing the tasks to the board
--------------------------------------------
With the board's power saving on ([esp32] power_saving) the tasks are pushed in each of
its WiFi windows. With it off the board is always listening: the PC asks it what it holds
(GET /status) every minute and right after every change, and pushes only when it differs.
Besides the tasks, the board only reports what it holds; tasks are changed on the page.
"""

import configparser
import logging
import threading
import time
from datetime import datetime

from rlcd.board import (ALWAYS_ON_PERIOD_S, UNCHANGED, BoardRefused, power_saving, push_if_changed,
                        push_in_window, window_bounds)

from .store import TaskStore

log = logging.getLogger(__name__)

PUSH_LEAD_S = 20     # Wake up this long before a window opens
CHANGE_DEBOUNCE_S = 1.0  # After a change, wait this long for more (a drag, several deletes) before pushing


class BoardStatus:
    """What the page shows about the last delivery to the board.

    delivered_rev is the store revision the board is known to hold (0 = none yet), so the
    page can tell whether the latest change is on the board, without comparing clocks.
    """

    def __init__(self, enabled: bool):
        self._lock = threading.Lock()
        self._state = {"enabled": enabled, "delivered_at": 0, "delivered_rev": 0, "error": "",
                       "next_window": 0, "always_on": False}

    def set(self, **values) -> None:
        with self._lock:
            self._state.update(values)

    def delivered(self, revision: int) -> None:
        with self._lock:
            if self._state["delivered_rev"] != revision or not self._state["delivered_at"]:
                self._state["delivered_at"] = int(time.time())
            self._state["delivered_rev"] = revision
            self._state["error"] = ""

    def snapshot(self) -> dict:
        with self._lock:
            return dict(self._state)


def push_loop(store: TaskStore, esp_cfg: configparser.SectionProxy, token: str, status: BoardStatus,
              stop: threading.Event) -> None:
    """In every WiFi window of the board: push the tasks (without power saving: see _push_changes)."""
    if not power_saving(esp_cfg):
        status.set(always_on=True)
        _push_changes(store, esp_cfg, token, status, stop)
        return
    while not stop.is_set():
        opens, closes = window_bounds(datetime.now())
        status.set(next_window=int(opens.timestamp()))
        wait = (opens - datetime.now()).total_seconds() - PUSH_LEAD_S
        if wait > 0 and stop.wait(wait):
            return

        body, revision = store.board_body()
        try:
            answer = push_in_window(esp_cfg, body, "/tasks", token)
            if answer is None:
                status.set(error="The board did not answer in its last WiFi window")
            else:
                status.delivered(revision)
        except BoardRefused as e:
            log.error("%s", e)
            status.set(error=f"The board refused the tasks: {e.hint or e.reason}")
        except Exception:  # noqa: BLE001 - keep the thread alive whatever happens
            log.exception("Push failed")
            status.set(error="Push failed (see logs/tasks_server.log)")

        # Past the end of this window
        stop.wait(max((closes - datetime.now()).total_seconds(), 0) + 1)


def _push_changes(store: TaskStore, esp_cfg: configparser.SectionProxy, token: str,
                  status: BoardStatus, stop: threading.Event) -> None:
    """Board always on: at any hour, every minute and right after each change, make sure the board
    holds the current tasks (it reports a hash of them), pushing them when it does not.

    A board that restarted (it keeps the tasks in RAM) is refilled within a minute; the check
    every minute also tells the board the PC is still there (its "PC offline" warning).
    """
    while not stop.is_set():
        store.changed.clear()  # Before reading: a change from now on triggers another round
        body, revision = store.board_body()
        status.set(next_window=int(time.time()) + ALWAYS_ON_PERIOD_S)
        try:
            answer = push_if_changed(esp_cfg, body, "/tasks", token)
            if answer is None:
                status.set(error="The board did not answer")
            else:
                status.delivered(revision)
                if answer is not UNCHANGED:
                    log.info("Tasks revision %d on the board", revision)
        except BoardRefused as e:
            log.error("%s", e)
            status.set(error=f"The board refused the tasks: {e.hint or e.reason}")
        except Exception:  # noqa: BLE001 - keep the thread alive whatever happens
            log.exception("Push failed")
            status.set(error="Push failed (see logs/tasks_server.log)")

        # Until the next minute, or a change (then a short pause to gather the ones that follow)
        if store.changed.wait(ALWAYS_ON_PERIOD_S):
            stop.wait(CHANGE_DEBOUNCE_S)
