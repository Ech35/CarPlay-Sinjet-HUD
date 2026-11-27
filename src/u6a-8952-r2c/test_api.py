"""Exercise production ARM CGI and gate logic with the existing Linux fixture."""
from pathlib import Path
import ast,ctypes as C,json,struct,hashlib,re,sys
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/"pylibs"))
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_R7

ROOT=Path(__file__).resolve().parent
source=ROOT.parent/'u5hud-runtime/test_cgi.py'
tree=ast.parse(source.read_text(encoding='utf-8'))
nodes=[]
for n in tree.body:
    if isinstance(n,(ast.Import,ast.ImportFrom,ast.ClassDef,ast.FunctionDef)):nodes.append(n)
    elif isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id in ['ROOT','STATE','STATE_BYTES','CONFIG'] for t in n.targets):nodes.append(n)
scope={'__file__':str(source)};exec(compile(ast.Module(body=nodes,type_ignores=[]),str(source),'exec'),scope)
Base=scope['Linux'];STATE='/tmp/u6diag/state-r2';BLE='/tmp/u6diag/ble-r2'
header=(ROOT/'u6diag.h').read_text(encoding='utf-8')
table=header.split('struct Diag {',1)[1].split('struct EventRecord records',1)[0]
diag_fields=[v.strip() for line in re.findall(r'uint32_t ([^;]+);',table) for v in line.split(',')]
def put_diag(files,field,value):struct.pack_into('<I',files[STATE],4*diag_fields.index(field),value)
BLE_BYTES=69632
class Peer(C.Structure):_fields_=[('address',C.c_ubyte*6),('type',C.c_ubyte),('connectable',C.c_ubyte),('rssi',C.c_int32),('name',C.c_char*64)]
class Event(C.Structure):_fields_=[('seconds',C.c_uint32),('error',C.c_uint32),('stage',C.c_char*32),('detail',C.c_char*80)]
class LinkEvent(C.Structure):
    _fields_=[('seconds',C.c_uint32),('attempt',C.c_uint32),('last_write_age_ms',C.c_uint32),
              ('last_navigation_write_age_ms',C.c_uint32),('milliseconds',C.c_uint16),('kind',C.c_uint16),('values',C.c_int32*3)]
class RequestPayload(C.Union):_fields_=[('peer',Peer),('test',C.c_uint32)]
class Request(C.Structure):
    _anonymous_=('payload',)
    _fields_=[('operation',C.c_uint32),('payload',RequestPayload)]
fields=['phase','busy','connected','request_id','controller_known','controller_index','controller_settings','classic_connections',
        'error','mgmt_status','att_error','write_uuid','write_handle','write_properties','test_count','last_test_seconds',
        'clear_sent','commands_sent','peer_count','event_count','event_next','clear_seen']
relay_fields=['relay_enabled','relay_input_count','relay_bad','relay_gaps','relay_stale','relay_epoch','relay_sequence','relay_route_state','relay_distance',
              'relay_frames','relay_chunks','relay_last_tx','relay_last_command','relay_icon','relay_core_phase','relay_retries','relay_retry_seconds','relay_clear_sent']
class Snapshot(C.Structure):
    _fields_=[(n,C.c_uint32) for n in fields]+[('stage',C.c_char*32),('selected',Peer),('peers',Peer*32),('events',Event*512)]+[(n,C.c_uint32) for n in relay_fields]+[('relay_source',C.c_char*64)]+[(n,C.c_uint32) for n in ['relay_keepalives','relay_keepalive_last_seconds','relay_input_last_seconds','relay_raw_index','relay_selected_index','relay_freshness_expired','relay_raw_type','relay_selected_type']]
    _fields_ += [('link_event_count', C.c_uint32), ('link_event_next', C.c_uint32),
                 ('link_last_attempt', C.c_uint32), ('link_events', LinkEvent * 128), ('last_test', C.c_uint32)]
