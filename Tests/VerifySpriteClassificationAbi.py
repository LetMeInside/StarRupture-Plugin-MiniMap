"""Offline installed-layout gate for passive R2.6b.4 reads; no new native calls."""
import bisect
import pathlib
import runpy
import struct

v = runpy.run_path(str(pathlib.Path(__file__).with_name('VerifyRepresentationAbi.py')))
def type_name(index):
    if index < 0x1000:
        return {0x30:'bool',0x20:'unsigned char',0x70:'char',0x74:'int',0x75:'unsigned int'}.get(index,hex(index))
    b=v['record'](index);leaf=struct.unpack_from('<H',b)[0]
    if leaf in (0x1001,0x1002):
        return type_name(struct.unpack_from('<I',b,2)[0])+('*' if leaf==0x1002 else '')
    if leaf in (0x1504,0x1505):
        _,at=v['numeric'](b,18);return b[at:].split(b'\0')[0].decode()
    if leaf==0x1507:return b[14:].split(b'\0')[0].decode()
    return hex(index)
contracts = {
    'UAuActorPlacementData': (0x1f0, {'HelperActorClass':0x30, 'ActorClass':0x38,
        'CustomPlacementMethod':0xb0, 'bCanBePlacedOnTerrain':0xc1,
        'bHasToBePlacedOnTerrain':0xc2, 'EntityType':0x120, 'BuildingID':0x170,
        'RequireHabitat':0x1b0, 'RequiresBeingInsideHabitat':0x1b1, 'SplineBased':0x1b2,
        'CheckBaseCoreArea':0x1b3, 'CheckOutsideBaseCoreArea':0x1b4}),
    'UCrBuildingData': (0x460, {'UIType':0x2c1, 'UISubType':0x2c2, 'Tags':0x3b0, 'CanBeDynamic':0x458}),
    'FAuAPMassSpawnedEntityType': (0x38, {'EntityConfig':0, 'EntityConfigPtr':0x30}),
    'FWeakObjectPtr': (8, {'ObjectIndex':0, 'ObjectSerialNumber':4}),
    'FSoftObjectPath': (0x20, {'AssetPath':0, 'SubPathString':0x10}),
    'FUtf8String': (0x10, {'Data':0}),
    'FTopLevelAssetPath': (0x10, {'PackageName':0, 'AssetName':8}),
    'TPersistentObjectPtr<FSoftObjectPath>': (0x28, {'WeakPtr':0, 'ObjectID':8}),
    'FSoftObjectPtr': (0x28, {}),
    'TSoftClassPtr<AActor>': (0x28, {'SoftObjectPtr':0}),
    'TSoftObjectPtr<UMassEntityConfigAsset>': (0x28, {'SoftObjectPtr':0}),
    'TSubclassOf<AAuAPHelperActor>': (8, {'Class':0}),
    'TObjectPtr<UAuCustomPlacementMethod>': (8, {'ObjectPtr':0}),
    'TArray<enum FGenericPlatformTypes::UTF8CHAR,TSizedDefaultAllocator<32> >':
        (0x10, {'AllocatorInstance':0, 'ArrayNum':8, 'ArrayMax':0xc}),
}
for name, (size, wanted) in contracts.items():
    at = 0
    while True:
        at = v['tpi'].find(name.encode()+b'\0', at)
        assert at >= 0, ('missing type', name)
        index = bisect.bisect_right(v['offsets'], at)-1+v['first']
        at += len(name)+1
        b = v['record'](index)
        if len(b)<20 or struct.unpack_from('<H',b)[0] not in (0x1504,0x1505) or struct.unpack_from('<H',b,4)[0]&0x80:
            continue
        actual, start = v['numeric'](b,18)
        if b[start:].split(b'\0')[0] != name.encode():
            continue
        assert actual == size, (name,actual,size)
        field_list=struct.unpack_from('<I',b,6)[0]
        fields = v['members'](field_list)
        if name=='FSoftObjectPtr':
            bases=v['base_types'].get(field_list,[])
            assert len(bases)==1 and bases[0][0]==0 and type_name(bases[0][1])=='TPersistentObjectPtr<FSoftObjectPath>',bases
        for field, offset in wanted.items():
            assert fields[field][0] == offset, (name,field,fields[field],offset)
            kind=type_name(fields[field][1])
            print('  field type:',name,field,kind)
            if name=='FSoftObjectPath' and field=='SubPathString':
                assert kind=='FUtf8String',kind
            if name=='FUtf8String' and field=='Data':
                assert kind=='TArray<enum FGenericPlatformTypes::UTF8CHAR,TSizedDefaultAllocator<32> >',kind
            if name=='UCrBuildingData' and field in ('UIType','UISubType'):
                enum=v['record'](fields[field][1])
                assert struct.unpack_from('<H',enum)[0]==0x1507 and struct.unpack_from('<I',enum,6)[0]==0x20
            if name=='UAuActorPlacementData' and field in ('bCanBePlacedOnTerrain','bHasToBePlacedOnTerrain','RequireHabitat','RequiresBeingInsideHabitat','SplineBased','CheckBaseCoreArea','CheckOutsideBaseCoreArea'):
                assert kind=='bool',kind
        print('Classification PDB contract:',name,hex(index),hex(size),wanted)
        break
name=b'FGenericPlatformTypes::UTF8CHAR\0'
at=0
while True:
    at=v['tpi'].find(name,at);assert at>=0,'UTF8CHAR underlying type missing'
    index=bisect.bisect_right(v['offsets'],at)-1+v['first'];at+=len(name)
    b=v['record'](index)
    if len(b)<15 or struct.unpack_from('<H',b)[0]!=0x1507 or b[14:].split(b'\0')[0]+b'\0'!=name:continue
    assert struct.unpack_from('<I',b,6)[0]==0x20,'UTF8CHAR is not unsigned byte'
    print('Classification PDB UTF8CHAR: unsigned-byte underlying type verified')
    break
print('R2.6b.4: fixed/opaque reference reads verified; no weak Get or semantic-name callable added.')
