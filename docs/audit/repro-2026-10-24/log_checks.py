import ast
import fcntl
import json
import os
import re
import tempfile
from pathlib import Path
from types import SimpleNamespace

path=Path(__file__).resolve().parents[3] / 'webserver/src/x6100_webserver/apps.py'
tree=ast.parse(path.read_text())
needed={'field_path','adif_split','adif_set','adif_del','edit_qso','adif_fields','log_records','api_error','put_log_qso'}
selected=[]
for node in tree.body:
    if isinstance(node,ast.FunctionDef) and node.name in needed:
        node.decorator_list=[];selected.append(node)
    elif isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id in ('SOTA_REF','POTA_REF','LOG_FIELDS') for t in node.targets):selected.append(node)
bottle=SimpleNamespace(request=SimpleNamespace(),response=SimpleNamespace())
ns={'fcntl':fcntl,'json':json,'os':os,'re':re,'bottle':bottle,'settings':SimpleNamespace()}
exec(compile(ast.Module(body=selected,type_ignores=[]),str(path),'exec'),ns)
with tempfile.TemporaryDirectory(prefix='x6log-') as td:
    ns['settings'].FIELD_DIR=td
    logs=Path(td)/'logs';logs.mkdir()
    log=logs/'day.adi'
    log.write_text(''.join(f'<CALL:5>{call}<eor>\n' for call in ('K1AAA','K1BBB','K1CCC')))
    # Both tabs saw K1AAA at index 0, K1BBB at index 1, K1CCC at index 2.
    result_a=ns['edit_qso']('day.adi',0,lambda _:None)
    result_b=ns['edit_qso']('day.adi',1,lambda _:None)
    remaining=[ns['adif_fields'](r)['call'] for r in ns['log_records']('day.adi')]
    print('delete tab A index 0, tab B stale index 1; errors:',result_a,result_b,'remaining:',remaining)
    assert remaining==['K1BBB'] # tab B intended this one, but K1CCC was deleted instead
    log.write_text('<CALL:5>K1BBB <MODE:4>MFSK <SUBMODE:3>JS8 <eor>\n')
    bottle.request.json={'mode':'FT8'}
    result=ns['put_log_qso']('day.adi',0)
    fields=ns['adif_fields'](ns['log_records']('day.adi')[0])
    effective=fields['submode'] if fields.get('submode') else fields['mode']
    print('save mode FT8:',result,'fields:',fields,'displayed/imported mode:',effective)
    assert fields['mode']=='FT8' and effective=='JS8'