class Shared(C.Structure):_fields_=[(n,C.c_uint32) for n in ['magic','request_seq','handled_seq','cancel','worker_pid','worker_heartbeat','start_low','start_high','clear_request','snapshot_seq']]+[('request',Request),('snapshot',Snapshot)]
assert C.sizeof(Shared)==68468 and Shared.snapshot.offset==120
def proc_stat(pid,name,ticks=77):return f'{pid} ({name}) S '.encode()+(' '.join(['0']*18+[str(ticks)]+['0']*12)+'\n').encode()
def base_files():
    state=bytearray(4*len(diag_fields)+1024*420)
    for field,value in dict(magic=0x55365232,enabled=1,producer_pid=123,session=1,phase=4,callback_ready=1,auth_complete_session=1).items():struct.pack_into('<I',state,4*diag_fields.index(field),value)
    return {STATE:state,'/tmp/lywcpok':bytearray(),'/proc/123/comm':bytearray(b'lylinkapp\n'),
        '/proc/123/stat':bytearray(proc_stat(123,'lylinkapp')),'/proc/123/exe':bytearray(b'/mnt/customer/app/lylinkapp'),
        '/proc/321/stat':bytearray(proc_stat(321,'u6hud_ble')),'/proc/456/stat':bytearray(proc_stat(456,'u6hud_ble'))}
class Linux(Base):
    def syscall(self,machine,interrupt,unused):
        number=machine.reg_read(UC_ARM_REG_R7)
        if number in [85,332]:
            path=self.string(machine.reg_read(UC_ARM_REG_R0 if number==85 else UC_ARM_REG_R1))
            dest=machine.reg_read(UC_ARM_REG_R1 if number==85 else UC_ARM_REG_R2)
            size=machine.reg_read(UC_ARM_REG_R2 if number==85 else UC_ARM_REG_R3)
            self.syscalls.append((number,[path]));data=self.files.get(path)
            result=-2 if data is None else min(len(data),size)
            if result>0:machine.mem_write(dest,bytes(data[:result]))
            machine.reg_write(UC_ARM_REG_R0,result&0xffffffff);return
        if number in [2,120,175]:
            self.syscalls.append((number,[]));machine.reg_write(UC_ARM_REG_R0,456 if number in [2,120] else 0);return
        if number==37:
            self.syscalls.append((number,[]));machine.reg_write(UC_ARM_REG_R0,0);return
        return super().syscall(machine,interrupt,unused)
def request(files,action='status',body=None,method=None,content='application/json',library=None):
    method=method or ('POST' if body is not None else 'GET')
    data=b'' if body is None else body
    env={'REQUEST_METHOD':method,'QUERY_STRING':'action='+action,'CONTENT_TYPE':content,'CONTENT_LENGTH':str(len(data))}
    m=Linux(files,environment=env,body=data,library=library or ROOT/'u6hud_ble');result=m.run()
    return json.loads(result.split(b'\r\n\r\n',1)[1]),m
def install_ble(files,connected=False,busy=False,worker=True):
    s=Shared();s.magic=0x55354242;s.worker_pid=321 if worker else 0;s.start_low=77;s.worker_heartbeat=180
    s.snapshot.phase=4 if connected else 0;s.snapshot.connected=int(connected);s.snapshot.busy=int(busy)
    s.snapshot.peer_count=1;s.snapshot.peers[0].address[:]=bytes.fromhex('f6e5d4c3b2a1')
    s.snapshot.peers[0].type=2;s.snapshot.peers[0].connectable=1;s.snapshot.peers[0].name=b'Sinjet HUD'
    s.snapshot.selected=s.snapshot.peers[0]
    files[BLE]=bytearray(bytes(s)).ljust(BLE_BYTES,b'\0');return s
cases=[]
files=base_files();result,m=request(files)
assert result['ok'] and result['carplay_ready'] and not result['connected'] and not result['running']
assert BLE not in files and not any(n in [2,120] for n,_ in m.syscalls)
cases.append('readonly_status_does_not_start_worker')
for action,body,method,content,error in [
    ('scan',None,'GET','application/json','post_required'),
    ('scan',b'{}','POST','text/plain','json_body_required'),
    ('scan',b'{"x":1}','POST','application/json','empty_json_required'),
    ('connect',b'{"address":"A1:B2:C3:D4:E5:F6","type":3}','POST','application/json','target_invalid'),
    ('connect',b'{"address":"A1:B2:C3:D4:E5:F6","type":4294967297}','POST','application/json','target_invalid'),
    ('connect',b'{"address":"00:00:00:00:00:00","type":1}','POST','application/json','target_invalid'),
    ('connect',b'{"address":"A1:B2:C3:D4:E5:F6","type":2,"cmd":"x"}','POST','application/json','target_invalid')]:
    result,m=request(base_files(),action,body,method,content);assert result['error']==error,result
    assert not any(n in [2,120] for n,_ in m.syscalls)
