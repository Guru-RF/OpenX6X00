import ast
import fcntl
import hashlib
import json
import os
import re
import runpy
import tempfile
from pathlib import Path
from types import SimpleNamespace

path=Path(__file__).resolve().parents[3] / 'webserver/src/x6100_webserver/apps.py'
tree=ast.parse(path.read_text())
needed={'field_path','adif_split','adif_tag','adif_set','adif_del','adif_get','edit_qso','adif_fields','log_records','api_error','put_log_qso','rec_id','set_ref','ref_prog','ref_norm'}
selected=[]
for node in tree.body:
    if isinstance(node,ast.FunctionDef) and node.name in needed:
        node.decorator_list=[];selected.append(node)
    elif isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id in ('SOTA_REF','POTA_REF','REF_SIG','TILE','LOG_FIELDS','SUBMODE_OF') for t in node.targets):selected.append(node)
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
                       ('LH BEL 2',{'sig':'ARLHS','sig_info':'BEL 002'}),('jo20AB',{'sig':'TILES','sig_info':'JO20ab'}),
                       ('OKR-0150',{'sig':'TOTA','sig_info':'OKR-0150'})):
        bottle.request.query={'id':ns['rec_id'](ns['log_records']('day.adi')[0])}
        bottle.request.json={'ref':typed}
        ns['put_log_qso']('day.adi',0)
        fields=ns['adif_fields'](ns['log_records']('day.adi')[0])
        got={k:fields[k] for k in ('sig','sig_info','pota_ref','sota_ref') if k in fields}
        print('save ref',typed,'->',got)
        assert got==want
        if want.get('sig')=='TILES':   # tile to tile: their tile is where they are
            assert fields.get('gridsquare')=='JO20ab', fields
        elif typed=='OKR-0150':        # and goes with it when the reference changes
            assert 'gridsquare' not in fields, fields
    # An older page sends the sota_ref / pota_ref pair: SIG and SIG_INFO follow (set_ref)
    bottle.request.query={'id':ns['rec_id'](ns['log_records']('day.adi')[0])}
    bottle.request.json={'sota_ref':'','pota_ref':'US-5678'}
    ns['put_log_qso']('day.adi',0)
    fields=ns['adif_fields'](ns['log_records']('day.adi')[0])
    got={k:fields[k] for k in ('sig','sig_info','pota_ref','sota_ref') if k in fields}
    print('older page saves pota_ref US-5678 ->',got)
    assert got=={'sig':'POTA','sig_info':'US-5678','pota_ref':'US-5678'}

# Unicode edits (audit C10 of the 26 October review): an ADIF length counts bytes, as the radio writes them
# (src/field/flog.c) and x6100-wavelog reads them; the web editor counted characters
helper=runpy.run_path(str(path.parents[3]/'rootfs/usr/sbin/x6100-wavelog'))
with tempfile.TemporaryDirectory(prefix='x6log-') as td:
    ns['settings'].FIELD_DIR=td
    logs=Path(td)/'logs';logs.mkdir()
    log=logs/'day.adi'
    def edit(n,change):
        bottle.request.query={'id':ns['rec_id'](ns['log_records']('day.adi')[n])}
        bottle.request.json=change
        return ns['put_log_qso']('day.adi',n)
    log.write_bytes('<CALL:6>DL1ABC<NAME:7>Jürgen<MODE:3>SSB<RST_SENT:2>59<eor>\n'.encode())
    fields=ns['adif_fields'](ns['log_records']('day.adi')[0])
    print('read <NAME:7>Jürgen<MODE:3>SSB:',{k:fields.get(k) for k in ('name','mode','rst_sent')})
    assert fields.get('name')=='Jürgen' and fields.get('mode')=='SSB' and fields.get('rst_sent')=='59'
    edit(0,{'name':'Peter'})
    raw=log.read_bytes()
    print('NAME Jürgen -> Peter:',raw)
    assert raw=='<CALL:6>DL1ABC<NAME:5>Peter<MODE:3>SSB<RST_SENT:2>59<eor>\n'.encode()
    # Typed into an ASCII record next to typed fields; another record holds a byte that is not UTF-8 (a log from
    # elsewhere, in Latin-1): it stays as it is when this one is saved
    other=b'<CALL:5>F4ABC <NAME:4>Ren\xe9 <MODE:2>CW <eor>\n'
    log.write_bytes(b'<CALL:5>K1ABC <QSO_DATE:8:D>20261027 <FREQ:6:N>14.200 <NAME:3>Joe <MODE:2>CW <eor>\n'+other)
    edit(0,{'name':'Jürgen','qth':'Łódź','comment':'73 de Jürgen ✓'})
    raw=log.read_bytes()
    first=ns['log_records']('day.adi')[0]
    fields,up=ns['adif_fields'](first),helper['adif_fields'](first)
    print('NAME, QTH, COMMENT typed:',raw.split(b'<eor>')[0])
    print('  web reads',{k:fields.get(k) for k in ('name','qth','comment','freq','mode')})
    print('  upload reads',{k:up.get(k) for k in ('name','qth','comment','mode')},'other record kept:',raw.endswith(other))
    assert '<NAME:7>Jürgen'.encode() in raw and '<QTH:7>Łódź'.encode() in raw and '<COMMENT:17>73 de Jürgen ✓'.encode() in raw
    for got in (fields,up):
        assert got['name']=='Jürgen' and got['qth']=='Łódź' and got['comment']=='73 de Jürgen ✓' and got['mode']=='CW'
    assert b'<QSO_DATE:8:D>20261027 <FREQ:6:N>14.200 ' in raw and raw.endswith(other), raw
    edit(0,{'qth':''})
    raw=log.read_bytes()
    fields=ns['adif_fields'](ns['log_records']('day.adi')[0])
    print('QTH removed:',raw.split(b'<eor>')[0])
    assert b'QTH' not in raw and fields['name']=='Jürgen' and fields['comment']=='73 de Jürgen ✓' and raw.endswith(other)
