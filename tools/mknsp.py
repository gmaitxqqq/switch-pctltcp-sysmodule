#!/usr/bin/env python3
"""
mknsp.py - Create NSP (PFS0) package from ELF + NPDM files

PFS0 format:
  Header:
    4 bytes: Magic "PFS0"
    4 bytes: Number of files (u32 LE)
    4 bytes: String table size (u32 LE)
    4 bytes: Reserved (0)
  File entries (24 bytes each):
    8 bytes: Offset from end of header (u64 LE)
    8 bytes: File size (u64 LE)
    4 bytes: String table offset (u32 LE)
    4 bytes: Reserved (0)
  String table (null-terminated filenames concatenated)
  File data (each file aligned to 0x10)

Usage: python3 mknsp.py <output.nsp> <main.elf> <main.npdm>
"""

import struct
import sys
import os


def align_up(value, alignment=0x10):
    return (value + alignment - 1) & ~(alignment - 1)


def create_nsp(output_path, files):
    """
    Create NSP file from a list of (filename, filepath) tuples.
    """
    num_files = len(files)
    
    # Read all file data
    file_data = []
    for name, path in files:
        with open(path, 'rb') as f:
            data = f.read()
        file_data.append((name, data))
    
    # Build string table
    string_table = b''
    string_offsets = []
    for name, _ in file_data:
        string_offsets.append(len(string_table))
        string_table += name.encode('ascii') + b'\x00'
    
    # Calculate header size
    header_size = 16 + (24 * num_files) + len(string_table)
    header_size_aligned = align_up(header_size)
    string_table_padding = header_size_aligned - header_size
    string_table += b'\x00' * string_table_padding
    
    # Calculate file offsets and total size
    data_start = align_up(16 + (24 * num_files) + len(string_table))
    current_offset = 0
    offsets = []
    for name, data in file_data:
        offsets.append(current_offset)
        aligned_size = align_up(len(data))
        current_offset += aligned_size
    
    # Write NSP
    with open(output_path, 'wb') as f:
        # PFS0 header
        f.write(b'PFS0')
        f.write(struct.pack('<I', num_files))
        f.write(struct.pack('<I', len(string_table) - string_table_padding))  # Original size
        f.write(struct.pack('<I', 0))  # Reserved
        
        # File entries
        for i, (name, data) in enumerate(file_data):
            f.write(struct.pack('<Q', offsets[i]))
            f.write(struct.pack('<Q', len(data)))
            f.write(struct.pack('<I', string_offsets[i]))
            f.write(struct.pack('<I', 0))
        
        # String table (with padding)
        f.write(string_table)
        
        # File data (aligned)
        for name, data in file_data:
            f.write(data)
            # Pad to 0x10 alignment
            padding = align_up(len(data)) - len(data)
            if padding > 0:
                f.write(b'\x00' * padding)
    
    total_size = os.path.getsize(output_path)
    print(f"Created NSP: {output_path} ({total_size} bytes)")
    for i, (name, data) in enumerate(file_data):
        print(f"  {name}: {len(data)} bytes at offset {offsets[i]}")


def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <output.nsp> <main.elf> <main.npdm>", file=sys.stderr)
        sys.exit(1)
    
    output_path = sys.argv[1]
    elf_path = sys.argv[2]
    npdm_path = sys.argv[3]
    
    files = [
        ("main", elf_path),
        ("main.npdm", npdm_path),
    ]
    
    create_nsp(output_path, files)


if __name__ == '__main__':
    main()