cases.append('method_body_mac_type_and_extra_field_validation')
files=base_files();put_diag(files,'phase',0)
result,m=request(files,'scan',b'{}');assert result['error']=='carplay_not_ready'
cases.append('active_carplay_required')
files=base_files();result,m=request(files,'scan',b'{}')
assert result['ok'] and result['queued'] and any(n in [2,120] for n,_ in m.syscalls)
snap=Shared.from_buffer_copy(files[BLE]);assert snap.request.operation==1 and snap.request_seq==1 and snap.worker_pid==456
cases.append('scan_is_queued_to_detached_worker')
files=base_files();install_ble(files,worker=False)
target=b'{"address":"A1:B2:C3:D4:E5:F6","type":2}'
result,m=request(files,'connect',target);assert result['ok']
s=Shared.from_buffer_copy(files[BLE]);assert s.request.operation==2 and bytes(s.request.peer.address)==bytes.fromhex('f6e5d4c3b2a1')
cases.append('connect_only_to_explicit_discovered_target')
files=base_files();install_ble(files,worker=False)
result,m=request(files,'connect',target.replace(b'A1:',b'A2:'));assert result['error']=='target_not_selected'
assert not any(n in [2,120] for n,_ in m.syscalls)
files=base_files();install_ble(files,connected=True,busy=True)
result,m=request(files,'scan',b'{}');assert result['error']=='operation_busy'
result,m=request(files,'disconnect',b'{}');assert result['ok'] and Shared.from_buffer_copy(files[BLE]).cancel==1
cases.append('busy_guard_and_cancel_without_killing_native_bluetooth')
files=base_files();install_ble(files,connected=True)
result,m=request(files,'status');assert result['connected'] and result['running']
result,m=request(files,'scan',b'{}');assert result['error']=='operation_busy'
result,m=request(files,'test',b'{}');assert result['ok'] and Shared.from_buffer_copy(files[BLE]).request.operation==3
assert not any(n in [2,120] for n,_ in m.syscalls)
cases.append('existing_worker_receives_single_test_command')
files=base_files();install_ble(files,connected=True);files['/proc/321/stat']=bytearray(proc_stat(321,'u6hud_ble',88))
result,m=request(files);assert not result['running'] and not result['connected']
cases.append('pid_reuse_not_mistaken_for_existing_worker')
files=base_files();m=Linux(files,library=ROOT/'gate_test');m.run()
cases.append('current_session_auth_required_and_logging_off_keeps_navigation_ready')

relay_config='/mnt/UDISK/u6hud-relay.enabled'
device_config='/mnt/UDISK/u6hud-device.bin'
files=base_files();s=install_ble(files,worker=False)
saved=struct.pack('<I',0x55354238)+bytes(s.snapshot.selected)
files=base_files();files[device_config]=bytearray(saved)
result,m=request(files)
assert result['selected']['name']=='Sinjet HUD' and not result['relay_enabled'] and BLE not in files
cases.append('r8_saved_device_migrates_without_radio_or_process_start')
result,m=request(base_files(),'relay',b'{"enabled":true}')
assert result['error']=='target_not_selected'
for invalid in [b'{}',b'{"enabled":1}',b'{"enabled":true,"x":1}']:
    result,m=request(base_files(),'relay',invalid);assert result['error']=='relay_body_invalid'
