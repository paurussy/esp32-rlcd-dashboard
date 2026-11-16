"""
store.py - The tasks, persisted in a JSON file, and the body the board receives
-------------------------------------------------------------------------------
The wire format is described in src/model/tasks.h.
"""

import json
import logging
import os
import shutil
import threading
import time
from dataclasses import asdict, dataclass
from dataclasses import fields as dataclass_fields
from pathlib import Path

from rlcd.board import TASK_TEXT_BYTES, TASKS_MAX_ITEMS, fit_utf8, make_body

log = logging.getLogger(__name__)

TEXT_LIMIT = 200      # Characters per task
DESCRIPTION_LIMIT = 2000  # Characters in a description
IMPORT_LIMIT = 2000   # Tasks in an imported file
EXPORT_FORMAT = "rlcd-tasks"
IMPORT_MODES = ("replace", "merge")


@dataclass
class Task:
    id: int
    text: str
    high: bool = False
    done: bool = False
    created: int = 0   # Epoch seconds
    done_at: int = 0
    description: str = ""  # Page only: never sent to the board


class InvalidTask(ValueError):
    """A field of a request is not acceptable; the message goes back to the page."""


def clean_text(text) -> str:
    """Single line (no tabs: they separate the fields of the push), bounded length."""
    if not isinstance(text, str):
        raise InvalidTask("text must be a string")
    text = " ".join(text.split())[:TEXT_LIMIT].rstrip()  # The cut may end on a space
    if not text:
        raise InvalidTask("text is empty")
    return text


def clean_description(text) -> str:
    """Free text (newlines kept, may be empty), bounded length."""
    if not isinstance(text, str):
        raise InvalidTask("description must be a string")
    return text.replace("\r\n", "\n").strip()[:DESCRIPTION_LIMIT]


def clean_flag(value, name: str) -> bool:
    if not isinstance(value, bool):
        raise InvalidTask(f"{name} must be true or false")
    return value


