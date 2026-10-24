#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingBlueprintIdentity.h"
#include "Native/NativeApi.h"
#include "plugin_helpers.h"
#include <SDK/AuActorPlacement_classes.hpp>
#include <SDK/Engine_parameters.hpp>
#include <Windows.h>
namespace
{
    SDK::UObject* library=nullptr;
    SDK::UFunction* conversion=nullptr;
}
namespace MiniMapBuildingBlueprintIdentity
{
    bool Initialize()
    {
        if(library && conversion)return true;
        const auto* n=MiniMapNative::Get();auto* hooks=GetHooks();
        if(!n || !hooks || !hooks->Engine || !hooks->ObjectWalker || !hooks->Memory)return false;
        const auto address=hooks->Engine->GetStaticFindObjectByNameAddress();
        if(!address)return false;
        using Find=SDK::UObject*(*)(SDK::UClass*,SDK::UObject*,const wchar_t*,bool);
        const auto find=reinterpret_cast<Find>(address);
        auto* type=static_cast<SDK::UClass*>(find(nullptr,nullptr,L"/Script/Engine.KismetSystemLibrary",false));
        auto* functionType=static_cast<SDK::UClass*>(find(nullptr,nullptr,L"/Script/CoreUObject.Function",false));
        auto* function=type && functionType?static_cast<SDK::UFunction*>(find(functionType,type,L"Conv_SoftClassReferenceToString",true)):nullptr;
        if(!type || !type->ClassDefaultObject || !function)return false;
        n->asset.retainObject(type->ClassDefaultObject);n->asset.retainObject(function);
        library=type->ClassDefaultObject;conversion=function;return true;
    }
    void Shutdown()
    {
        if(const auto* n=MiniMapNative::Get())
        {if(conversion)n->asset.releaseObject(conversion);if(library)n->asset.releaseObject(library);}
        conversion=nullptr;library=nullptr;
    }
    std::shared_ptr<const std::string> Copy(const SDK::UAuActorPlacementData& definition)
    {
        if(!library || !conversion)return {};
        using Params=SDK::Params::KismetSystemLibrary_Conv_SoftClassReferenceToString;
        Params params{};params.SoftClassReference=definition.ActorClass;
        auto* hooks=GetHooks();
        // The engine owns the input soft path; the returned FString owns a new
        // allocation and must be freed even if UTF-8 allocation throws.
        struct StringOwner {Params& P;IPluginMemoryUtils* Memory;~StringOwner(){if(P.ReturnValue.CStr())Memory->Free(P.ReturnValue.CStr());}} owner{params,hooks->Memory};
        if(!hooks->ObjectWalker->InvokeResolvedUFunction(library,conversion,&params))return {};
        const auto& value=params.ReturnValue;const auto count=value.Num();
        if(count<=1 || value.Max()<count || !value.CStr() || value.CStr()[count-1]!=0)return {};
        const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.CStr(),count-1,nullptr,0,nullptr,nullptr);
        if(size<=0)return {};
        std::string path(size_t(size),'\0');
        if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.CStr(),count-1,path.data(),size,nullptr,nullptr)!=size || path=="None")return {};
        return std::make_shared<const std::string>(std::move(path));
    }
}
#endif
