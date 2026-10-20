"""
web.py - The task page and its JSON API
---------------------------------------
    GET    /                  The page (static/index.html)
    GET    /api/tasks         {"tasks": [...], "updated_at": epoch, "board": {delivery status}}
    POST   /api/tasks         {"text", "high"} -> the new task
    PATCH  /api/tasks/<id>    Any of text, high, done -> the task
    DELETE /api/tasks/<id>
With [tasks] web_token set, /api/* needs it in the X-Web-Token header.
"""

import hmac
import json
import logging
from http.server import BaseHTTPRequestHandler
from pathlib import Path

from .delivery import BoardStatus
from .store import InvalidTask, TaskStore

log = logging.getLogger(__name__)

PAGE_PATH = Path(__file__).resolve().parent / "static" / "index.html"
MAX_REQUEST_BYTES = 4096


class Handler(BaseHTTPRequestHandler):
    # Set by cli.main() before serving
    store: TaskStore
    status: BoardStatus
    web_token: str

    server_version = "RLCDTasks"

    def log_message(self, fmt, *args):
        log.debug("%s %s", self.address_string(), fmt % args)

    # ----- helpers -----
    def _send(self, code: int, body: bytes, content_type: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code: int, payload) -> None:
        self._send(code, json.dumps(payload, ensure_ascii=False).encode("utf-8"), "application/json; charset=utf-8")

    def _error(self, code: int, message: str) -> None:
        self._json(code, {"error": message})

    def _authorized(self) -> bool:
        if not self.web_token:
            return True
        sent = self.headers.get("X-Web-Token", "")
        if hmac.compare_digest(sent.encode(), self.web_token.encode()):
            return True
        self._error(401, "bad token")
        return False

    def _body(self) -> dict | None:
        """The JSON object of the request, or None after answering with the error."""
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if not 0 < length <= MAX_REQUEST_BYTES:
            self._error(413 if length > 0 else 400, "bad body size")
            return None
        try:
            data = json.loads(self.rfile.read(length).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self._error(400, "body is not JSON")
            return None
        if not isinstance(data, dict):
            self._error(400, "body must be an object")
            return None
        return data

    def _task_id(self) -> int | None:
        """The id in /api/tasks/<id>, or None (not found) after answering."""
        tail = self.path.removeprefix("/api/tasks/")
        if self.path == tail or not tail.isdigit():
            self._error(404, "not found")
            return None
        return int(tail)

    def _answer(self, action) -> None:
        try:
            result = action()
        except InvalidTask as e:
            self._error(400, str(e))
            return
        if result is None or result is False:
            self._error(404, "no such task")
        else:
            self._json(200, result if result is not True else {"ok": True})

    # ----- routes -----
    def do_GET(self):
        if self.path in ("/", "/index.html"):
            try:
                self._send(200, PAGE_PATH.read_bytes(), "text/html; charset=utf-8")
            except OSError:
                self._error(500, f"{PAGE_PATH.name} is missing")
        elif self.path == "/api/tasks":
            if self._authorized():
                self._json(200, {"tasks": self.store.all(), "updated_at": self.store.updated_at,
                                 "board": self.status.snapshot()})
        else:
            self._error(404, "not found")

    def do_POST(self):
        if self.path != "/api/tasks":
            self._error(404, "not found")
        elif self._authorized() and (fields := self._body()) is not None:
            self._answer(lambda: self.store.add(fields))

    def do_PATCH(self):
        if not self._authorized() or (task_id := self._task_id()) is None:
            return
        if (fields := self._body()) is not None:
            self._answer(lambda: self.store.update(task_id, fields))

    def do_DELETE(self):
        if self._authorized() and (task_id := self._task_id()) is not None:
            self._answer(lambda: self.store.delete(task_id))
