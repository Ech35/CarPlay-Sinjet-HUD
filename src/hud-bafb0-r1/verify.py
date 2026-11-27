"""Execute the BAFB0 Thumb paths and independently check the complete package.

SPI reads, allocation and GUI scheduling are simulated. The actual icon draw,
picture mapping/setter, name formatter, PAK reader and MD5 code are executed.
These checks do not replace a physical HUD/bootloader/vehicle test.
"""
from hashlib import md5, sha256
import json
import struct

from common import *
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import *

ARGS=(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)
SAVED=(UC_ARM_REG_R4,UC_ARM_REG_R5,UC_ARM_REG_R6,UC_ARM_REG_R7,
       UC_ARM_REG_R8,UC_ARM_REG_R9,UC_ARM_REG_R10,UC_ARM_REG_R11)
STACK,STOP=0x2003E000,0x080FF000
SCRATCH,OBJECT,PICTURE,GROUP,HEAP=0x20008000,0x20009000,0x20009100,0x2000C000,0x20010000
FLASH,THEME,NAV,NAV_UI=0x200260F0,0x20023CCC,0x20027770,0x20027718
CAPACITY=0x800000


class HudCpu:
    def __init__(self,rom,package,version):
        self.uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS)
        self.uc.mem_map(0x08020000,0xE0000)
        self.uc.mem_write(BASE,rom)
        self.uc.mem_map(0x20000000,0x40000)
        self.uc.mem_write(0x20000000,initial_ram(rom))
        self.uc.mem_write(FLASH+4,struct.pack('<IIII',CAPACITY,1,version,version))
        self.spi=bytearray((b'\xff'*SECTOR+package).ljust(CAPACITY,b'\xff'))
        self.gui=[]
        self.native_emitter=False
        for address in (0x08086EE0,0x08086E08,0x08086370,0x08086C60,0x080903E4,
                        0x08061234,0x080838F0,0x080822C4,0x080985D8,0x08021A7C):
            self.uc.hook_add(UC_HOOK_CODE,self.external,begin=address,end=address)

    def external(self,uc,address,size,data):
        args=tuple(uc.reg_read(r) for r in ARGS)
        result=0
        if address==0x08086EE0:
            result=GROUP
            uc.mem_write(GROUP,bytes(0xf9c))
            uc.mem_write(GROUP,struct.pack('<H',0xf90))
            uc.mem_write(GROUP+0xf94,struct.pack('<I',4))
        elif address==0x08086E08 and self.native_emitter:
            return
        elif address in (0x08086E08,0x08086370,0x08086C60,0x080903E4,0x08061234):
            self.gui.append((address,args))
        elif address==0x080838F0:
            dest,start,count,_=args
            assert 0<=start<=start+count<=CAPACITY,(hex(start),count)
            uc.mem_write(dest,bytes(self.spi[start:start+count]))
            result=1
        elif address==0x080822C4:
            assert args[0]<=0x8000
            result=HEAP
        # Mock hardware/scheduler/libc logging boundaries only; clobber volatile
        # registers as a normal ARM caller may do, to catch ABI mistakes.
        for r in (UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_R12):
            uc.reg_write(r,0xcccccccc)
        uc.reg_write(UC_ARM_REG_R0,result)
        uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))

    def run(self,start,*args,stop=STOP,count=5_000_000,check_saved=True):
        for r,v in zip(ARGS,(*args,0,0,0,0)):self.uc.reg_write(r,v)
        saved={r:0x55550000+i for i,r in enumerate(SAVED)}
        if check_saved:
            for r,v in saved.items():self.uc.reg_write(r,v)
        self.uc.reg_write(UC_ARM_REG_SP,STACK)
        self.uc.reg_write(UC_ARM_REG_LR,STOP|1)
        self.uc.emu_start(start|1,stop,count=count)
        assert self.uc.reg_read(UC_ARM_REG_PC)==stop,(hex(start),hex(self.uc.reg_read(UC_ARM_REG_PC)))
        assert self.uc.reg_read(UC_ARM_REG_SP)==STACK,hex(start)
        if check_saved:
            assert all(self.uc.reg_read(r)==v for r,v in saved.items()),hex(start)
        return self.uc.reg_read(UC_ARM_REG_R0)

    def draw(self,icon,theme=0):
        self.gui.clear()
        self.uc.mem_write(THEME+0x24,bytes([theme]))
        self.uc.mem_write(NAV_UI+0x28,bytes(4))
        result=self.run(0x08090394,icon)
        ids=[a[3] for address,a in self.gui if address==0x08086E08]
        visibility=[a[3] for address,a in self.gui if address==0x08086370]
        for address,a in self.gui:
            if address in (0x08086E08,0x08086370):assert a[:3]==(GROUP,5,0x3b)
        flag,=struct.unpack('<I',self.uc.mem_read(NAV_UI+0x28,4))
        assert result==1 and flag==1
        assert [a for address,a in self.gui if address==0x08086C60]==[(0x2766,GROUP,0xcccccccc,0xcccccccc)]
        return ids,visibility

    def receive(self,icon,previous_icon=255,previous_distance=0,distance=0x10203):
        self.gui.clear()
        self.uc.mem_write(SCRATCH,b'\x01\0'+bytes([icon])+struct.pack('<I',distance)+b'\0\0')
        self.uc.mem_write(NAV,bytes([previous_icon])+bytes(3)+struct.pack('<I',previous_distance))
        self.uc.reg_write(UC_ARM_REG_R4,SCRATCH)
        self.uc.reg_write(UC_ARM_REG_R9,0)
        self.run(0x0805c990,stop=0x0805c9da,check_saved=False)
        assert self.uc.mem_read(NAV,1)==bytes([icon])
        assert self.uc.mem_read(NAV+4,4)==struct.pack('<I',distance)
        return (list(self.gui),self.uc.reg_read(UC_ARM_REG_R9))

    def descriptor(self,logical):
        ptr=self.run(0x08060bf8,logical)
        return bytes(self.uc.mem_read(ptr,8)) if ptr else None

    def set_picture(self,logical,initial=(0,0,0),null_object=False,null_style=False):
        self.gui.clear()
        self.uc.mem_write(OBJECT+0x14,struct.pack('<I',0 if null_style else PICTURE))
        self.uc.mem_write(PICTURE,struct.pack('<HHH',*initial))
        self.run(0x0805b974,0 if null_object else OBJECT,logical)
        return struct.unpack('<HHH',self.uc.mem_read(PICTURE,6)),list(self.gui)

    def name(self,resource,capacity=256):
        self.uc.mem_write(SCRATCH,b'\xa5'*260)
        result=self.run(0x080653b8,resource,SCRATCH,capacity)
        assert self.uc.mem_read(SCRATCH+256,4)==b'\xa5'*4
        text=bytes(self.uc.mem_read(SCRATCH,256)).split(b'\0')[0]
        return result,text

    def locate(self,name):
        self.uc.mem_write(SCRATCH,name.encode('ascii')+b'\0')
        self.uc.mem_write(SCRATCH+256,bytes(4))
        assert self.run(0x08060948,SCRATCH,SCRATCH+256)==1,name
        ptr,=struct.unpack('<I',self.uc.mem_read(SCRATCH+256,4))
        return ptr-SECTOR

    def md5_record(self,record):
        address=SECTOR+record.offset
        header=self.spi[address:address+record.payload_start-record.offset]
        self.uc.mem_write(SCRATCH,bytes(header))
        # Execute native text-MD5 decoder, chunked SPI reader, MD5 transform,
        # finalizer and comparator; only SPI/alloc/free/logging are substituted.
        return self.run(0x08059808,address,SCRATCH,count=100_000_000)


