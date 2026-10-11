"""Offline CL-127004 Interior comparison gate; no native execution.
Usage: python VerifyInteriorTagAbi.py <matching.exe> <matching.pdb>
RVAs are offline assertions only. Runtime resolves a unique fingerprint.
"""
import mmap
import pathlib
import re
import struct

reader = pathlib.Path(__file__).with_name('VerifyRepresentationAbi.py').read_text()
exec(reader.split('contracts={')[0])

patterns = pathlib.Path(__file__).parent.parent/'Src/Experiments/RepresentationMetadataPatterns.h'
pattern = re.search(r'NameEqualsWide\[\]="([0-9A-F ]+)"', patterns.read_text()).group(1)
needle = bytes.fromhex(pattern)
text = next(s for s in section_list if s[0] == '.text')
code = data(text[1], text[4])
assert len(needle) == 64 and code.count(needle) == 1
rva = text[1] + code.find(needle)
assert rva == 0x014b60f0
symbol = b'?Equals@FName@@CA_NV1@PEB_W@Z\0'
with pdb_path.open('rb') as f:
    m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    pos = 0
    hits = []
    while True:
        pos = m.find(symbol, pos)
        if pos < 0:
            break
        if pos >= 14 and struct.unpack_from('<H', m, pos-12)[0] == 0x110e:
            _, offset, segment = struct.unpack_from('<IIH', m, pos-10)
            hits.append(section_list[segment-1][1] + offset)
        pos += len(symbol)
    assert hits and set(hits) == {rva}
    m.close()

# Native thunk counts only the supplied constant string and tail-calls the
# nonallocating equality helper. Installed caller passes FName by value in RCX.
assert data(rva, 30) == bytes.fromhex(
    '45 33 C0 48 8B C2 48 85 D2 74 13 66 44 39 02 74 0D '
    '48 8D 40 02 41 FF C0 66 83 38 00 75 F3')
assert data(rva+30, 1) == b'\xe9'
assert rva+35+struct.unpack('<i', data(rva+31, 4))[0] == 0x014ac620
assert data(0x0771a8d0, 3) == bytes.fromhex('48 8B 0B')
assert data(0x0771a8da, 1) == b'\xe8'
assert 0x0771a8df+struct.unpack('<i', data(0x0771a8db, 4))[0] == rva
# The helper rejects differing name lengths before character comparison.
assert data(0x014ac6f3, 12) == bytes.fromhex('0F B7 0B 8B C1 C1 E8 06 3B F8 74 04')
print('Interior: matching PE/PDB GUID/age; exact static FName::Equals(FName,const wchar_t*) symbol; unique 64-byte runtime fingerprint; installed IsInterior caller and length check verified.')
