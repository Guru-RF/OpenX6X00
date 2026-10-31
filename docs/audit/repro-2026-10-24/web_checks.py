import ast
import io
import json
import os
import re
import runpy
import shutil
import tempfile
import threading
from pathlib import Path
from types import SimpleNamespace

repo = Path(__file__).resolve().parents[3]
source = (repo / 'webserver/src/x6100_webserver/apps.py').read_text()
tree = ast.parse(source)

class FakeBottle:
    request = threading.local()
    response = threading.local()

bottle = FakeBottle()
ns = {'bottle': bottle, 'os': os, 'io': io, 'json': json, 're': re, 'tempfile': tempfile, 'secrets': __import__('secrets'),
      'threading': threading, 'settings': SimpleNamespace(), 'time': __import__('time')}
wanted = {'replace_file', 'read_cluster_conf', 'read_cluster_options', 'cluster_option_lines',
          'write_cluster_conf', 'cluster_save', 'cluster_state', 'cluster_alerts_seen',
          'cluster_error', 'cluster_alert_input', 'cluster_alert_dup', 'add_cluster_alert', 'cluster_update',
          'api_error', 'upload_update', 'update_reserve', 'update_release', 'update_launch', 'update_job_run'}
selected = []
for node in tree.body:
    if isinstance(node, ast.FunctionDef) and node.name in wanted:
        node.decorator_list = []
        selected.append(node)
    elif isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and (t.id.startswith('CLUSTER_') or t.id in ('config_lock','UPDATE_MAX_BYTES','update_lock','update_job')) for t in node.targets):
        selected.append(node)
exec(compile(ast.Module(body=selected,type_ignores=[]), 'apps-production-extract', 'exec'), ns)

with tempfile.TemporaryDirectory(prefix='x6cluster-') as td:
    ns['settings'].CLUSTER_CONF = str(Path(td)/'cluster.conf')
    ns['CLUSTER_ALERTS_SEEN'] = str(Path(td)/'seen')
    original = ns['read_cluster_options']
    first = threading.Event()
    def slow_first_read():
        # the first request lingers after its read: without the lock around read-change-write the
        # second one read the same list meanwhile and the two saves kept one alert (audit B13)
        result = original()
        if not first.is_set():
            first.set()
            __import__('time').sleep(0.3)
        return result
    ns['read_cluster_options'] = slow_first_read
    responses = []
    def request_alert(pattern):
        bottle.request.json = {'pattern': pattern, 'bands': [], 'mode': ''}
        responses.append(ns['add_cluster_alert']())
    threads = [threading.Thread(target=request_alert, args=(p,)) for p in ('ON6URE','K1ABC')]
    for thread in threads: thread.start()
    for thread in threads: thread.join()
    print('cluster success responses:', len(responses), 'final alerts:', [a['pattern'] for a in original()['alerts']])
    assert len(responses) == 2 and len(original()['alerts']) == 2   # fixed (audit B13): both kept