cases.append('relay_requires_selected_target_and_strict_boolean_body')
files=base_files();files[device_config]=bytearray(saved);put_diag(files,'phase',0)
result,m=request(files,'relay',b'{"enabled":true}')
assert result['ok'] and files[relay_config]==b'1'
s=Shared.from_buffer_copy(files[BLE]);assert s.request.operation==4 and s.request.peer.name==b'Sinjet HUD'
cases.append('enable_persists_and_queues_before_carplay_is_connected')
files=base_files();install_ble(files,connected=True);files[relay_config]=bytearray(b'1')
result,m=request(files,'test',b'{}');assert result['error']=='relay_active'
result,m=request(files);assert result['relay_enabled']
cases.append('manual_test_excluded_during_navigation_relay')
files=base_files();install_ble(files,connected=True,busy=True);files[relay_config]=bytearray(b'1')
put_diag(files,'phase',0)
result,m=request(files,'relay',b'{"enabled":false}')
assert result['ok'] and files[relay_config]==b'0'
s=Shared.from_buffer_copy(files[BLE]);assert s.cancel==1 and s.request.operation==5
cases.append('disable_interrupts_busy_operation_without_carplay')
files=base_files();install_ble(files,connected=True);files[relay_config]=bytearray(b'1')
result,m=request(files,'disconnect',b'{}')
assert result['ok'] and files[relay_config]==b'0' and Shared.from_buffer_copy(files[BLE]).cancel==1
cases.append('manual_disconnect_disables_persistent_auto_reconnect')
files=base_files();s=install_ble(files,connected=True)
s.snapshot.relay_keepalives=12;s.snapshot.relay_freshness_expired=1
files[BLE]=bytearray(bytes(s)).ljust(BLE_BYTES,b'\0')
result,m=request(files)
assert result['version']=='U6A_R2c' and result['relay_keepalives']==12 and result['relay_freshness_expired'] is True
cases.append('r10_refresh_and_freshness_state_export')
icon_config='/mnt/UDISK/u6hud-icons.bin'
files=base_files();files[device_config]=bytearray(saved)
result,m=request(files)
assert result['icon_target_ready'] and not result['expanded_icons'] and icon_config not in files
result,m=request(files,'icons',b'{"enabled":true}')
assert result['ok'] and result['expanded_icons'] and files[icon_config]==bytes.fromhex('02f6e5d4c3b2a1')
assert not any(n in [2,120] for n,_ in m.syscalls)
assert request(files)[0]['expanded_icons']
cases.append('icons_default_legacy_and_opt_in_persists_without_starting_worker')

other=bytearray(saved);other[4]=0x01
files[device_config]=other
assert not request(files)[0]['expanded_icons']
files[device_config]=bytearray(saved)
files[icon_config].append(0)
assert not request(files)[0]['expanded_icons']
files[icon_config].pop()
cases.append('icons_address_bound_and_malformed_config_fails_closed')

install_ble(files,connected=True)
files[relay_config]=bytearray(b'1')
before=bytes(files[BLE])
assert request(files,'icons',b'{"enabled":0}')[0]['error']=='icons_body_invalid'
result,m=request(files,'icons',b'{"enabled":false}')
assert result['ok'] and not result['expanded_icons'] and not request(files)[0]['expanded_icons']
result,m=request(files,'icons',b'{"enabled":true}')
assert result['ok'] and result['expanded_icons']
assert files[relay_config]==b'1' and files[BLE]==before and not any(n in [2,120] for n,_ in m.syscalls)
assert request(base_files(),'icons',b'{"enabled":true}')[0]['error']=='target_not_selected'
cases.append('icon_toggle_persists_during_relay_without_queue_or_disconnect')

files=base_files();install_ble(files,connected=True,busy=True);files[device_config]=bytearray(saved)
assert request(files,'icons',b'{"enabled":true}')[0]['error']=='operation_busy' and icon_config not in files
cases.append('icon_toggle_while_manual_operation_busy_is_rejected')

files=base_files();install_ble(files,connected=True)
assert request(files,'test',b'{"icon":40}')[0]['error']=='test_icons_disabled'
for malformed in [b'{"icon":39}',b'{"icon":86}',b'{"icon":4000000000}',b'{"icon":40,"lanes":true}',b'{"lanes":false}',b'{"lanes":6}']:
    assert request(files,'test',malformed)[0]['error']=='test_body_invalid', malformed
assert Shared.from_buffer_copy(files[BLE]).request_seq==0
cases.append('test_validates_presets_and_requires_expanded_mode_for_new_ids')

files[relay_config]=bytearray(b'1')
assert request(files,'test',b'{"lanes":true}')[0]['error']=='relay_active'
files[relay_config]=bytearray(b'0')
result,m=request(files,'test',b'{"lanes":true}')
queued=Shared.from_buffer_copy(files[BLE])
assert result['ok'] and queued.request.operation==3 and queued.request.test==1 and not queued.cancel
cases.append('six_lane_test_uses_saved_link_only_when_relay_is_off')

