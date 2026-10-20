import hashlib
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import subprocess
import threading
import time
import zipfile


def content_files(root: Path, limit: int):
    """Reject links, special files and names unsafe for Windows extraction."""
    if root.is_symlink() or not root.is_dir():
        raise ValueError('relay.missing_content')
    files, total = [], 0
    seen = set()
    for parent, dirs, names in os.walk(root, followlinks=False):
        for name in dirs + names:
            path = Path(parent) / name
            mode = path.lstat().st_mode
            if not (stat.S_ISDIR(mode) or stat.S_ISREG(mode)):
                raise ValueError('relay.unsafe_content')
            if (re.search(r'[<>:"/\\|?*\x00-\x1f]', name) or name.endswith((' ', '.'))
                or re.fullmatch(r'(?i)(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\..*)?', name)):
                raise ValueError('relay.unsafe_content')
            relative = path.relative_to(root).as_posix()
            normalized = relative.casefold()
            if normalized in seen or len(relative) > 220:
                raise ValueError('relay.unsafe_content')
            seen.add(normalized)
            if stat.S_ISREG(mode):
                total += path.stat().st_size
                files.append((path, relative))
            if total > limit or len(seen) > 20000:
                raise ValueError('relay.item_too_large')
    if not files:
        raise ValueError('relay.missing_content')
    return files


class Worker:
    def __init__(self, config, store, data):
        self.config, self.store, self.data = config, store, data
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.loop, daemon=True)

    def shutdown(self):
        self.stop.set()
        self.thread.join(timeout=15)

    def loop(self):
        while not self.stop.wait(0.5):
            with self.store.connect() as db:
                row = db.execute("SELECT * FROM jobs WHERE status='queued' ORDER BY created LIMIT 1").fetchone()
                if row:
                    db.execute("UPDATE jobs SET status='running' WHERE id=?", (row['id'],))
            if row:
                self.run(row)

    def cancelled(self, job):
        with self.store.connect() as db:
            row = db.execute('SELECT cancel FROM jobs WHERE id=?', (job,)).fetchone()
        return self.stop.is_set() or bool(row['cancel'])

    def run(self, row):
        job = row['id']
        archive = self.data / 'archives' / (job + '.zip')
        partial = archive.with_suffix('.partial')
        process = None
        log = None
        try:
            cfg = self.config
            root = Path(cfg['steam_root']).resolve()
            item = root / 'steamapps' / 'workshop' / 'content' / '431960' / row['item']
            # Old output must not be mistaken for a successful download.
            if item.exists() or item.is_symlink():
                raise ValueError('relay.existing_content_requires_review')
            account = cfg.get('steam_account', '')
            if not re.fullmatch(r'[A-Za-z0-9_]{2,64}', account):
                raise ValueError('relay.steam_not_configured')
            executable = Path(cfg['steamcmd']).resolve()
            if not executable.is_file():
                raise ValueError('relay.steam_not_configured')
            usage = shutil.disk_usage(root)
            if usage.free < cfg['min_free_bytes'] + cfg['max_item_bytes'] * 2:
                raise ValueError('relay.insufficient_space')
            # Steam Guard/password login is performed manually in this dedicated SteamCMD home.
            log_path = self.data / (job + '.steam.log')
            log = log_path.open('xb')
            process = subprocess.Popen([str(executable), '+login', account,
                '+workshop_download_item', '431960', row['item'], 'validate', '+quit'],
                cwd=root, stdin=subprocess.DEVNULL, stdout=log,
                stderr=subprocess.STDOUT, start_new_session=(os.name != 'nt'))
            deadline = time.monotonic() + cfg.get('download_timeout_seconds', 1800)
            while process.poll() is None:
                if self.cancelled(job):
                    raise ValueError('relay.cancelled')
                if time.monotonic() > deadline:
                    raise ValueError('relay.download_timeout')
                if log_path.stat().st_size > 2 * 1024 * 1024:
                    raise ValueError('relay.steam_output_limit')
                self.check_storage()
                time.sleep(0.5)
            log.close()
            output = log_path.read_bytes()[:2 * 1024 * 1024].decode('utf-8', errors='replace')
            marker = re.search(r'Success\. Downloaded item\s+' + re.escape(row['item']) + r'\b', output)
            if process.returncode != 0 or not marker:
                raise ValueError('relay.steam_download_failed')
            files = content_files(item, cfg['max_item_bytes'])
            self.check_storage()
            archive.parent.mkdir(exist_ok=True)
            with zipfile.ZipFile(partial, 'x', compression=zipfile.ZIP_STORED) as output:
                for path, relative in files:
                    if self.cancelled(job):
                        raise ValueError('relay.cancelled')
                    self.check_storage()
                    # Link handling is disabled here too as defense against accidental filesystem changes.
                    with path.open('rb') as source, output.open(relative, 'w', force_zip64=True) as dest:
                        while chunk := source.read(1024 * 1024):
                            if self.cancelled(job):
                                raise ValueError('relay.cancelled')
                            dest.write(chunk)
                            self.check_storage()
            with partial.open('rb') as source:
                sha = hashlib.file_digest(source, 'sha256').hexdigest()
            if self.cancelled(job):
                raise ValueError('relay.cancelled')
            partial.replace(archive)
            with self.store.connect() as db:
                db.execute("UPDATE jobs SET status='ready',sha256=?,size=? WHERE id=?",
                           (sha, archive.stat().st_size, job))
        except Exception as exc:
            code = str(exc) if isinstance(exc, ValueError) and str(exc).startswith('relay.') else 'relay.download_failed'
            status = 'cancelled' if code == 'relay.cancelled' else 'failed'
            with self.store.connect() as db:
                db.execute('UPDATE jobs SET status=?,error=? WHERE id=?', (status, code, job))
            # Preserve Steam content and partial archives for manual recovery.
        finally:
            if process and process.poll() is None:
                if os.name != 'nt':
                    os.killpg(process.pid, signal.SIGTERM)
                else:
                    process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    if os.name != 'nt':
                        os.killpg(process.pid, signal.SIGKILL)
                    else:
                        process.kill()
                    process.wait(timeout=5)
            if log:
                log.close()

    def check_storage(self):
        roots = {self.data.resolve(), Path(self.config['steam_root']).resolve()}
        total = 0
        for root in roots:
            if shutil.disk_usage(root).free < self.config['min_free_bytes']:
                raise ValueError('relay.insufficient_space')
            for parent, dirs, files in os.walk(root, followlinks=False):
                dirs[:] = [name for name in dirs if not (Path(parent) / name).is_symlink()]
                for name in files:
                    path = Path(parent) / name
                    if not path.is_symlink():
                        total += path.stat().st_size
        if total > self.config['max_storage_bytes']:
            raise ValueError('relay.storage_limit')