with tempfile.TemporaryDirectory(prefix='x6upload-') as td:
    from hashlib import sha256
    import tarfile
    root = Path(td)
    upload_dir = root/'uploads'
    upload_dir.mkdir()
    target = root/'installed'
    target.mkdir()
    app_path = target/'application'
    ver_path = target/'version'
    app_path.write_text('original')
    ver_path.write_text('0\n')
    updater = runpy.run_path(str(repo/'rootfs/usr/sbin/x6100-update'))
    updater['__name__'] = 'updater-check'
    # Functions refer to the globals dictionary returned by __globals__, not runpy's copy.
    ug = updater['cmd_install'].__globals__
    work, state = root/'work', root/'state'
    work.mkdir()
    state.mkdir()
    ug.update(WORK=str(work), STATE=str(state), PREV=str(state/'prev'),
              JOURNAL=str(state/'journal.json'), PENDING=str(state/'pending.json'), LAST=str(state/'last.json'),
              PROGRESS=str(work/'progress'), ALLOW_UNSIGNED=str(work/'allow-unsigned'),
              VERSION_FILE=str(ver_path), ALLOWED=(str(target)+'/',), restart_apps=lambda: None,
              radio_model=lambda: 'x6100', TAILSCALE=str(root/'no-tailscale'), TAILSCALE_STATE=str(root/'no-state'))
    Path(ug['ALLOW_UNSIGNED']).touch()
    def bundle(version):
        body = ('application '+version).encode()
        vb = (version+'\n').encode()
        entries = [{'path':str(p), 'sha256':sha256(b).hexdigest(), 'mode':'644'} for p,b in ((app_path,body),(ver_path,vb))]
        manifest = {'product':'OpenX6X00', 'model':'x6100','version':version,'files':entries}
        out = io.BytesIO()
        with tarfile.open(fileobj=out, mode='w:gz') as tar:
            for name,data in [('manifest.json',json.dumps(manifest).encode()), ('files'+str(app_path),body), ('files'+str(ver_path),vb)]:
                info=tarfile.TarInfo(name);info.size=len(data);tar.addfile(info,io.BytesIO(data))
        return out.getvalue()
    # Redirect only the production upload handler's fixed storage constants.
    upload_node = next(n for n in selected if isinstance(n,ast.FunctionDef) and n.name=='upload_update')
    for n in ast.walk(upload_node):
        if isinstance(n,ast.Constant) and isinstance(n.value,str) and n.value.startswith('/mnt/update'):
            n.value = str(upload_dir)+n.value[len('/mnt/update'):]
    exec(compile(ast.Module(body=[upload_node],type_ignores=[]),'apps-production-upload','exec'),ns)
    job_started, helper_may_open = threading.Event(), threading.Event()
    # the production job runner with the updater's install in place of the helper process: the
    # job is reserved before the upload is written and installs the file it was given (audit B08)
    def run(args):
        job_started.set();helper_may_open.wait()
        return ug['cmd_install'](args[1:])
    ns['update_run']=run
    ns['get_update']=lambda:{'running':True}
    bottle.request.files={'file':SimpleNamespace(raw_filename='A.tar.gz',file=io.BytesIO(bundle('1')))}
    a_result=ns['upload_update']()
    job_started.wait()
    bottle.request.files={'file':SimpleNamespace(raw_filename='B.tar.gz',file=io.BytesIO(bundle('2')))}
    b_result=ns['upload_update']()
    helper_may_open.set()
    for _ in range(200):
        with ns['update_lock']:
            running=ns['update_job']['running']
        if not running: break
        __import__('time').sleep(0.05)
    print('upload A:',a_result,'upload B:',b_result,'A job installed:',ver_path.read_text().strip(),app_path.read_text())
    assert 'error' in b_result.values() and ver_path.read_text()=='1\n' and len(list(upload_dir.iterdir()))==0

# Issue #58: a request from the internet through Tailscale Funnel carries tailscaled's Tailscale-Funnel-Request, and
# its name is a *.ts.net name, which the site guard takes as one of the radio's own. Every such request is refused,
# a page or a read too; the same requests from the LAN and the tailnet pass as they did
class Refused(Exception):
    def __init__(self, status, body):
        super().__init__(status)
        self.status, self.body = status, body

class GuardBottle:
    class HTTPError(Refused):
        def __init__(self, status, body=''):
            super().__init__(status, body)
    class HTTPResponse(Refused):
        def __init__(self, body, status, headers=None):
            super().__init__(status, body)
    @staticmethod
    def abort(status, text=''):
        raise Refused(status, text)
    request = SimpleNamespace()

class Headers(dict):
    """A request's headers, their names without case as bottle's request.headers (WSGIHeaderDict) has them: in a
    plain dict of lower-case names refuse_other_sites's Origin and Sec-Fetch-Site were never found, and no case here
    ran its Origin check"""
    def get(self, name, default=None):
        return dict.get(self, name.lower(), default)

guard = {'bottle': GuardBottle, 'json': json, 'os': os, 'socket': __import__('socket'), 're': re, 'tempfile': tempfile,
         'ipaddress': __import__('ipaddress'), 'urllib': __import__('urllib.parse')}
# with #105's own names: the domains of the radio's network, the list on the Network page and its routes
names = {'host_refused', 'via_funnel', 'funnel_refused', 'refuse_other_sites', 'local_host_ok', 'tailscale_status',
         'file_lines', 'public_suffix', 'network_domains', 'listed_names', 'own_names', 'request_host', 'radio_address',
         'own_name_error', 'not_own_name', 'own_names_state', 'own_names_set', 'api_error', 'replace_file'}
nodes = []
for node in tree.body:
    if isinstance(node, ast.FunctionDef) and node.name in names:
        node.decorator_list = []
        nodes.append(node)
    elif isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id in (
            'HOST_ONLY_READS', 'POST_READS', 'TAILSCALE_STATUS', 'TAILSCALE_CONF', 'RESOLV_CONF', 'OWN_NAMES_CONF',
            'OWN_NAMES_MAX', 'OWN_NAMES_REFUSED', 'PRIVATE_DOMAINS', 'REGISTRY_LABELS', 'DNS_NAME_RE',
            'NAMES_FILE_MAX', 'file_cache') for t in node.targets):
        nodes.append(node)
