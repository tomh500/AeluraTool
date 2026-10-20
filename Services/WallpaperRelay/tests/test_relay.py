import json
import hashlib
import subprocess
import sys
import time
import zipfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import jwt
import pytest
from cryptography.hazmat.primitives.asymmetric import rsa
from fastapi import HTTPException
from fastapi.testclient import TestClient

from relay.app import create_app
from relay.store import Store, digest
from relay.worker import content_files


@pytest.fixture
def app(tmp_path):
    cfg = dict(data_dir=str(tmp_path / 'data'), public_url='https://relay.example',
               providers={'google': {'client_id': 'test-client', 'client_secret': 'test-secret'}},
               allowlist=['google:alice', 'google:bob'], downloads_enabled=True,
               steam_root=str(tmp_path / 'steam'), max_item_bytes=4096,
               max_storage_bytes=1024*1024, min_free_bytes=0)
    instance = create_app(cfg)
    instance.state.auth.metadata['google'] = dict(issuer='https://accounts.google.com',
        authorization_endpoint='https://accounts.google.com/auth',
        token_endpoint='https://accounts.google.com/token', jwks_uri='https://accounts.google.com/keys')
    with instance.state.store.connect() as db:
        for user in ('alice', 'bob'):
            db.execute('INSERT INTO sessions VALUES(?,?,?)', (digest(user), 'google:'+user, time.time()+3600))
    return instance


def headers(user='alice'):
    return {'Authorization': 'Bearer '+user}


def test_jobs_require_permission_and_ownership(app):
    client = TestClient(app)
    assert client.post('/v1/jobs', json={'item_id': '123'}).status_code == 401
    assert client.post('/v1/jobs', headers=headers('fake'), json={'item_id':'123'}).status_code == 401
    response = client.post('/v1/jobs', headers=headers(), json={'item_id':'123'})
    assert response.status_code == 202
    job = response.json()['id']
    assert client.get('/v1/jobs/'+job, headers=headers('bob')).status_code == 404
    assert client.get('/v1/jobs/'+job+'/content', headers=headers('bob')).status_code == 404
    assert client.delete('/v1/jobs/'+job, headers=headers('bob')).status_code == 404
    assert client.get('/v1/jobs/'+job+'/content', headers=headers()).status_code == 409
    assert client.delete('/v1/jobs/'+job, headers=headers()).status_code == 202
    assert client.get('/v1/jobs/'+job, headers=headers()).json()['status'] == 'cancelled'


@pytest.mark.parametrize('item', ['../secret', '1;id', '-1', '0', '1\n', '9'*21])
def test_item_validation(app, item):
    assert TestClient(app).post('/v1/jobs', headers=headers(), json={'item_id':item}).status_code == 422


def test_queue_limit_and_revocation(app):
    client = TestClient(app)
    for _ in range(2):
        assert client.post('/v1/jobs', headers=headers(), json={'item_id':'123'}).status_code == 202
    assert client.post('/v1/jobs', headers=headers(), json={'item_id':'123'}).status_code == 429
    app.state.auth.config['allowlist'].remove('google:alice')
    assert client.get('/v1/me', headers=headers()).status_code == 403


def test_expiry_logout_and_defaults(app):
    client = TestClient(app)
    app.state.auth.config['downloads_enabled'] = False
    assert client.post('/v1/jobs', headers=headers(), json={'item_id':'123'}).status_code == 503
    assert client.post('/v1/auth/start/microsoft').status_code == 503
    assert client.post('/v1/auth/logout', headers=headers()).status_code == 204
    assert client.get('/v1/me', headers=headers()).status_code == 401
    with app.state.store.connect() as db:
        db.execute('UPDATE sessions SET expires=0')
    assert client.get('/v1/me', headers=headers('bob')).status_code == 401


@pytest.mark.parametrize('mutation', ['valid', 'unlisted', 'nonce', 'audience', 'issuer', 'expired', 'signature'])
def test_oidc_validation_and_single_use(app, mutation):
    auth = app.state.auth
    result = auth.start('google')
    with auth.store.connect() as db:
        row = db.execute('SELECT * FROM logins WHERE state=?', (result['ticket'],)).fetchone()
    assert row['secret'] != result['poll_secret']
    assert 'code_challenge_method=S256' in result['authorization_url']
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    claims = dict(iss='https://accounts.google.com', sub='alice', aud='test-client',
                  exp=int(time.time())+300, iat=int(time.time()), nonce=row['nonce'])
    if mutation == 'unlisted': claims['sub'] = 'eve'
    if mutation == 'nonce': claims['nonce'] = 'wrong'
    if mutation == 'audience': claims['aud'] = 'other-client'
    if mutation == 'issuer': claims['iss'] = 'https://evil.example'
    if mutation == 'expired': claims['exp'] = int(time.time())-1
    signing_key = rsa.generate_private_key(public_exponent=65537, key_size=2048) if mutation == 'signature' else key
    encoded = jwt.encode(claims, signing_key, algorithm='RS256')
    fake_response = SimpleNamespace(raise_for_status=lambda: None, json=lambda: {'id_token':encoded})
    with patch('relay.auth.httpx.Client') as client, patch('relay.auth.jwt.PyJWKClient') as jwks:
        client.return_value.__enter__.return_value.post.return_value = fake_response
        jwks.return_value.get_signing_key_from_jwt.return_value = SimpleNamespace(key=key.public_key())
        if mutation in ('valid', 'unlisted'):
            auth.complete(result['ticket'], 'code')
        else:
            with pytest.raises(HTTPException): auth.complete(result['ticket'], 'code')
    with pytest.raises(HTTPException): auth.complete(result['ticket'], 'replayed')
    with pytest.raises(HTTPException): auth.poll(result['ticket'], 'wrong-secret')
    polled = auth.poll(result['ticket'], result['poll_secret'])
    if mutation == 'valid':
        assert auth.owner(polled['access_token']) == 'google:alice'
        assert auth.poll(result['ticket'], result['poll_secret'])['status'] == 'consumed'
    elif mutation == 'unlisted':
        assert polled == {'status':'denied', 'identity':'google:eve'}
    else:
        assert polled['status'] == 'failed'


