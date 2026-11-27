"""Native tests for navigation selection and preferred-lane rendering."""
from pathlib import Path
import ctypes as C,json,struct
import hashlib
import re
import sys
ROOT=Path(__file__).resolve().parent
d=C.CDLL(str(ROOT/'adapter-test.dll'))
P,U,T,S=C.c_void_p,C.c_uint,C.c_uint64,C.c_size_t
def api(name,args,ret=U):
    f=getattr(d,name);f.argtypes=args;f.restype=ret;return f
create=api('u5_core_create',[C.c_uint16],P);destroy=api('hc_destroy',[P],None)
set_icons=api('u5_core_set_expanded_icons',[P,C.c_int],None)
feed=api('u5_core_input',[P,P,S,T,T]);found=api('hc_found',[P,U,T]);connected=api('hc_connected',[P,U,T]);ready=api('hc_ready',[P,U,T])
generation=api('hc_generation',[P]);chunk=api('hc_next_chunk',[P,T,P,S],S);written=api('hc_written',[P,U,C.c_int,T])
refresh=api('u5_core_refresh',[P],C.c_int);end=api('u5_core_session_end',[P]);reset=api('hc_transport_reset',[P,C.c_int],None)
class Selection(C.Structure):_fields_=[(name,U) for name in ['raw','selected','raw_type','selected_type']]
choice=api('u5_core_selection',[P,C.POINTER(Selection)],None)
def tlv(tag,b):return struct.pack('>HH',4+len(b),tag)+b
def message(mid,params):return b'@@'+struct.pack('>HH',len(params)+6,mid)+params
def maneuver(index,kind=20,between=0,angle=None,description=None,road=None,side=None):
    body=tlv(0,b'\x7f\1')+tlv(1,struct.pack('>H',index))
    if kind is not None:body+=tlv(3,bytes([kind]))
    if between is not None:body+=tlv(5,struct.pack('>I',between))
    if angle is not None:body+=tlv(11,struct.pack('>h',angle))
    if side is not None:body+=tlv(8,bytes([side]))
    if description is not None:body+=tlv(2,description.encode()+b'\0')
    if road is not None:body+=tlv(4,road.encode()+b'\0')
    return message(0x5202,body)
def route(indices=(0,1),distance=19,state=6,current_road=None):
    body=tlv(0,b'\x7f\1')
    if state is not None:body+=tlv(1,bytes([state]))
    if current_road is not None:body+=tlv(3,current_road.encode()+b'\0')
    if indices is not None:body+=tlv(13,b''.join(struct.pack('>H',i) for i in indices))
    if distance is not None:body+=tlv(10,struct.pack('>I',distance))
    return message(0x5201,body)
class Rig:
    def __init__(self,first=11,angle=None,next_type=20,between=0,indices=(0,1),state=6,description='前往这条路线',road=None,side=None,expanded=False):
        self.c=create(0x7f01);assert self.c;self.now=1000
        set_icons(self.c,expanded)
        self.put(maneuver(0,first,0,angle,description,road,side));self.put(maneuver(1,next_type,between));self.put(route(indices,state=state))
        g=generation(self.c);assert found(self.c,g,self.now)==6 and connected(self.c,g,self.now)==8;ready(self.c,g,self.now);refresh(self.c)
    def put(self,p):feed(self.c,p,len(p),self.now,self.now);self.now+=1
    def selected(self):s=Selection();choice(self.c,C.byref(s));return s
    def drain(self):
        pending=bytearray();frames=[];out=C.create_string_buffer(20)
        for _ in range(300):
            n=chunk(self.c,self.now,out,20)
            if not n:break
            pending.extend(out.raw[:n]);written(self.c,generation(self.c),1,self.now);self.now+=16
            while len(pending)>=3 and len(pending)>=pending[2]+4:
                size=pending[2]+4;f=bytes(pending[:size]);del pending[:size]
                assert f[:2]==b'\xff\x55' and f[-1]==sum(f[2:-1])&255;frames.append(f)
        assert not pending;return frames
    def close(self):destroy(self.c)
