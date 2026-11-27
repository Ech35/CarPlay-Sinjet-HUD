"""Generic in-memory Linux syscall runner; no captured navigation data."""
from io import BytesIO
from pathlib import Path
import json
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_INTR
from unicorn.arm_const import *
ROOT = Path(__file__).resolve().parent
STATE = '/tmp/u5hud/state-v6'
STATE_BYTES = 1048576
CONFIG = '/mnt/UDISK/u5hud-log.enabled'

class Linux:

    def __init__(self, files, args=(), environment=None, body=b'', statx_errno=22, library=None):
        self.files = files
        self.machine = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.machine.reg_write(UC_ARM_REG_FPEXC, 1073741824)
        self.elf = ELFFile(BytesIO((library or ROOT / 'u5hud.cgi').read_bytes()))
        self.machine.mem_map(65536, 2097152)
        for segment in self.elf.iter_segments():
            if segment['p_type'] == 'PT_LOAD':
                self.machine.mem_write(segment['p_vaddr'], segment.data())
        self.machine.mem_map(8388608, 4194304)
        self.machine.mem_map(2130706432, 1048576)
        self.next_map = 67108864
        self.descriptors = {0: {'data': body, 'position': 0}, 1: {}, 2: {}}
        self.mappings = {}
        self.output = bytearray()
        self.errors = bytearray()
        self.exited = None
        self.brk = 8388608
        self.storage_type = 16914836
        self.mount_errno = 0
        self.reject_tmpfs = False
        self.mounts = []
        self.statx_errno = statx_errno
        self.syscalls = []
        self.seek_errno = 0
        self.state_write_limit = None
        strings = 2131230720

        def put_string(value):
            nonlocal strings
            address = strings
            data = value.encode() + b'\x00'
            self.machine.mem_write(address, data)
            strings += len(data)
            return address
        argv = [put_string('/cgi-bin/u5hud.cgi'), *(put_string(arg) for arg in args)]
        env = [put_string(key + '=' + value) for key, value in (environment or {}).items()]
        random = put_string('0123456789abcdef')
        aux = [3, 65536 + self.elf['e_phoff'], 4, self.elf['e_phentsize'], 5, self.elf['e_phnum'], 6, 4096, 9, self.elf['e_entry'], 11, 0, 12, 0, 13, 0, 14, 0, 23, 0, 25, random, 0, 0]
        stack = [len(argv), *argv, 0, *env, 0, *aux]
        sp = 2131226624
        self.machine.mem_write(sp, struct.pack('<' + 'I' * len(stack), *stack))
        self.machine.reg_write(UC_ARM_REG_SP, sp)
        self.machine.hook_add(UC_HOOK_INTR, self.syscall)

    def string(self, address):
        value = bytearray()
        while True:
            byte = self.machine.mem_read(address + len(value), 1)[0]
            if not byte:
                return value.decode()
            value.append(byte)

    def open(self, path, flags):
        if path == '/proc':
            descriptor = {'path': path, 'data': b'', 'position': 0}
        else:
            if path not in self.files:
                if not flags & 64:
                    return -2
                self.files[path] = bytearray()
            elif flags & 128 and flags & 64:
                return -17
            if flags & 512:
                self.files[path] = bytearray()
            descriptor = {'path': path, 'position': 0}
        fd = max(self.descriptors) + 1
        self.descriptors[fd] = descriptor
        return fd

    def read(self, fd, address, count):
        descriptor = self.descriptors[fd]
        data = descriptor.get('data', self.files.get(descriptor.get('path'), b''))
        position = descriptor['position']
        part = bytes(data[position:position + count])
        if part:
            self.machine.mem_write(address, part)
        descriptor['position'] += len(part)
        return len(part)

    def write(self, fd, data):
        if fd in (1, 2):
            (self.output if fd == 1 else self.errors).extend(data)
        else:
            descriptor = self.descriptors[fd]
            if descriptor['path'] == STATE and self.state_write_limit is not None and (descriptor['position'] + len(data) > self.state_write_limit):
                return -28
            contents = self.files[descriptor['path']]
            position = descriptor['position']
            if len(contents) < position + len(data):
                contents.extend(bytes(position + len(data) - len(contents)))
            contents[position:position + len(data)] = data
            descriptor['position'] += len(data)
        return len(data)

    def sync_mapping(self, address):
        mapping = self.mappings.get(address)
        if mapping and mapping['writable']:
            self.files[mapping['path']][:] = self.machine.mem_read(address, mapping['length'])

    def stat(self, path, address):
        if path != '/proc' and path not in self.files:
            return -2
        result = bytearray(104)
        struct.pack_into('<I', result, 16, 16877 if path == '/proc' else 33152)
        struct.pack_into('<Q', result, 48, len(self.files.get(path, b'')))
        struct.pack_into('<I', result, 56, 4096)
        self.machine.mem_write(address, bytes(result))
        return 0

    def syscall(self, machine, interrupt, unused):
        number = machine.reg_read(UC_ARM_REG_R7)
        a = [machine.reg_read(register) for register in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5)]
        self.syscalls.append((number, a))
        if number in (1, 248):
            self.exited = a[0]
            machine.emu_stop()
            return
        if number == 3:
            result = self.read(*a[:3])
        elif number == 145:
            result = 0
            for index in range(a[2]):
                address, count = struct.unpack('<II', machine.mem_read(a[1] + index * 8, 8))
                taken = self.read(a[0], address, count)
                result += taken
                if taken < count:
                    break
        elif number == 4:
            result = self.write(a[0], bytes(machine.mem_read(a[1], a[2])))
        elif number == 146:
            result = 0
            for index in range(a[2]):
                address, count = struct.unpack('<II', machine.mem_read(a[1] + index * 8, 8))
                result += self.write(a[0], bytes(machine.mem_read(address, count)))
        elif number == 5:
            result = self.open(self.string(a[0]), a[1])
        elif number == 322:
            result = self.open(self.string(a[1]), a[2])
        elif number == 6:
            self.descriptors.pop(a[0], None)
            result = 0
        elif number in (197,):
            result = self.stat(self.descriptors[a[0]]['path'], a[1])
        elif number == 195:
            result = self.stat(self.string(a[0]), a[1])
        elif number in (33, 334):
            path = self.string(a[0] if number == 33 else a[1])
            result = 0 if path in self.files else -2
        elif number in (194, 93):
            length = a[2] if number == 194 else a[1]
            data = self.files[self.descriptors[a[0]]['path']]
            data[:] = bytes(data[:length]).ljust(length, b'\x00')
            result = 0
        elif number == 140:
            descriptor = self.descriptors[a[0]]
            offset = a[1] << 32 | a[2]
            if offset & 1 << 63:
                offset -= 1 << 64
            data = descriptor.get('data', self.files.get(descriptor.get('path'), b''))
            origin = 0 if a[4] == 0 else descriptor['position'] if a[4] == 1 else len(data)
            position = origin + offset
            if self.seek_errno or position < 0:
                result = -self.seek_errno if self.seek_errno else -22
            else:
                descriptor['position'] = position
                machine.mem_write(a[3], struct.pack('<q', position))
                result = 0
        elif number == 192:
            length = a[1] + 4095 & ~4095
            address = self.next_map
            self.next_map += length
            machine.mem_map(address, length)
            if a[4] != 4294967295:
                path = self.descriptors[a[4]]['path']
                machine.mem_write(address, bytes(self.files[path]).ljust(length, b'\x00'))
                self.mappings[address] = {'path': path, 'length': a[1], 'writable': bool(a[2] & 2)}
            result = address
        elif number == 91:
            self.sync_mapping(a[0])
            self.mappings.pop(a[0], None)
            machine.mem_unmap(a[0], a[1] + 4095 & ~4095)
            result = 0
        elif number == 45:
            if a[0]:
                self.brk = a[0]
            result = self.brk
        elif number in (20, 224, 256):
            result = 321
        elif number == 983045:
            machine.reg_write(UC_ARM_REG_C13_C0_3, a[0])
            result = 0
        elif number in (263,):
            machine.mem_write(a[1], struct.pack('<ii', 180, 0))
            result = 0
        elif number == 397:
            result = -self.statx_errno
        elif number == 403:
            result = -38
        elif number in (55, 118, 143, 162, 221, 240, 338):
            result = 0
        elif number == 37:
            result = 0 if a[0] == 321 else -3
        elif number == 266:
            machine.mem_write(a[2], struct.pack('<I', self.storage_type) + bytes(a[1] - 4))
            result = 0
        elif number == 99:
            machine.mem_write(a[1], struct.pack('<I', self.storage_type) + bytes(60))
            result = 0
        elif number == 122:
            values = [b'Linux', b'u5-offline', b'offline-kernel', b'#1', b'armv7l', b'']
            machine.mem_write(a[0], b''.join((v.ljust(65, b'\x00') for v in values)))
            result = 0
        elif number == 39:
            path = self.string(a[0])
            result = -17 if path in self.files else 0
            if result == 0:
                self.files[path] = bytearray()
        elif number == 21:
            target, kind = (self.string(a[1]), self.string(a[2]))
            self.mounts.append((target, kind))
            result = -self.mount_errno if self.mount_errno else -19 if self.reject_tmpfs and kind == 'tmpfs' else 0
            if result == 0:
                self.storage_type = 16914836 if kind == 'tmpfs' else 2240043254
        elif number == 38:
            old, new = (self.string(a[0]), self.string(a[1]))
            self.files[new] = self.files.pop(old)
            result = 0
        elif number in (10, 328):
            path = self.string(a[0] if number == 10 else a[1])
            result = 0 if self.files.pop(path, None) is not None else -2
        elif number == 217:
            descriptor = self.descriptors[a[0]]
            if descriptor['position']:
                result = 0
            else:
                entries = bytearray()
                pids = sorted({path.split('/')[2] for path in self.files if path.startswith('/proc/') and path.endswith('/comm')})
                for index, pid in enumerate(pids):
                    name = pid.encode() + b'\x00'
                    length = 19 + len(name) + 7 & ~7
                    entries += (struct.pack('<QQHB', index + 1, index + 1, length, 4) + name).ljust(length, b'\x00')
                machine.mem_write(a[1], bytes(entries))
                descriptor['position'] = 1
                result = len(entries)
        elif number in (54, 19):
            result = -25 if number == 54 else 0
        else:
            raise AssertionError(f'unsupported syscall {number}, arguments {a}, pc={machine.reg_read(UC_ARM_REG_PC):x}')
        machine.reg_write(UC_ARM_REG_R0, result & 4294967295)

    def run(self, expected_exit=0):
        try:
            self.machine.emu_start(self.elf['e_entry'], 0, count=15000000)
        except UcError:
            print('PC', hex(self.machine.reg_read(UC_ARM_REG_PC)))
            raise
        assert self.exited == expected_exit, (self.exited, self.errors.decode(errors='replace'))
        for address in self.mappings:
            self.sync_mapping(address)
        return bytes(self.output)
