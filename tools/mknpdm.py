#!/usr/bin/env python3
"""
mknpdm.py - Generate NPDM binary from JSON config

Based on the NPDM format specification from Switchbrew:
https://switchbrew.org/wiki/NPDM

Usage: python3 mknpdm.py <input.json> <output.npdm>
"""

import json
import struct
import sys


def encode_sac(services, is_host=False):
    """Encode Service Access Control entries.
    Each entry: byte0 = (0x80 if is_host else 0x00) | (name_length - 1), then name bytes.
    """
    data = b''
    for svc in services:
        if svc == '*':
            name = b'*'
            first_byte = 0x80 | (len(name) - 1)
            data += bytes([first_byte]) + name
        else:
            name = svc.encode('ascii')[:8]
            first_byte = (0x80 if is_host else 0x00) | (len(name) - 1)
            data += bytes([first_byte]) + name
    return data


def encode_fac_acid(permissions_hex):
    """ACID FAC: version(1) + counts(2) + pad(1) + flags(8) + id ranges(32)"""
    flags = int(permissions_hex, 16)
    return struct.pack('<BBBB', 1, 0, 0, 0) + struct.pack('<Q', flags) + b'\x00' * 32


def encode_fac_aci0(permissions_hex):
    """ACI0 FAC: version(1) + pad(3) + flags(8) + offsets/sizes(16)"""
    flags = int(permissions_hex, 16)
    return struct.pack('<B', 1) + b'\x00\x00\x00' + struct.pack('<Q', flags) + b'\x00' * 16


def kac_thread_info(lowest_prio, highest_prio, min_core, max_core):
    """ThreadInfo: marker=0b0111 (bits 0-2 set, bit 3 clear)"""
    val = 0b0111
    val |= (lowest_prio & 0x3F) << 4
    val |= (highest_prio & 0x3F) << 10
    val |= (min_core & 0xFF) << 16
    val |= (max_core & 0xFF) << 24
    return val


def kac_enable_syscalls(syscall_ids, index=0):
    """EnableSystemCalls: marker=0b01111 (bits 0-3 set, bit 4 clear)"""
    mask = 0
    for sid in syscall_ids:
        if index * 24 <= sid < (index + 1) * 24:
            mask |= 1 << (sid - index * 24)
    val = 0b01111
    val |= (mask & 0xFFFFFF) << 5
    val |= (index & 0x7) << 29
    return val


def kac_kernel_version(version_hex):
    """KernelVersion: marker=0x3FFF (bits 0-13 set, bit 14 clear)"""
    version = int(version_hex, 16)
    major = (version >> 4) & 0x1FFF
    minor = version & 0xF
    return 0x3FFF | (minor << 15) | (major << 19)


def kac_handle_table_size(size):
    """HandleTableSize: marker=0x7FFF (bits 0-14 set, bit 15 clear)"""
    return 0x7FFF | ((size & 0x3FF) << 16)


def kac_misc_params(program_type):
    """MiscParams: marker=0x1FFF (bits 0-12 set, bit 13 clear)"""
    return 0x1FFF | ((program_type & 0x7) << 14)


