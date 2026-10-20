"""
logs.py - Logging for the PC programs: a rotating file in pc/logs/ and the console
"""

import logging
import logging.handlers
import sys

from .config import LOG_DIR


def setup_logging(name: str, debug: bool = False) -> None:
    """Logs to pc/logs/<name>.log and, when there is a console, to it as well."""
    LOG_DIR.mkdir(exist_ok=True)
    root = logging.getLogger()
    root.setLevel(logging.DEBUG if debug else logging.INFO)

    fmt = logging.Formatter("%(asctime)s %(levelname)s %(message)s")
    file_handler = logging.handlers.RotatingFileHandler(LOG_DIR / f"{name}.log", maxBytes=512_000,
                                                        backupCount=2, encoding="utf-8")
    file_handler.setFormatter(fmt)
    root.addHandler(file_handler)
    if sys.stdout is not None:  # pythonw has no console
        console = logging.StreamHandler(sys.stdout)
        console.setFormatter(fmt)
        root.addHandler(console)
