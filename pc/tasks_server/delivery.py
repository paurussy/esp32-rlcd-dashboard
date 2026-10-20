"""
delivery.py - Pushing the tasks to the board in each of its WiFi windows
------------------------------------------------------------------------
The answer to each push lists what was finished or deleted with BOOT on the board.
"""

import configparser
import logging
import threading
import time
from datetime import datetime

from rlcd.board import push_in_window, window_bounds

from .store import TaskStore

log = logging.getLogger(__name__)

PUSH_LEAD_S = 20  # Wake up this long before a window opens


class BoardStatus:
    """What the page shows about the last delivery to the board."""

    def __init__(self, enabled: bool):
        self._lock = threading.Lock()
        self._state = {"enabled": enabled, "delivered_at": 0, "error": "", "next_window": 0}

    def set(self, **values) -> None:
        with self._lock:
            self._state.update(values)

    def snapshot(self) -> dict:
        with self._lock:
            return dict(self._state)


def push_loop(store: TaskStore, esp_cfg: configparser.SectionProxy, token: str, status: BoardStatus,
              stop: threading.Event) -> None:
    """In every WiFi window of the board: push the tasks and apply what it did with them."""
    while not stop.is_set():
        opens, closes = window_bounds(datetime.now())
        status.set(next_window=int(opens.timestamp()))
        wait = (opens - datetime.now()).total_seconds() - PUSH_LEAD_S
        if wait > 0 and stop.wait(wait):
            return

        answer = None
        try:
            answer = push_in_window(esp_cfg, store.board_body(), "/tasks", token)
            if answer is not None:
                store.apply_board_actions(answer.splitlines()[1:])  # The first line is "ok <n>"
        except Exception:
            log.exception("Push failed")
        if answer is None:
            status.set(error="The board did not answer in its last WiFi window")
        else:
            status.set(delivered_at=int(time.time()), error="")

        # Past the end of this window
        stop.wait(max((closes - datetime.now()).total_seconds(), 0) + 1)
