"""Recover stock theme/widget data without changing the input firmware."""
import json
import struct
from common import *

_,rom,_,_,_=load_source()
ram=initial_ram(rom)
(HERE/'initial-ram.bin').write_bytes(ram)
catalog=dict(catalog_rows(rom))
logical=logical_rows(rom)
styles=[]
for offset in range(0,len(ram)-32,4):
    kind,flags,number=struct.unpack_from('<HHI',ram,offset)
    if kind==3 and number==0x3b:
        rect=struct.unpack_from('<hhhh',ram,offset+8)
        pointer=struct.unpack_from('<I',ram,offset+20)[0]
        if not 0x20000000<=pointer<0x20000000+len(ram)-6:continue
        ids=struct.unpack_from('<HHH',ram,pointer-0x20000000)
        styles.append({'widget':hex(0x20000000+offset),'flags':flags,'rectangle':rect,
                       'style':hex(pointer),'resources':ids,
                       'paths':[catalog.get(n) for n in ids]})
nav=[{'logical':row[0],'states':row[1:],'paths':[catalog.get(n) for n in row[1:]]}
     for row in logical if 0x4e<=row[0]<=0x5f]
report={'initialized_ram_bytes':len(ram),'navigation_widgets':styles,
        'stock_navigation_mapping':nav,'logical_rows':len(logical),
        'catalog_rows':len(catalog_rows(rom)),
        'theme_table_pointers':{str(theme):hex(pointer) for theme,pointer in
                                [(0,0x20006b08),(1,0x20006858),(2,0x200069b0),(3,0x20006700)]}}
(HERE/'layout-audit.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps(report,ensure_ascii=True,indent=2))
