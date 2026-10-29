"""Issue #58: x6100-tailscale's Funnel (rootfs/usr/sbin/x6100-tailscale, its own functions) against a fake tailscaled:
a local API on a UNIX socket in a temporary folder that answers status, serve-config (with its Etag, If-Match and
the rule that a port's serve type does not change in one step) and query-feature as tailscaled 1.104.2 does
(ipn/localapi/serve.go, ipn/ipnlocal/serve.go). Nothing of Tailscale's runs, nothing goes to the network.
x6100-update's step back to a version without Funnel runs x6100-tailscale as a program: here a wrapper that runs
it against the same fake, on temporary installed files."""
import ast
import base64
import contextlib
import hashlib
import http.server
import io
import json
import os
import runpy
import socketserver
import stat
import sys
import tarfile
import tempfile
import threading
import time
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
    refuse = ''             # tailscaled refusing a new serve config, as it does with no login: "netMap is nil"
    cut = False             # its answer to a serve-config read breaking off (tailscaled stopping or crashing meanwhile)
    store = None            # its state file: a serve config it takes is written there, as its FileStore does (_serve/a1b2)
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
            if Fake.cut:                    # fewer bytes than its Content-Length, then the connection closes
                self.send_response(200)
                self.send_header('Content-Length', '400')
                self.end_headers()
                self.wfile.write(b'{"TCP": {"443": ')
                self.close_connection = True
                return
            return self.reply(200, json.dumps(Fake.serve), {'Etag': Fake.etag(), 'Content-Type': 'application/json'})
        self.reply(404, 'no such')

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length') or 0))
        if self.path == '/localapi/v0/serve-config':
            if Fake.refuse:
                return self.reply(500, json.dumps({'error': Fake.refuse}))
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
            if Fake.store:
                state = json.loads(Path(Fake.store).read_text())
                state['_serve/a1b2'] = base64.b64encode(json.dumps(new).encode()).decode()
                Path(Fake.store).write_text(json.dumps(state))
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
        Fake.serve = by_hand()
        s = step(f, st)
        check('the pages missing: a funnel made by hand taken off meanwhile', s['funnel'] == 'error' and
              'AllowFunnel' not in Fake.serve, Fake.serve)
        Fake.serve = None
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
        tree = ast.parse((repo / 'webserver/src/x6100_webserver/apps.py').read_text())
        web_files = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign)
                         and any(isinstance(t, ast.Name) and t.id == 'FUNNEL_FILES' for t in n.targets))
        check('the web server writes the files tailscaled serves', tuple(web_files) == tuple(g['FUNNEL_FILES']),
              web_files)

        # 13. Tailscale stopping (off, the shutdown, a start that failed): the radio's funnel and any made by hand go out
        # of tailscaled first, and out of its state file once it has stopped, so that a version without Funnel, gone
        # back to by hand, never starts tailscaled with one. The setting stays, for the next start
        state_dir = Path(td) / 'state'
        state_dir.mkdir()
        alive = Path(td) / 'tailscaled.alive'          # a tailscaled runs on the radio's state, for tailscaled_pids()
        g.update(STATE=str(state_dir), STATE_FILE=str(state_dir / 'tailscaled.state'), BIN=str(Path(td) / 'tailscaled'))
        g['tailscaled_pids'] = lambda: [4242] if alive.exists() else []

        class Daemon:
            """The supervisor's tailscaled as stop_daemon sees it; the serve config as it was when it was stopped"""
            def __init__(self):
                self.returncode = None
                alive.touch()

            def poll(self):
                return self.returncode

            def terminate(self):
                self.at_stop = json.loads(json.dumps(Fake.serve))
                self.returncode = 0
                alive.unlink(missing_ok=True)
            kill = terminate

            def wait(self, timeout=None):
                return self.returncode

        def on_and_by_hand():
            """The radio's funnel on, a funnel made by hand on 8443, serving to the tailnet alone on 8444"""
            sc, _ = g['funnel_config'](None, NAME, 50002, 50001, True)
            sc['Web'][NAME + ':8443'] = {'Handlers': {'/': {'Proxy': 'http://127.0.0.1:50001'}}}
            sc['Web'][NAME + ':8444'] = {'Handlers': {'/': {'Text': 'hello'}}}
            sc['TCP'].update({'8443': {'HTTPS': True}, '8444': {'HTTPS': True}})
            sc['AllowFunnel'][NAME + ':8443'] = True
            return sc

        def tailnet_only(sc):
            """No funnel, none of the radio's pages, 443 free; the tailnet's own serving (8443 and 8444) as it was"""
            sc = sc or {}
            return (not g['funnel_left'](sc) and set(sc.get('Web') or {}) == {NAME + ':8443', NAME + ':8444'}
                    and set(sc.get('TCP') or {}) == {'8443', '8444'})

        def b64(obj):
            return base64.b64encode(json.dumps(obj).encode()).decode()

        def stored():
            """The serve config in the state file"""
            return json.loads(base64.b64decode(json.loads(Path(g['STATE_FILE']).read_text())['_serve/a1b2']))

        g['cmd_funnel']('on')
        Fake.serve = on_and_by_hand()
        d = Daemon()
        g['stop_tailscaled'](d)
        check('stopping: the radio\'s funnel and one made by hand out of tailscaled before it stops, the tailnet\'s '
              'serving kept', tailnet_only(d.at_stop), d.at_stop)
        check('stopping: tailscaled stopped, the Funnel setting kept for the next start',
              not alive.exists() and g['funnel_wanted']())

        others = {'_machinekey': base64.b64encode(b'privkey:00ff').decode(),
                  '_current-profile': base64.b64encode(b'a1b2').decode(),
                  'profile-a1b2': b64({'ControlURL': 'https://controlplane.tailscale.com', 'WantRunning': True})}
        Path(g['STATE_FILE']).write_text(json.dumps(dict(others, **{'_serve/a1b2': b64(on_and_by_hand())})))
        Fake.serve = on_and_by_hand()
        Fake.refuse = 'netMap is nil'
        d = Daemon()
        g['stop_tailscaled'](d)
        Fake.refuse = ''
        check('stopping, tailscaled refusing the change (no login): out of its state file once it stopped',
              g['funnel_left'](d.at_stop) and tailnet_only(stored()), stored())
        kept = json.loads(Path(g['STATE_FILE']).read_text())
        check('the state file: the login, the keys and the prefs as they were', {k: kept[k] for k in others} == others)
        check('the state file: still readable by root alone', stat.S_IMODE(os.stat(g['STATE_FILE']).st_mode) == 0o600)

        Path(g['STATE_FILE']).write_text(json.dumps(dict(others, **{'_serve/a1b2': b64(on_and_by_hand())})))
        alive.touch()                                  # one a killed supervisor left behind, say
        g['stop_tailscaled'](None)
        check('another tailscaled still runs: its state file not written under it', g['funnel_left'](stored()))
        alive.unlink()
        Path(g['STATE_FILE']).write_text(json.dumps(dict(others, **{'_serve/a1b2': b64(on_and_by_hand())})))
        Fake.serve = on_and_by_hand()
        Fake.cut = True
        d = Daemon()
        try:
            g['stop_tailscaled'](d)
            check('stopping, tailscaled\'s answer breaking off: stopped all the same, the funnel out of its state file',
                  not alive.exists() and d.returncode == 0 and tailnet_only(stored()), stored())
        except Exception as e:
            check('stopping, tailscaled\'s answer breaking off: stopped all the same', False,
                  f'{type(e).__name__} raised, tailscaled still running: {alive.exists()}')
        Fake.cut = False
        alive.unlink(missing_ok=True)
        Path(g['STATE_FILE']).write_text('{"_serve/a1b2": ')
        try:
            g['stop_tailscaled'](None)
            check('a state file that is no JSON: the stop goes ahead, the file left as it is',
                  Path(g['STATE_FILE']).read_text() == '{"_serve/a1b2": ')
        except Exception as e:
            check('a state file that is no JSON: the stop goes ahead', False, e)
        Path(g['STATE_FILE']).unlink()

        run_def = next(n for n in ast.parse((repo / 'rootfs/usr/sbin/x6100-tailscale').read_text()).body
                       if isinstance(n, ast.FunctionDef) and n.name == 'run')
        calls = [n.func.id for n in ast.walk(run_def) if isinstance(n, ast.Call) and isinstance(n.func, ast.Name)]
        check('the supervisor stops tailscaled that way alone: turned off, the shutdown, a failed start',
              calls.count('stop_tailscaled') == 2 and 'stop_daemon' not in calls)
        g['cmd_funnel']('off')

        # 14. funnel-clear, for x6100-update before a step back to a version without Funnel: the setting off, the funnel
        # out of tailscaled (through its local API while it runs, out of its state file while it does not), waited for
        g.update(STATUS=str(Path(td) / 'status'), LOCK=str(Path(td) / 'lock'))

        def clear(seconds='2'):
            """The command as x6100-update runs it: its exit code, and what it says"""
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = g['main'](['funnel-clear', seconds])
            return code, (out.getvalue() + err.getvalue()).strip()

        def with_funnel():
            """The state file with the radio's funnel and one made by hand in its serve config"""
            Path(g['STATE_FILE']).write_text(json.dumps(dict(others, **{'_serve/a1b2': b64(on_and_by_hand())})))

        g['cmd_funnel']('on')
        Fake.serve = on_and_by_hand()
        alive.touch()
        code, out = clear()
        check('funnel-clear, tailscaled running: the Funnel setting off, Tailscale left on',
              code == 0 and not g['funnel_wanted']() and g['enabled'](), out)
        check('funnel-clear, tailscaled running: no funnel left in it, the tailnet\'s serving kept',
              tailnet_only(Fake.serve), Fake.serve)
        check('funnel-clear: says it took one off', out == 'ok Funnel taken off', out)
        code, out = clear()
        check('funnel-clear, nothing on: ok at once, and says so', code == 0 and out == 'ok no funnel', out)

        g['cmd_funnel']('on')
        Fake.serve = on_and_by_hand()
        Fake.refuse = 'netMap is nil'
        t0 = time.time()
        code, out = clear('2')
        took = time.time() - t0
        Fake.refuse = ''
        check('funnel-clear, tailscaled refusing the change (no login): an error naming it, after the wait',
              code == 1 and 'netMap is nil' in out and 1.5 < took < 6, f'{out} ({took:.1f} s)')
        check('funnel-clear, not sure: the setting off all the same', not g['funnel_wanted']())
        Fake.serve = on_and_by_hand()
        Fake.cut = True
        try:
            t0 = time.time()
            code, out = clear('2')
            took = time.time() - t0
            check('funnel-clear, tailscaled\'s answers breaking off: tried again, then an error line naming it',
                  code == 1 and out.startswith('error tailscaled: serve-config: its answer broke off') and 1.5 < took < 6,
                  f'{out} ({took:.1f} s)')
        except Exception as e:
            check('funnel-clear, tailscaled\'s answers breaking off: an error line', False, f'{type(e).__name__} raised')
        Fake.cut = False

        real_supervisor_pid = g['supervisor_pid']
        g['supervisor_pid'] = lambda: 4343             # a supervisor runs; its status still says on
        Fake.serve = None
        Path(g['STATUS']).write_text('state=running\nfunnel=on\n')
        code, out = clear('1')
        check('funnel-clear: a supervisor that has not taken the setting: not sure, said', code == 1 and 'setting' in out,
              out)
        timer = threading.Timer(1.0, lambda: Path(g['STATUS']).write_text('state=running\nfunnel=off\n'))
        timer.start()
        t0 = time.time()
        code, out = clear('5')
        took = time.time() - t0
        check('funnel-clear: waits for the supervisor to say Funnel is off', code == 0 and 0.9 < took < 4,
              f'{out} ({took:.1f} s)')
        g['supervisor_pid'] = real_supervisor_pid
        Path(g['STATUS']).unlink()

        # tailscaled not connected yet (starting, no internet, no login): with no netmap its serve config reads as none
        # (reloadServeConfigLocked), while its state keeps the funnel it loads again once it connects
        g['cmd_funnel']('on')
        with_funnel()
        Fake.serve = None
        code, out = clear('1')
        check('funnel-clear, tailscaled not connected, the funnel in its state for when it is: not sure, said',
              code == 1 and 'in its state for when it connects' in out and g['funnel_left'](stored()), out)
        check('funnel-clear, not connected: its state file not written under it, the setting off',
              g['funnel_left'](stored()) and not g['funnel_wanted']())
        Fake.serve, Fake.store = stored(), g['STATE_FILE']     # connected: it loaded the funnel from its state
        code, out = clear('2')
        Fake.store = None
        check('funnel-clear, then connected: out through its local API, and so out of its state, ok',
              code == 0 and out == 'ok Funnel taken off' and tailnet_only(Fake.serve) and tailnet_only(stored()), out)

        alive.unlink()
        g['cmd_funnel']('on')
        with_funnel()
        code, out = clear()
        check('funnel-clear, tailscaled not running: out of its state file, the setting off',
              code == 0 and out == 'ok Funnel taken off' and tailnet_only(stored()) and not g['funnel_wanted'](), out)
        Path(g['STATE_FILE']).write_text('{"_serve/a1b2": ')
        code, out = clear('1')
        check('funnel-clear, a state file that cannot be read: not sure, said', code == 1 and 'state' in out, out)
        Path(g['STATE_FILE']).unlink()
        conf = g['CONF']
        g['CONF'] = str(Path(td) / 'never.conf')       # Tailscale never used on this card
        code, out = clear()
        check('funnel-clear, Tailscale never used: ok at once, nothing written',
              code == 0 and out == 'ok no funnel' and not Path(g['CONF']).exists(), out)
        g['CONF'] = conf

        # 15. x6100-update going back by hand to a version before Funnel (#58), the Update page's button (rollback) or
        # an older version's bundle (install): Funnel off first, through x6100-tailscale, or not at all. The start loop's
        # own step back (boot-check) tries the same quietly, and goes back whatever came of it
        upd = Path(td) / 'upd'
        work, ustate, inst = upd / 'work', upd / 'state', upd / 'installed'
        for folder in (work, ustate, inst):
            folder.mkdir(parents=True)
        app, ver = inst / 'application', inst / 'version'
        app.write_text('application 0.3.6')
        ver.write_text('0.3.6\n')
        asked = upd / 'asked'                          # each run of x6100-tailscale, with its arguments
        helper = upd / 'x6100-tailscale'
        paths = {k: g[k] for k in ('SOCK', 'CONF', 'STATUS', 'LOCK', 'STATE', 'STATE_FILE', 'BIN', 'REMOTE_CONF',
                                   'TCI_CONF', 'FUNNEL_DIR')}
        helper.write_text(f'''#!{sys.executable}
import os, runpy, sys
g = runpy.run_path({str(repo / 'rootfs/usr/sbin/x6100-tailscale')!r}, run_name='tailscale-check')['main'].__globals__
g.update({paths!r})
g['tailscaled_pids'] = lambda: [4242] if os.path.exists({str(alive)!r}) else []
with open({str(asked)!r}, 'a') as f:
    f.write(' '.join(sys.argv[1:]) + '\\n')
sys.exit(g['main'](sys.argv[1:]))
''')
        helper.chmod(0o755)
        up = runpy.run_path(str(repo / 'rootfs/usr/sbin/x6100-update'), run_name='updater-check')['cmd_rollback'].__globals__
        up.update(WORK=str(work), STATE=str(ustate), PREV=str(ustate / 'prev'), JOURNAL=str(ustate / 'journal.json'),
                  PENDING=str(ustate / 'pending.json'), LAST=str(ustate / 'last.json'), LOCK=str(ustate / 'lock'),
                  PROGRESS=str(work / 'progress'), ALLOW_UNSIGNED=str(work / 'allow-unsigned'), VERSION_FILE=str(ver),
                  ALLOWED=(str(inst) + '/',), restart_apps=lambda: None, radio_model=lambda: 'x6100',
                  TAILSCALE=str(helper), TAILSCALE_STATE=g['STATE_FILE'], FUNNEL_WAIT=2, FUNNEL_QUIET_WAIT=1)
        Path(up['ALLOW_UNSIGNED']).touch()

        def bundle(version):
            """A bundle of the two installed files, as tools/update/mkbundle.py makes it (unsigned)"""
            body, vb = ('application ' + version).encode(), (version + '\n').encode()
            manifest = {'product': 'OpenX6X00', 'model': 'x6100', 'version': version,
                        'files': [{'path': str(p), 'sha256': hashlib.sha256(b).hexdigest(), 'mode': '644'}
                                  for p, b in ((app, body), (ver, vb))]}
            path = upd / f'bundle-{version}.tar.gz'
            with tarfile.open(path, 'w:gz') as tar:
                for name, data in (('manifest.json', json.dumps(manifest).encode()), ('files' + str(app), body),
                                   ('files' + str(ver), vb)):
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    tar.addfile(info, io.BytesIO(data))
            return str(path)

        def runs():
            return asked.read_text().count('\n') if asked.exists() else 0

        def refusal(fn, *args):
            try:
                fn(*args)
            except up['Fail'] as e:
                return str(e)
            return ''

        table = {'0.3.6': True, '0.3.7': False, '0.3.10': False, '0.4': False, '1.0.0': False, '0.3.6-12-g076be5a': True,
                 'v0.3.6': True, '0.3': True, '0.3.7-rc1': False, 'unknown': True, None: True, '1': False}
        got = {k: up['before_funnel'](k) for k in table}
        check('before Funnel: 0.3.6 and older, builds after them, and a version not known; not 0.3.7, 0.3.10, 0.4',
              got == table, {k: v for k, v in got.items() if v != table[k]})

        out = up['cmd_install']([bundle('0.3.7')])
        check('installing v0.3.7 over v0.3.6: x6100-tailscale not asked', out == 'ok 0.3.7' and runs() == 0, out)

        g['cmd_funnel']('on')
        Fake.serve = on_and_by_hand()
        alive.touch()
        at_undo = []
        real_undo = up['undo']
        up['undo'] = lambda journal: (at_undo.append((g['funnel_left'](Fake.serve), g['funnel_wanted']())),
                                      real_undo(journal))
        out = up['cmd_rollback']([])
        check('going back to v0.3.6, Funnel on: off and out of tailscaled before any file goes back',
              at_undo == [(False, False)] and runs() == 1, at_undo)
        check('going back to v0.3.6: gone back, and the result says Funnel is off now',
              out == 'ok back to 0.3.6; Funnel is off now: v0.3.6 knows nothing of it' and ver.read_text() == '0.3.6\n'
              and app.read_text() == 'application 0.3.6', out)
        check('going back to v0.3.6: the tailnet\'s own serving kept', tailnet_only(Fake.serve), Fake.serve)

        up['cmd_install']([bundle('0.3.7')])
        g['cmd_funnel']('on')
        Fake.serve = on_and_by_hand()
        Fake.refuse = 'netMap is nil'
        why = refusal(up['cmd_rollback'], [])
        check('Funnel that cannot be taken off (tailscaled refusing it): not gone back, why and what to do said',
              why.startswith('not gone back: Funnel could not be made sure to be off (tailscaled: serve-config: HTTP 500')
              and 'netMap is nil' in why and why.endswith('turn Funnel off first, or Tailscale, wait until it says Off, '
                                                          'and try again'), why)
        check('not gone back: no file touched, the update still there to go back from',
              ver.read_text() == '0.3.7\n' and app.read_text() == 'application 0.3.7' and len(at_undo) == 1
              and up['load'](up['JOURNAL'])['state'] == 'applied')
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            code = up['main'](['rollback'])
        signal_alarm = __import__('signal').alarm
        signal_alarm(0)                                # main's run limit, not this harness's
        line = printed.getvalue().strip().splitlines()[-1]
        check('the line the web page shows for it: error, then why and what to do',
              code == 1 and line.startswith('error not gone back: Funnel could not be made sure to be off') and
              'turn Funnel off first' in line, line)
        Fake.refuse, Fake.serve = '', None              # tailscaled not connected: its serve config reads as none
        g['cmd_funnel']('on')
        with_funnel()
        why = refusal(up['cmd_rollback'], [])
        check('tailscaled not connected, the funnel in its state for when it is: not gone back, why said',
              why.startswith('not gone back: Funnel could not be made sure to be off (tailscaled keeps a funnel in its '
                             'state for when it connects)') and ver.read_text() == '0.3.7\n', why)
        if not why:                                    # gone back all the same: v0.3.7 again, for what follows
            up['cmd_install']([bundle('0.3.7')])
        Fake.refuse, Fake.serve = 'netMap is nil', on_and_by_hand()
        with_funnel()
        d = Daemon()                                   # Tailscale turned off then: still refused, so out of the file
        alive.touch()
        g['stop_tailscaled'](d)
        Fake.refuse = ''
        out = up['cmd_rollback']([])
        check('then Tailscale off: no funnel left in its state, and the step back goes',
              out == 'ok back to 0.3.6' and ver.read_text() == '0.3.6\n' and not g['funnel_left'](stored()), out)

        up['cmd_install']([bundle('0.3.7')])
        g['cmd_funnel']('on')
        with_funnel()
        out = up['cmd_install']([bundle('0.3.6')])
        check('v0.3.6\'s bundle, tailscaled not running: Funnel out of its state file before the install',
              out == 'ok 0.3.6; Funnel is off now: v0.3.6 knows nothing of it' and ver.read_text() == '0.3.6\n'
              and tailnet_only(stored()) and not g['funnel_wanted'](), out)

        up['cmd_install']([bundle('0.3.7')])
        g['cmd_funnel']('on')
        Path(g['STATE_FILE']).write_text('{"_serve/a1b2": ')
        why = refusal(up['cmd_install'], [bundle('0.3.6')])
        check('v0.3.6\'s bundle, Funnel not sure to be off: not installed, said, nothing staged left',
              why.startswith('not installed: Funnel could not be made sure to be off') and 'turn Funnel off first' in why
              and ver.read_text() == '0.3.7\n' and not (work / 'stage').exists(), why)
        Path(g['STATE_FILE']).unlink()

        n = runs()
        out = up['cmd_install']([bundle('0.3.8')])
        again = up['cmd_install']([bundle('0.3.8')])
        down = up['cmd_install']([bundle('0.3.7')])
        check('a newer bundle, the same again, and v0.3.7 from v0.3.8: x6100-tailscale not asked',
              (out, again, down) == ('ok 0.3.8', 'ok 0.3.8', 'ok 0.3.7') and runs() == n, (out, again, down))

        def failing_starts(version_from='0.3.6'):
            """v0.3.7 installed over version_from, its GUI failed its starts: the next boot-check takes it back"""
            ver.write_text(version_from + '\n')
            up['cmd_install']([bundle('0.3.7')])
            pend = up['load'](up['PENDING'])
            pend['starts'] = up['MAX_STARTS']
            up['save'](up['PENDING'], pend)

        def boot_check():
            t0 = time.time()
            out = up['cmd_boot_check']([])
            return out, time.time() - t0

        failing_starts()
        Path(g['CONF']).write_text('enabled=0\nfunnel=0\n')       # Tailscale off: no tailscaled, no state file
        alive.unlink(missing_ok=True)
        out, took = boot_check()
        check('boot-check, Tailscale off: back to v0.3.6 as before, at once', out == 'rolled back to 0.3.6' and
              ver.read_text() == '0.3.6\n' and took < 5, f'{out} ({took:.1f} s)')
        failing_starts()
        up['TAILSCALE'] = str(upd / 'not-installed')
        out, took = boot_check()
        check('boot-check, x6100-tailscale not there: back as before', out == 'rolled back to 0.3.6' and
              ver.read_text() == '0.3.6\n', f'{out} ({took:.1f} s)')
        up['TAILSCALE'] = str(helper)
        failing_starts()
        Path(g['CONF']).write_text('enabled=1\nfunnel=1\n')
        Fake.serve = on_and_by_hand()
        alive.touch()
        out, took = boot_check()
        check('boot-check, Funnel on: off and out of tailscaled on the way back', out == 'rolled back to 0.3.6' and
              tailnet_only(Fake.serve) and not g['funnel_wanted'](), f'{out} ({took:.1f} s)')
        failing_starts()
        g['cmd_funnel']('on')
        Fake.serve = on_and_by_hand()
        Fake.refuse = 'netMap is nil'
        out, took = boot_check()
        Fake.refuse = ''
        check('boot-check, Funnel that cannot be taken off: back all the same, within seconds',
              out == 'rolled back to 0.3.6' and ver.read_text() == '0.3.6\n' and took < 10, f'{out} ({took:.1f} s)')
        n = runs()
        failing_starts('0.3.7')
        out, took = boot_check()
        check('boot-check back to v0.3.7, which knows Funnel: x6100-tailscale not asked',
              out == 'rolled back to 0.3.7' and runs() == n, out)
        alive.unlink(missing_ok=True)
        g['cmd_funnel']('off')
        server.shutdown()
        server.server_close()
    finally:
        os.chdir(here)

bad = results.count(False)
print('x6100-tailscale Funnel against a fake tailscaled (#58): %d of %d cases as they should be'
      % (len(results) - bad, len(results)))
assert not bad
