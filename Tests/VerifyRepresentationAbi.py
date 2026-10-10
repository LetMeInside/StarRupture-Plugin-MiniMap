"""Read-only installed CL-127004 PE/PDB contract check. Python standard library only.
Usage: python VerifyRepresentationAbi.py <matching.exe> <matching.pdb>
RVAs below are offline verification references, never runtime addresses.
"""
import array
import bisect
import pathlib
import re
import struct
import sys
import uuid

exe_path,pdb_path=map(pathlib.Path,sys.argv[1:3])
expected_guid=uuid.UUID('41cbaea9-7ac3-99da-b947-a1db9c1021ed')
image=exe_path.read_bytes()
pe=struct.unpack_from('<I',image,0x3c)[0]
assert image[pe:pe+4]==b'PE\0\0'
machine,sections=struct.unpack_from('<HH',image,pe+4)
assert machine==0x8664
optional=pe+24
section_at=optional+struct.unpack_from('<H',image,pe+20)[0]
section_list=[]
for i in range(sections):
    at=section_at+i*40
    name=image[at:at+8].rstrip(b'\0').decode()
    virtual_size,rva,raw_size,raw=struct.unpack_from('<IIII',image,at+8)
    section_list.append((name,rva,virtual_size,raw,raw_size))
def data(rva,size):
    for _,start,_,raw,raw_size in section_list:
        if start<=rva and rva+size<=start+raw_size:
            return image[raw+rva-start:raw+rva-start+size]
    raise AssertionError(f'RVA not file backed: {rva:x}')
