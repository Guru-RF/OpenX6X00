"""params.db as the GUI leaves it after a power cut: WAL mode, the last change only in -wal (-shm too)"""
import os
import sqlite3
import sys

c = sqlite3.connect(sys.argv[1])
c.execute("pragma journal_mode=wal")
c.execute("create table params(name text primary key, val text)")
c.execute("insert into params values('call', 'ON6URE')")
c.commit()
c.execute("pragma wal_checkpoint(truncate)")
c.execute("pragma wal_autocheckpoint=0")
c.execute("insert into params values('after', 'only_in_wal')")
c.commit()
os._exit(0)
