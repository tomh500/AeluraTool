import base64
import hashlib
import secrets
import time
from urllib.parse import urlencode, urlparse

import httpx
import jwt
from fastapi import HTTPException

from .store import digest


class Auth:
    def __init__(self, config, store):
        self.config, self.store = config, store
        self.metadata = {}

    def provider(self, name):
        provider = self.config.get('providers', {}).get(name)
        if not provider or not provider.get('client_id'):
            raise HTTPException(503, 'relay.identity_not_configured')
        if name not in self.metadata:
            url = provider['discovery_url']
            if urlparse(url).scheme != 'https':
                raise ValueError('Discovery must use HTTPS')
            with httpx.Client(timeout=15) as client:
                response = client.get(url)
                response.raise_for_status()
                metadata = response.json()
            for key in ('issuer', 'authorization_endpoint', 'token_endpoint', 'jwks_uri'):
                if urlparse(metadata[key]).scheme != 'https' or '{' in metadata[key]:
                    raise ValueError('Provider must have a fixed HTTPS issuer and endpoints')
            self.metadata[name] = metadata
        return provider, self.metadata[name]

    def start(self, name):
        provider, metadata = self.provider(name)
        state, secret, nonce, verifier = [secrets.token_urlsafe(32) for _ in range(4)]
        challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).decode().rstrip('=')
        now = time.time()
        with self.store.connect() as db:
            db.execute('DELETE FROM logins WHERE expires < ?', (now,))
            db.execute('DELETE FROM sessions WHERE expires < ?', (now,))
            if db.execute('SELECT count(*) FROM logins').fetchone()[0] >= 100:
                raise HTTPException(429, 'relay.busy')
            db.execute('INSERT INTO logins(state,secret,provider,nonce,verifier,expires) VALUES(?,?,?,?,?,?)',
                       (state, digest(secret), name, nonce, verifier, now + 600))
        query = urlencode(dict(client_id=provider['client_id'], response_type='code',
                               redirect_uri=self.callback_url, scope='openid', state=state,
                               nonce=nonce, code_challenge=challenge, code_challenge_method='S256'))
        return dict(ticket=state, poll_secret=secret, expires_in=600,
                    authorization_url=metadata['authorization_endpoint'] + '?' + query)

    @property
    def callback_url(self):
        return self.config['public_url'].rstrip('/') + '/v1/auth/callback'

    def complete(self, state, code):
        with self.store.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            row = db.execute('SELECT * FROM logins WHERE state=? AND expires>? AND status=?',
                             (state, time.time(), 'pending')).fetchone()
            if not row:
                raise HTTPException(400, 'relay.login_expired')
            db.execute("UPDATE logins SET status='processing' WHERE state=?", (state,))
        try:
            provider, metadata = self.provider(row['provider'])
            with httpx.Client(timeout=15) as client:
                response = client.post(metadata['token_endpoint'], data=dict(
                    grant_type='authorization_code', code=code, client_id=provider['client_id'],
                    client_secret=provider.get('client_secret', ''), redirect_uri=self.callback_url,
                    code_verifier=row['verifier']))
                response.raise_for_status()
                encoded = response.json()['id_token']
            key = jwt.PyJWKClient(metadata['jwks_uri'], timeout=15).get_signing_key_from_jwt(encoded)
            claims = jwt.decode(encoded, key.key, algorithms=['RS256'],
                                audience=provider['client_id'], issuer=metadata['issuer'],
                                options={'require': ['exp', 'iat', 'sub', 'iss', 'aud', 'nonce']})
            if not secrets.compare_digest(claims['nonce'], row['nonce']):
                raise ValueError('Nonce mismatch')
            if claims.get('azp', provider['client_id']) != provider['client_id']:
                raise ValueError('Authorized party mismatch')
            if not isinstance(claims['sub'], str) or not claims['sub']:
                raise ValueError('Missing subject')
            owner = row['provider'] + ':' + claims['sub']
            status = 'approved' if owner in self.config.get('allowlist', []) else 'denied'
            token = secrets.token_urlsafe(48) if status == 'approved' else None
            with self.store.connect() as db:
                if token:
                    db.execute('INSERT INTO sessions VALUES(?,?,?)', (digest(token), owner, time.time()+3600))
                db.execute('UPDATE logins SET status=?,subject=?,token=? WHERE state=?',
                           (status, owner, token, state))
        except Exception:
            with self.store.connect() as db:
                db.execute("UPDATE logins SET status='failed' WHERE state=?", (state,))
            raise HTTPException(400, 'relay.login_failed') from None

    def poll(self, ticket, secret):
        with self.store.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            row = db.execute('SELECT * FROM logins WHERE state=? AND expires>?', (ticket, time.time())).fetchone()
            if not row or not secrets.compare_digest(row['secret'], digest(secret)):
                raise HTTPException(401, 'relay.invalid_login_ticket')
            result = {'status': row['status']}
            if row['status'] in ('approved', 'denied'):
                result['identity'] = row['subject']
            if row['status'] == 'approved':
                result.update(access_token=row['token'], token_type='Bearer', expires_in=3600)
                db.execute("UPDATE logins SET status='consumed', token=NULL WHERE state=?", (ticket,))
            return result

    def owner(self, token):
        with self.store.connect() as db:
            row = db.execute('SELECT owner FROM sessions WHERE token_hash=? AND expires>?',
                             (digest(token), time.time())).fetchone()
        if not row:
            raise HTTPException(401, 'relay.invalid_session')
        # Recheck permission on every request; changing the allowlist and restarting revokes access.
        if row['owner'] not in self.config.get('allowlist', []):
            raise HTTPException(403, 'relay.access_denied')
        return row['owner']
