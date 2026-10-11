"""Offline CL-127004 passive loading-screen read ABI gate. No game calls."""
import pathlib,struct
reader=pathlib.Path(__file__).with_name('VerifyStaticAssemblyCaptureAbi.py').read_text()
exec(reader.split("assert fields('UStaticMesh')")[0])
def typename(ix):
 b=record(ix);assert struct.unpack_from('<H',b)[0] in (0x1504,0x1505)
 _,at=numeric(b,18);return b[at:].split(b'\0')[0].decode()
def inherited(ix):
 _,fl,f=fields(typename(ix));result=dict(f)
 for off,base in base_types.get(fl,[]):
  result.update({k:(off+v[0],v[1]) for k,v in inherited(base).items()})
 return result
assert fields('UWorld')[2]['OwningGameInstance'][0]==0x230
assert fields('UGameInstance')[2]['SubsystemCollection'][0]==0x130
sz,_,f=fields('FSubsystemCollectionBase')
assert sz==0xc0 and {k:f[k][0] for k in ['SubsystemMap','Outer','bPopulating','bIterating']}=={'SubsystemMap':8,'Outer':0xb0,'bPopulating':0xb8,'bIterating':0xb9}
pairs=inherited(f['SubsystemMap'][1])['Pairs'];assert pairs[0]==0
setfields=inherited(pairs[1]);assert setfields['Elements'][0]==0
sparse=inherited(setfields['Elements'][1]);assert sparse['Data'][0]==0 and sparse['AllocationFlags'][0]==0x10
array=inherited(sparse['Data'][1]);assert array['ArrayNum'][0]==8 and array['ArrayMax'][0]==12
bit=inherited(sparse['AllocationFlags'][1]);assert bit['NumBits'][0]==0x18 and bit['MaxBits'][0]==0x1c
inline=inherited(bit['AllocatorInstance'][1]);assert inline['InlineData'][0]==0 and inline['SecondaryData'][0]==0x10
ix,_=find_type('TSetElement<TTuple<TObjectPtr<UClass>,TObjectPtr<USubsystem> > >',(0x1504,0x1505));size,_,_=fields(typename(ix));element=inherited(ix)
assert size==24 and element['Value'][0]==0
pair=inherited(element['Value'][1]);assert sorted(v[0] for v in pair.values())==[0,8],pair
size,_,manager=fields('ULoadingScreenManager');assert size==0xc8
for name,offset in [('bCurrentlyInLoadMap',0xb0),('bCurrentlyShowingLoadingScreen',0xb1)]:
 assert manager[name]==(offset,0x30),(name,manager[name]) # bool, not a bitfield
print('Startup readiness ABI: matching PE/PDB; world/game-instance ownership, bounded sparse map stride/flags, exact loading manager bools verified')