assert {n.name for n in nodes if isinstance(n, ast.FunctionDef)} == names, 'the site guard changed its names'
exec(compile(ast.Module(body=nodes, type_ignores=[]), 'apps-site-guard', 'exec'), guard)
with tempfile.TemporaryDirectory(prefix='x6funnel-') as td:
    guard['TAILSCALE_STATUS'] = str(Path(td) / 'status')
    guard['TAILSCALE_CONF'] = str(Path(td) / 'tailscale.conf')
    Path(guard['TAILSCALE_STATUS']).write_text('state=running\nname=x6200.tail1234.ts.net\nfunnel=on\n')

    def ask(method, path, host, **headers):
        hdr = {k.replace('_', '-').lower(): v for k, v in headers.items()}
        hdr['host'] = host
        GuardBottle.request = SimpleNamespace(method=method, path=path, headers=Headers(hdr), environ={},
                                              get_header=lambda name, default=None: hdr.get(name.lower(), default))
        try:
            guard['refuse_other_sites']()
            return 'passes'
        except Refused as e:
            return 'refused %d' % e.status

    funnel = {'Tailscale_Funnel_Request': '?1', 'X_Forwarded_For': '203.0.113.7'}
    page = 'https://x6200.tail1234.ts.net'
    cases = [
        ('the LAN: a page', ask('GET', '/', '192.168.1.50'), 'passes'),
        ('the LAN: Files', ask('GET', '/files/', '192.168.1.50'), 'passes'),
        ('the LAN: a change', ask('POST', '/api/remote', '192.168.1.50', Origin='http://192.168.1.50'), 'passes'),
        ('the tailnet: Files', ask('GET', '/files/', 'x6200.tail1234.ts.net'), 'passes'),
        ('the tailnet: a change', ask('POST', '/api/tailscale', 'x6200.tail1234.ts.net', Origin=page), 'passes'),
        ('Funnel: a page', ask('GET', '/', 'x6200.tail1234.ts.net', **funnel), 'refused 403'),
        ('Funnel: the Panel', ask('GET', '/panel', 'x6200.tail1234.ts.net', **funnel), 'refused 403'),
        ('Funnel: an API read', ask('GET', '/api/remote', 'x6200.tail1234.ts.net', **funnel), 'refused 403'),
        ('Funnel: Files', ask('GET', '/files/backups/backup.tgz', 'x6200.tail1234.ts.net', **funnel), 'refused 403'),
        ('Funnel: a change', ask('POST', '/api/backup/restore', 'x6200.tail1234.ts.net', Origin=page, **funnel),
         'refused 403'),
        ('Funnel by an address (a funnel to port 80 made by hand)', ask('GET', '/', '127.0.0.1', **funnel), 'refused 403'),
        ('Funnel, the header empty', ask('GET', '/', 'x6200.tail1234.ts.net', Tailscale_Funnel_Request=''),
         'refused 403'),
    ]
    hdr = {'host': 'x6200.tail1234.ts.net', 'tailscale-funnel-request': '?1'}
    GuardBottle.request = SimpleNamespace(headers=Headers(hdr), get_header=lambda name, default=None: hdr.get(name.lower(), default))
    cases.append(('Funnel: local_host_ok', str(guard['local_host_ok']()), 'False'))
    bad = [c for c in cases if c[1] != c[2]]
    for what, got, want in cases:
        print(('ok   ' if got == want else 'FAIL ') + what + ': ' + got)
    print('Tailscale Funnel requests (#58): %d of %d cases as they should be' % (len(cases) - len(bad), len(cases)))
    assert not bad

