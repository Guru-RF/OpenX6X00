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
              radio_model=lambda: 'x6100')
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
