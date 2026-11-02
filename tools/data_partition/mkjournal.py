"""qso_log.db as the GUI leaves it after a power cut in a change: rollback mode, a hot -journal, the
change's pages already in the file"""
import os
import sqlite3
import sys

c = sqlite3.connect(sys.argv[1], isolation_level=None)
c.execute("create table if not exists qsos(call text, pad text)")
c.execute("begin")
for i in range(200):
    c.execute("insert into qsos values(?, ?)", ("SYS%d" % i, "x" * 400))
c.execute("commit")
c.execute("pragma cache_size=1")
c.execute("begin")
c.execute("update qsos set pad=?", ("y" * 400,))
os._exit(0)
