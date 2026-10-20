from contextlib import asynccontextmanager
import json
import os
from pathlib import Path
import secrets
import time
import threading
from collections import defaultdict, deque

import httpx
from fastapi import Depends, FastAPI, Header, HTTPException, Request
from fastapi.responses import FileResponse, JSONResponse
from pydantic import BaseModel, Field

from .auth import Auth
from .store import Store, digest
from .worker import Worker


class DownloadRequest(BaseModel):
    item_id: str = Field(pattern=r'^[1-9][0-9]{0,19}$')


def create_app(config=None):
    if config is None:
        config = json.loads(Path(os.environ.get('RELAY_CONFIG', 'config.json')).read_text(encoding='utf-8'))
    data = Path(config.get('data_dir', './data')).resolve()
    data.mkdir(parents=True, exist_ok=True)
    store = Store(data / 'relay.sqlite3')
    auth = Auth(config, store)
    worker = Worker(config, store, data)
    limits, lock = defaultdict(deque), threading.Lock()

    @asynccontextmanager
    async def lifespan(app):
        worker.thread.start()
        yield
        worker.shutdown()

    app = FastAPI(title='Wallpaper Relay', lifespan=lifespan, docs_url=None, redoc_url=None,
                  openapi_url=None)
    app.state.store, app.state.auth, app.state.worker = store, auth, worker

    @app.middleware('http')
    async def request_limits(request: Request, call_next):
        size = request.headers.get('content-length', '0')
        if not size.isdigit() or int(size) > 4096:
            return JSONResponse({'detail': 'relay.request_too_large'}, status_code=413)
        if request.method == 'POST':
            received = bytearray()
            async for chunk in request.stream():
                received.extend(chunk)
                if len(received) > 4096:
                    return JSONResponse({'detail': 'relay.request_too_large'}, status_code=413)
            request._body = bytes(received)
        response = await call_next(request)
        response.headers['Cache-Control'] = 'no-store'
        response.headers['Referrer-Policy'] = 'no-referrer'
        response.headers['X-Content-Type-Options'] = 'nosniff'
        return response

    def rate_limit(request: Request):
        ip = request.client.host
        now = time.monotonic()
        with lock:
            for key in list(limits):
                if not limits[key] or limits[key][-1] < now - 60:
                    del limits[key]
            queue = limits[ip]
            while queue and queue[0] < now - 60:
                queue.popleft()
            if len(queue) >= 60:
                raise HTTPException(429, 'relay.rate_limited')
            queue.append(now)

    def token(authorization: str = Header(default='')):
        if not authorization.startswith('Bearer ') or len(authorization) > 512:
            raise HTTPException(401, 'relay.authentication_required')
        return authorization[7:]

    def owner(value=Depends(token)):
        return auth.owner(value)

    def get_job(job_id, identity):
        with store.connect() as db:
            row = db.execute('SELECT * FROM jobs WHERE id=? AND owner=?', (job_id, identity)).fetchone()
        if not row:
            raise HTTPException(404, 'relay.job_not_found')
        return dict(row)

    @app.get('/healthz')
    def health():
        return {'status': 'ok', 'downloads_enabled': bool(config.get('downloads_enabled'))}

    @app.post('/v1/auth/start/{provider}', dependencies=[Depends(rate_limit)])
    def start(provider: str):
        try:
            return auth.start(provider)
        except (httpx.HTTPError, ValueError, KeyError):
            raise HTTPException(503, 'relay.identity_unavailable') from None

    @app.get('/v1/auth/callback', dependencies=[Depends(rate_limit)])
    def callback(state: str = '', code: str = ''):
        if not state or not code or len(state) > 128 or len(code) > 4096:
            raise HTTPException(400, 'relay.login_failed')
        auth.complete(state, code)
        return {'status': 'complete'}

    @app.post('/v1/auth/poll/{ticket}', dependencies=[Depends(rate_limit)])
    def poll(ticket: str, secret=Depends(token)):
        return auth.poll(ticket, secret)

    @app.get('/v1/me')
    def me(identity=Depends(owner)):
        return {'identity': identity}

    @app.post('/v1/auth/logout', status_code=204)
    def logout(value=Depends(token), identity=Depends(owner)):
        with store.connect() as db:
            db.execute('DELETE FROM sessions WHERE token_hash=?', (digest(value),))

    @app.post('/v1/jobs', status_code=202, dependencies=[Depends(rate_limit)])
    def create_job(body: DownloadRequest, identity=Depends(owner)):
        if not config.get('downloads_enabled'):
            raise HTTPException(503, 'relay.downloads_disabled')
        job_id = secrets.token_hex(16)
        with store.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            count = db.execute("SELECT count(*) FROM jobs WHERE status IN ('queued','running')").fetchone()[0]
            owned = db.execute("SELECT count(*) FROM jobs WHERE owner=? AND status IN ('queued','running')",
                               (identity,)).fetchone()[0]
            if count >= 16 or owned >= 2:
                raise HTTPException(429, 'relay.queue_limit')
            db.execute('INSERT INTO jobs(id,owner,item,status,created) VALUES(?,?,?,?,?)',
                       (job_id, identity, body.item_id, 'queued', time.time()))
        return get_job(job_id, identity)

    @app.get('/v1/jobs/{job_id}')
    def job(job_id: str, identity=Depends(owner)):
        return get_job(job_id, identity)

    @app.delete('/v1/jobs/{job_id}', status_code=202)
    def cancel(job_id: str, identity=Depends(owner)):
        row = get_job(job_id, identity)
        with store.connect() as db:
            db.execute('UPDATE jobs SET cancel=1 WHERE id=?', (job_id,))
            db.execute("UPDATE jobs SET status='cancelled' WHERE id=? AND status='queued'", (job_id,))
        return {'id': row['id'], 'cancel_requested': True}

    @app.get('/v1/jobs/{job_id}/content')
    def content(job_id: str, identity=Depends(owner)):
        row = get_job(job_id, identity)
        if row['status'] != 'ready':
            raise HTTPException(409, 'relay.content_not_ready')
        archive = data / 'archives' / (row['id'] + '.zip')
        if not archive.is_file() or archive.is_symlink():
            raise HTTPException(409, 'relay.content_unavailable')
        return FileResponse(archive, media_type='application/zip', filename=row['item']+'.zip',
                            headers={'X-Content-SHA256': row['sha256']})

    return app
