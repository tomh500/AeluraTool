import hashlib
import sqlite3
from contextlib import contextmanager


def digest(value: str) -> str:
    return hashlib.sha256(value.encode()).hexdigest()


class Store:
    def __init__(self, path):
        self.path = path
        with self.connect() as db:
            db.executescript("""
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS logins (
                    state TEXT PRIMARY KEY, secret TEXT NOT NULL, provider TEXT NOT NULL,
                    nonce TEXT NOT NULL, verifier TEXT NOT NULL, expires REAL NOT NULL,
                    status TEXT NOT NULL DEFAULT 'pending', subject TEXT,
                    token TEXT);
                CREATE TABLE IF NOT EXISTS sessions (
                    token_hash TEXT PRIMARY KEY, owner TEXT NOT NULL, expires REAL NOT NULL);
                CREATE TABLE IF NOT EXISTS jobs (
                    id TEXT PRIMARY KEY, owner TEXT NOT NULL, item TEXT NOT NULL,
                    status TEXT NOT NULL, created REAL NOT NULL,
                    error TEXT, sha256 TEXT, size INTEGER, cancel INTEGER NOT NULL DEFAULT 0);
            """)
            # Never silently retry downloads after a crash or restart.
            db.execute("UPDATE jobs SET status='interrupted', error='relay.interrupted' "
                       "WHERE status IN ('queued','running')")

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.path, timeout=10)
        db.row_factory = sqlite3.Row
        try:
            with db:
                yield db
        finally:
            db.close()