def test_content_download_range_and_hash(app):
    client = TestClient(app)
    job = client.post('/v1/jobs', headers=headers(), json={'item_id':'123'}).json()['id']
    root = Path(app.state.auth.config['data_dir']) / 'archives'
    root.mkdir()
    (root / (job+'.zip')).write_bytes(b'0123456789')
    with app.state.store.connect() as db:
        db.execute("UPDATE jobs SET status='ready',sha256='testhash' WHERE id=?", (job,))
    response = client.get('/v1/jobs/'+job+'/content', headers={**headers(), 'Range':'bytes=2-5'})
    assert response.status_code == 206
    assert response.content == b'2345'
    assert response.headers['x-content-sha256'] == 'testhash'


def test_safe_archive_preflight(tmp_path):
    root = tmp_path/'content'
    root.mkdir()
    (root/'project.json').write_text('{}')
    assert len(content_files(root, 10)) == 1
    with pytest.raises(ValueError, match='item_too_large'): content_files(root, 1)
    (root/'CON.txt').write_text('bad')
    with pytest.raises(ValueError, match='unsafe_content'): content_files(root, 100)


def test_symlink_rejected(tmp_path):
    root = tmp_path/'content'
    root.mkdir()
    target = tmp_path/'outside'
    target.write_text('private')
    try:
        (root/'link').symlink_to(target)
    except OSError:
        pytest.skip('Host does not permit symlinks')
    with pytest.raises(ValueError, match='unsafe_content'): content_files(root, 100)


def test_restart_marks_pending_interrupted(app):
    client = TestClient(app)
    job = client.post('/v1/jobs', headers=headers(), json={'item_id':'123'}).json()['id']
    Store(app.state.store.path)
    assert client.get('/v1/jobs/'+job, headers=headers()).json()['status'] == 'interrupted'


def test_request_limit(app):
    client = TestClient(app)
    assert client.post('/v1/jobs', headers=headers(), content=b'x'*4097).status_code == 413


@pytest.mark.parametrize('mode', ['success', 'missing_marker', 'cancel'])
def test_worker_process_archive_and_failure_recovery(app, tmp_path, mode):
    worker = app.state.worker
    cfg = app.state.auth.config
    steam = Path(cfg['steam_root'])
    steam.mkdir()
    fake = tmp_path/'fake_steamcmd.py'
    fake.write_text('''import pathlib,sys
root=pathlib.Path(sys.argv[1])/'steamapps'/'workshop'/'content'/'431960'/'123'
root.mkdir(parents=True)
(root/'project.json').write_text('{"type":"scene"}')
(root/'scene.pkg').write_bytes(b'wallpaper-payload')
if sys.argv[2] != 'missing_marker': print('Success. Downloaded item 123 to content')
''')
    cfg.update(steam_account='test_account', steamcmd=str(fake))
    client = TestClient(app)
    job = client.post('/v1/jobs', headers=headers(), json={'item_id':'123'}).json()['id']
    with app.state.store.connect() as db:
        if mode == 'cancel': db.execute('UPDATE jobs SET cancel=1 WHERE id=?', (job,))
        row = db.execute('SELECT * FROM jobs WHERE id=?', (job,)).fetchone()
    original = subprocess.Popen
    def launch(args, **kwargs):
        assert args[1:3] == ['+login', 'test_account']
        assert args[3:] == ['+workshop_download_item', '431960', '123', 'validate', '+quit']
        return original([sys.executable, str(fake), str(steam), mode], **kwargs)
    with patch('relay.worker.subprocess.Popen', side_effect=launch):
        worker.run(row)
    result = client.get('/v1/jobs/'+job, headers=headers()).json()
    if mode == 'success':
        assert result['status'] == 'ready'
        response = client.get('/v1/jobs/'+job+'/content', headers=headers())
        assert hashlib.sha256(response.content).hexdigest() == result['sha256']
        with zipfile.ZipFile(worker.data/'archives'/(job+'.zip')) as archive:
            assert archive.read('scene.pkg') == b'wallpaper-payload'
    else:
        assert result['status'] in ('failed', 'cancelled')
        assert client.get('/v1/jobs/'+job+'/content', headers=headers()).status_code == 409
    # Failure/cancellation never silently deletes Steam's downloaded content.
    # Cancellation can happen before the child has created it, so check failures only.
    if mode == 'missing_marker':
        assert (steam/'steamapps/workshop/content/431960/123/scene.pkg').exists()
