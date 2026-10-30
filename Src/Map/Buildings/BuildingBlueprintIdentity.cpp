#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingBlueprintIdentity.h"
#include "Native/NativeApi.h"
#include "Services/Objects.h"
#include "Services/Runtime.h"
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
        const auto* n=MiniMapNative::Get();auto* objects=MiniMap::Services::TryObjects();
        if(!n || !objects || !objects->Ready())return false;
        const auto typeResult=objects->FindResident(nullptr,nullptr,L"/Script/Engine.KismetSystemLibrary",false);
        auto* type=typeResult.value?static_cast<SDK::UClass*>(*typeResult.value):nullptr;
        const auto functionTypeResult=objects->FindResident(nullptr,nullptr,L"/Script/CoreUObject.Function",false);
        auto* functionType=functionTypeResult.value?static_cast<SDK::UClass*>(*functionTypeResult.value):nullptr;
        const auto functionResult=type && functionType
            ?objects->FindResident(functionType,type,L"Conv_SoftClassReferenceToString",true)
            :MiniMap::Services::Result<SDK::UObject*>{};
        auto* function=functionResult.value?static_cast<SDK::UFunction*>(*functionResult.value):nullptr;
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
        auto* objects=MiniMap::Services::TryObjects();
        if(!library || !conversion || !objects)return {};
        using Params=SDK::Params::KismetSystemLibrary_Conv_SoftClassReferenceToString;
        Params params{};params.SoftClassReference=definition.ActorClass;
        // The engine owns the input soft path; the returned FString owns a new
        // allocation and must be freed even if UTF-8 allocation throws.
        struct StringOwner
        {
            Params& P;
            MiniMap::Services::IObjects& Objects;
            ~StringOwner()
            {
                if (auto* value=P.ReturnValue.CStr())
                {
                    const auto status=Objects.FreeEngineAllocation(const_cast<wchar_t*>(value));
                    if (status.error!=MiniMap::Services::Error::None)
                        LOG_ERROR("MiniMap: soft-class result release status=%u", unsigned(status.error));
                }
            }
        } owner{params,*objects};
        const auto invoke=objects->Invoke(library,conversion,&params);
        if(invoke.error!=MiniMap::Services::Error::None)return {};
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