def turns(frames):return [(f[5],int.from_bytes(f[6:10],'little')) for f in frames if f[4]==0]
def text_lines(frames):
    return [f[5:-1].rstrip(b'\0').decode('utf-8') for f in frames if f[4]==5]

cases=[]
scenarios=[
    ('zero_distance_following_left_stays_future',{},0,9),('following_right_stays_future',{'next_type':2},0,9),
    ('route_set_keeps_advertised_current',{'state':1},0,9),('missing_distance_does_not_promote',{'between':None},0,9),
    ('future_distance_does_not_promote',{'between':70},0,9),('unadvertised_future_not_promoted',{'indices':(0,)},0,9),
    ('real_first_left_preserved',{'first':1,'next_type':2},0,2),('explicit_heading_preserved',{'angle':90},0,3),
    ('explicit_zero_heading_preserved',{'angle':0},0,9),('sentinel_heading_keeps_start_fallback',{'angle':1000},0,9),
    ('missing_future_type',{'next_type':None},0,9),('unknown_future_type_not_promoted',{'next_type':200},0,9),
    ('missing_prompt_leaves_road_empty',{'description':None},0,9),
    ('next_road_has_prefix',{'description':None,'road':'路线入口'},0,9),
    ('description_has_priority_over_next_road',{'road':'路线入口'},0,9),
    ('left_only_after_becoming_current',{'indices':(1,)},1,2)]
for name,kwargs,index,icon in scenarios:
    r=Rig(**kwargs)
    try:
        assert r.selected().selected==index,(name,r.selected().selected)
        frames=r.drain();assert turns(frames)==[(icon,19)],name
        texts=text_lines(frames)
        expected=''
        if index==0:
            expected=kwargs.get('description','前往这条路线') or ''
            if not expected and kwargs.get('road'):
                expected='前方：'+kwargs['road']
        assert texts==[expected],(name,texts,expected)
        cases.append(name)
    finally:r.close()
r=Rig()
try:
    assert turns(r.drain())==[(9,19)]
    r.put(route(None,18,None));assert turns(r.drain())==[(9,18)]
    cases.append('incremental_distance_keeps_current_start')
    r.put(maneuver(2,2,70));r.put(route((2,),None,None))
    assert not turns(r.drain()),'distance leaked across raw maneuver change'
    r.put(route(None,55,None));assert turns(r.drain())==[(3,55)]
    cases.append('new_raw_selection_waits_for_its_own_distance')
    assert refresh(r.c)==1;assert turns(r.drain())==[(3,55)]
    cases.append('valid_current_state_can_refresh_without_new_input')
    end(r.c);assert refresh(r.c)==0;r.drain();r.put(maneuver(0,1,0));r.put(route(None,30,1))
    assert not turns(r.drain()) and r.selected().selected==65535
    cases.append('new_session_never_inherits_old_advertised_selection')
finally:r.close()
r=Rig()
try:
    r.drain();r.put(maneuver(0,1,None));frames=r.drain()
    assert turns(frames)==[(2,19)] and all('前往'.encode() not in f for f in frames)
    cases.append('start_prompt_never_leaks_into_real_turn')
finally:r.close()
r=Rig()
try:
    r.drain();reset(r.c,0);assert refresh(r.c)==0;cases.append('no_refresh_after_transport_reset')
finally:r.close()

