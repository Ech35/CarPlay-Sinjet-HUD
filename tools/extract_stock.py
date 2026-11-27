"""Extract only pinned original executables to ignored local inputs."""
from pathlib import Path
from hashlib import sha256
import argparse,sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'src'))
from repack_u5 import parse_update
from PySquashfsImage import SquashFsImage

def extract(platform):
    if platform!='u6a':raise ValueError(platform)
    source=ROOT/'inputs/u6a-stock.img'
    blob=source.read_bytes()
    assert sha256(blob).hexdigest()=='172a455fed34347fd7fdb4d4758733a9986c02b5e811c80f9decb655e45c5ffa','Unexpected 8952 stock image'
    entries=parse_update(blob)
    assert b'ly_project_name = "ly8952";' in entries[0].data
    needed={'libiap.so','lylinkapp'}
    for node in SquashFsImage.from_bytes(entries[1].data):
        name=node.path.lstrip('/')
        if node.is_file and name in {'app/'+n for n in needed}:
            out=ROOT/'inputs/u6a-app'/Path(name).name
            out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(node.read_bytes())
            needed.remove(out.name)
    assert not needed
    print('Pinned stock input checked; native application files staged locally.')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('platform',choices=['u6a'])
    extract(parser.parse_args().platform)