# Issue #105: the radio's own names are also <hostname>.<domain> for the domains its network gives it (resolv.conf's
# search and domain lines, as NetworkManager writes them) and the names its owner lists on the Network page; never
# <hostname>.<a public suffix alone> (x6100.nl could be anyone's). The list changes only by one of those names, so a
# site that points its own name at the radio (DNS rebinding) is refused as before and cannot add that name. A
# refusal says the name used, the names that work (Tailscale's only while it runs) and where another one is added
with tempfile.TemporaryDirectory(prefix='x6names-') as td:
    guard['TAILSCALE_STATUS'] = str(Path(td) / 'status')
    guard['TAILSCALE_CONF'] = str(Path(td) / 'tailscale.conf')
    guard['RESOLV_CONF'] = str(Path(td) / 'resolv.conf')
    guard['OWN_NAMES_CONF'] = str(Path(td) / 'own_names.conf')
    Path(guard['RESOLV_CONF']).write_text('# Generated by NetworkManager\nsearch Home.LAN. nl co.uk lan box\n'
                                          'domain tri-angle.nl\nnameserver 192.168.1.1\n')
    me = guard['socket'].gethostname().lower()
    GuardBottle.response = SimpleNamespace(status=200)

    def change(host, path='/api/remote'):
        return ask('POST', path, host, Origin='http://' + host)

    def set_list(host, listed):
        """The production route by itself (refuse_other_sites aside): its status and the list it answers"""
        hdr = {'host': host, 'origin': 'http://' + host}
        GuardBottle.request = SimpleNamespace(method='POST', path='/api/own_names', headers=Headers(hdr), environ={},
                                              json={'names': listed},
                                              get_header=lambda name, default=None: hdr.get(name.lower(), default))
        GuardBottle.response.status = 200
        r = guard['own_names_set']()
        return '%d %s' % (GuardBottle.response.status, ' '.join(r.get('listed', [])) or r.get('msg', '')[:40])

    def refusal(host):
        try:
            change(host)
        except Refused:
            pass
        return guard['host_refused']().body

    cases = [
        ('<hostname>.home.lan (search Home.LAN.): a change', change(f'{me}.home.lan'), 'passes'),
        ('<hostname>.lan (lan: a private single name): Files', ask('GET', '/files/', f'{me}.lan'), 'passes'),
        ('<hostname>.tri-angle.nl (the domain line): Files', ask('GET', '/files/', f'{me}.tri-angle.nl:80'), 'passes'),
        ('<hostname>.nl: nl alone is a public suffix', ask('GET', '/files/', f'{me}.nl'), 'refused 403'),
        ('<hostname>.co.uk: a country\'s second level alone', change(f'{me}.co.uk'), 'refused 403'),
        ('<hostname>.box: a single name not kept private', change(f'{me}.box'), 'refused 403'),
        ('another machine in the domain', change(f'not-{me}.home.lan'), 'refused 403'),
        ('a site\'s own name pointed at the radio (DNS rebinding)', change('evil.example'), 'refused 403'),
        ('x6100.tri-angle.nl, not listed yet: a change', change('x6100.tri-angle.nl'), 'refused 403'),
        ('x6100.tri-angle.nl, not listed yet: the list changed through the site guard',
         change('x6100.tri-angle.nl', '/api/own_names'), 'refused 403'),
        ('x6100.tri-angle.nl lists itself: the route alone', set_list('x6100.tri-angle.nl', ['x6100.tri-angle.nl']),
         '403 This page was opened as x6100.tri-angle.'),
        ('evil.example lists itself: the route alone', set_list('evil.example', ['evil.example']),
         '403 This page was opened as evil.example, wh'),
        ('nothing written by them', str(os.path.exists(guard['OWN_NAMES_CONF'])), 'False'),
        ('the list set by <hostname>.local', set_list(f'{me}.local', ['X6100.Tri-Angle.NL.', 'shack.example.lan', '']),
         '200 x6100.tri-angle.nl shack.example.lan'),
        ('own_names.conf', Path(guard['OWN_NAMES_CONF']).read_text().replace('\n', '|'),
         'x6100.tri-angle.nl|shack.example.lan|'),
        ('x6100.tri-angle.nl listed: Files', ask('GET', '/files/', 'x6100.tri-angle.nl'), 'passes'),
        ('x6100.tri-angle.nl listed: a change', change('x6100.tri-angle.nl'), 'passes'),
        ('shack.example.lan listed: a change', change('SHACK.example.lan.'), 'passes'),
        ('a look-alike of a listed name', change('x6100.tri-angle.nl.evil.example'), 'refused 403'),
        ('evil.example, a name listed: still refused', change('evil.example'), 'refused 403'),
        ('a listed name, the page another site\'s: a change', ask('POST', '/api/remote', 'x6100.tri-angle.nl',
                                                                Origin='http://evil.example'), 'refused 403'),
        ('a listed name, the request marked cross-site: a change',
         ask('POST', '/api/remote', 'x6100.tri-angle.nl', Sec_Fetch_Site='cross-site'), 'refused 403'),
        ('<hostname>.local, a file:// page: a change', ask('POST', '/api/remote', f'{me}.local', Origin='null'),
         'refused 403'),
        # a page on another port of the radio's own name: Pat's mailbox (8080), where a received message's attachment
        # opens and its file names show as they came. The radio's pages send the same port in Origin and Host
        ('a page of Pat\'s mailbox (port 8080) lists a name', ask('POST', '/api/own_names', f'{me}.local',
                                                               Origin=f'http://{me}.local:8080'), 'refused 403'),
        ('a page of Pat\'s mailbox: Go back to the previous version (a form\'s POST, sent without asking first)',
         ask('POST', '/api/update/rollback', f'{me}.local', Origin=f'http://{me}.local:8080',
             Sec_Fetch_Site='same-site'), 'refused 403'),
        ('a page of Pat\'s mailbox by the address: a change', ask('POST', '/api/remote', '192.168.1.40',
                                                               Origin='http://192.168.1.40:8080'), 'refused 403'),
        ('a page of Pat\'s mailbox, the request naming port 80: a change',
         ask('POST', '/api/remote', '192.168.1.40:80', Origin='http://192.168.1.40:8080'), 'refused 403'),
        ('the https page: a change', ask('POST', '/api/remote', f'{me}.local', Origin=f'https://{me}.local'), 'passes'),
        ('the pages on a PC on port 8080: a change', ask('POST', '/api/remote', '127.0.0.1:8080',
                                                         Origin='http://127.0.0.1:8080'), 'passes'),
        ('an address of IPv6 and its port: a change', ask('POST', '/api/remote', '[fe80::1]:8000',
                                                          Origin='http://[fe80::1]:8000'), 'passes'),
        ('a request naming the port 80, the page not: a change', ask('POST', '/api/remote', '192.168.1.40:80',
                                                                   Origin='http://192.168.1.40'), 'passes'),
    ]
    for bad in ('*.tri-angle.nl', '192.168.1.40', 'nl', 'co.uk', 'shack', '0x7f.1', 'a_b.example.nl'):
        cases.append(('not listed: ' + bad, set_list(f'{me}.local', [bad])[:3], '400'))
    cases += [
        ('not listed: nine names', set_list(f'{me}.local', ['n%d.example.nl' % i for i in range(9)]),
         '400 Up to 8 names'),
        ('the list kept after the refused ones', ' '.join(guard['listed_names']()),
         'x6100.tri-angle.nl shack.example.lan'),
        ('the list emptied by x6100.tri-angle.nl', set_list('x6100.tri-angle.nl', []), '200 '),
        ('x6100.tri-angle.nl taken off: Files', ask('GET', '/files/', 'x6100.tri-angle.nl'), 'refused 403'),
    ]
    text = refusal('x6100.fritz.box')
    cases += [
        ('the refusal: the name used', str('opened as x6100.fritz.box,' in text), 'True'),
        ('the refusal: the names that work',
         str(f'its address, {me}.local, {me}.home.lan, {me}.lan or {me}.tri-angle.nl' in text), 'True'),
        ('the refusal: where to add the name', str('add it to Other names on System > Network' in text), 'True'),
        # a rebinding site can open the radio's own pages by its name, this text and all: the name is offered only as
        # the owner's own, never one a site asks for
        ('the refusal: only a name of the owner\'s own',
         str('If x6100.fritz.box is a name your own DNS server or reverse proxy gives the radio, add it' in text and
             'never a name a web site or a message asks for' in text), 'True'),
        ('the refusal: no Tailscale while it is off', str('Tailscale' in text), 'False'),
        ('the refusal for a single name: nothing to add', str('Other names' in refusal('shack')), 'False'),
    ]
    Path(guard['TAILSCALE_STATUS']).write_text('state=running\nname=X6200.tail1234.ts.net\n')
    cases.append(('the refusal: its Tailscale name while it runs',
                  str(f'{me}.tri-angle.nl or its Tailscale name (x6200.tail1234.ts.net)' in refusal('x6100.fritz.box')),
                  'True'))
    # a search line cut where the file is read no further (4096 bytes): no domain taken cut short (example.co of
    # example.co.uk would count)
    head = '#' * 32 + '\n'
    line = ('search ' + 'f.lan ' * 600).ljust(guard['NAMES_FILE_MAX'] - len(head) - len('example.co'))
    Path(guard['RESOLV_CONF']).write_text(head + line + 'example.co.uk\n')
    cases.append(('a search line cut at 4096 bytes: left out', change(f'{me}.example.co'), 'refused 403'))
    bad = [c for c in cases if c[1] != c[2]]
    for what, got, want in cases:
        print(('ok   ' if got == want else 'FAIL ') + what + ': ' + got)
    print('The radio\'s own names (#105): %d of %d cases as they should be' % (len(cases) - len(bad), len(cases)))
    print('  a refusal reads: ' + text)
    assert not bad