def verify(blob,manifest):
    from build import PATCH_SITES,check_png
    old,before,old_records,old_tops,old_children=load_source()
    records,tops,children=index_records(blob)
    assert len(records)==391 and len(tops)==17 and len(children)==374
    assert list(tops)==list(old_tops) and len(blob)%SECTOR==0
    assert len(blob)+2*SECTOR<0x400000
    cursor=0;bitmap_count=0
    changed={'system.bin','naviInfo.pak'}
    for name,r in tops.items():
        assert r.offset==cursor and r.tag==VERSION
        cursor=align(r.end)
        assert set(blob[r.end:cursor])<={255}
        previous=old_tops[name]
        if name not in changed:assert blob[r.payload_start:r.end]==old[previous.payload_start:previous.end]
    assert cursor==len(blob)
    for r in records:
        bitmap=blob[r.search_start+1:r.payload_start]
        assert len(bitmap)==align(r.length)//SECTOR and set(bitmap)=={0xfe}
        bitmap_count+=len(bitmap)
    for key,r in old_children.items():
        newer=children[key]
        assert old[r.offset:r.end]==blob[newer.offset:newer.end]
    assert children.keys()-old_children.keys()=={('naviInfo.pak',f'turnArrow_{n}.png') for n in ICONS}
    rom=blob[tops['system.bin'].payload_start:tops['system.bin'].end]
    marker=before.index(b'#SW_DATE#20201012')+len(b'#SW_DATE#')
    allowed={i for site in PATCH_SITES for i in range(site-BASE,site-BASE+4)}|set(range(marker,marker+8))
    changes=[i for i,(a,b) in enumerate(zip(before,rom)) if a!=b]
    assert set(changes)<=allowed and rom[marker:marker+8]==b'20201013'
    assert rom[:512]==before[:512]
    assert align(BASE+len(rom))==align(BASE+len(before))==0x080b5000
    assert initial_ram(rom[:len(before)])==initial_ram(before)
    legacy=HudCpu(before,old,STOCK_VERSION)
    patched=HudCpu(rom,blob,VERSION)
    print('Container, original assets, patch bounds: OK',flush=True)

    # The actual stock startup decoder produces exactly the recovered RAM image.
    ram=initial_ram(before)
    patched.uc.mem_write(0x20000000,bytes(len(ram)))
    patched.run(0x08021428,0x080b242c,0x20000000,len(ram))
    assert patched.uc.mem_read(0x20000000,len(ram))==ram
    # Theme switch table: group 5 uses the same pane in all four themes.
    for table in (0x6700,0x6858,0x69b0,0x6b08):
        pairs=[struct.unpack_from('<II',ram,off) for off in range(table,table+0x158,8)]
        assert [p for n,p in pairs if n==5]==[0x20000954]
    assert struct.unpack_from('<HHIhhhh',ram,0xad4)==(3,0,0x3b,116,20,68,68)
    assert struct.unpack_from('<I',ram,0xad4+20)==(0x200008ac,)
    assert struct.unpack_from('<HHH',ram,0x8ac)==(145,145,145)
    print('Native startup decoder and shared navigation layout: OK',flush=True)

    for theme in range(4):
        for icon in range(256):
            result=patched.draw(icon,theme)
            if icon in ICONS:assert result==([LOGICAL_BASE+icon-40],[1]),(theme,icon,result)
            else:assert result==legacy.draw(icon,theme),(theme,icon,result)
    for icon in range(256):
        previous=0 if icon==255 else 255
        result=patched.receive(icon,previous_icon=previous)
        if icon in ICONS:
            ids=[a[3] for address,a in result[0] if address==0x08086E08]
            assert ids==[LOGICAL_BASE+icon-40] and result[1]==1
        else:assert result==legacy.receive(icon,previous_icon=previous)
    # Same-value update, zero distance, changed distance, clear after extended.
    receive_extras=0
    for icon in (0,2,9,19,40,55,67,85,86,255):
        for same_distance in (False,True):
            dist=0 if same_distance else 987
            result=patched.receive(icon,previous_icon=icon,previous_distance=0,distance=dist)
            assert result==legacy.receive(icon,previous_icon=icon,previous_distance=0,distance=dist)
            receive_extras+=1
    assert patched.draw(0)==([],[0])
    # Execute the original byte emitter, verifying a full 32-bit logical ID.
    for icon in ICONS:
        patched.native_emitter=True
        patched.draw(icon)
        size,=struct.unpack('<I',patched.uc.mem_read(GROUP+0xf94,4))
        assert size==17
        # The native queue length excludes the trailing zero-command sentinel.
        packet=bytes(patched.uc.mem_read(GROUP+4,size-4+2))
        assert packet==struct.pack('<HBBBIIH',11,1,7,5,0x3b,LOGICAL_BASE+icon-40,0)
    patched.native_emitter=False
    print('All byte-valued icons, cache, clear and native command encoding: OK',flush=True)

    logical_ids={r[0] for r in logical_rows(before)}|set(range(0x152,0x2b84))|{0,0x3fff,0x402e,0xffff}
    for logical in sorted(logical_ids):
        assert patched.descriptor(logical)==legacy.descriptor(logical),hex(logical)
    for entry in manifest:
        logical=entry['logical_resource_id']
        assert patched.descriptor(logical)==struct.pack('<HHHH',logical,logical,logical,logical)
        pixels,calls=patched.set_picture(logical)
        assert pixels==(logical,)*3
        assert [(address,args[:2]) for address,args in calls]==[(0x08061234,(OBJECT,0))]
        assert patched.set_picture(logical,(logical,)*3)==((logical,)*3,[])
        assert patched.set_picture(logical,null_object=True)==((0,0,0),[])
        assert patched.set_picture(logical,null_style=True)==((0,0,0),[])
        for partial in ((logical,0,0),(logical,logical,0),(0,logical,logical)):
            assert patched.set_picture(logical,partial)[0]==(logical,)*3
    for logical in {r[0] for r in logical_rows(before)}|{0x152,0x2861,0x2862,0x2b83,0,0xffff}:
        assert patched.set_picture(logical)==legacy.set_picture(logical),hex(logical)
    catalog_ids={n for n,_ in catalog_rows(before)}
    for resource in sorted(catalog_ids|{0x19a,0x19b,0x28aa,0x28ab,0x28ac,0x290f,0x2910,0x2973,
                                       0x2974,0x29d7,0x29d8,0x2a3b,0x2a3c,0x2a9f,0x2aa0,0x2b03,
                                       0x2b04,0x2b67,0x2b68,0x2bcb,0x2bcd,0x2c30,0x2c31,0,0xffff}):
        assert patched.name(resource)==legacy.name(resource),hex(resource)
    print('Legacy logical IDs/catalog and unchanged native GUI setter: OK',flush=True)

    for edge in (b'',b'\xff',b'\x80\x7f',b'\xff\x01',bytes(range(256))):
        patched.uc.mem_write(SCRATCH,edge or b'\0')
        assert patched.run(0x08050dfc,SCRATCH,len(edge))==checksum(edge)
    for r in records:
        head=blob[r.offset:r.payload_start]
        patched.uc.mem_write(SCRATCH,head)
        assert patched.run(0x08050dfc,SCRATCH,r.search_start-r.offset)==blob[r.search_start]
        assert patched.run(0x0806cf12,SCRATCH)==r.search_start-r.offset+1
        assert patched.run(0x0806ced8,SCRATCH)==r.payload_start-r.offset
        assert patched.run(0x0806cf62,SCRATCH)==align(r.length)//SECTOR
    for entry in manifest:
        resource=entry['resource']
        logical=entry['logical_resource_id']
        ok,name=patched.name(logical,len(resource)+1)
        assert ok==1 and name.decode()==resource
        for capacity in (0,1,len(resource)):
            patched.uc.mem_write(SCRATCH,b'\xa5'*260)
            assert patched.run(0x080653b8,logical,SCRATCH,capacity)==0
            assert patched.uc.mem_read(SCRATCH+capacity,260-capacity)==b'\xa5'*(260-capacity)
        group,name=resource.split('>')
        r=children[group,name]
        png=blob[r.payload_start:r.end]
        check_png(png)
        assert sha256(png).hexdigest()==entry['sha256']
        assert patched.locate(resource)==r.offset
        # Exercise both stock consumers used by the picture cache/loader.
        patched.uc.mem_write(SCRATCH+512,b'\xa5'*36)
        assert patched.run(0x08082410,logical,SCRATCH+512)==1
        assert patched.uc.mem_read(SCRATCH+512,32)==md5(png).hexdigest().encode('ascii')
        patched.uc.mem_write(SCRATCH,resource.encode()+b'\0')
        patched.uc.mem_write(OBJECT,bytes(0x60))
        assert patched.run(0x08060ad0,OBJECT,SCRATCH)==1
        assert struct.unpack('<II',patched.uc.mem_read(OBJECT+0x28,8))==(SECTOR+r.payload_start,len(png))
        assert patched.uc.mem_read(OBJECT+0x30,32)==md5(png).hexdigest().encode('ascii')
    for (group,name),r in old_children.items():
        assert patched.locate(group+'>'+name)==children[group,name].offset
    print('Native headers, bounded names and all old/new resource lookups: OK',flush=True)

    # Native chunked flash MD5 verification of the whole set of 17 top records.
    # Every original and appended inner record is independently MD5 checked by
    # index_records; additionally execute the MCU MD5 path on all 46 additions.
    native_md5_cases=0
    for r in [*tops.values(),*(children['naviInfo.pak',f'turnArrow_{n}.png'] for n in ICONS)]:
        assert patched.md5_record(r)==1,r.name
        native_md5_cases+=1
    sample=children['naviInfo.pak','turnArrow_40.png']
    at=SECTOR+sample.payload_start+40
    patched.spi[at]^=1
    assert patched.md5_record(sample)==0
    patched.spi[at]^=1
    print('Native MCU MD5 accepts all top records/new images and rejects tampering: OK',flush=True)

    return {'model':'HUDBAFB0','version':VERSION,'previous_package_version':STOCK_VERSION,
            'source_sha256':SOURCE_SHA,'source_bytes':len(old),'bytes':len(blob),
            'md5':md5(blob).hexdigest(),'sha256':sha256(blob).hexdigest(),
            'system_original_bytes':len(before),'system_bytes':len(rom),
            'appended_code_bytes':len(rom)-len(before),'system_changed_original_bytes':len(changes),
            'system_end':hex(BASE+len(rom)),'original_and_new_final_4k_boundary':hex(align(BASE+len(rom))),
            'patch_sites':[hex(n) for n in PATCH_SITES],
            'records':len(records),'unchanged_inner_records':len(old_children),
            'original_resource_groups':len(tops)-2,'added_icons':len(manifest),
            'theme_navigation_shared':True,'icon_size':[68,68],'icon_mode':'RGBA',
            'sector_completion_markers':bitmap_count,'sector_marker_value':'0xfe',
            'native_header_checks':len(records),'native_md5_accept_cases':native_md5_cases,
            'native_md5_tamper_rejection':True,'draw_cases':4*256,'receive_cases':256+receive_extras,
            'native_emitter_cases':len(manifest),'legacy_logical_cases':len(logical_ids),
            'legacy_catalog_cases':len(catalog_ids),'native_resource_reader_cases':len(children),
            'native_image_loader_open_cases':len(manifest),'native_image_cache_md5_cases':len(manifest),
            'native_startup_decoder':True,'new_heap_or_globals':False,
            'physical_hud_tested':False,'bootloader_flash_tested':False,'icons':manifest}
