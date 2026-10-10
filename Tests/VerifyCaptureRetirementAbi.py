"""Read-only CL-127004 fence ABI / contextual fingerprint check. No game calls.
Usage: python VerifyCaptureRetirementAbi.py <matching.exe> <matching.pdb>
RVAs are offline assertions only; runtime code resolves contextual fingerprints.
"""
import mmap
import pathlib
import re
import struct
import bisect

# Reuse the standard-library PE/MSF/TPI reader, including exact GUID/age checks.
reader = pathlib.Path(__file__).with_name('VerifyRepresentationAbi.py').read_text()
exec(reader.split('contracts={')[0])

def find_type(name, leaves):
    pos=0
    while True:
        pos=tpi.find(name.encode()+b'\0',pos)
        assert pos>=0, name
        ix=bisect.bisect_right(offsets,pos)-1+first;pos+=len(name)+1;b=record(ix)
        leaf=struct.unpack_from('<H',b)[0]
        if leaf not in leaves or struct.unpack_from('<H',b,4)[0]&0x80:continue
        if leaf==0x1507:
            if b[14:].split(b'\0')[0]!=name.encode():continue
        else:
            _,at=numeric(b,18)
            if b[at:].split(b'\0')[0]!=name.encode():continue
        return ix,b

_,fence=find_type('FRenderCommandFence',(0x1504,0x1505))
assert numeric(fence,18)[0]==8
fields_ix=struct.unpack_from('<I',fence,6)[0]
fields=members(fields_ix)
assert fields['CompletionTask'][0]==0
for name,count,result in [('BeginFence',1,3),('IsFenceComplete',0,0x30),('~FRenderCommandFence',0,3)]:
    method=record(method_types[(fields_ix,name)])
    ret,cls,this,call,attr,argc,args,adjust=struct.unpack_from('<IIIBBHIi',method,2)
    assert ret==result and call==0 and argc==count and adjust==0,(name,method.hex())
_,enum=find_type('FRenderCommandFence::ESyncDepth',(0x1507,))
b=record(struct.unpack_from('<I',enum,10)[0]);pos=2;values={}
while pos<len(b):
    if b[pos]>=0xf0:pos+=b[pos]&15;continue
    assert struct.unpack_from('<H',b,pos)[0]==0x1502
    value,pos=numeric(b,pos+4);end=b.index(0,pos);values[b[pos:end].decode()]=value;pos=end+1
assert values=={'RenderThread':0,'RHIThread':1,'Swapchain':2}
print('PDB: fence size=8, handle +0, native x64 member frames, RHIThread=1')

expected={'RetirementConstruct':0x01853a00,'RetirementDestroy':0x0128ea80,
          'RetirementBegin':0x03afa9f0,'RetirementPoll':0x03b0fca0}
patterns=pathlib.Path(__file__).parent.parent/'Src/Experiments/SmelterCapturePatterns.inl'
text=next(s for s in section_list if s[0]=='.text');code=data(text[1],text[4])
er,es=struct.unpack_from('<II',image,optional+112+3*8)
starts={v[0] for v in struct.iter_unpack('<III',data(er,es))}
for name,pattern,leaf in re.findall(r'\{"(Retirement\w+)", "([0-9A-F ]+)", &Bindings::\w+, (true|false)\}',patterns.read_text()):
    needle=bytes.fromhex(pattern);assert len(needle)==64 and code.count(needle)==1
    rva=text[1]+code.find(needle);assert rva==expected.pop(name)
    if leaf=='false':assert rva in starts
    print(f'PE: {name} unique contextual fingerprint, RVA 0x{rva:08x}')
assert not expected
assert data(0x01853a00,11)==bytes.fromhex('48 C7 01 00 00 00 00 48 8B C1 C3')
# Verify the constructor/destructor symbol identity independently of fingerprints.
with pdb_path.open('rb') as f:
    m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
    for name,rva in [(b'??0FRenderCommandFence@@QEAA@XZ',0x01853a00),
                     (b'??1FRenderCommandFence@@QEAA@XZ',0x0128ea80)]:
        pos=0;hits=[]
        while True:
            pos=m.find(name+b'\0',pos)
            if pos<0:break
            if pos>=14 and struct.unpack_from('<H',m,pos-12)[0]==0x110e:
                _,off,seg=struct.unpack_from('<IIH',m,pos-10)
                hits.append(section_list[seg-1][1]+off)
            pos+=len(name)
        assert hits and set(hits)=={rva},(name,hits)
print('PE/PDB fence contract passed; no native functions invoked')
