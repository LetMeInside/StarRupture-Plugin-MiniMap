"""Offline CL-127004 component/material/transform/bounds/membership gate.
No game calls. Usage: python script.py matching.exe matching.pdb
"""
import mmap
import pathlib
import re
import struct
import bisect
reader=pathlib.Path(__file__).with_name('VerifyCaptureRetirementAbi.py').read_text()
exec(reader.split('_,fence=')[0])

def fields(name):
    ix,b=find_type(name,(0x1504,0x1505));fl=struct.unpack_from('<I',b,6)[0]
    return numeric(b,18)[0],fl,members(fl)

assert fields('UStaticMesh')[2]['ExtendedBounds'][0]==0x240
size,_,f=fields('UE::Math::TBoxSphereBounds<double,double>')
assert size==0x38 and {n:f[n][0] for n in ['Origin','BoxExtent','SphereRadius']}=={'Origin':0,'BoxExtent':24,'SphereRadius':48}
assert fields('USceneCaptureComponent')[2]['ShowOnlyComponents'][0]==0x278
assert fields('USceneCaptureComponent')[2]['ShowOnlyActors'][0]==0x288
assert fields('UMeshComponent')[2]['OverrideMaterials'][0]==0x530
_,_,actor_fields=fields('UActorComponent')
assert actor_fields['WorldPrivate'][0]==0xa8 and actor_fields['bRegistered'][0]==0x94
bit=record(actor_fields['bRegistered'][1]);assert struct.unpack_from('<H',bit)[0]==0x1205 and bit[6:8]==b'\x01\x00'
for name in ['ShowOnlyComponents','ShowOnlyActors']:
    b=record(fields('USceneCaptureComponent')[2][name][1]);_,at=numeric(b,18)
    array_name=b[at:].split(b'\0')[0].decode();size,_,array_fields=fields(array_name)
    assert size==16 and array_fields['ArrayNum'][0]==8 and array_fields['ArrayMax'][0]==12
size,_,f=fields('FWeakObjectPtr')
assert size==8 and f['ObjectIndex'][0]==0 and f['ObjectSerialNumber'][0]==4
size,_,f=fields('FUObjectItem')
assert size==24 and f['SerialNumber'][0]==16
size,_,f=fields('UE::Math::TTransform<double>')
assert size==96 and f['Rotation'][0]==0 and f['Translation'][0]==32 and f['Scale3D'][0]==64
for owner,name,count in [('UMeshComponent','SetMaterial',2),('USceneComponent','SetWorldTransform',4),('USceneCaptureComponent','ShowOnlyComponent',1)]:
    _,fl,_=fields(owner)
    b=record(method_types[(fl,name)])
    ret,cls,this,call,attr,argc,args,adjust=struct.unpack_from('<IIIBBHIi',b,2)
    assert ret==3 and call==0 and argc==count and adjust==0,(owner,name,b.hex())
    print('PDB native x64 member contract:',owner,name,'arguments',count)
_,e=find_type('ETeleportType',(0x1507,))
assert struct.unpack_from('<I',e,6)[0]==0x20 # unsigned byte
b=record(struct.unpack_from('<I',e,10)[0]);pos=2;values={}
while pos<len(b):
    if b[pos]>=0xf0:pos+=b[pos]&15;continue
    assert struct.unpack_from('<H',b,pos)[0]==0x1502
    val,pos=numeric(b,pos+4);end=b.index(0,pos);values[b[pos:end].decode()]=val;pos=end+1
assert values['TeleportPhysics']==1

expected={'AssemblySetMaterial':0x048b2a60,'AssemblySetWorldTransform':0x04911140,'AssemblyShowOnlyExec':0x04922e60}
text=next(s for s in section_list if s[0]=='.text');code=data(text[1],text[4])
er,es=struct.unpack_from('<II',image,optional+112+3*8)
starts={v[0] for v in struct.iter_unpack('<III',data(er,es))}
patterns=pathlib.Path(__file__).parent.parent/'Src/Experiments/StaticAssemblyCapturePatterns.inl'
for pattern,name in re.findall(r'\{"([0-9A-F ]+)", &Bindings::(\w+)\}',patterns.read_text()):
    needle=bytes.fromhex(pattern);assert len(needle)==64 and code.count(needle)==1
    rva=text[1]+code.find(needle);assert rva==expected[name] and rva in starts
    print('PE unique function-entry fingerprint:',name,hex(rva))
