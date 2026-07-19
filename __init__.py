"""Importable, read-only exchange endpoint contract probe."""

import sys
from pathlib import Path

# Existing profile modules also support direct `python run.py` execution.
_MODULE_DIR = str(Path(__file__).resolve().parent)
if _MODULE_DIR not in sys.path:
    sys.path.insert(0, _MODULE_DIR)

from .probe import private_rest, public_rest, public_ws

__all__ = ("private_rest", "public_rest", "public_ws")