for icon in range(40,86):
    files=base_files();install_ble(files,connected=True)
    files[icon_config]=bytearray(bytes.fromhex('02f6e5d4c3b2a1'))
    result,m=request(files,'test',f'{{"icon":{icon}}}'.encode())
    queued=Shared.from_buffer_copy(files[BLE])
    assert result['ok'] and queued.request.operation==3 and queued.request.test==icon and not queued.cancel
cases.append('all_46_expanded_test_ids_are_queued_verbatim')

# Reproduce the extra R2 comm-name gate with the exact previously released ELF.
files=base_files();put_diag(files,'phase',3)
files['/proc/123/comm']=bytearray(b'CPClientThread\n')
files['/proc/123/stat']=bytearray(proc_stat(123,'CPClientThread'))
result,m=request(files)
assert result['carplay_ready'] and result['gate_reason']=='ready' and result['gate_comm']=='CPClientThread'
assert result['gate_executable']=='/mnt/customer/app/lylinkapp' and result['gate_iap_phase']==3
assert not result['relay_input_count'] and not any(n in [2,120] for n,_ in m.syscalls)
assert request(files,'scan',b'{}')[0]['ok']
cases.append('current_gate_accepts_renamed_task_by_live_executable_before_navigation')

files=base_files();files['/proc/123/exe']=bytearray(b'/bin/unrelated')
result,_=request(files)
assert not result['carplay_ready'] and result['gate_reason']=='process_exe_mismatch'
assert request(files,'scan',b'{}')[0]['error']=='carplay_not_ready'
cases.append('matching_task_name_cannot_authorize_an_unrelated_executable')

files=base_files();files.pop('/proc/123/exe')
result,_=request(files)
assert not result['carplay_ready'] and result['gate_reason']=='process_exe_unavailable' and result['gate_errno']==2
files=base_files();files.pop('/proc/123/stat')
assert request(files)[0]['gate_reason']=='process_not_live'
files=base_files();files['/proc/123/stat']=bytearray(proc_stat(123,'lylinkapp').replace(b') S ',b') Z '))
assert not request(files)[0]['carplay_ready']
cases.append('missing_executable_missing_process_and_zombie_fail_closed_with_visible_reasons')

files=base_files();files.pop('/proc/123/comm')
assert request(files)[0]['carplay_ready']
put_diag(files,'enabled',0)
assert request(files)[0]['carplay_ready']
cases.append('mutable_task_label_and_logging_switch_do_not_control_executable_identity')

for field,value,reason in [('session',0,'waiting_session'),('producer_pid',0,'waiting_producer'),
                         ('phase',2,'waiting_subscription'),('auth_complete_session',0,'waiting_auth'),
                         ('callback_ready',0,'waiting_callback')]:
    files=base_files();put_diag(files,field,value);result,_=request(files)
    assert not result['carplay_ready'] and result['gate_reason']==reason,(field,result)
files=base_files();files.pop('/tmp/lywcpok')
assert request(files)[0]['gate_reason']=='waiting_carplay_marker'
cases.append('authentication_subscription_callback_and_carplay_marker_remain_required')

files=base_files();files[STATE].pop()
assert request(files)[0]['gate_reason']=='state_size_invalid'
files=base_files();put_diag(files,'magic',0)
assert request(files)[0]['gate_reason']=='state_magic_invalid'
cases.append('invalid_native_state_is_reported_without_enabling_scan')
files=base_files();install_ble(files)
files[device_config]=bytearray(saved);files[relay_config]=bytearray(b'1')
result,m=request(files)
assert result['relay_enabled'] and result['saved_hud_available'] and not result['worker_relay_enabled']
assert not any(n in [2,120,281] for n,_ in m.syscalls)
running=Shared.from_buffer_copy(files[BLE]);running.snapshot.relay_enabled=1
files[BLE]=bytearray(bytes(running)).ljust(BLE_BYTES,b'\0')
result,_=request(files)
assert result['worker_relay_enabled'] and result['relay_enabled']
cases.append('saved_preferences_and_actual_worker_state_are_reported_separately')
(ROOT/'api-tests.json').write_text(json.dumps({'cases':cases,'production_arm_cgi':True,'physical_box_tested':False,
    'production_binary_sha256':hashlib.sha256((ROOT/'u6hud_ble').read_bytes()).hexdigest()},indent=2),encoding='utf-8')
print(len(cases),'production ARM CGI/gate cases passed')