debug_rva,debug_size=struct.unpack_from('<II',image,optional+112+6*8)
rsds=[]
for i in range(debug_size//28):
    debug=data(debug_rva+i*28,28)
    typ,size,address=struct.unpack_from('<III',debug,12)
    if typ==2:
        cv=data(address,size)
        if cv[:4]==b'RSDS':rsds.append((uuid.UUID(bytes_le=cv[4:20]),struct.unpack_from('<I',cv,20)[0]))
assert (expected_guid,1) in rsds

with pdb_path.open('rb') as f:
    header=f.read(56)
    block_size,_,_,directory_size,_,directory_map=struct.unpack_from('<6I',header,32)
    f.seek(directory_map*block_size)
    blocks=struct.unpack('<'+'I'*((directory_size+block_size-1)//block_size),f.read(4*((directory_size+block_size-1)//block_size)))
    directory=bytearray()
    for block in blocks:f.seek(block*block_size);directory.extend(f.read(block_size))
    count=struct.unpack_from('<I',directory)[0]
    sizes=struct.unpack_from('<'+'I'*count,directory,4)
    at=4+4*count;streams=[]
    for size in sizes:
        n=0 if size==0xffffffff else (size+block_size-1)//block_size
        streams.append(struct.unpack_from('<'+'I'*n,directory,at));at+=n*4
    def stream(index):
        result=bytearray()
        for block in streams[index]:f.seek(block*block_size);result.extend(f.read(block_size))
        return result[:sizes[index]]
    info=stream(1)
    assert uuid.UUID(bytes_le=bytes(info[12:28]))==expected_guid and struct.unpack_from('<I',info,8)[0]==1
    tpi=stream(2)

first,last=struct.unpack_from('<II',tpi,8)
offsets=array.array('I');at=struct.unpack_from('<I',tpi,4)[0]
while at<len(tpi):offsets.append(at);at+=struct.unpack_from('<H',tpi,at)[0]+2
assert len(offsets)==last-first
def record(index):
    at=offsets[index-first];length=struct.unpack_from('<H',tpi,at)[0]
    return tpi[at+2:at+2+length]
def numeric(b,at):
    value=struct.unpack_from('<H',b,at)[0]
    if value<0x8000:return value,at+2
    fmt={0x8000:'b',0x8001:'h',0x8002:'H',0x8003:'i',0x8004:'I',0x8009:'q',0x800a:'Q'}[value]
    return struct.unpack_from('<'+fmt,b,at+2)[0],at+2+struct.calcsize(fmt)
method_types={}
base_types={}
def members(index):
    b=record(index);at=2;result={}
    while at<len(b):
        if b[at]>=0xf0:at+=b[at]&15;continue
        leaf=struct.unpack_from('<H',b,at)[0]
        if leaf==0x150d:
            typ=struct.unpack_from('<I',b,at+4)[0];offset,at=numeric(b,at+8)
            end=b.index(0,at);result[b[at:end].decode()]=(offset,typ);at=end+1
        elif leaf==0x1400:
            typ=struct.unpack_from('<I',b,at+4)[0];offset,at=numeric(b,at+8)
            base_types.setdefault(index,[]).append((offset,typ))
        elif leaf==0x1511:
            attr=struct.unpack_from('<H',b,at+2)[0];typ=struct.unpack_from('<I',b,at+4)[0];at+=8
            if (attr>>2)&7 in (4,6):at+=4
            end=b.index(0,at);method_types[(index,b[at:end].decode())]=typ;at=end+1
        elif leaf in (0x150f,0x1510,0x150e):at=b.index(0,at+8)+1
        elif leaf==0x1409:at+=8
        elif leaf==0x1404:
            result.update(members(struct.unpack_from('<I',b,at+4)[0]));at+=8
        else:raise AssertionError(f'unsupported field leaf {leaf:x} while verifying {index:x}')
    return result

contracts={
 'UObjectBase':(0x28,{'InternalIndex':0xc,'ClassPrivate':0x10,'NamePrivate':0x18,'OuterPrivate':0x20}),
 'FUObjectArray':(0xb8,{'ObjObjects':0x10}),
 'FChunkedFixedUObjectArray':(0x20,{'Objects':0,'MaxElements':0x10,'NumElements':0x14,'MaxChunks':0x18,'NumChunks':0x1c}),
 'FUObjectItem':(0x18,{'Object':0,'Flags':8,'SerialNumber':0x10}),
 'FObjectPtr':(8,{'Handle':0,'DebugPtr':0}),
 'UClass':(0x200,{'ClassDefaultObject':0x110}),
 'UStruct':(0xb0,{'SuperStruct':0x40}),
 'UBlueprintGeneratedClass':(0x360,{'ComponentTemplates':0x220,'SimpleConstructionScript':0x268,'InheritableComponentHandler':0x270}),
 'USimpleConstructionScript':(0xb0,{'RootNodes':0x28,'AllNodes':0x38}),
 'USCS_Node':(0xd8,{'ComponentClass':0x28,'ComponentTemplate':0x30,'AttachToName':0x80,'ParentComponentOrVariableName':0x88,'ParentComponentOwnerClassName':0x90,'bIsParentComponentNative':0x98,'ChildNodes':0xa0,'VariableGuid':0xc0,'InternalVariableName':0xd0}),
 'UInheritableComponentHandler':(0x48,{'Records':0x28}),
 'FComponentOverrideRecord':(0x78,{'ComponentClass':0,'ComponentTemplate':8,'ComponentKey':0x10}),
 'FComponentKey':(0x20,{'OwnerClass':0,'SCSVariableName':8,'AssociatedGuid':0x10}),
 'UActorComponent':(0xb8,{'bRegistered':0x94}),
 'USceneComponent':(0x250,{'AttachParent':0xc8,'AttachSocketName':0xd0,'RelativeLocation':0x140,'RelativeRotation':0x158,'RelativeScale3D':0x170,'bAbsoluteLocation':0x1a0,'bAbsoluteRotation':0x1a0,'bAbsoluteScale':0x1a0,'bVisible':0x1a0,'bHiddenInGame':0x1a1}),
 'UMeshComponent':(0x580,{'OverrideMaterials':0x530}),
 'UStaticMeshComponent':(0x630,{'StaticMesh':0x588}),
 'UStaticMesh':(0x290,{'StaticMaterials':0x160}),
 'USkeletalMesh':(0x568,{'Materials':0x1a0}),
 'USkinnedMeshComponent':(0x910,{'SkeletalMesh':0x588,'SkinnedAsset':0x590}),
 'USkeletalMeshComponent':(0x1040,{}),
 'FStructBaseChain':(0x10,{'StructBaseChainArray':0,'NumStructBasesInChainMinusOne':8}),
 'UE::Math::TTransform<double>':(0x60,{'Rotation':0,'Translation':0x20,'Scale3D':0x40}),
 'FName':(8,{'ComparisonIndex':0,'Number':4}),
 'FStaticMaterial':(0x38,{'MaterialInterface':0}),
 'FSkeletalMaterial':(0x30,{'MaterialInterface':0}),
 'UChildActorComponent':(0x2b0,{'ChildActorClass':0x250,'ChildActor':0x258,'ChildActorTemplate':0x260}),
 'UMassEntityConfigAsset':(0x60,{'Config':0x30}),
 'FMassEntityConfig':(0x30,{'Parent':0,'Traits':8}),
 'UMassVisualizationTrait':(0x160,{'StaticMeshInstanceDesc':0x30,'HighResTemplateActor':0xd0,'LowResTemplateActor':0xd8}),
 'UCrMassRepVisCosmeticsTrait':(0xf0,{'StaticMeshInstanceDesc':0x30,'HighResTemplateActor':0xd0,'LowResTemplateActor':0xd8}),
 'FStaticMeshInstanceVisualizationDesc':(0xa0,{'Meshes':8,'bUseTransformOffset':0x18,'TransformOffset':0x20,'TransformOffsets':0x80}),
 'FMassStaticMeshInstanceVisualizationMeshDesc':(0xa0,{'LocalTransform':0,'Mesh':0x60,'MaterialOverrides':0x68,'MinLODSignificance':0x78,'MaxLODSignificance':0x7c}),
 'UAuActorPlacementData':(0x1f0,{'EntityType':0x120,'BuildingID':0x170}),
 'FAuAPMassSpawnedEntityType':(0x38,{'EntityConfigPtr':0x30}),
}
found={}
for name,(size,wanted) in contracts.items():
    needle=name.encode()+b'\0';at=0
    while True:
        at=tpi.find(needle,at)
        if at<0:break
        index=bisect.bisect_right(offsets,at)-1+first;at+=len(needle)
        b=record(index)
        if len(b)<20 or struct.unpack_from('<H',b)[0] not in (0x1504,0x1505) or struct.unpack_from('<H',b,4)[0]&0x80:continue
        actual,name_at=numeric(b,18)
        if b[name_at:].split(b'\0')[0]!=name.encode():continue
        fields=members(struct.unpack_from('<I',b,6)[0])
        assert size is None or actual==size,(name,'size',hex(actual),hex(size))
        for field,offset in wanted.items():assert fields[field][0]==offset,(name,field,fields[field],offset)
        found[name]=(index,fields)
        print(f'PDB {name} type=0x{index:x} size=0x{actual:x}: '+', '.join(f'{k}=+0x{v:x}' for k,v in wanted.items()))
        break
    assert name in found,('missing complete PDB type',name)

# Raw TObjectPtr contract must be a native pointer in THIS PDB, not encoded handles.
handle_type=found['FObjectPtr'][1]['Handle'][1]
assert struct.unpack_from('<H',record(handle_type))[0]==0x1002

def complete(index):
    b=record(index)
    if struct.unpack_from('<H',b)[0]==0x1001:return complete(struct.unpack_from('<I',b,2)[0])
    assert struct.unpack_from('<H',b)[0] in (0x1504,0x1505)
    if not struct.unpack_from('<H',b,4)[0]&0x80:return index
    _,at=numeric(b,18);name=b[at:].split(b'\0')[0];needle=name+b'\0';at=0
    while True:
        at=tpi.find(needle,at)
        assert at>=0,('no complete type',name)
        candidate=bisect.bisect_right(offsets,at)-1+first;at+=len(needle);c=record(candidate)
        if struct.unpack_from('<H',c)[0] not in (0x1504,0x1505) or struct.unpack_from('<H',c,4)[0]&0x80:continue
        _,name_at=numeric(c,18)
        if c[name_at:].split(b'\0')[0]==name:return candidate

array_fields=[('UBlueprintGeneratedClass','ComponentTemplates'),('USimpleConstructionScript','RootNodes'),
 ('USimpleConstructionScript','AllNodes'),('USCS_Node','ChildNodes'),('UInheritableComponentHandler','Records'),
 ('UMeshComponent','OverrideMaterials'),('UStaticMesh','StaticMaterials'),('USkeletalMesh','Materials'),
 ('FMassEntityConfig','Traits'),('FStaticMeshInstanceVisualizationDesc','Meshes'),
 ('FStaticMeshInstanceVisualizationDesc','TransformOffsets'),('FMassStaticMeshInstanceVisualizationMeshDesc','MaterialOverrides')]
def allocator_data(index,depth=0):
    assert depth<8
    index=complete(index);field_index=struct.unpack_from('<I',record(index),6)[0];fields=members(field_index)
    if 'Data' in fields:return fields['Data']
    for offset,typ in base_types.get(field_index,[]):
        if offset==0:return allocator_data(typ,depth+1)
    raise AssertionError(('allocator Data field not verified',hex(index)))
for owner,field in array_fields:
    array_type=complete(found[owner][1][field][1]);b=record(array_type);size,_=numeric(b,18)
    fields=members(struct.unpack_from('<I',b,6)[0])
    assert size==16 and fields['AllocatorInstance'][0]==0 and fields['ArrayNum'][0]==8 and fields['ArrayMax'][0]==12,(owner,field,fields)
    allocator=complete(fields['AllocatorInstance'][1]);allocator_size,_=numeric(record(allocator),18)
    assert allocator_size==8
    data_offset,data_type=allocator_data(allocator)
    assert data_offset==0 and struct.unpack_from('<H',record(data_type))[0]==0x1002
    print(f'PDB array {owner}.{field}: size=0x10 allocator=+0 num=+8 max=+c; allocator size=8')

bits={'UActorComponent':{'bRegistered':0},'USceneComponent':{'bAbsoluteLocation':2,'bAbsoluteRotation':3,'bAbsoluteScale':4,'bVisible':5,'bHiddenInGame':3}}
for owner,fields in bits.items():
    for field,bit in fields.items():
        b=record(found[owner][1][field][1])
        assert struct.unpack_from('<H',b)[0]==0x1205 and b[6]==1 and b[7]==bit,(owner,field,b.hex())
print('PDB component flag bit positions verified')

struct_fields=struct.unpack_from('<I',record(found['UStruct'][0]),6)[0]
assert any(offset==0x30 and complete(typ)==found['FStructBaseChain'][0] for offset,typ in base_types[struct_fields])
print('PDB UStruct FStructBaseChain base=+0x30; installed fixed getter class-array preflight supported')

enum_contracts={'EInternalObjectFlags':{'Unreachable':0x10000000,'Garbage':0x00200000},
 'EObjectFlags':{'RF_ClassDefaultObject':0x10,'RF_NeedLoad':0x400,'RF_NeedPostLoad':0x1000,
                'RF_NeedPostLoadSubobjects':0x2000,'RF_BeginDestroyed':0x8000,'RF_FinishDestroyed':0x10000}}
for name,wanted in enum_contracts.items():
    needle=name.encode()+b'\0';at=0;verified=False
    while True:
        at=tpi.find(needle,at)
        if at<0:break
        index=bisect.bisect_right(offsets,at)-1+first;at+=len(needle);b=record(index)
        if len(b)<16 or struct.unpack_from('<H',b)[0]!=0x1507 or b[14:].split(b'\0')[0]!=name.encode():continue
        enum_fields=record(struct.unpack_from('<I',b,10)[0]);pos=2;values={}
        while pos<len(enum_fields):
            if enum_fields[pos]>=0xf0:pos+=enum_fields[pos]&15;continue
            assert struct.unpack_from('<H',enum_fields,pos)[0]==0x1502
            value,pos=numeric(enum_fields,pos+4);end=enum_fields.index(0,pos)
            values[enum_fields[pos:end].decode()]=value;pos=end+1
        for key,value in wanted.items():assert values[key]==value,(name,key,values[key],value)
        print(f'PDB {name} identity/lifetime flags verified: '+', '.join(f'{k}=0x{v:x}' for k,v in wanted.items()))
        verified=True;break
    assert verified,('missing flag enum',name)

getter_fields=struct.unpack_from('<I',record(found['USkeletalMeshComponent'][0]),6)[0]
getter=record(method_types[(getter_fields,'GetSkeletalMeshAsset')])
assert struct.unpack_from('<H',getter)[0]==0x1009
ret,cls,this,call,attr,argc,args,adjust=struct.unpack_from('<IIIBBHIi',getter,2)
assert call==0 and argc==0 and adjust==0
ret_record=record(ret)
assert struct.unpack_from('<H',ret_record)[0]==0x1002
returned=record(complete(struct.unpack_from('<I',ret_record,2)[0]));_,at=numeric(returned,18)
assert returned[at:].split(b'\0')[0]==b'USkeletalMesh'
print('PDB GetSkeletalMeshAsset: native x64 member, no arguments, USkeletalMesh* return (RAX), no sret ownership')

patterns_path=pathlib.Path(__file__).parent.parent/'Src/Experiments/RepresentationMetadataPatterns.h'
patterns=re.findall(r'char\s+(\w+)\[\]="([0-9A-F? ]+)"',patterns_path.read_text())
text=next(x for x in section_list if x[0]=='.text')
text_bytes=image[text[3]:text[3]+text[4]]
references={'ObjectArrayAnchor':0x17b2740,'SkeletalAsset':0x48f6190}
exception_rva,exception_size=struct.unpack_from('<II',image,optional+112+3*8)
exception_data=data(exception_rva,exception_size)
starts={struct.unpack_from('<I',exception_data,i)[0] for i in range(0,len(exception_data),12)}
for name,pattern in patterns:
    values=pattern.split();literal=max(re.split(r'(?:\?\? ?)+',pattern),key=lambda s:len(s.split())).strip()
    literal_values=literal.split();literal_at=next(i for i in range(len(values)) if values[i:i+len(literal_values)]==literal_values)
    anchor=bytes.fromhex(literal);hits=[];at=0
    while True:
        at=text_bytes.find(anchor,at)
        if at<0:break
        start=at-literal_at;at+=1
        if start<0 or start+len(values)>len(text_bytes):continue
        if all(v=='??' or text_bytes[start+i]==int(v,16) for i,v in enumerate(values)):hits.append(text[1]+start)
    assert hits==[references[name]],(name,hits)
    assert hits[0] in starts,(name,'not unwind function start')
    print(f'PE unique masked signature {name}: RVA=0x{hits[0]:08x}; unwind start verified')
anchor=references['ObjectArrayAnchor']
assert data(anchor+0x15,3)==b'\x48\x8d\x0d'
target=anchor+0x1c+struct.unpack('<i',data(anchor+0x18,4))[0]
assert target==0xe359230
print(f'PE GUObjectArray RIP target RVA=0x{target:08x}; identity validation uses targeted slot equality')
print('PE/PDB GUID and age match; direct read contracts and optional signatures passed')
