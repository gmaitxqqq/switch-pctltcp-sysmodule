#!/usr/bin/env python3
"""
elf2nsp.py - Convert ELF to NSP (NX boot2 sysmodule format)
Minimal implementation: wrap ELF in PFS0, write to NSP.
NSP = PFS0 container with 'main' entry.
"""
import struct, sys, os

def align_u32(v, a=4):
    return (v + a - 1) & ~(a - 1)

def make_pfs0(files):
    """
    files: list of (name_bytes, data_bytes)
    Returns PFS0 data.
    """
    # Header: 4 magic, 4 num_files, 4 string_table_size, 4 padding
    string_table = b''
    file_entries = b''
    file_data = b''

    offset = 0
    for name, data in files:
        name_null = name + b'\x00'
        string_table += name_null
        # File entry: 4 offset in string table, 8 file data offset, 8 file size, 4 padding
        file_entries += struct.pack('<IIQ', len(string_table) - len(name_null), offset, len(data), 0)
        padded = data + b'\x00' * (align_u32(len(data)) - len(data))
        file_data += padded
        offset += len(padded)

    string_table_size = len(string_table)
    header = struct.pack('<III', 0x50465330, len(files), string_table_size) + b'\x00' * 4  # magic, count, strtab_size, padding

    # File entries: 24 bytes each
    file_entries_data = b''
    for i, (name, data) in enumerate(files):
        name_off = 0
        for j in range(i):
            name_off += len(files[j][0]) + 1
        file_offset = 0
        for j in range(i):
            d = files[j][1]
            file_offset += align_u32(len(d))
        file_entries_data += struct.pack('<IIQ', name_off, file_offset, len(data)) + b'\x00' * 4

    # Actually recalculate properly
    # PFS0 header: magic(4) + num_files(4) + string_table_size(4) + reserved(4) = 16 bytes
    # Then num_files * 24 bytes of file entries
    # Then string table
    # Then file data (each aligned to 4 bytes)

    string_table = b''
    for name, _ in files:
        string_table += name + b'\x00'

    file_entries = b''
    data_offset = 16 + len(files) * 24 + len(string_table)
    data_offset = align_u32(data_offset)

    current_offset = 0
    for i, (name, data) in enumerate(files):
        name_off = 0
        for j in range(i):
            name_off += len(files[j][0]) + 1
        # Find data offset
        off = 0
        for j in range(i):
            d = files[j][1]
            off += align_u32(len(d))
        file_entries += struct.pack('<IIQ', name_off, off, len(data)) + b'\x00' * 4

    # Recalculate data offsets properly
    entries = []
    strtab = b''
    for name, data in files:
        entries.append((name, data, len(strtab)))
        strtab += name + b'\x00'

    file_entries = b''
    data_offsets = []
    running = 0
    for name, data, _ in entries:
        data_offsets.append(running)
        running += align_u32(len(data))

    for i, (name, data, name_off) in enumerate(entries):
        file_entries += struct.pack('<IIQ', name_off, data_offsets[i], len(data)) + b'\x00' * 4

    pfs0 = b'PFS0'
    pfs0 += struct.pack('<I', len(files))
    pfs0 += struct.pack('<I', len(strtab))
    pfs0 += b'\x00' * 4  # padding
    pfs0 += file_entries
    pfs0 += strtab
    # Align to 4 bytes before data
    while len(pfs0) % 4 != 0:
        pfs0 += b'\x00'
    for name, data, _ in entries:
        pfs0 += data
        while len(pfs0) % 4 != 0:
            pfs0 += b'\x00'

    return pfs0

def elf2nsp(elf_path, nsp_path):
    with open(elf_path, 'rb') as f:
        elf_data = f.read()
    # NSP for boot2: PFS0 containing 'main' (the ELF)
    pfs0 = make_pfs0([(b'main', elf_data)])
    with open(nsp_path, 'wb') as f:
        f.write(pfs0)
    print(f'Wrote {nsp_path} ({len(pfs0)} bytes)')

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f'Usage: {sys.argv[0]} <input.elf> <output.nsp>')
        sys.exit(1)
    elf2nsp(sys.argv[1], sys.argv[2])
