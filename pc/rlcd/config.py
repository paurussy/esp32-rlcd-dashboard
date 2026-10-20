"""
config.py - Where things live on disk, and config.ini
-----------------------------------------------------
Everything is relative to the pc/ folder, whichever directory a program is started from.
"""

import configparser
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent  # pc/
CONFIG_PATH = ROOT / "config.ini"
LOG_DIR = ROOT / "logs"
DATA_DIR = ROOT / "data"

SECTIONS = ("esp32", "outlook", "tasks")


def load_config(required: bool = True) -> configparser.ConfigParser:
    """config.ini with every section present (empty when missing).

    Exits with a message if the file is missing and `required`; the dry run of the
    calendar bridge works without one.
    """
    config = configparser.ConfigParser()
    if not config.read(CONFIG_PATH, encoding="utf-8") and required:
        sys.exit(f"Missing {CONFIG_PATH}: copy config.example.ini and edit it.")
    for section in SECTIONS:
        if not config.has_section(section):
            config.add_section(section)
    return config
