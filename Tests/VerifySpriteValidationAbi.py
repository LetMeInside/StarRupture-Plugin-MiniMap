"""Offline CL-127004 verification; no game execution or native invocation.
Usage: python VerifySpriteValidationAbi.py <matching.exe> <matching.pdb>
Reuses all existing representation read-contract/fingerprint checks, then verifies
the seven observed integer IDs against the matching PDB enum. Enum labels are
offline evidence only; the runtime validator does not invent display names.
"""
import pathlib
import runpy
import struct

verified = runpy.run_path(str(pathlib.Path(__file__).with_name('VerifyRepresentationAbi.py')))
record, numeric = verified['record'], verified['numeric']
matches = []
for index in range(verified['first'], verified['last']):
    b = record(index)
    if len(b) >= 15 and struct.unpack_from('<H', b)[0] == 0x1507:
        if b[14:b.index(0, 14)] == b'ECrBuildingID' and not struct.unpack_from('<H', b, 4)[0] & 0x80:
            matches.append((index, struct.unpack_from('<I', b, 10)[0]))
assert len(matches) == 1, matches

def enumerators(index):
    b = record(index)
    assert struct.unpack_from('<H', b)[0] == 0x1203
    at, values = 2, {}
    while at < len(b):
        if b[at] >= 0xf0:
            at += b[at] & 15
            continue
        leaf = struct.unpack_from('<H', b, at)[0]
        if leaf == 0x1502:
            value, at = numeric(b, at + 4)
            end = b.index(0, at)
            values[b[at:end].decode()] = value
            at = end + 1
        elif leaf == 0x1404:
            values.update(enumerators(struct.unpack_from('<I', b, at + 4)[0]))
            at += 8
        else:
            raise AssertionError(f'Unexpected enum field 0x{leaf:x}')
    return values

values = enumerators(matches[0][1])
expected = {4:'Airlock_StartingHUB', 18:'CloningBed_Lander', 220:'Hub',
            238:'ConnectingPlatformStability', 317:'PersonalStorage_NonDeconstructible',
            336:'Smelter', 341:'StartingItemPrinter'}
for value, label in expected.items():
    matches_for_label = [n for n, v in values.items() if v == value and n.split('::')[-1] == label]
    assert len(matches_for_label) == 1, (value, label, matches_for_label)
    print(f'Matching PDB ECrBuildingID: {value} = {label}')
print(f'PDB enum type 0x{matches[0][0]:x}; seven identities verified. Ownership remains unknown.')