# Issue #58: the Funnel pages may connect to their own address alone. 'self' covers wss:// to it in today's browsers;
# Safari before 16 takes 'self' for https only, so the pages name the address too once tailscaled has told it
# (funnel_name, from x6100-tailscale's status file), and the web server writes them anew when it changes
names = {'funnel_name', 'tailscale_status'}
nodes = []
for node in tree.body:
    if isinstance(node, ast.FunctionDef) and node.name in names:
        node.decorator_list = []
        nodes.append(node)
    elif isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id in (
            'FUNNEL_NAME', 'TAILSCALE_STATUS', 'TAILSCALE_CONF') for t in node.targets):
        nodes.append(node)
named = {'re': re, 'os': os}
exec(compile(ast.Module(body=nodes, type_ignores=[]), 'apps-funnel-name', 'exec'), named)
base = (repo / 'webserver/src/x6100_webserver/views/funnel_base.html').read_text()
csp = re.search(r'http-equiv="Content-Security-Policy" content="([^"]*)"', base).group(1)
connect = [d for d in csp.split('; ') if d.startswith('connect-src')]


def connect_src(host):
    """The page's connect-src as bottle writes it for host ("": not known yet)"""
    env = {'funnel_host': host} if host else {}
    return re.sub(r'\{\{(.*?)\}\}', lambda m: str(eval(m.group(1), {'get': env.get, **env})), connect[0])


