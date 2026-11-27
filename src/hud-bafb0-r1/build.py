"""Build only HUDBAFB0 201013, with the approved 46 standard icon extensions."""
from hashlib import md5, sha256
from io import BytesIO
import json
import struct
import zlib

from common import *
from PIL import Image
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS
from keystone import Ks, KS_ARCH_ARM, KS_MODE_THUMB

PATCH_SITES = {
    0x08090394: '70b50446',
    0x08060bf8: '01460022',
    0x080653b8: '2de9f041',
}


def check_png(data):
    assert data[:8]==b'\x89PNG\r\n\x1a\n'
    cursor=8
    tags=[]
    while cursor<len(data):
        n,tag=struct.unpack_from('>I4s',data,cursor)
        end=cursor+n+12
        assert end<=len(data)
        assert zlib.crc32(data[cursor+4:end-4])==struct.unpack_from('>I',data,end-4)[0]
        tags.append(tag)
        if tag==b'IHDR':assert struct.unpack_from('>IIBBBBB',data,cursor+8)==(68,68,8,6,0,0,0)
        cursor=end
    assert tags[0]==b'IHDR' and tags[-1]==b'IEND' and cursor==len(data)
    assert set(tags)<={b'IHDR',b'IDAT',b'pHYs',b'IEND'}
    image=Image.open(BytesIO(data));image.load()
    assert image.size==(68,68) and image.mode=='RGBA'
    return image


def approved_assets(stock_blob,stock_children):
    catalog=json.loads((ROOT/'assets/hud-icons/manifest.json').read_text(encoding='utf-8'))
    approved=json.loads((ROOT/'assets/hud-icons/fb0-manifest.json').read_text(encoding='utf-8'))
    output={}
    for entry in approved:
        n=entry['ble_id']
        ref=next(e for e in catalog if e['theme']=='STD' and e['ble_id']==n)
        if ref['copied_stock'] is not None:
            r=stock_children['naviInfo.pak',f"turnArrow_{ref['copied_stock']}.png"]
            png=stock_blob[r.payload_start:r.end]
        else:
            png=(ROOT/f'assets/hud-icons/STD/turnArrow_{n}.png').read_bytes()
        check_png(png)
        assert sha256(png).hexdigest()==entry['sha256']
        output[f'turnArrow_{n}.png']=png
    assert len(output)==46
    return output,approved


def patch_system(before):
    start=BASE+len(before)
    assert start==0x080b47a4
    source=(HERE/'icon_hooks.s').read_text(encoding='utf-8')
    source='\n'.join(s.split('//',1)[0] for s in source.splitlines())
    ks=Ks(KS_ARCH_ARM,KS_MODE_THUMB)
    code,_=ks.asm(source,addr=start,as_bytes=True)
    # Stay inside the original final 4KiB address range; no new internal-flash range.
    assert align(start)==align(start+len(code))
    out=bytearray(before)
    for index,(site,pin) in enumerate(PATCH_SITES.items()):
        off=site-BASE
        assert out[off:off+4].hex()==pin
        target=start+index*4
        patch,_=ks.asm(f'b.w #{target:#x}',addr=site,as_bytes=True)
        decoded,=Cs(CS_ARCH_ARM,CS_MODE_THUMB|CS_MODE_MCLASS).disasm(patch,site)
        assert decoded.mnemonic=='b.w' and int(decoded.op_str[1:],0)==target
        out[off:off+4]=patch
    old=b'#SW_MODULE#HUDBAFB0\0#SW_DATE#20201012\0'
    assert out.count(old)==1
    out=out.replace(old,old.replace(b'20201012',b'20201013'))
    assert len(out)==len(before) and out[:512]==before[:512]
    return bytes(out)+code


def pack_record(name,tag,payload):
    encoded=name.encode('ascii')
    fixed=48+len(encoded)
    sectors=1
    while sectors!=align(fixed+sectors+len(payload))//SECTOR:
        sectors=align(fixed+sectors+len(payload))//SECTOR
    size=fixed+sectors+len(payload)
    header=struct.pack('<I4sIB',size,MAGIC,tag,len(encoded))+encoded+b'\0'+md5(payload).hexdigest().encode()+b'\0'
    assert len(header)+1==fixed
    # Match FB0's stock sector-completion bitmap (native writer writes 0xFE).
    # This is NOT a per-sector arithmetic checksum table.
    return header+bytes([checksum(header)])+b'\xfe'*sectors+payload


def build():
    original,rom,_,tops,children=load_source()
    assets,manifest=approved_assets(original,children)
    output=bytearray()
    for name,record in tops.items():
        payload=original[record.payload_start:record.end]
        if name=='system.bin':
            packed=pack_record(name,VERSION,patch_system(payload))
        elif name=='naviInfo.pak':
            extra=b''.join(pack_record(n,INNER_RECORD_TAG,png) for n,png in assets.items())
            packed=pack_record(name,VERSION,payload+extra)
        else:
            packed=bytearray(original[record.offset:record.end])
            struct.pack_into('<I',packed,8,VERSION)
            off=record.search_start-record.offset
            packed[off]=checksum(packed[:off])
        output.extend(packed)
        output.extend(b'\xff'*(align(len(output))-len(output)))
    return bytes(output),manifest


if __name__=='__main__':
    from verify import verify
    candidate,manifest=build()
    (HERE/'candidate.pak').write_bytes(candidate)
    report=verify(candidate,manifest)
    (HERE/'verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k!='icons'},ensure_ascii=True,indent=2))
