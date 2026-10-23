import ast
import subprocess
import threading
from pathlib import Path
from types import SimpleNamespace
source=Path(__file__).resolve().parents[3] / 'webserver/src/x6100_webserver/apps.py'
tree=ast.parse(source.read_text());nodes=[]
for node in tree.body:
    if isinstance(node,ast.FunctionDef) and node.name in ('backup_error','gui_stop_for_restore','restore_backup'):
        node.decorator_list=[];nodes.append(node)
bottle=SimpleNamespace(request=SimpleNamespace(json={'parts':['settings'],'token':'old-preview'}),response=SimpleNamespace())
ns={'bottle':bottle,'backup_lock':threading.Lock(),'subprocess':subprocess,'os':__import__('os'),'sqlite3':__import__('sqlite3'),
    'stage_token':lambda:'new-upload','time':SimpleNamespace(sleep=lambda _:None)}
exec(compile(ast.Module(body=nodes,type_ignores=[]),str(source),'exec'),ns)
print('old preview after new upload:',ns['restore_backup'](),'HTTP',bottle.response.status)
assert bottle.response.status==409
# Current token, but persistent GUI writer. No file or stage enumeration beyond an immutable fake list.
bottle.request.json['token']='new-upload'
ns.update(stage_files=lambda:{'params.db':'unused'},backup_member=lambda _ :('settings','unused'),BACKUP_GUI_PARTS=('settings',),
          gui_running=lambda:True,gui_service=lambda action:1)
print('writer stays alive:',ns['restore_backup'](),'HTTP',bottle.response.status)
assert bottle.response.status==500