main = ast.parse((repo / 'webserver/src/x6100_webserver/__main__.py').read_text())
follow = [n for n in main.body if isinstance(n, ast.FunctionDef) and n.name == 'follow_funnel_name']
seen, written = iter(['', 'x6200.tail1234.ts.net', 'x6200.tail1234.ts.net', 'x6200-1.tail1234.ts.net', '']), []


def sleep(seconds):
    if next(steps, None) is None:
        raise StopIteration


steps = iter(range(5))
loop = {'time': SimpleNamespace(sleep=sleep), 'apps': SimpleNamespace(funnel_name=lambda: next(seen)),
        'write_funnel_pages': lambda out_dir, name: written.append(name), 'FUNNEL_NAME_POLL': 5}
exec(compile(ast.Module(body=follow, type_ignores=[]), 'main-follow', 'exec'), loop)
try:
    loop['follow_funnel_name']('/run/x6100-funnel', '')
except StopIteration:
    pass
with tempfile.TemporaryDirectory(prefix='x6funnelname-') as td:
    named['TAILSCALE_STATUS'] = str(Path(td) / 'status')
    named['TAILSCALE_CONF'] = str(Path(td) / 'tailscale.conf')

    def name_of(status):
        Path(named['TAILSCALE_STATUS']).write_text(status)
        return named['funnel_name']()

    cases = [
        ('the name from the status, as the address bar has it', name_of('state=running\nname=X6200.Tail1234.ts.net.\n'),
         'x6200.tail1234.ts.net'),
        ('no name yet (a login wanted)', name_of('state=login\nurl=https://login.tailscale.com/a/x\n'), ''),
        ('a name that is no DNS name: none', name_of('state=running\nname=evil.example; connect-src *\n'), ''),
        ('connect-src, the name known', connect_src('x6200.tail1234.ts.net'),
         "connect-src 'self' wss://x6200.tail1234.ts.net"),
        ('connect-src, no name yet', connect_src(''), "connect-src 'self'"),
        ('one connect-src, no other wss: in the policy',
         str(len(connect) == 1 and 'wss:' not in csp.replace(connect[0], '')), 'True'),
        ('the pages written anew for each new name, not for none', ' '.join(written),
         'x6200.tail1234.ts.net x6200-1.tail1234.ts.net'),
    ]
    bad = [c for c in cases if c[1] != c[2]]
    for what, got, want in cases:
        print(('ok   ' if got == want else 'FAIL ') + what + ': ' + got)
    print('Tailscale Funnel pages\' policy (#58): %d of %d cases as they should be' % (len(cases) - len(bad), len(cases)))
    assert not bad


# Issue #58: the https page's sockets on the page's own port. The https server (make_https_server, from __main__.py
# as it is) reads a request's head: GET /ws/remote or /ws/tci with Upgrade: websocket goes to the GUI's browser remote
# or TCI on 127.0.0.1, its bytes as they came (Origin, Host, Tailscale-Funnel-Request and X-Forwarded-For untouched,
# nothing added: the GUI's own checks decide, src/tci_ws.c); anything else is a page, read again whole by the web
# server. Real sockets: the server, two stand-ins for the GUI's sockets and the browser on 127.0.0.1, the
# certificate made with openssl. The front doors on 50003 and 50004 of v0.3.7's pages (WssProxy) still go through
import hashlib
import socket
import socketserver
import ssl
import subprocess
import time

wanted = {'ThreadingWSGIServer', 'RequestHandler', 'TlsContext', 'pipe', 'read_head', 'ws_port', 'Replay', 'Replayed',
          'make_https_server', 'WssProxy'}