def encode_kac(kernel_capabilities):
    entries = []
    for cap in kernel_capabilities:
        ctype = cap['type']
        if ctype == 'kernel_flags':
            v = cap['value']
            entries.append(kac_thread_info(
                v.get('lowest_thread_priority', 24),
                v.get('highest_thread_priority', 63),
                v.get('lowest_cpu_id', 0),
                v.get('highest_cpu_id', 3)))
        elif ctype == 'syscalls':
            v = cap['value']
            ids = sorted(int(x, 16) for x in v.values())
            max_sid = max(ids) if ids else 0
            for idx in range((max_sid // 24) + 1):
                entry = kac_enable_syscalls(ids, idx)
                if (entry >> 5) & 0xFFFFFF:
                    entries.append(entry)
        elif ctype == 'min_kernel_version':
            entries.append(kac_kernel_version(cap['value']))
        elif ctype == 'handle_table_size':
            entries.append(kac_handle_table_size(cap['value']))
        elif ctype == 'misc_params':
            entries.append(kac_misc_params(cap.get('value', {}).get('program_type', 0)))

    if not any(c['type'] == 'misc_params' for c in kernel_capabilities):
        entries.append(kac_misc_params(0))

    return b''.join(struct.pack('<I', e) for e in entries)


def generate_npdm(config):
    program_id = int(config.get('program_id', '0x0100000000001000'), 16)
    pid_min = int(config.get('program_id_range_min', config.get('program_id', '0x0100000000001000')), 16)
    pid_max = int(config.get('program_id_range_max', config.get('program_id', '0x0100000000001000')), 16)
    priority = config.get('main_thread_priority', 48)
    cpu_id = config.get('default_cpu_id', 0)
    stack_size = int(config.get('main_thread_stack_size', '0x8000'), 16)
    is_64bit = config.get('is_64_bit', True)
    addr_space = config.get('address_space_type', 3)
    is_retail = config.get('is_retail', True)
    sys_res = int(config.get('system_resource_size', '0'), 16)
    name = config.get('name', 'sysmodule')
    fs_perms = config.get('filesystem_access', {}).get('permissions', '0xFFFFFFFFFFFFFFFF')
    if isinstance(fs_perms, list):
        fs_perms = '0xFFFFFFFFFFFFFFFF'
    svc_access = config.get('service_access', [])
    svc_host = config.get('service_host', [])
    kcaps = config.get('kernel_capabilities', [])

    sac_data = encode_sac(svc_access) + encode_sac(svc_host, is_host=True)
    fac_acid = encode_fac_acid(fs_perms)
    fac_aci0 = encode_fac_aci0(fs_perms)
    kac_data = encode_kac(kcaps)

    # --- ACID ---
    acid = bytearray(0x240)
    acid[0x200:0x204] = b'ACID'
    struct.pack_into('<I', acid, 0x204, 0x240 + len(fac_acid) + len(sac_data) + len(kac_data) - 0x200)
    acid[0x208] = 1
    acid[0x209] = 0 if is_retail else 1
    flags = 1 if is_retail else 0
    struct.pack_into('<I', acid, 0x20C, flags)
    struct.pack_into('<Q', acid, 0x210, pid_min)
    struct.pack_into('<Q', acid, 0x218, pid_max)
    struct.pack_into('<I', acid, 0x220, 0x240)
    struct.pack_into('<I', acid, 0x224, len(fac_acid))
    struct.pack_into('<I', acid, 0x228, 0x240 + len(fac_acid))
    struct.pack_into('<I', acid, 0x22C, len(sac_data))
    struct.pack_into('<I', acid, 0x230, 0x240 + len(fac_acid) + len(sac_data))
    struct.pack_into('<I', acid, 0x234, len(kac_data))
    acid += fac_acid + sac_data + kac_data

    # --- ACI0 ---
    aci0 = bytearray(0x40)
    aci0[0x00:0x04] = b'ACI0'
    struct.pack_into('<Q', aci0, 0x10, program_id)
    struct.pack_into('<I', aci0, 0x20, 0x40)
    struct.pack_into('<I', aci0, 0x24, len(fac_aci0))
    struct.pack_into('<I', aci0, 0x28, 0x40 + len(fac_aci0))
    struct.pack_into('<I', aci0, 0x2C, len(sac_data))
    struct.pack_into('<I', aci0, 0x30, 0x40 + len(fac_aci0) + len(sac_data))
    struct.pack_into('<I', aci0, 0x34, len(kac_data))
    aci0 += fac_aci0 + sac_data + kac_data

    # --- META ---
    meta = bytearray(0x80)
    meta[0x00:0x04] = b'META'
    mflags = (1 if is_64bit else 0) | ((addr_space & 7) << 1)
    meta[0x0C] = mflags
    meta[0x0E] = priority & 0xFF
    meta[0x0F] = cpu_id & 0xFF
    struct.pack_into('<I', meta, 0x14, sys_res)
    struct.pack_into('<I', meta, 0x1C, stack_size)
    name_b = name.encode('ascii')[:15]
    meta[0x20:0x20 + len(name_b)] = name_b
    struct.pack_into('<I', meta, 0x70, 0x80 + len(acid))  # AciOffset
    struct.pack_into('<I', meta, 0x74, len(aci0))          # AciSize
    struct.pack_into('<I', meta, 0x78, 0x80)               # AcidOffset
    struct.pack_into('<I', meta, 0x7C, len(acid))          # AcidSize

    return bytes(meta) + bytes(acid) + bytes(aci0)


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.json> <output.npdm>", file=sys.stderr)
        sys.exit(1)
    with open(sys.argv[1], 'r') as f:
        config = json.load(f)
    npdm = generate_npdm(config)
    with open(sys.argv[2], 'wb') as f:
        f.write(npdm)
    print(f"Generated NPDM: {sys.argv[2]} ({len(npdm)} bytes)")


if __name__ == '__main__':
    main()