rig = Rig(first=1, indices=(0,), state=1, description='左转', road='人民大道')
try:
    rig.put(route(None, None, None, current_road='中山路'))
    frames = rig.drain()
    assert text_lines(frames) == ['左转'] and turns(frames) == [(2,19)]
    rig.put(maneuver(0, None, None, description='  '))
    assert text_lines(rig.drain()) == ['前方：人民大道']
    rig.put(maneuver(0, None, None, road=''))
    assert text_lines(rig.drain()) == ['当前：中山路']
    rig.put(route(None, None, None, current_road='更新路'))
    assert text_lines(rig.drain()) == ['当前：更新路']
    rig.put(route(None, None, None, current_road=''))
    assert text_lines(rig.drain()) == ['']
    cases.append('description_next_current_priority_and_explicit_empty_deltas')

    rig.put(maneuver(0, None, None, description='进入 古港路'))
    rig.put(maneuver(1, 2, description='右转'))
    assert text_lines(rig.drain()) == ['进入 古港路']
    rig.put(route((1,), None, None))
    assert text_lines(rig.drain()) == []
    rig.put(route(None, 30, None))
    assert text_lines(rig.drain()) == ['右转']
    rig.put(maneuver(1, 1, None))
    assert text_lines(rig.drain()) == ['']
    cases.append('text_follows_selected_action_and_clears_reused_index')

    rig.put(route(None, None, None, current_road='长' * 60))
    text, = text_lines(rig.drain())
    assert text.startswith('当前：') and 117 < len(text.encode()) <= 120
    end(rig.c)
    rig.drain()
    rig.put(maneuver(1, 1))
    rig.put(route((1,), 25, 1))
    assert text_lines(rig.drain()) == ['']
    cases.append('utf8_boundary_and_current_road_session_reset')
finally:
    rig.close()

lane_pair = api('hc_lane_pair', [P, S, C.c_int, C.c_int, P], C.c_int)
pair = (C.c_uint8 * 2)()
assert lane_pair(None, 0, 0, 1, pair) and list(pair) == [0, 15]
assert lane_pair(None, 0, 90, 2, pair) and list(pair) == [3, 3]
cases.append('good_lane_keeps_arrow_when_only_primary_direction_is_present')

def lane_report(lanes):
    groups = []
    for index, status, angle in lanes:
        angle_bytes = struct.pack('>h', angle)
        groups.append(tlv(2, tlv(0, struct.pack('>H', index)) + tlv(1, bytes([status]))
                          + tlv(2, angle_bytes) + tlv(3, angle_bytes)))
    return message(0x5204, tlv(0, b'\x7f\1') + tlv(1, b'\0\0') + b''.join(groups))

def verify_lane_frames(frames, pairs):
    detail = bytes(value for pair in pairs for value in pair)
    simple = bytes([len(pairs), *(int(highlight != 15) for _, highlight in pairs)])
    actual = {f[4]: f[5:-1] for f in frames if f[4] in (0x11, 0x12, 0x15)}
    expected = {0x11: simple, 0x12: detail, 0x15: detail}
    assert actual == expected, (actual, expected)

rig = Rig(first=1, indices=(0,), state=1)
try:
    rig.drain()
    rig.put(message(0x5201, tlv(0, b'\x7f\1') + tlv(16, b'\0\0') + tlv(18, b'\1')))
    rig.drain()
    rig.put(lane_report([(3, 2, 90), (1, 0, -90), (2, 1, 0)]))
    verify_lane_frames(rig.drain(), [(1, 15), (0, 15), (3, 3)])
    cases.append('unordered_lane_positions_render_in_ascending_order')

    seven = [(0, 1, -90)] + [(i, 1, 0) for i in range(1, 6)] + [(6, 2, 90)]
    rig.put(lane_report(reversed(seven)))
    verify_lane_frames(rig.drain(), [(0, 15)] * 5 + [(3, 3)])
    cases.append('seven_lanes_keep_the_rightmost_preferred_lane')

    eight = [(0, 0, 0), (1, 2, -90)] + [(i, 1, 0) for i in range(2, 6)] + [(6, 2, 90), (7, 0, 0)]
    rig.put(lane_report(eight))
    verify_lane_frames(rig.drain(), [(1, 1)] + [(0, 15)] * 4 + [(3, 3)])
    cases.append('eight_lanes_keep_both_preferred_lanes_in_a_contiguous_window')

    rig.put(lane_report([(0, 0, -90)] + [(i, 0, 0) for i in range(1, 7)] + [(7, 1, 90)]))
    verify_lane_frames(rig.drain(), [(0, 15)] * 5 + [(3, 15)])
    rig.put(lane_report([(0, 0, -90)] + [(i, 0, 0) for i in range(1, 8)]))
    verify_lane_frames(rig.drain(), [(1, 15)] + [(0, 15)] * 5)
    cases.append('lane_window_ties_prefer_usable_lanes_then_the_lowest_index')

    for invalid in ([(0, 1, 0), (0, 2, 90)], [(0, 1, 0), (2, 2, 90)], [(i, 1, 0) for i in range(9)]):
        rig.put(lane_report(seven))
        verify_lane_frames(rig.drain(), [(0, 15)] * 5 + [(3, 3)])
        rig.put(lane_report(invalid))
        frames = rig.drain()
        verify_lane_frames(frames, [])
        assert not any(f[4:6] == b'\xe0\0' for f in frames)
    assert refresh(rig.c) == 1 and turns(rig.drain()) == [(2, 19)]
    cases.append('ambiguous_or_incomplete_lane_groups_clear_without_ending_navigation')
