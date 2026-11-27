"""Run the released ARM worker with configuration becoming visible after boot."""
from pathlib import Path
import ast,json,struct,sys

HERE=Path(__file__).resolve().parent
p=HERE/'test_api.py'
tree=ast.parse(p.read_text(encoding='utf-8'));body=[]
for node in tree.body:
    if isinstance(node,ast.Assign) and any(isinstance(v,ast.Name) and v.id=='cases' for v in node.targets):break
    body.append(node)
scope={'__file__':str(p)}
exec(compile(ast.Module(body=body,type_ignores=[]),str(p),'exec'),scope)
sys.path.insert(0,str(HERE.parent/'pylibs'))
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R7

class BootLinux(scope['Linux']):
    def __init__(self,files,pending,config_at=2000,manual_off_at=None,**kwargs):
        self.elapsed=0;self.pending=pending;self.mounted=False;self.authenticated=False;self.sockets=0
        self.config_at=config_at;self.manual_off_at=manual_off_at;self.manual_off_sent=False
        if config_at<=0:files.update({k:bytearray(v) for k,v in pending.items()});self.mounted=True
        super().__init__(files,args=('--worker',),environment={},**kwargs)

    def update(self):
        if self.elapsed>=self.config_at and not self.mounted:
            self.files.update({k:bytearray(v) for k,v in self.pending.items()});self.mounted=True
        if self.manual_off_at is not None and self.elapsed>=self.manual_off_at and not self.manual_off_sent:
            for address,mapping in self.mappings.items():
                if mapping['path']==scope['BLE']:self.sync_mapping(address)
            state=scope['Shared'].from_buffer_copy(self.files[scope['BLE']]);state.request.operation=5
            state.request_seq+=1;state.cancel=1
            self.files[scope['BLE']][:]=bytes(state).ljust(scope['BLE_BYTES'],b'\0')
            self.files['/mnt/UDISK/u6hud-relay.enabled']=bytearray(b'0')
            for address,mapping in self.mappings.items():
                if mapping['path']==scope['BLE']:self.machine.mem_write(address,bytes(self.files[scope['BLE']]))
            self.manual_off_sent=True
        if self.elapsed>=3000 and not self.authenticated:
            scope['put_diag'](self.files,'phase',3)
            for address,mapping in self.mappings.items():
                if mapping['path']=='/tmp/u6diag/nav-r2':self.sync_mapping(address)
            nav=self.files.get('/tmp/u6diag/nav-r2')
            if nav is not None:struct.pack_into('<7I',nav,8,2,1,123,1,0,0,0)
            for address,mapping in self.mappings.items():
                if mapping['path'] in [scope['STATE'],'/tmp/u6diag/nav-r2']:
                    self.machine.mem_write(address,bytes(self.files[mapping['path']]))
            self.authenticated=True

    def syscall(self,machine,interrupt,unused):
        number=machine.reg_read(UC_ARM_REG_R7)
        if number==263:
            self.syscalls.append((number,[]));stamp=180000+self.elapsed
            machine.mem_write(machine.reg_read(UC_ARM_REG_R1),struct.pack('<ii',stamp//1000,(stamp%1000)*1000000))
            machine.reg_write(UC_ARM_REG_R0,0);return
        if number in [162,265,407]:
            self.syscalls.append((number,[]))
            if number==407:machine.reg_write(UC_ARM_REG_R0,(-38)&0xffffffff);return
            pointer=machine.reg_read(UC_ARM_REG_R0 if number==162 else UC_ARM_REG_R2)
            seconds,nanoseconds=struct.unpack('<ii',machine.mem_read(pointer,8))
            self.elapsed+=max(1,seconds*1000+nanoseconds//1000000)
            self.update();machine.reg_write(UC_ARM_REG_R0,0)
            if self.elapsed>=6000:machine.emu_stop()
            return
        if number==174:
            self.syscalls.append((number,[]));machine.reg_write(UC_ARM_REG_R0,0);return
        if number==281:
            self.syscalls.append((number,[]));self.sockets+=1
            machine.reg_write(UC_ARM_REG_R0,(-19)&0xffffffff);return
        return super().syscall(machine,interrupt,unused)

def run_case(name,library,config_at=2000,enabled=True,logging=True,manual_off_at=None,no_saved=False):
    files=scope['base_files']();scope['put_diag'](files,'phase',0)
    scope['put_diag'](files,'enabled',int(logging))
    peer=scope['Peer']();peer.address[:]=bytes.fromhex('f6e5d4c3b2a1');peer.type=2;peer.connectable=1;peer.name=b'Sinjet HUD'
    pending={} if no_saved else {'/mnt/UDISK/u6hud-device.bin':struct.pack('<I',0x55354238)+bytes(peer),'/mnt/UDISK/u6hud-relay.enabled':b'1' if enabled else b'0'}
    m=BootLinux(files,pending,config_at=config_at,manual_off_at=manual_off_at,library=library);m.run(expected_exit=None)
    assert m.elapsed>=6000 and m.authenticated and m.mounted
    snap=scope['Shared'].from_buffer_copy(files[scope['BLE']]).snapshot
    result={'case':name,'elapsed_ms':m.elapsed,'saved_device_visible':'/mnt/UDISK/u6hud-device.bin' in files,
            'saved_relay_enabled':files.get('/mnt/UDISK/u6hud-relay.enabled')==b'1',
            'worker_selected_type':snap.selected.type,'worker_relay_enabled':snap.relay_enabled,'radio_attempts':m.sockets,'nav_epoch':snap.relay_epoch}
    assert snap.relay_epoch==1
    return result

def run():
    current=HERE/'u6hud_ble';old=HERE.parent/'u6a-8952-r2a/u6hud_ble'
    cases=[]
    for name,params in [('early_configuration',{'config_at':0}),('late_configuration',{}),
                        ('configuration_after_carplay_ready',{'config_at':4500}),('logging_disabled',{'logging':False})]:
        result=run_case(name,current,**params)
        assert result['worker_selected_type']==2 and result['worker_relay_enabled'] and result['radio_attempts']>0,result
        cases.append(result)
    for name,params in [('saved_auto_relay_off',{'enabled':False}),('explicit_user_off_before_carplay',{'manual_off_at':2500}),
                        ('first_boot_without_saved_target',{'no_saved':True})]:
        result=run_case(name,current,**params)
        assert not result['worker_relay_enabled'] and not result['radio_attempts'],result
        cases.append(result)
    report={'cases':cases,'actual_production_arm_worker':True,'physical_radio_used':False,
            'connection_attempts_simulated_without_http_requests':True}
    (HERE/'boot-tests.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,indent=2))
    print(len(cases),'current-worker boot cases passed: late configuration restored, manual off respected, logging independent')

if __name__=='__main__':run()