nodes = [n for n in main.body if isinstance(n, ast.ClassDef | ast.FunctionDef) and n.name in wanted or
         isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id in ('WS_PATHS', 'HEAD_MAX') for t in n.targets)]
gui = {}   # what each stand-in of the GUI's sockets got: the bytes, from where


class GuiStandIn(socketserver.ThreadingTCPServer):
    """One of the GUI's WebSocket servers: the head as it came, a 101, then each piece echoed"""
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, name):
        self.name = name
        super().__init__(('127.0.0.1', 0), None)

    def finish_request(self, request, client_address):
        head = b''
        while b'\r\n\r\n' not in head:
            data = request.recv(65536)
            if not data:
                return
            head += data
        gui[self.name] = (head, client_address[0])
        request.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n')
        while True:
            data = request.recv(65536)
            if not data:
                break
            request.sendall(b'echo:' + data)


def page_app(environ, start_response):
    body = environ['wsgi.input'].read(int(environ.get('CONTENT_LENGTH') or 0))
    start_response('200 OK', [('Content-Type', 'text/plain')])
    return [('page %s %s %d %s' % (environ['REQUEST_METHOD'], environ['PATH_INFO'], len(body),
                                    hashlib.sha256(body).hexdigest()[:12])).encode()]


remote_gui, tci_gui = GuiStandIn('remote'), GuiStandIn('tci')
closed = socket.socket()
closed.bind(('127.0.0.1', 0))
gone = closed.getsockname()[1]   # bound, never listening: the GUI with its remote off
closed.close()
ports = {'remote': remote_gui.server_address[1], 'tci': tci_gui.server_address[1]}
srv_ns = {'apps': SimpleNamespace(read_remote_conf=lambda: {'port': ports['remote']},
                                  read_tci_conf=lambda: {'port': ports['tci']}),
          'socket': socket, 'ssl': ssl, 'io': io, 'threading': threading, 'socketserver': socketserver,
          'ThreadingMixIn': socketserver.ThreadingMixIn,
          **{k: getattr(__import__('wsgiref.simple_server', fromlist=['x']), k)
             for k in ('WSGIRequestHandler', 'WSGIServer', 'make_server')}}