finally:
    rig.close()

# Send real iAP2 TLVs through the adapter and verify actual one-byte BLE IDs.
ordinary = [
    9,2,3,9,8,40,11,12,9,9,15,49,15,45,46,56,57,58,8,65,47,48,41,42,50,51,8,15,
    *range(67,86),6,7,4,5,52,53,54,
]
angle_cases = [
    (4,90,0,55), (4,-90,1,8), (4,None,1,55), (4,0,1,55), (4,1000,255,8),
    (18,90,0,55), (26,-90,0,8), (26,None,1,55),
    (6,None,1,61), (6,None,255,11),
    (7,None,1,12), (7,1000,1,12), (7,-158,0,65), (7,158,1,66),
    (7,-157,0,59), (7,157,0,60), (7,-23,1,62), (7,23,1,64),
    (7,-22,1,63), (7,22,1,63), (7,0,0,12), (19,None,1,66),
    (8,-45,0,41), (8,45,0,42), (8,0,0,9), (8,1000,0,9),
    (9,45,0,43), (9,-45,0,44), (9,0,0,9), (9,1000,1,9),
    (28,-160,1,67), (46,160,1,85),
    (51,-23,0,53), (51,23,0,54), (51,22,0,52), (51,-22,0,52), (51,-1000,0,52),
    (54,None,0,16), (254,None,0,16), (255,None,0,16),
]
assert len(ordinary)==54
expanded_cases=[(kind,None,None,icon) for kind,icon in enumerate(ordinary)]+angle_cases
used=set()
for kind,angle,side,icon in expanded_cases:
    rig=Rig(first=kind,angle=angle,side=side,expanded=True)
    try:
        assert turns(rig.drain())==[(icon,19)], (kind,angle,side,icon)
        used.add(icon)
    finally:
        rig.close()
assert set(range(40,86)) <= used
cases.append('expanded_all_actions_all_46_icons_and_optional_angle_side')

legacy=api('hc_turn_icon_angle',[U,C.c_int,C.c_int],C.c_int)
for kind,angle,side,_ in expanded_cases:
    rig=Rig(first=kind,angle=angle,side=side)
    try:
        expected=legacy(kind,angle or 0,angle is not None)
        assert 2<=expected<=19 and turns(rig.drain())==[(expected,19)], (kind,angle,side,expected)
    finally:
        rig.close()
cases.append('legacy_mode_preserves_r11_action_bytes')

rig=Rig(first=6,expanded=True)
try:
    assert turns(rig.drain())==[(11,19)]
    rig.put(maneuver(0,None,None,side=1))
    assert turns(rig.drain())==[(61,19)]
    rig.put(maneuver(0,None,None,side=0))
    assert turns(rig.drain())==[(11,19)]
    rig.put(route(None,None,2))
    assert any(frame[4:6]==b'\xe0\x00' for frame in rig.drain())
finally:
    rig.close()
cases.append('late_traffic_side_redraws_and_route_end_clears')

print(len(cases), 'synthetic adapter cases passed; radio_accessed=False')
