"""
store.py - The tasks, persisted in a JSON file, and the body the board receives
-------------------------------------------------------------------------------
The wire format is described in src/model/tasks.h.
"""

import json
import os
import threading
import time
from dataclasses import asdict, dataclass
from dataclasses import fields as dataclass_fields
from pathlib import Path

TEXT_LIMIT = 200  # Characters per task
BOARD_LIMIT = 40  # Tasks sent to the board (it shows TASKS_MAX_ITEMS, but must see the rest too)


@dataclass
class Task:
    id: int
    text: str
    high: bool = False
    done: bool = False
    created: int = 0   # Epoch seconds
    done_at: int = 0


class InvalidTask(ValueError):
    """A field of a request is not acceptable; the message goes back to the page."""


def clean_text(text) -> str:
    """Single line (no tabs: they separate the fields of the push), bounded length."""
    if not isinstance(text, str):
        raise InvalidTask("text must be a string")
    text = " ".join(text.split())[:TEXT_LIMIT]
    if not text:
        raise InvalidTask("text is empty")
    return text


def clean_flag(value, name: str) -> bool:
    if not isinstance(value, bool):
        raise InvalidTask(f"{name} must be true or false")
    return value


class TaskStore:
    """Thread-safe: the web server and the push to the board both use it."""

    def __init__(self, path: Path):
        self._path = path
        self._lock = threading.Lock()
        self._tasks: list[Task] = []
        self._next_id = 1
        self._updated_at = int(path.stat().st_mtime) if path.exists() else 0
        self._load()

    def _load(self) -> None:
        if not self._path.exists():
            return
        data = json.loads(self._path.read_text(encoding="utf-8"))
        known = {f.name for f in dataclass_fields(Task)}  # Ignores fields of older versions (due dates)
        self._tasks = [Task(**{k: v for k, v in t.items() if k in known}) for t in data["tasks"]]
        self._next_id = max(data["next_id"], max((t.id for t in self._tasks), default=0) + 1)

    @property
    def updated_at(self) -> int:
        """When the tasks last changed (epoch seconds); the page compares it with the last delivery."""
        return self._updated_at

    def _save(self) -> None:
        """Atomic: a crash in the middle never leaves a half-written file."""
        self._updated_at = int(time.time())
        self._path.parent.mkdir(exist_ok=True)
        data = {"next_id": self._next_id, "tasks": [asdict(t) for t in self._tasks]}
        tmp = self._path.with_suffix(".tmp")
        tmp.write_text(json.dumps(data, indent=2, ensure_ascii=False), encoding="utf-8")
        os.replace(tmp, self._path)

    def _find(self, task_id: int) -> Task | None:
        return next((t for t in self._tasks if t.id == task_id), None)

    def _new_id(self) -> int:
        self._next_id += 1
        return self._next_id - 1

    def all(self) -> list[dict]:
        with self._lock:
            return [asdict(t) for t in self._tasks]

    def add(self, fields: dict) -> dict:
        with self._lock:
            text = clean_text(fields.get("text"))  # Validate first: a rejected task must not use up an id
            high = clean_flag(fields.get("high", False), "high")
            task = Task(id=self._new_id(), text=text, high=high, created=int(time.time()))
            self._tasks.append(task)
            self._save()
            return asdict(task)

    def update(self, task_id: int, fields: dict) -> dict | None:
        with self._lock:
            task = self._find(task_id)
            if task is None:
                return None
            # Validate everything before changing anything
            changes = {}
            if "text" in fields:
                changes["text"] = clean_text(fields["text"])
            if "high" in fields:
                changes["high"] = clean_flag(fields["high"], "high")
            if "done" in fields:
                changes["done"] = clean_flag(fields["done"], "done")

            was_done = task.done
            for name, value in changes.items():
                setattr(task, name, value)
            if task.done and not was_done:
                task.done_at = int(time.time())
            elif was_done and not task.done:
                task.done_at = 0
            self._save()
            return asdict(task)

    def delete(self, task_id: int) -> bool:
        with self._lock:
            task = self._find(task_id)
            if task is None:
                return False
            self._tasks.remove(task)
            self._save()
            return True

    def reorder(self, ids) -> bool:
        """Puts the pending tasks in the order of `ids` (all of them, once each).

        The list order is the display order, on the page and on the board. Done tasks
        keep their place; the pending ones are redistributed over the places the
        pending ones held.
        """
        if not isinstance(ids, list) or not all(isinstance(i, int) and not isinstance(i, bool) for i in ids):
            raise InvalidTask("ids must be a list of task ids")
        with self._lock:
            pending = {t.id: t for t in self._tasks if not t.done}
            if len(ids) != len(pending) or set(ids) != set(pending):
                raise InvalidTask("The task list changed, reload the page")
            ordered = iter(pending[i] for i in ids)
            self._tasks = [t if t.done else next(ordered) for t in self._tasks]
            self._save()
            return True

    def board_body(self) -> str:
        """The pending tasks in the order the board shows them (the list order)."""
        with self._lock:
            pending = [t for t in self._tasks if not t.done]
        lines = [f"v1\t{int(time.time())}"]
        for t in pending[:BOARD_LIMIT]:
            lines.append(f"{t.id}\t{int(t.high)}\t{t.text}")
        return "\n".join(lines) + "\n"