exec(compile(ast.Module(body=nodes, type_ignores=[]), 'main-https', 'exec'), srv_ns)
with tempfile.TemporaryDirectory(prefix='x6https-') as td:
    cert, key = str(Path(td) / 'cert.pem'), str(Path(td) / 'key.pem')
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', '/CN=x6100',
                    '-keyout', key, '-out', cert], check=True, capture_output=True)
    tls = srv_ns['TlsContext'](cert, key)
    https = srv_ns['make_https_server'](page_app, '127.0.0.1', 0, tls)
    wss = srv_ns['WssProxy'](('127.0.0.1', 0), tls, lambda: ports['remote'])
    for s in (remote_gui, tci_gui, https, wss):
        threading.Thread(target=s.serve_forever, daemon=True).start()
    client = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    client.check_hostname, client.verify_mode = False, ssl.CERT_NONE

    def ask(raw, port=None, pieces=0, then=b''):
        """raw sent over TLS (in pieces of that many bytes, a pause between), and what comes back: a page's answer
        whole; a socket's 101, then (sent after it) and its echo"""
        gui.clear()
        got, sent = b'', False
        with socket.create_connection(('127.0.0.1', port or https.server_address[1]), timeout=5) as plain:
            with client.wrap_socket(plain) as c:
                step = pieces or len(raw)
                for i in range(0, len(raw), step):
                    c.sendall(raw[i:i + step])
                    if pieces:
                        time.sleep(0.02)
                try:
                    while True:
                        data = c.recv(65536)
                        if not data:
                            break
                        got += data
                        if got.startswith(b'HTTP/1.1 101') and b'\r\n\r\n' in got:
                            if not then or got.endswith(b'echo:' + then):
                                break
                            if not sent:
                                c.sendall(then)
                                sent = True
                except (socket.timeout, ssl.SSLError, OSError):
                    pass
        return got

    def upgrade(path, host='x6100.local', origin='https://x6100.local', extra='', upg='Upgrade: websocket'):
        return ('GET %s HTTP/1.1\r\nHost: %s\r\n%s\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n'
                'Sec-WebSocket-Version: 13\r\nOrigin: %s\r\n%s\r\n' % (path, host, upg, origin, extra)).encode()

    cases = []
    head = upgrade('/ws/remote')
    got = ask(head, then=b'hello')
    cases.append(('GET /ws/remote with Upgrade: the browser remote, the bytes as they came, from 127.0.0.1',
                  str(gui.get('remote') == (head, '127.0.0.1') and 'tci' not in gui), 'True'))
    cases.append(('and both ways after the 101', str(got.startswith(b'HTTP/1.1 101') and got.endswith(b'echo:hello')), 'True'))
    head = upgrade('/ws/tci')
    ask(head)
    cases.append(('GET /ws/tci with Upgrade: TCI', str(gui.get('tci') == (head, '127.0.0.1') and 'remote' not in gui), 'True'))
    head = upgrade('/ws/remote', 'x6200.tail1234.ts.net', 'https://x6200.tail1234.ts.net',
                   'Tailscale-Funnel-Request: ?1\r\nX-Forwarded-For: 203.0.113.7\r\n')
    ask(head)
    cases.append(('Tailscale-Funnel-Request and X-Forwarded-For handed on as they came, nothing added',
                  str(gui.get('remote', (b'',))[0] == head), 'True'))
    head = upgrade('/ws/remote', origin='http://x6100.local:8080')
    ask(head)
    cases.append(('a page of port 8080 (Pat\'s mailbox): to the GUI as it came, whose check refuses it',
                  str(gui.get('remote', (b'',))[0] == head), 'True'))
    head = upgrade('/ws/remote', upg='upgrade: WebSocket')
    ask(head, pieces=7)
    cases.append(('the head in pieces of 7 bytes, the header in other case: the browser remote',
                  str(gui.get('remote', (b'',))[0] == head), 'True'))
    cases.append(('GET /ws/remote without Upgrade: a page', ask(upgrade('/ws/remote', upg='X-Nothing: 1'))
                  .split(b'\r\n\r\n')[-1].decode(), 'page GET /ws/remote 0 e3b0c44298fc'))
    cases.append(('GET /ws/remote/x with Upgrade: a page', ask(upgrade('/ws/remote/x')).split(b'\r\n\r\n')[-1].decode(),
                  'page GET /ws/remote/x 0 e3b0c44298fc'))
    cases.append(('POST /ws/remote with Upgrade: a page', ask(upgrade('/ws/remote').replace(b'GET', b'POST', 1))
                  .split(b'\r\n\r\n')[-1].decode(), 'page POST /ws/remote 0 e3b0c44298fc'))
    cases.append(('GET /panel: the page', ask(b'GET /panel HTTP/1.1\r\nHost: x6100.local\r\n\r\n')
                  .split(b'\r\n\r\n')[-1].decode(), 'page GET /panel 0 e3b0c44298fc'))
    body = os.urandom(300000)
    got = ask(b'POST /api/upload HTTP/1.1\r\nHost: x6100.local\r\nContent-Length: %d\r\n\r\n' % len(body) + body)
    cases.append(('a POST of 300 kB: the page gets it whole (the head read once, read again)',
                  got.split(b'\r\n\r\n')[-1].decode(), 'page POST /api/upload 300000 ' + hashlib.sha256(body).hexdigest()[:12]))
    got = ask(b'GET /panel HTTP/1.1\r\nHost: x6100.local\r\n\r\n', pieces=3)
    cases.append(('a page\'s head in pieces of 3 bytes', got.split(b'\r\n\r\n')[-1].decode(), 'page GET /panel 0 e3b0c44298fc'))
    ports['remote'] = gone
    t0 = time.time()
    got = ask(upgrade('/ws/remote'))
    cases.append(('the browser remote off: the socket closed at once, no answer',
                  str(got == b'' and time.time() - t0 < 3), 'True'))
    cases.append(('and the pages still served', ask(b'GET /panel HTTP/1.1\r\nHost: x6100.local\r\n\r\n')
                  .split(b'\r\n\r\n')[-1].decode(), 'page GET /panel 0 e3b0c44298fc'))
    ports['remote'] = remote_gui.server_address[1]
    head = upgrade('/', 'x6100.local:50003')
    got = ask(head, port=wss.server_address[1], then=b'hi')
    cases.append(('the front door of v0.3.7\'s pages (50003): the browser remote, both ways',
                  str(gui.get('remote', (b'',))[0] == head and got.endswith(b'echo:hi')), 'True'))
    for s in (https, wss, remote_gui, tci_gui):
        s.shutdown()
    bad = [c for c in cases if c[1] != c[2]]
    for what, got, want in cases:
        print(('ok   ' if got == want else 'FAIL ') + what + ': ' + got)
    print('The https page\'s sockets on its own port (#58): %d of %d cases as they should be' % (len(cases) - len(bad), len(cases)))
    assert not bad
