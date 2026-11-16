"""
web.py - The task page and its JSON API
---------------------------------------
    GET    /                  The page (static/index.html)
    GET    /api/tasks         {"tasks": [...], "updated_at", "revision", "board": {delivery status}}
    POST   /api/tasks         {"text", "high", "description"?} -> the new task, added at the end.
                              An undo of a delete also sends "position", "created" and "done".
    POST   /api/tasks/order   {"ids": [...]} -> the pending tasks in that order (all of them)
    PATCH  /api/tasks/<id>    Any of text, description, high, done -> the task
                              (the description stays on the PC: it is never sent to the board)
    DELETE /api/tasks/<id>    -> the deleted task, with its "position" (for an undo)
    GET    /api/export        Every task, as a JSON file to download (same layout as tasks.json)
    POST   /api/import        {"mode": "replace"|"merge", "data": <an export or a tasks.json>}
With [tasks] web_token set, /api/* needs it in the X-Web-Token header.

Requests that change something must be JSON (Content-Type: application/json), which a
browser only sends cross-site after asking (CORS preflight, never granted here): another
web page open in the browser cannot change the tasks. /api/* also only answers to the
names this PC is reached by (see _host_allowed), against DNS rebinding.
"""

import hmac
import ipaddress
import json
import logging
import socket
import time
from http.server import BaseHTTPRequestHandler
from pathlib import Path
from urllib.parse import urlsplit

from .delivery import BoardStatus
from .store import InvalidTask, TaskStore

log = logging.getLogger(__name__)

PAGE_PATH = Path(__file__).resolve().parent / "static" / "index.html"
MAX_REQUEST_BYTES = 8192  # A 2000 character description in multibyte text fits
MAX_IMPORT_BYTES = 2_000_000


def local_host_names(extra: str = "") -> set[str]:
    """Host names the page may be opened with (besides IP addresses): this PC's names and `extra`
    ([tasks] allowed_hosts, comma separated)."""
    names = {"localhost"}
    for name in (socket.gethostname(), socket.getfqdn()):
        if name:
            names.update({name.lower(), name.lower().split(".")[0], name.lower().split(".")[0] + ".local"})
    names.update(h.strip().lower() for h in extra.split(",") if h.strip())
    return names


class Handler(BaseHTTPRequestHandler):
    # Set by cli.main() before serving
    store: TaskStore
    status: BoardStatus
    web_token: str
    allowed_hosts: set[str] = {"localhost"}

    server_version = "RLCDTasks"

    def log_message(self, fmt, *args):
        log.debug("%s %s", self.address_string(), fmt % args)

    # ----- helpers -----
    def _send(self, code: int, body: bytes, content_type: str, extra_headers: dict | None = None) -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        for name, value in (extra_headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code: int, payload, extra_headers: dict | None = None) -> None:
        self._send(code, json.dumps(payload, ensure_ascii=False).encode("utf-8"),
                   "application/json; charset=utf-8", extra_headers)

    def _error(self, code: int, message: str) -> None:
        self._json(code, {"error": message})

    @staticmethod
    def _hostname(value: str) -> str:
        return (urlsplit(f"//{value}").hostname or "").lower()

    def _host_allowed(self, host: str) -> bool:
        if not host:
            return True  # HTTP/1.0 clients (curl -0); browsers always send it
        name = self._hostname(host)
        try:
            ipaddress.ip_address(name)
            return True
        except ValueError:
            return name in self.allowed_hosts

    def _api_allowed(self) -> bool:
        """Same-origin and known-host checks, then the token."""
        host = self.headers.get("Host", "")
        if not self._host_allowed(host):
            log.warning("Refused /api request for host %r (add it to [tasks] allowed_hosts if it is yours)", host)
            self._error(403, "unknown host name: add it to [tasks] allowed_hosts in config.ini")
            return False
        origin = self.headers.get("Origin")
        if origin and origin != "null" and urlsplit(origin).netloc.lower() != host.lower():
            self._error(403, "cross-site request")
            return False
        if not self.web_token:
            return True
        sent = self.headers.get("X-Web-Token", "")
        if hmac.compare_digest(sent.encode(), self.web_token.encode()):
            return True
        self._error(401, "bad token")
        return False

    def _body(self, limit: int = MAX_REQUEST_BYTES) -> dict | None:
        """The JSON object of the request, or None after answering with the error."""
        if self.headers.get_content_type() != "application/json":
            self._error(415, "body must be application/json")
            return None
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if not 0 < length <= limit:
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
            if self._api_allowed():
                tasks, revision = self.store.snapshot()
                self._json(200, {"tasks": tasks, "updated_at": self.store.updated_at, "revision": revision,
                                 "board": self.status.snapshot()})
        elif self.path == "/api/export":
            if self._api_allowed():
                name = time.strftime("tasks-%Y-%m-%d.json")
                self._json(200, self.store.export(), {"Content-Disposition": f'attachment; filename="{name}"'})
        else:
            self._error(404, "not found")

    def do_POST(self):
        routes = {"/api/tasks": MAX_REQUEST_BYTES, "/api/tasks/order": MAX_REQUEST_BYTES,
                  "/api/import": MAX_IMPORT_BYTES}
        if self.path not in routes:
            self._error(404, "not found")
        elif self._api_allowed() and (fields := self._body(routes[self.path])) is not None:
            if self.path == "/api/tasks":
                self._answer(lambda: self.store.add(fields))
            elif self.path == "/api/tasks/order":
                self._answer(lambda: self.store.reorder(fields.get("ids")))
            else:
                self._answer(lambda: self.store.import_tasks(fields.get("data"), fields.get("mode")))

    def do_PATCH(self):
        if not self._api_allowed() or (task_id := self._task_id()) is None:
            return
        if (fields := self._body()) is not None:
            self._answer(lambda: self.store.update(task_id, fields))

    def do_DELETE(self):
        if self._api_allowed() and (task_id := self._task_id()) is not None:
            self._answer(lambda: self.store.delete(task_id))