with pdb_path.open('rb') as f:
    m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
    for name,prefix in [('AssemblySetMaterial',b'?SetMaterial@UMeshComponent@@'),('AssemblySetWorldTransform',b'?SetWorldTransform@USceneComponent@@'),('AssemblyShowOnlyExec',b'?execShowOnlyComponent@USceneCaptureComponent@@')]:
        pos=0;found=[]
        while True:
            pos=m.find(prefix,pos)
            if pos<0:break
            if pos>=14 and struct.unpack_from('<H',m,pos-12)[0]==0x110e:
                _,off,seg=struct.unpack_from('<IIH',m,pos-10);found.append(section_list[seg-1][1]+off)
            pos+=len(prefix)
        assert found and set(found)=={expected[name]},(name,found)
# Installed wrapper writes UseShowOnlyList=2 and calls native AddUnique after
# constructing a weak index/serial pair. It does not clear/replace the list.
assert data(0x04922ed1,7)==bytes.fromhex('C6 87 50 02 00 00 02')
assert data(0x04922f25,7)==bytes.fromhex('48 8D 8F 78 02 00 00')
assert data(0x04922f2c,1)==b'\xe8'
# Unattached world-transform branch forwards the full transform, without
# requiring an actor or parent construction.
assert data(0x0491126a,1)==b'\xe8'
baseline={
 'FRHIGPUTextureReadback':(0x58,{'DestinationStagingTextures':0x18}),
 'FRHIGPUMemoryReadback':(0x18,{'Fence':8,'LastCopyGPUMask':0x10,'LastLockGPUIndex':0x14}),
 'FRHIGPUFence':(0x20,{'NumPendingWriteCommands':0x10}),
 'FRHITextureDesc':(0x38,{}),
 'FRHITextureCreateDesc':(0x60,{'InitialState':0x38,'DebugName':0x40,'BulkData':0x48}),
 'FRHITexture':(0x60,{'TextureDesc':0x20}),
 'FRHITransitionInfo':(0x30,{'Texture':8,'AccessAfter':0x18}),
 'FResourceBulkDataArrayView':(0x18,{'Data':8,'SizeInBytes':0x10}),
}
for name,(size,offsets_expected) in baseline.items():
    actual,_,f=fields(name);assert actual==size
    for field,offset in offsets_expected.items():assert f[field][0]==offset,(name,field)
_,_,f=fields('FRHIGPUTextureReadback');arr=record(f['DestinationStagingTextures'][1])
assert struct.unpack_from('<H',arr)[0]==0x1503 and numeric(arr,10)[0]==64
root=pathlib.Path(__file__).parent.parent/'Src/Experiments'
capture=re.findall(r'\{"(\w+)", "([?0-9A-F ]+)", &Bindings::\w+, (true|false)\}',(root/'SmelterCapturePatterns.inl').read_text())
readback=re.findall(r'\{\s*"(\w+)", "([?0-9A-F ]+)", &Bindings::\w+, (-?\d+), (\d+), (true|false)',(root/'ReadbackNativeResolution.cpp').read_text())
assert len(capture)==22 and len(readback)==18
for name,pattern,*tail in capture+readback:
    expression=b''.join(b'.' if token=='??' else re.escape(bytes.fromhex(token)) for token in pattern.split())
    hits=list(re.finditer(expression,code,re.S));assert len(hits)==1,(name,len(hits))
    rva=text[1]+hits[0].start()
    is_function=len(tail)==1 or int(tail[0])<0
    if is_function and tail[-1]=='false':assert rva in starts,(name,hex(rva))
print('Baseline RHI/readback layouts and 40 capture/readback fingerprints passed')
print('Static assembly native gate passed: matching PE/PDB, stored bounds, QST, weak identity and native setters')
