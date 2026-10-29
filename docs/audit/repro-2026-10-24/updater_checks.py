import json
import os
import runpy
import tempfile
from pathlib import Path

repo=Path(__file__).resolve().parents[3]
up=runpy.run_path(str(repo/'rootfs/usr/sbin/x6100-update'))['apply'].__globals__
class SimulatedCrash(BaseException):pass
with tempfile.TemporaryDirectory(prefix='x6journal-') as td:
    root=Path(td);work=root/'work';state=root/'state';installed=root/'installed'
    work.mkdir();state.mkdir();installed.mkdir()
    version=installed/'version';version.write_text('0\n')
    target=installed/'application';target.write_text('old')
    up.update(WORK=str(work),STATE=str(state),PREV=str(state/'prev'),JOURNAL=str(state/'journal'),
              PENDING=str(state/'pending'),LAST=str(state/'last'),PROGRESS=str(work/'progress'),VERSION_FILE=str(version),restart_apps=lambda:None,
              TAILSCALE=str(root/'no-tailscale'),TAILSCALE_STATE=str(root/'no-state'))
    manifest={'version':'1','files':[{'path':str(target)},{'path':str(version)}]}
    def stage():
        for p,body in ((target,'new'),(version,'1\n')):
            staged=work/'stage'/str(p).lstrip('/');staged.parent.mkdir(parents=True,exist_ok=True);staged.write_text(body)
    stage();real_save=up['save']
    def crash_after_replace(path,obj):
        if path==up['JOURNAL'] and obj['files'][0]['done']:
            raise SimulatedCrash()
        return real_save(path,obj)
    up['save']=crash_after_replace
    try:up['apply'](manifest)
    except SimulatedCrash:pass
    up['save']=real_save
    durable=up['load'](up['JOURNAL'])
    print('crash after target replace: stored replacing=',durable['files'][0]['replacing'],'done=',durable['files'][0]['done'],'target=',target.read_text())
    up['undo'](durable);once=target.read_text();up['undo'](up['load'](up['JOURNAL']));twice=target.read_text()
    print('A02/A03 undo once=',once,'twice=',twice)
    assert once==twice=='old'
    # Crash between PENDING and state=applied: durable journal remains applying; boot-check undoes.
    stage()
    def crash_after_pending(path,obj):
        real_save(path,obj)
        if path==up['PENDING']:raise SimulatedCrash()
    up['save']=crash_after_pending
    try:up['apply'](manifest)
    except SimulatedCrash:pass
    up['save']=real_save
    print('pending-marker gap: journal=',up['load'](up['JOURNAL'])['state'],'pending=',bool(up['load'](up['PENDING'])))
    result=up['cmd_boot_check']([])
    print('boot-check:',result,'target=',target.read_text(),'version=',version.read_text().strip())
    assert target.read_text()=='old' and version.read_text()=='0\n'
