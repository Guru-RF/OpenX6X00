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
        GuardBottle.request = SimpleNamespace(method=method, path=path, headers=hdr, environ={},
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
    GuardBottle.request = SimpleNamespace(headers=hdr, get_header=lambda name, default=None: hdr.get(name.lower(), default))
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
        GuardBottle.request = SimpleNamespace(method='POST', path='/api/own_names', headers=hdr, environ={},
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
