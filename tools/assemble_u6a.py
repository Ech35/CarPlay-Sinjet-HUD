"""Assemble R2c directly from pinned stock and locally compiled open components."""
from pathlib import Path
from hashlib import sha256
import json
from tempfile import TemporaryDirectory
from importlib.util import module_from_spec,spec_from_file_location
import struct,sys
ROOT=Path(__file__).resolve().parents[1];SRC=ROOT/'src/u6a-8952-r2c'
sys.path.insert(0,str(ROOT/'src'))
from repack_u5 import parse_update,package_update,make_tar,rebuild_squashfs
from PySquashfsImage import SquashFsImage

def management_page(stock):
    """Apply the project changes to the user's original management page."""
    patch = json.loads((SRC / 'management-page.patch.json').read_text(encoding='utf-8'))
    if sha256(stock).hexdigest() != patch['stock_sha256']:
        raise ValueError('Unexpected stock management page')
    lines = stock.decode('utf-8').splitlines(keepends=True)
    lines = [line.replace('\r\n', '\n') for line in lines]
    result = []
    cursor = 0
    for edit in patch['edits']:
        start, end = edit['start'], edit['end']
        if not cursor <= start <= end <= len(lines):
            raise ValueError('Invalid management page edit range')
        result.extend(lines[cursor:start])
        result.append(edit['text'])
        cursor = end
    result.extend(lines[cursor:])
    page = ''.join(result).encode('utf-8')
    if sha256(page).hexdigest() != patch['result_sha256']:
        raise ValueError('The reconstructed management page differs from the verified page')
    return page

def renderer(stock):
    assert sha256(stock).hexdigest()=='d07fa1a8a42b38738b85be3d480651e1aa77e74529f6036ba275802ac27eb24a'
    edits=[(0x261c4,'e4089fe5','220000ea'),(0x26278,'0f10a0e3','1210a0e3'),
           (0x262a4,'2080cde5','2030cde5'),(0x262ac,'2180cde5','2130cde5'),
           (0x262b0,'2280cde5','2230cde5'),(0x262cc,'450fa0e3','a40100e3'),
           (0x262dc,'0a2042e0','822082e0'),(0x262e8,'a22f82e0','a222a0e1'),(0x262f4,'c220a0e1','0000a0e1')]
    out=bytearray(stock)
    for va,old,new in edits:
        at=va-0x10000
        assert out[at:at+4].hex()==old
        out[at:at+4]=bytes.fromhex(new)
    return bytes(out)

def main():
    base=(ROOT/'inputs/u6a-stock.img').read_bytes()
    assert sha256(base).hexdigest()=='172a455fed34347fd7fdb4d4758733a9986c02b5e811c80f9decb655e45c5ffa'
    entries=parse_update(base)
    assert b'ly_project_name = "ly8952";' in entries[0].data
    nodes={n.path.lstrip('/'):n for n in SquashFsImage.from_bytes(entries[1].data)}
    spec=spec_from_file_location('u6pack',SRC/'pack.py');mod=module_from_spec(spec);spec.loader.exec_module(mod)
    iap,cp=mod.binaries()
    original=nodes['app/lyLink.sh'].read_bytes();newline=b'\r\n' if b'\r\n' in original else b'\n'
    anchor=b'mkdir /tmp/app'+newline
    assert original.count(anchor)==1
    init=(b'# Start the RAM diagnostics before the CarPlay application.\n'
          b'if [ -x "$WORKDIR/../ota/www/cgi-bin/u6diag.cgi" ]; then\n'
          b'    "$WORKDIR/../ota/www/cgi-bin/u6diag.cgi" --init\n'
          b'    "$WORKDIR/u6hud_ble" --worker </dev/null >/dev/null 2>&1 &\nfi\n').replace(b'\n',newline)
    changes={'app/libiap.so':iap,'app/lylinkapp':cp,'app/rcfservice':renderer(nodes['app/rcfservice'].read_bytes()),
             'app/lyLink.sh':original.replace(anchor,anchor+init),'app/bg.png':(ROOT/'assets/u6a-waiting.png').read_bytes(),
             'ota/www/index.html':management_page(nodes['ota/www/index.html'].read_bytes())}
    additions={'app/u6hud_ble':((SRC/'u6hud_ble').read_bytes(),0o755),
               'ota/www/cgi-bin/u6diag.cgi':((SRC/'u6diag.cgi').read_bytes(),0o755),
               'ota/www/cgi-bin/u6hud_ble.cgi':((SRC/'u6hud_ble.cgi').read_bytes(),0o755)}
    remove={'app/a.png','app/b.png','ota/www/js/aliyun-oss-sdk.min.js'}
    with TemporaryDirectory(prefix='u6-build-') as temp:
        folder=Path(temp);tar=folder/'customer.tar';fs=folder/'customer.squash'
        expected,pseudo=make_tar(entries[1].data,tar,changes,additions,removals=remove)
        rebuilt=rebuild_squashfs(entries[1].data,tar,fs,expected,pseudo)
        image=package_update(entries,rebuilt.ljust(len(entries[1].data),b'\0'))
        after=parse_update(image)
        assert len(image)==len(base) and after[0]==entries[0] and after[2:]==entries[2:]
    target=ROOT/'dist/u6a-8952-r2c.img';target.parent.mkdir(exist_ok=True);target.write_bytes(image)
    print('Built',target.name,'bytes=',len(image),'SHA256=',sha256(image).hexdigest())

if __name__=='__main__':main()
