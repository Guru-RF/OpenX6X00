"""Issue #58: x6100-tailscale's Funnel (rootfs/usr/sbin/x6100-tailscale, its own functions) against a fake tailscaled:
a local API on a UNIX socket in a temporary folder that answers status, serve-config (with its Etag, If-Match and
the rule that a port's serve type does not change in one step) and query-feature as tailscaled 1.104.2 does
(ipn/localapi/serve.go, ipn/ipnlocal/serve.go). Nothing of Tailscale's runs, nothing goes to the network."""
import hashlib
import http.server
import json
import os
import runpy
import socketserver
import tempfile
import threading
from pathlib import Path

repo = Path(__file__).resolve().parents[3]
ts = runpy.run_path(str(repo / 'rootfs/usr/sbin/x6100-tailscale'), run_name='tailscale-check')
g = ts['funnel_step'].__globals__      # the functions' own globals, not runpy's copy
NAME = 'x6200.tail1234.ts.net'
CAPS = {'https': None, 'funnel': None, 'https://tailscale.com/cap/funnel-ports?ports=443,8443,10000': None}
ENABLE = 'https://login.tailscale.com/f/funnel?node=nTEST123'


class Fake:
    serve = None            # tailscaled's serve config, None: none set
    status = {}
    gets = posts = queries = 0
    changed_meanwhile = False
    seen_if_match = []

    @classmethod
    def etag(cls):
        return hashlib.sha256(json.dumps(cls.serve, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def kind(h):
    return 'https' if h.get('HTTPS') else 'http' if h.get('HTTP') else 'tcp'


class LocalAPI(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, code, body=b'', headers=None):
        body = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        assert self.headers.get('Sec-Tailscale') == 'localapi'
        if self.path.startswith('/localapi/v0/status'):
            return self.reply(200, json.dumps(Fake.status))
        if self.path == '/localapi/v0/serve-config':
            Fake.gets += 1
            return self.reply(200, json.dumps(Fake.serve), {'Etag': Fake.etag(), 'Content-Type': 'application/json'})
        self.reply(404, 'no such')

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length') or 0))
        if self.path == '/localapi/v0/serve-config':
            Fake.seen_if_match.append(self.headers.get('If-Match'))
            if Fake.changed_meanwhile:      # someone else's change between our GET and our POST
                Fake.changed_meanwhile = False
                Fake.serve = dict(Fake.serve or {}, Web=dict((Fake.serve or {}).get('Web') or {},
                                  **{NAME + ':8443': {'Handlers': {'/': {'Text': 'hello'}}}}))
            if self.headers.get('If-Match') != Fake.etag():
                return self.reply(412, 'etag mismatch')
            new = json.loads(body)
            for port in (new.get('TCP') or {}):     # a background listener over a foreground session's
                if any(port in (fg.get('TCP') or {}) for fg in ((Fake.serve or {}).get('Foreground') or {}).values()):
                    return self.reply(500, json.dumps({'error': f'updating config: foreground listener already '
                                                                f'exists for port {port}'}))
            for port, h in (new.get('TCP') or {}).items():
                old = ((Fake.serve or {}).get('TCP') or {}).get(port)
                if old and kind(old) != kind(h):
                    return self.reply(500, json.dumps({'error': f'updating config: want to serve "{kind(h)}", but '
                                                                f'port {port} is already serving "{kind(old)}"'}))
            Fake.serve = new or None
            Fake.posts += 1
            return self.reply(200)
        if self.path == '/localapi/v0/query-feature?feature=funnel':
            Fake.queries += 1
            return self.reply(200, json.dumps({'Text': 'Funnel is not enabled on your tailnet.', 'URL': ENABLE,
                                               'ShouldWait': True}))
        self.reply(404, 'no such')


class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True


results = []


def check(what, ok, got=''):
    results.append(ok)
    print(('ok   ' if ok else 'FAIL ') + what + (': ' + str(got) if got != '' else ''))


def by_hand(port=50001):
    """What "tailscale funnel 50001" leaves in the serve config, as in #58"""
    return {'TCP': {'443': {'HTTPS': True}},
            'Web': {NAME + ':443': {'Handlers': {'/': {'Proxy': f'http://127.0.0.1:{port}'}}}},
            'AllowFunnel': {NAME + ':443': True}}


here = os.getcwd()
with tempfile.TemporaryDirectory(prefix='x6ts-', dir=os.environ.get('AUDIT_TMP')) as td:
    os.chdir(td)                        # the socket by a short relative name: a UNIX socket's path has 108 bytes
    try:
        server = Server('ts.sock', LocalAPI)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        funnel_dir = Path(td) / 'funnel'
        (funnel_dir / 'static').mkdir(parents=True)
        for page in ('panel.html', 'remote.html'):     # as the web server writes them at its start
            (funnel_dir / page).write_text('<!doctype html>')
        g.update(SOCK='ts.sock', CONF=str(Path(td) / 'tailscale.conf'), REMOTE_CONF=str(Path(td) / 'remote.conf'),
                 TCI_CONF=str(Path(td) / 'tci.conf'), FUNNEL_DIR=str(funnel_dir))
        Path(g['CONF']).write_text('enabled=1\n')
        f = g['Funnel']()
        step = g['funnel_step']
        me = {'DNSName': NAME + '.', 'TailscaleIPs': ['100.101.102.103'], 'CapMap': {}}
        Fake.status = st = {'BackendState': 'Running', 'Self': me}

        # The settings: funnel= and enabled= written apart, each keeping the other
        g['cmd_funnel']('on')
        g['set_enabled'](True)
        check('the setting: funnel on, Tailscale on, both kept', Path(g['CONF']).read_text() == 'enabled=1\nfunnel=1\n',
              Path(g['CONF']).read_text().replace('\n', ' '))
        g['cmd_funnel']('off')
        check('funnel off keeps enabled', g['enabled']() and not g['funnel_wanted']())

        # 1. Funnel off, a funnel made by hand (#58): taken off the internet, the tailnet's serving kept
        Fake.serve = by_hand()
        s = step(f, st)
        check('off: a funnel made by hand taken off the internet', 'AllowFunnel' not in (Fake.serve or {}), Fake.serve)
        check('off: what it serves to the tailnet stays', (Fake.serve or {}).get('Web') == by_hand()['Web'])
        check('off: the status says so', s['funnel'] == 'off' and 'made by hand' in s.get('funnel_text', ''), s)
        posts = Fake.posts
        gets = Fake.gets
        step(f, st)
        check('off: nothing asked again within the minute', Fake.posts == posts and Fake.gets == gets)

        # 2. On, the tailnet without HTTPS and Funnel: the enable link, no funnel meanwhile
        Fake.serve = by_hand()
        g['cmd_funnel']('on')
        s = step(f, st)
        check('on, not allowed yet: the enable link asked for', Fake.queries == 1 and s.get('funnel_url') == ENABLE, s)
        check('on, not allowed yet: status enable', s['funnel'] == 'enable')
        check('on, not allowed yet: no funnel made by hand meanwhile', 'AllowFunnel' not in (Fake.serve or {}))
        step(f, st)
        check('on, not allowed yet: the link not asked again at every poll', Fake.queries == 1)

        # 3. Allowed (the caps come with the netmap): set, with If-Match; the hand-made "/" proxy replaced
        me['CapMap'] = dict(CAPS)
        etag = Fake.etag()
        s = step(f, st)
        web = ((Fake.serve or {}).get('Web') or {}).get(NAME + ':443', {}).get('Handlers', {})
        check('on: status on, the public address', s['funnel'] == 'on' and s.get('public') == f'https://{NAME}/panel', s)
        check('on: set with If-Match', Fake.seen_if_match[-1] == etag)
        check('on: AllowFunnel the radio\'s own alone', Fake.serve.get('AllowFunnel') == {NAME + ':443': True})
        check('on: 443 served as HTTPS', Fake.serve.get('TCP') == {'443': {'HTTPS': True}})
        check('on: the pages from FUNNEL_DIR, each file on its own',
              web.get('/panel') == {'Path': f'{funnel_dir}/panel.html'} and web.get('/remote') == {'Path': f'{funnel_dir}/remote.html'}
              and web.get('/static/js/radio_session.js') == {'Path': f'{funnel_dir}/static/js/radio_session.js'}
              and '/static/' not in web)
        check('on: the two sockets', web.get('/ws/remote') == {'Proxy': 'http://127.0.0.1:50002'}
              and web.get('/ws/tci') == {'Proxy': 'http://127.0.0.1:50001'})
        check('on: everything else to the Panel, nothing to the web server or TCI\'s programs',
              web.get('/') == {'Redirect': '/panel'} and not any('80' in (h.get('Proxy') or '') for h in web.values()))
        check('on: nothing else proxied', sorted(k for k, h in web.items() if 'Proxy' in h) == ['/ws/remote', '/ws/tci'])

        # 4. A port changes: set again; another serving to the tailnet is kept; someone changed it meanwhile
        Path(g['REMOTE_CONF']).write_text('enabled=1\nport=50012\n')
        Fake.changed_meanwhile = True
        posts = Fake.posts
        s = step(f, st)
        web = Fake.serve['Web'][NAME + ':443']['Handlers']
        check('a new remote port: the socket follows', web['/ws/remote'] == {'Proxy': 'http://127.0.0.1:50012'})
        check('changed meanwhile (412): read again and set', Fake.posts == posts + 1 and s['funnel'] == 'on', Fake.seen_if_match[-2:])
        check('serving to the tailnet on 8443 kept', NAME + ':8443' in Fake.serve['Web'])

        # 5. The name changes (a new network name, or x6200-1 on a new card): the old one's entries go
        me['DNSName'] = 'x6200-1.tail1234.ts.net.'
        s = step(f, st)
        check('a new name: on under it', s.get('public') == 'https://x6200-1.tail1234.ts.net/panel', s)
        check('a new name: only its AllowFunnel', Fake.serve['AllowFunnel'] == {'x6200-1.tail1234.ts.net:443': True})
        check('a new name: the old name\'s pages gone', NAME + ':443' not in Fake.serve['Web'])
        me['DNSName'] = NAME + '.'
        step(f, st)

        # 6. A minute on: looked at again, a funnel added by hand meanwhile taken off
        Fake.serve['AllowFunnel'][NAME + ':8443'] = True
        f.done -= 61
        step(f, st)
        check('a minute later: the funnel added by hand taken off', Fake.serve['AllowFunnel'] == {NAME + ':443': True})

        # 7. A raw TCP funnel made by hand on 443 (tailscale funnel --tcp 443 ...): dropped first, then ours
        Fake.serve = {'TCP': {'443': {'TCPForward': '127.0.0.1:22'}}, 'AllowFunnel': {NAME + ':443': True}}
        f.done -= 61
        s = step(f, st)
        check('a TCP forward on 443 made by hand: replaced in two steps', s['funnel'] == 'on' and
              Fake.serve['TCP'] == {'443': {'HTTPS': True}}, s)

        # 8. The pages missing (the web server did not write them): not set, said
        (funnel_dir / 'panel.html').unlink(missing_ok=True)
        Fake.serve = None
        f.done -= 61
        s = step(f, st)
        check('the pages missing: error, nothing on the internet', s['funnel'] == 'error' and Fake.serve is None, s)
        (funnel_dir / 'panel.html').write_text('<!doctype html>')
        s = step(f, st)
        check('the pages there at the next poll: on', s['funnel'] == 'on')

        # 9. Off: the radio's own entries gone, 443 freed, the tailnet's 8443 kept
        Fake.serve['Web'][NAME + ':8443'] = {'Handlers': {'/': {'Text': 'hello'}}}
        Fake.serve['TCP']['8443'] = {'HTTPS': True}
        g['cmd_funnel']('off')
        s = step(f, st)
        check('off again: no AllowFunnel, no pages, 443 free', s['funnel'] == 'off' and 'AllowFunnel' not in Fake.serve
              and NAME + ':443' not in Fake.serve['Web'] and '443' not in Fake.serve['TCP'], Fake.serve)
        check('off again: the tailnet\'s 8443 kept', Fake.serve['TCP'].get('8443') == {'HTTPS': True})
        check('off again: the radio\'s own funnel not called one made by hand', 'made by hand' not in
              s.get('funnel_text', ''), s)

        # 9b. On, while "tailscale funnel 443" runs in the foreground over SSH (a CLI installed by hand): tailscaled
        # refuses the radio's 443, and the hand-made funnel still goes off the internet
        Fake.serve = {'Foreground': {'ssh-session': {'TCP': {'443': {'HTTPS': True}},
                                                     'Web': by_hand()['Web'], 'AllowFunnel': {NAME + ':443': True}}}}
        g['cmd_funnel']('on')
        f.done -= 61
        s = step(f, st)
        fg = Fake.serve['Foreground']['ssh-session']
        check('a foreground funnel on 443: the radio\'s refused, said', s['funnel'] == 'error' and 'foreground' in
              s.get('funnel_text', ''), s)
        check('a foreground funnel on 443: taken off the internet all the same', 'AllowFunnel' not in fg and
              fg.get('Web') == by_hand()['Web'], Fake.serve)
        g['cmd_funnel']('off')
        step(f, st)

        # 10. tailscaled not up yet (a login wanted): waiting, or off
        g['cmd_funnel']('on')
        check('while Tailscale starts: funnel starting', g['funnel_idle']()['funnel'] == 'starting')
        g['cmd_funnel']('off')
        check('while Tailscale starts, Funnel off: off', g['funnel_idle']() == {'funnel': 'off'})

        # 11. The caps: HTTPS without Funnel, or Funnel without port 443
        check('caps: https alone', not g['caps_ok']({'CapMap': {'https': None}}))
        check('caps: 443 not among the ports', not g['caps_ok']({'CapMap': {'https': None, 'funnel': None,
              'https://tailscale.com/cap/funnel-ports?ports=8443,10000': None}}))
        check('caps: 443 in a range', g['caps_ok']({'CapMap': {'https': None, 'funnel': None,
              'https://tailscale.com/cap/funnel-ports?ports=400-500': None}}))

        # 12. The files the web server writes for the Funnel pages are the ones served, one by one
        import ast
        tree = ast.parse((repo / 'webserver/src/x6100_webserver/apps.py').read_text())
        web_files = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign)
                         and any(isinstance(t, ast.Name) and t.id == 'FUNNEL_FILES' for t in n.targets))
        check('the web server writes the files tailscaled serves', tuple(web_files) == tuple(g['FUNNEL_FILES']),
              web_files)
        server.shutdown()
        server.server_close()
    finally:
        os.chdir(here)

bad = results.count(False)
print('x6100-tailscale Funnel against a fake tailscaled (#58): %d of %d cases as they should be'
      % (len(results) - bad, len(results)))
assert not bad
