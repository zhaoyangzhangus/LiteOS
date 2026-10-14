"""Actual read-only cache probe distinguishes data, empty and bad snapshots."""
import ast
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sqlite3
import struct
import zlib
import sys
from types import SimpleNamespace

root = Path(__file__).resolve().parents[2]
tree = ast.parse((root/'tools/chromium/http-cache-probe.py').read_text())
code = next(ast.literal_eval(n.value) for n in tree.body
            if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='code' for t in n.targets))
class Pointer:
    def __init__(self,value):self.value=value
    def __int__(self):return 1
    def cast(self,kind):return self
    def dereference(self):return self.value
class Text(str):
    def string(self):return str(self)
def node(path,data,size,directory=False):
    backend={'path':Text(path),'directory':directory,'size':size,
             'data_capacity':size,'data':data}
    return Pointer({'size':size,'fs_private':Pointer(backend)})
def database(rows):
    db=sqlite3.connect(':memory:')
    try:
        db.execute('CREATE TABLE resources (hash INTEGER)')
        db.execute('CREATE TABLE blobs (data BLOB)')
        for i in range(rows):
            db.execute('INSERT INTO resources VALUES (?)',(i,))
            db.execute('INSERT INTO blobs VALUES (?)',(b'fixture',))
        db.commit()
        return db.serialize()
    finally:db.close()
reads=[]
def probe(payload,capacity=3):
    nodes=[node('/tmp/chrome-data/Default/Cache/Cache_Data/sqldb0',99,len(payload)),
           node('/tmp/chrome-data/Default/Cache/Cache_Data',0,0,True),
           node('/tmp/not-chrome/Default/Cache/sqldb0',100,4096)]
    def read(address,size):
        assert address==99 and size==len(payload)
        reads.append((address,size));return payload
    fake=SimpleNamespace(GdbError=RuntimeError,
        parse_and_eval=lambda name:capacity if name=='g_vfs_node_capacity' else nodes,
        lookup_type=lambda name:SimpleNamespace(pointer=lambda:None),
        selected_inferior=lambda:SimpleNamespace(read_memory=read))
    previous=sys.modules.get('gdb')
    sys.modules['gdb']=fake
    output=io.StringIO()
    try:
        with redirect_stdout(output):exec(code,{})
        lines=output.getvalue().splitlines()
        assert len(lines)==1 and lines[0].startswith('HTTP_CACHE ')
        return json.loads(lines[0][len('HTTP_CACHE '):])
    finally:
        if previous is None:sys.modules.pop('gdb',None)
        else:sys.modules['gdb']=previous
for rows in (0,3):
    result=probe(database(rows))
    assert result['file_count']==1 and len(result['directories'])==1
    assert result['files'][0]['sqlite_rows']=={'resources':rows,'blobs':rows}
assert len(reads)==2
bad=probe(b'not a SQLite database')
assert 'sqlite_snapshot_error' in bad['files'][0]
assert 'sqlite_rows' not in bad['files'][0]
try:probe(database(0),32769)
except RuntimeError:pass
else:raise AssertionError('Unbounded registry was accepted')
assert len(reads)==3
def checksummed_database(corrupt=None):
    db=sqlite3.connect(':memory:')
    try:
        db.execute('CREATE TABLE resources (res_id INTEGER,head BLOB,check_sum INTEGER,cache_key_hash INTEGER)')
        db.execute('CREATE TABLE blobs (blob_id INTEGER,res_id INTEGER,blob BLOB,check_sum INTEGER)')
        key_hash=0x123456789abcdef
        def crc(data):
            unsigned=zlib.crc32(struct.pack('>I',key_hash & 0xffffffff),zlib.crc32(data))
            return unsigned if unsigned<0x80000000 else unsigned-0x100000000
        head=b'Chromium header fixture'
        body=bytes(range(256))*17
        db.execute('INSERT INTO resources VALUES (1,?,?,?)',
                   (head,crc(head),key_hash))
        db.execute('INSERT INTO blobs VALUES (2,1,?,?)',(body,crc(body)))
        if corrupt=='head':db.execute('UPDATE resources SET head=?',(b'corrupt',))
        if corrupt=='body':db.execute('UPDATE blobs SET blob=?',(b'corrupt',))
        db.commit()
        return db.serialize()
    finally:db.close()
for corrupt in (None,'head','body'):
    record=probe(checksummed_database(corrupt))['files'][0]
    assert record['sqlite_quick_check']==['ok']
    checks=record['sqlite_checksum_snapshot']
    assert checks['headers_checked']==checks['blobs_checked']==1
    assert checks['bad_headers']==([1] if corrupt=='head' else [])
    assert checks['bad_blobs']==([2] if corrupt=='body' else [])
    assert not checks['partial']
outer=next(n for n in tree.body if isinstance(n,ast.Try))
cleanup=ast.unparse(ast.Module(body=outer.finalbody,type_ignores=[]))
assert 'gdbserver none' in cleanup and "command('cont')" in cleanup
assert "command('query-status')['running']" in cleanup
print('Cache probe: SQLite integrity, Chromium network-order CRC, corrupt headers/body, scoped reads and cleanup passed')