def clean_epoch(value, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0:
        raise InvalidTask(f"{name} must be a time in epoch seconds")
    return int(value)


def clean_task(raw, where: str = "") -> dict:
    """The fields of a task given by a client (an import or an undo), validated. No id."""
    prefix = f"{where}: " if where else ""
    if not isinstance(raw, dict):
        raise InvalidTask(f"{prefix}a task must be an object")
    try:
        task = {"text": clean_text(raw.get("text")),
                "description": clean_description(raw.get("description", "")),
                "high": clean_flag(raw.get("high", False), "high"),
                "done": clean_flag(raw.get("done", False), "done"),
                "created": clean_epoch(raw.get("created", 0), "created"),
                "done_at": clean_epoch(raw.get("done_at", 0), "done_at")}
    except InvalidTask as e:
        raise InvalidTask(f"{prefix}{e}") from None
    if not task["done"]:
        task["done_at"] = 0
    return task


class TaskStore:
    """Thread-safe: the web server and the push to the board both use it.

    `revision` grows with every change, so the delivery can tell exactly which version
    reached the board; `changed` is set on every change, to push without waiting.
    """

    def __init__(self, path: Path):
        self._path = path
        self._backup = path.with_name(path.name + ".bak")
        self._lock = threading.Lock()
        self._tasks: list[Task] = []
        self._next_id = 1
        self._revision = 1
        self.changed = threading.Event()
        self._updated_at = int(path.stat().st_mtime) if path.exists() else 0
        self._load()

    # ----- persistence -----
    def _load(self) -> None:
        if not self._path.exists():
            return
        try:
            self._tasks, self._next_id = self._parse(self._path)
        except (OSError, ValueError, KeyError, TypeError) as e:
            # Never refuse to start over a damaged file: keep it aside and use the backup
            aside = self._path.with_name(f"{self._path.stem}.corrupt-{int(time.time())}{self._path.suffix}")
            os.replace(self._path, aside)
            log.error("%s is damaged (%s): moved to %s", self._path.name, e, aside.name)
            try:
                self._tasks, self._next_id = self._parse(self._backup)
                log.warning("Tasks restored from %s", self._backup.name)
                self._save(backup=False)
            except (OSError, ValueError, KeyError, TypeError):
                log.error("No usable backup: starting with an empty list")
                self._tasks, self._next_id = [], 1

    @staticmethod
    def _parse(path: Path) -> tuple[list[Task], int]:
        data = json.loads(path.read_text(encoding="utf-8"))
        known = {f.name for f in dataclass_fields(Task)}  # Ignores fields of older versions (due dates)
        tasks = [Task(**{k: v for k, v in t.items() if k in known}) for t in data["tasks"]]
        next_id = max(int(data.get("next_id", 1)), max((t.id for t in tasks), default=0) + 1)
        return tasks, next_id

    def _save(self, backup: bool = True) -> None:
        """Atomic: a crash in the middle never leaves a half-written file. The previous version
        is kept as tasks.json.bak."""
        self._updated_at = int(time.time())
        self._revision += 1
        self._path.parent.mkdir(parents=True, exist_ok=True)
        data = {"next_id": self._next_id, "tasks": [asdict(t) for t in self._tasks]}
        tmp = self._path.with_suffix(".tmp")
        tmp.write_text(json.dumps(data, indent=2, ensure_ascii=False), encoding="utf-8")
        if backup and self._path.exists():
            shutil.copyfile(self._path, self._backup)
        os.replace(tmp, self._path)
        self.changed.set()

    # ----- queries -----
    @property
    def updated_at(self) -> int:
        """When the tasks last changed (epoch seconds)."""
        return self._updated_at

    @property
    def revision(self) -> int:
        """Grows with every change (starts at 1 with each run of the server)."""
        return self._revision

    def _find(self, task_id: int) -> Task | None:
        return next((t for t in self._tasks if t.id == task_id), None)

    def _new_id(self) -> int:
        self._next_id += 1
        return self._next_id - 1

    def all(self) -> list[dict]:
        with self._lock:
            return [asdict(t) for t in self._tasks]

    def snapshot(self) -> tuple[list[dict], int]:
        """The tasks and the revision they belong to, read together."""
        with self._lock:
            return [asdict(t) for t in self._tasks], self._revision

    # ----- changes -----
    def add(self, fields: dict) -> dict:
        """A new task at the end. An undo of a delete also sends `position` (index in the list),
        `created` and `done`, so the task comes back where it was."""
        with self._lock:
            text = clean_text(fields.get("text"))  # Validate first: a rejected task must not use up an id
            high = clean_flag(fields.get("high", False), "high")
            description = clean_description(fields.get("description", ""))
            restore = "position" in fields
            done = clean_flag(fields.get("done", False), "done") if restore else False
            created = clean_epoch(fields.get("created", 0), "created") if restore else 0
            position = fields.get("position", len(self._tasks))
            if isinstance(position, bool) or not isinstance(position, int):
                raise InvalidTask("position must be a number")
            now = int(time.time())
            task = Task(id=self._new_id(), text=text, high=high, done=done, created=created or now,
                        done_at=now if done else 0, description=description)
            self._tasks.insert(max(0, min(position, len(self._tasks))), task)
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
            if "description" in fields:
                changes["description"] = clean_description(fields["description"])
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

    def delete(self, task_id: int) -> dict | None:
        """Removes a task. Returns it with its `position`, for an undo."""
        with self._lock:
            task = self._find(task_id)
            if task is None:
                return None
            position = self._tasks.index(task)
            self._tasks.remove(task)
            self._save()
            return {**asdict(task), "position": position}

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

    # ----- export / import -----
    def export(self) -> dict:
        """Everything, in the same layout as tasks.json (so that file can be imported too)."""
        with self._lock:
            return {"format": EXPORT_FORMAT, "version": 1, "exported_at": int(time.time()),
                    "next_id": self._next_id, "tasks": [asdict(t) for t in self._tasks]}

    def import_tasks(self, data, mode) -> dict:
        """Loads an export (or a tasks.json). `mode`: "replace" the whole list, or "merge" (add the
        imported tasks after the current ones, skipping those already there with the same text
        and state). Everything is validated before anything changes. Returns the counts."""
        if mode not in IMPORT_MODES:
            raise InvalidTask(f"mode must be one of {', '.join(IMPORT_MODES)}")
        if not isinstance(data, dict) or not isinstance(data.get("tasks"), list):
            raise InvalidTask("Not a task list: the file has no \"tasks\" list")
        if data.get("format", EXPORT_FORMAT) != EXPORT_FORMAT:
            raise InvalidTask(f"Unknown format \"{data.get('format')}\"")
        raw_tasks = data["tasks"]
        if len(raw_tasks) > IMPORT_LIMIT:
            raise InvalidTask(f"Too many tasks ({len(raw_tasks)}, at most {IMPORT_LIMIT})")
        cleaned = [clean_task(t, f"task {i + 1}") for i, t in enumerate(raw_tasks)]
        ids = [t.get("id") if isinstance(t, dict) else None for t in raw_tasks]

        with self._lock:
            if mode == "replace":
                # Keep the ids when they are valid and unique, so the board keeps its selection
                keep_ids = (all(isinstance(i, int) and not isinstance(i, bool) and i > 0 for i in ids)
                            and len(set(ids)) == len(ids))
                if keep_ids:
                    self._tasks = [Task(id=i, **t) for i, t in zip(ids, cleaned)]
                    self._next_id = max(ids, default=0) + 1
                else:
                    self._next_id = 1
                    self._tasks = [Task(id=self._new_id(), **t) for t in cleaned]
                added, skipped = len(cleaned), 0
            else:
                present = {(t.text.casefold(), t.done) for t in self._tasks}
                added = skipped = 0
                for t in cleaned:
                    key = (t["text"].casefold(), t["done"])
                    if key in present:
                        skipped += 1
                        continue
                    present.add(key)
                    self._tasks.append(Task(id=self._new_id(), **t))
                    added += 1
            self._save()
            return {"ok": True, "mode": mode, "added": added, "skipped": skipped, "total": len(self._tasks)}

    # ----- the board -----
    def board_body(self) -> tuple[str, int]:
        """The push body and the revision it belongs to.

        The pending tasks in the order the board shows them (the list order), at most what the
        board keeps, each text cut to what it stores. The header carries the total number of
        pending tasks, so the board can tell how many did not fit.
        """
        with self._lock:
            pending = [t for t in self._tasks if not t.done]
            revision = self._revision
        lines = [f"{t.id}\t{int(t.high)}\t{fit_utf8(t.text, TASK_TEXT_BYTES)}" for t in pending[:TASKS_MAX_ITEMS]]
        return make_body(lines, extra=(len(pending),)), revision
