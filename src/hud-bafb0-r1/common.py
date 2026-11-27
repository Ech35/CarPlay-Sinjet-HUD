"""Pinned BAFB0 input and verified container/layout primitives."""
from hashlib import sha256
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent
WORK = HERE.parent
ROOT = WORK.parent
TOOLS = ROOT / 'tools'
sys.path.insert(0,str(TOOLS))
from hud_pak_tool import INNER_RECORD_TAG, MAGIC, parse_records, record_header_valid

SOURCE = ROOT / 'inputs/mcu_HUDBAFB0_201012.pak'
SOURCE_SHA = 'eb6b541a6a6050349f64d1cdb7b52459bf5d810c4e214ccc1b7777f63788f9dc'
SYSTEM_SHA = '141c748daba3cf06f62294f1660a280397f9693ca80bbf9cc37881dd4be0a458'
BASE = 0x08020200
STOCK_VERSION, VERSION = 20201012, 20201013
SECTOR = 4096
LOGICAL_BASE = 0x4000
ICONS = range(40,86)
SCATTER = 0x080B240C
LOGICAL_TABLE = 0x0809BE10
CATALOG = 0x0809C8D0


def align(n, unit=SECTOR):
    return (n + unit - 1) // unit * unit


def checksum(data):
    v = sum(data) & 255
    return 0 if v == 255 else v


def index_records(blob):
    records = parse_records(blob)
    assert all(r.payload_start is not None and record_header_valid(blob,r) for r in records)
    tops = {r.name:r for r in records if r.kind=='top'}
    children = {}
    for parent in tops.values():
        cursor = parent.payload_start
        for child in (r for r in records if parent.offset < r.offset < parent.end):
            assert child.kind=='inner' and child.offset==cursor
            key=parent.name,child.name
            assert key not in children
            children[key]=child
            cursor=child.end
        assert cursor in (parent.payload_start,parent.end)
    return records,tops,children


def load_source():
    blob=SOURCE.read_bytes()
    assert sha256(blob).hexdigest()==SOURCE_SHA
    records,tops,children=index_records(blob)
    assert len(records)==345 and len(tops)==17 and len(children)==328
    assert all(r.tag==STOCK_VERSION for r in tops.values())
    rom=blob[tops['system.bin'].payload_start:tops['system.bin'].end]
    assert sha256(rom).hexdigest()==SYSTEM_SHA
    return blob,rom,records,tops,children


def initial_ram(rom):
    source,destination,size,decoder=struct.unpack_from('<IIII',rom,SCATTER-BASE)
    assert (source,destination,size,decoder)==(0x080B242C,0x20000000,0x76D4,0x08021428)
    cursor=source-BASE
    ram=bytearray()
    while len(ram)<size:
        control=rom[cursor];cursor+=1
        literals=control&7
        if not literals:
            literals=rom[cursor];cursor+=1
        count=control>>4
        if not count:
            count=rom[cursor];cursor+=1
        assert literals>=1
        ram.extend(rom[cursor:cursor+literals-1]);cursor+=literals-1
        if control&8:
            distance=rom[cursor];cursor+=1
            assert 0<distance<=len(ram)
            for _ in range(count+2):ram.append(ram[-distance])
        else:ram.extend(bytes(count))
    assert len(ram)==size and BASE+cursor==0x080B47A1
    assert len(rom)>=607652 and rom[cursor:607652]==bytes(3)
    return bytes(ram)


def logical_rows(rom):
    at=LOGICAL_TABLE-BASE
    rows=[]
    while True:
        row=struct.unpack_from('<HHHH',rom,at)
        if row[0]==0:break
        rows.append(row);at+=8
    assert BASE+at<CATALOG
    return rows


def catalog_rows(rom):
    rows=[]
    at=CATALOG-BASE
    while True:
        n,p=struct.unpack_from('<II',rom,at)
        if n==0:break
        assert n<=0xffff and BASE<=p<BASE+len(rom)
        s=rom[p-BASE:].split(b'\0')[0].decode('ascii')
        rows.append((n,s));at+=8
    assert len(rows)==415 and BASE+at==0x0809D5C8
    return rows
