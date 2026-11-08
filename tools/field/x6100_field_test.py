#!/usr/bin/env python3
"""x6100-field's list readers on small lists: the refs.tsv lines they write.

  python3 tools/field/x6100_field_test.py   (X6100_FIELD=path: another copy of the script)

The cases are the corners of the downloaded lists: a byte order mark, inactive and
out-of-date entries, tabs and line breaks in names, short and blank rows, bad numbers
and a list without the expected columns.
"""
import datetime
import importlib.machinery
import importlib.util
import io
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.environ.get("X6100_FIELD") or os.path.join(HERE, "..", "..", "rootfs", "usr", "sbin", "x6100-field")


def load():
    loader = importlib.machinery.SourceFileLoader("x6100_field", SCRIPT)
    spec = importlib.util.spec_from_loader("x6100_field", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def run(fn, text):
    out = io.StringIO()
    n = fn(io.BytesIO(text.encode("utf-8")), out)
    return n, out.getvalue()


POTA = ('﻿"reference","name","active","entityId","locationDesc","latitude","longitude","grid"\n'
        '"K-0001","Acadia National Park","1","291","US-ME","44.31","-68.2034","FN54vh"\n'
        '"K-0002","Closed Park","0","291","US-ME","44.0","-68.0","FN54"\n'
        '"K-0003","Tab\tand\nbreak","1","291","US-ME","40.5","-70.25","FN30"\n'
        '\n'
        '"K-0004","Short row","1"\n'
        '"K-0005","Bad latitude","1","291","US-ME","north","-70","FN30"\n')

SOTA = ('SOTA Summits List (Date=07/11/2026)\n'
        'SummitCode,AssociationName,RegionName,SummitName,AltM,AltFt,GridRef1,GridRef2,Longitude,Latitude,Points,'
        'BonusPoints,ValidFrom,ValidTo,ActivationCount,ActivationDate,ActivationCall\n'
        'W1/HA-001,USA,Hampshire,Mount Washington,1917,6288,-71.3,44.27,-71.3033,44.2706,10,3,01/07/2010,31/12/2099,10,,\n'
        'W1/HA-002,USA,Hampshire,Retired Summit,1000,3281,-71,44,-71.0,44.0,4,0,01/07/2010,31/12/2015,0,,\n'
        'W1/HA-003,USA,Hampshire,Future Summit,900,2953,-71,44,-71.1,44.1,4,0,01/01/2099,31/12/2099,0,,\n'
        'W1/HA-004,USA,Hampshire,Bad points,900,2953,-71,44,-71.1,44.1,x,0,01/07/2010,31/12/2099,0,,\n')

WWFF = ('reference,status,name,program,dxcc,state,county,continent,iota,iaruLocator,latitude,longitude\n'
        'KFF-0001,active,Acadia National Park,KFF,K,ME,,NA,,FN54,44.31,-68.2\n'
        'KFF-0002,deleted,Gone Park,KFF,K,ME,,NA,,FN54,44.0,-68.0\n'
        'KFF-0003,active,"Quoted, comma",KFF,K,ME,,NA,,FN54,43.5,-69.75\n')

WWBOTA = ('Reference,Name,Lat,Long,Type\n'
          'B/G-0001,Pillbox One,51.5,-0.12,Pillbox\n'
          'B/G-0002,No position,,,Pillbox\n')

NO_COLUMNS = 'id,title\n1,Nothing useful\n'

EXPECTED = {
    "parks": (2, "P\tK-0001\tAcadia National Park\t44.31000\t-68.20340\t\t\n"
                 "P\tK-0003\tTab and break\t40.50000\t-70.25000\t\t\n"),
    "summits": (1, "S\tW1/HA-001\tMount Washington\t44.27060\t-71.30330\t1917\t10\n"),
    "wwff": (2, "F\tKFF-0001\tAcadia National Park\t44.31000\t-68.20000\t\t\n"
                "F\tKFF-0003\tQuoted, comma\t43.50000\t-69.75000\t\t\n"),
    "bunkers": (1, "B\tB/G-0001\tPillbox One\t51.50000\t-0.12000\t\t\n"),
}


def main():
    m = load()
    today = datetime.date(2026, 11, 7)
    wwff = [entry[3] for entry in m.LISTS if entry[0] == "F"][0]
    bunkers = [entry[3] for entry in m.LISTS if entry[0] == "B"][0]
    got = {
        "parks": run(m.write_parks, POTA),
        "summits": run(lambda s, o: m.write_summits(s, o, today), SOTA),
        "wwff": run(wwff, WWFF),
        "bunkers": run(bunkers, WWBOTA),
    }
    bad = 0
    for name, want in EXPECTED.items():
        if got[name] != want:
            print("FAIL %s\n  want %r\n  got  %r" % (name, want, got[name]))
            bad += 1
    for name, fn in (("parks", m.write_parks), ("wwff", wwff), ("bunkers", bunkers)):
        if run(fn, NO_COLUMNS) != (0, ""):
            print("FAIL %s without its columns: %r" % (name, run(fn, NO_COLUMNS)))
            bad += 1
    print("%s: %d cases" % ("FAIL" if bad else "OK", len(EXPECTED) + 3))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
