import ast
import fcntl
import hashlib
import json
import os
import re
import tempfile
from pathlib import Path
from types import SimpleNamespace

path=Path(__file__).resolve().parents[3] / 'webserver/src/x6100_webserver/apps.py'
tree=ast.parse(path.read_text())
needed={'field_path','adif_split','adif_set','adif_del','edit_qso','adif_fields','log_records','api_error','put_log_qso','rec_id','set_ref','ref_prog','ref_norm'}
selected=[]
for node in tree.body:
    if isinstance(node,ast.FunctionDef) and node.name in needed:
        node.decorator_list=[];selected.append(node)
    elif isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id in ('SOTA_REF','POTA_REF','REF_SIG','LOG_FIELDS','SUBMODE_OF') for t in node.targets):selected.append(node)
bottle=SimpleNamespace(request=SimpleNamespace(),response=SimpleNamespace())
ns={'fcntl':fcntl,'hashlib':hashlib,'json':json,'os':os,'re':re,'bottle':bottle,'settings':SimpleNamespace()}
exec(compile(ast.Module(body=selected,type_ignores=[]),str(path),'exec'),ns)
with tempfile.TemporaryDirectory(prefix='x6log-') as td:
    ns['settings'].FIELD_DIR=td
    logs=Path(td)/'logs';logs.mkdir()
    log=logs/'day.adi'
    log.write_text(''.join(f'<CALL:5>{call}<eor>\n' for call in ('K1AAA','K1BBB','K1CCC')))
    # Both tabs saw K1AAA at index 0, K1BBB at index 1, K1CCC at index 2, each with its id (fixed, audit B09)
    ids=[ns['rec_id'](r) for r in ns['log_records']('day.adi')]
    result_a=ns['edit_qso']('day.adi',0,lambda _:None,ids[0])
    result_b=ns['edit_qso']('day.adi',1,lambda _:None,ids[1])   # the row number is stale, the id says K1BBB
    remaining=[ns['adif_fields'](r)['call'] for r in ns['log_records']('day.adi')]
    print('delete tab A index 0, tab B stale index 1 with its id; errors:',result_a,result_b,'remaining:',remaining)
    assert result_a is None and result_b is None and remaining==['K1CCC']
    stale=ns['edit_qso']('day.adi',0,lambda _:None,ids[0])      # K1AAA is gone: a conflict, nothing deleted
    print('delete a QSO no longer there:',stale,'HTTP',bottle.response.status)
    assert bottle.response.status==409 and [ns['adif_fields'](r)['call'] for r in ns['log_records']('day.adi')]==['K1CCC']
    log.write_text('<CALL:5>K1BBB <MODE:4>MFSK <SUBMODE:3>JS8 <eor>\n')
    bottle.request.query={'id':ns['rec_id'](ns['log_records']('day.adi')[0])}
    bottle.request.json={'mode':'FT8'}
    result=ns['put_log_qso']('day.adi',0)
    fields=ns['adif_fields'](ns['log_records']('day.adi')[0])
    effective=fields['submode'] if fields.get('submode') else fields['mode']
    print('save mode FT8:',result,'fields:',fields,'displayed/imported mode:',effective)
    assert fields['mode']=='FT8' and 'submode' not in fields and effective=='FT8'   # fixed (audit B17)
    bottle.request.query={'id':ns['rec_id'](ns['log_records']('day.adi')[0])}
    bottle.request.json={'mode':'JS8'}
    result=ns['put_log_qso']('day.adi',0)
    fields=ns['adif_fields'](ns['log_records']('day.adi')[0])
    print('save mode JS8:',result,'fields:',fields)
    assert fields['mode']=='MFSK' and fields['submode']=='JS8'
    # The other station's reference, any programme, written as the radio writes it (TOTA, WCA, MOTA added)
    for typed,want in (('OKR-0150',{'sig':'TOTA','sig_info':'OKR-0150'}),('DL-01156',{'sig':'WCA','sig_info':'DL-01156'}),
                       ('X42018',{'sig':'MOTA','sig_info':'X42018'}),('US-1234',{'sig':'POTA','sig_info':'US-1234','pota_ref':'US-1234'}),
                       ('I/LO-123',{'sota_ref':'I/LO-123'}),('BEL-002',{'sig':'ARLHS','sig_info':'BEL 002'}),
                       ('BEL 002',{'sig':'ARLHS','sig_info':'BEL 002'}),('SCO-0144',{'sig':'ARLHS','sig_info':'SCO 144'}),
                       ('LH BEL 2',{'sig':'ARLHS','sig_info':'BEL 002'}),('OKR-0150',{'sig':'TOTA','sig_info':'OKR-0150'})):
        bottle.request.query={'id':ns['rec_id'](ns['log_records']('day.adi')[0])}
        bottle.request.json={'ref':typed}
        ns['put_log_qso']('day.adi',0)
        fields=ns['adif_fields'](ns['log_records']('day.adi')[0])
        got={k:fields[k] for k in ('sig','sig_info','pota_ref','sota_ref') if k in fields}
        print('save ref',typed,'->',got)
        assert got==want
    # An older page sends the sota_ref / pota_ref pair: SIG and SIG_INFO follow (set_ref)
    bottle.request.query={'id':ns['rec_id'](ns['log_records']('day.adi')[0])}
    bottle.request.json={'sota_ref':'','pota_ref':'US-5678'}
    ns['put_log_qso']('day.adi',0)
    fields=ns['adif_fields'](ns['log_records']('day.adi')[0])
    got={k:fields[k] for k in ('sig','sig_info','pota_ref','sota_ref') if k in fields}
    print('older page saves pota_ref US-5678 ->',got)
    assert got=={'sig':'POTA','sig_info':'US-5678','pota_ref':'US-5678'}
