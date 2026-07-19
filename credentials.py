from __future__ import annotations

import os
import re
from pathlib import Path

from model import Credentials


def load_env(path: Path) -> None:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError:
        return
    for source in lines:
        line = source.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        key, separator, value = line.partition("=")
        if not separator or not key.strip() or key.strip() in os.environ:
            continue
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
            value = value[1:-1]
        os.environ[key.strip()] = value


def load_profile_config(path: Path) -> Path:
    """Load the env file selected by a generic INI profile without exposing it.

    This follows an `env_path` convention, resolves a relative path
    from the INI directory, and returns the resolved location for diagnostics.
    Values are loaded into the current process only and are never returned.
    """
    env_path: str | None = None
    for source in path.read_text(encoding="utf-8").splitlines():
        line = source.strip()
        if not line or line.startswith(("#", ";")):
            continue
        key, separator, value = line.partition("=")
        if separator and key.strip().lower() == "env_path":
            env_path = value.strip().strip("'\"")
            break
    if not env_path:
        raise ValueError("profile_env_path_missing")
    resolved = Path(env_path)
    if not resolved.is_absolute():
        resolved = path.parent / resolved
    load_env(resolved)
    return resolved


def slots(prefix: str) -> tuple[int, ...]:
    pattern = re.compile(rf"^{re.escape(prefix)}_(\d+)_(?:KEY|JWT|SECRET)$")
    return tuple(sorted({int(match.group(1)) for name in os.environ
                         if (match := pattern.match(name)) and 1 <= int(match.group(1)) <= 255}))


def resolve(prefix: str, slot: int) -> Credentials | None:
    base = f"{prefix}_{slot}"
    key = os.environ.get(f"{base}_KEY", "").strip()
    secret = os.environ.get(f"{base}_SECRET", "").strip()
    jwt = os.environ.get(f"{base}_JWT", "").strip()
    if not jwt and (not key or not secret):
        return None
    return Credentials(
        key=key,
        secret=secret,
        passphrase=os.environ.get(f"{base}_PASSPHRASE", "").strip(),
        account_id=(os.environ.get(f"{base}_ACCOUNT_ID", "").strip() or
                    os.environ.get(f"{base}_UID", "").strip() or
                    os.environ.get(f"{base}_USER_ID", "").strip()),
        jwt=jwt,
    )
