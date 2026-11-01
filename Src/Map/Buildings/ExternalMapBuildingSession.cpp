#if defined(MODLOADER_CLIENT_BUILD)
#include "ExternalMapBuildingSession.h"
#include "BuildingAtlas.h"
#include "BuildingScheduler.h"
#include "Native/ResidentMetadata.h"
#include "plugin_helpers.h"
#include <algorithm>
#include <stdexcept>
#include "CapturePng.h"
#include "Map/Map.h"
#include "Native/NativeApi.h"
#include "SDK/Engine_classes.hpp"
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

namespace MiniMapBuildingCollector
{
    namespace
    {
        // Submission adapter only. Geometry, sprite UVs, and painter ordering
        // come from BuildingVisuals. Clip final triangles with interpolated UVs.
        class NativeCanvas final : public MiniMap::Services::ICanvas
        {
            using V=MiniMap::Services::Vec2;
            using L=MiniMap::Services::DrawLayer;
            using T=MiniMap::Services::Texture;
            using Q=MiniMap::Services::Quad;
            struct Vertex { V P,UV; };
            struct Draw { SDK::FCanvasUVTri Triangle; bool Sprite; };
            std::vector<Draw> draws_;
            V size_;
            bool record_;
            bool failed_=false;
            void TriangleData(const std::array<V,3>& positions,const std::array<V,3>& uv,uint32_t color,bool sprite)
            {
                if(!record_)return;
                std::array<Vertex,12> input{},output{};
                size_t count=3;
                for(size_t i=0;i<3;++i)input[i]={positions[i],uv[i]};
                for(int edge=0;edge<4 && count;++edge)
                {
                    const bool x=edge<2; const float boundary=(edge==1?size_.x:edge==3?size_.y:0.f);
                    const auto distance=[&](const Vertex& v) {const float d=(x?v.P.x:v.P.y)-boundary;return (edge==0 || edge==2)?d:-d;};
                    size_t next=0;
                    for(size_t i=0;i<count;++i)
                    {
                        const auto& a=input[i];const auto& b=input[(i+1)%count];
                        const float da=distance(a),db=distance(b);
                        if(da>=0)output[next++]=a;
                        if((da>=0)!=(db>=0))
                        {
                            const float t=da/(da-db);
                            output[next++]={{a.P.x+t*(b.P.x-a.P.x),a.P.y+t*(b.P.y-a.P.y)},
                                {a.UV.x+t*(b.UV.x-a.UV.x),a.UV.y+t*(b.UV.y-a.UV.y)}};
                        }
                    }
                    input=output;count=next;
                }
                const SDK::FLinearColor tint{float(color&255)/255.f,float((color>>8)&255)/255.f,
                    float((color>>16)&255)/255.f,float(color>>24)/255.f};
                for(size_t i=1;i+1<count;++i)
                {
                    const auto& a=input[0];const auto& b=input[i];const auto& c=input[i+1];
                    SDK::FCanvasUVTri tri{};
                    tri.V0_Pos={a.P.x,a.P.y};tri.V1_Pos={b.P.x,b.P.y};tri.V2_Pos={c.P.x,c.P.y};
                    tri.V0_UV={a.UV.x,a.UV.y};tri.V1_UV={b.UV.x,b.UV.y};tri.V2_UV={c.UV.x,c.UV.y};
                    tri.V0_Color=tri.V1_Color=tri.V2_Color=tint;
                    draws_.push_back({tri,sprite});
                }
            }
            [[noreturn]] static void Unsupported(){throw std::logic_error("external building submission operation");}
        public:
            explicit NativeCanvas(V size,bool record):size_(size),record_(record){}
            V WindowPosition() const override{return {0,0};}
            V WindowSize() const override{return size_;}
            V ContentAvailable() const override{return size_;}
            void ReserveContent(V) override{Unsupported();}
            V MeasureText(std::string_view,bool,float) const override{Unsupported();}
            void PushClip(L,V,V,bool) override{} // Shared pass clip is exactly the destination rectangle.
            void PopClip(L) override{}
            void Image(L,T,V,V,V,V,uint32_t) override{Unsupported();}
            void ImageQuad(L,T,const Q& p,const Q& uv,uint32_t color) override
            {
                const auto start=draws_.size();
                try
                {
                    TriangleData({p[0],p[1],p[2]},{uv[0],uv[1],uv[2]},color,true);
                    TriangleData({p[0],p[2],p[3]},{uv[0],uv[2],uv[3]},color,true);
                }
                catch(...) {draws_.resize(start);failed_=true;throw;}
            }
            void FilledTriangle(L,const std::array<V,3>& p,uint32_t color) override
            {
                const auto start=draws_.size();
                try {TriangleData(p,{{{0.5f,0.5f},{0.5f,0.5f},{0.5f,0.5f}}},color,false);}
                catch(...) {draws_.resize(start);failed_=true;throw;}
            }
            void FilledRect(L,V,V,uint32_t,float) override{Unsupported();}
            void Rect(L,V,V,uint32_t,float,float) override{Unsupported();}
            void FilledCircle(L,V,float,uint32_t,int) override{Unsupported();}
            void Triangle(L,const std::array<V,3>&,uint32_t,float) override{Unsupported();}
            void Text(L,V,uint32_t,std::string_view) override{Unsupported();}
            bool Failed() const {return failed_;}
            bool NeedsAtlas() const
            {return std::any_of(draws_.begin(),draws_.end(),[](const Draw& d){return d.Sprite;});}
            void Submit(const MiniMapBuildingRenderRequest& request)
            {
                // Adjacent operations only: never reorder ordinary/spline/Base Core passes.
                std::vector<SDK::FCanvasUVTri> batch;
                for(size_t first=0;first<draws_.size();)
                {
                    const bool sprite=draws_[first].Sprite;batch.clear();
                    do { batch.push_back(draws_[first++].Triangle); }
                    while(first<draws_.size() && draws_[first].Sprite==sprite && batch.size()<4096);
                    const SDK::TArray<SDK::FCanvasUVTri> borrowed(batch.data(),static_cast<int32_t>(batch.size()),static_cast<int32_t>(batch.size()));
                    request.Canvas->K2_DrawMaterialTriangle(sprite?request.SpriteMaterial:request.SolidMaterial,borrowed);
                }
            }
        };

        bool ValidWorldRectangle(const Bounds& rectangle)
        {
            if(!rectangle.Valid || !std::isfinite(rectangle.MinX) || !std::isfinite(rectangle.MinY) ||
                !std::isfinite(rectangle.MaxX) || !std::isfinite(rectangle.MaxY) ||
                rectangle.MinX>=rectangle.MaxX || rectangle.MinY>=rectangle.MaxY)return false;
            const double width=rectangle.MaxX-rectangle.MinX;
            const double height=rectangle.MaxY-rectangle.MinY;
            return std::isfinite(width) && std::isfinite(height) && std::isfinite(std::hypot(width,height));
        }
    }

    ExternalMapBuildingSession::~ExternalMapBuildingSession(){Shutdown();}

    bool ExternalMapBuildingSession::IsCurrentAdmission(SDK::UWorld*& world,uint64_t& worldGeneration) const
    {
        world=nullptr;worldGeneration=0;
        if(stopping_ || !MiniMapMap::GameplayReady() || !MiniMapResidentMetadata::Available())return false;
        const auto* native=MiniMapNative::Get();
        world=MiniMapMap::GetWorld();
        if(!world || !native || !native->player.isGameThread())return false;
        worldGeneration=MiniMapMap::GetWorldGeneration();
        return true;
    }

    bool ExternalMapBuildingSession::OpenSession(const Bounds& worldRectangle)
    {
        if(active_ || collector_ || !ValidWorldRectangle(worldRectangle))return false;
        SDK::UWorld* world=nullptr;uint64_t worldGeneration=0;
        if(!IsCurrentAdmission(world,worldGeneration))return false;

        std::unique_ptr<BuildingCollector> candidate;
        std::shared_ptr<MiniMapBuildingVisuals::State> visualCandidate;
        try
        {
            visualCandidate=MiniMapBuildingVisuals::CreateState();
            candidate=std::make_unique<BuildingCollector>();
            if(!candidate->InitializeResources() || !candidate->BeginExplicitSession())
            {
                candidate->ShutdownResources();
                return false;
            }
            candidate->SetRegion(worldRectangle);
        }
        catch(...)
        {
            if(candidate)candidate->ShutdownResources();
            return false;
        }

        SDK::UWorld* currentWorld=nullptr;uint64_t currentGeneration=0;
        if(!IsCurrentAdmission(currentWorld,currentGeneration) || currentWorld!=world || currentGeneration!=worldGeneration)
        {
            candidate->ShutdownResources();
            return false;
        }
        visuals_=std::move(visualCandidate);
        collector_=std::move(candidate);
        admittedWorld_=world;
        admittedWorldGeneration_=worldGeneration;
        active_=true;
        worldInvalidated_=false;
        ++sessionGeneration_;
        return true;
    }

    bool ExternalMapBuildingSession::UpdateWorldRectangle(const Bounds& worldRectangle)
    {
        if(!active_ || !collector_ || !ValidWorldRectangle(worldRectangle))return false;
        const auto* native=MiniMapNative::Get();
        if(!native || !native->player.isGameThread())return false;
        SDK::UWorld* world=nullptr;uint64_t worldGeneration=0;
        if(!IsCurrentAdmission(world,worldGeneration) || world!=admittedWorld_ || worldGeneration!=admittedWorldGeneration_)
        {
            InvalidateOnWorldEnd();
            return false;
        }
        collector_->SetRegion(worldRectangle);
        return true;
    }

    void ExternalMapBuildingSession::Service(float delta)
    {
        if(!active_ || !collector_)return;
        const auto* native=MiniMapNative::Get();
        if(!native || !native->player.isGameThread())return;
        SDK::UWorld* world=nullptr;uint64_t worldGeneration=0;
        if(!IsCurrentAdmission(world,worldGeneration) || world!=admittedWorld_ || worldGeneration!=admittedWorldGeneration_)
        {
            InvalidateOnWorldEnd();
            return;
        }
        collector_->SetRenderDroneRails(MiniMapConfig::GetRenderSettings().Buildings.RenderDroneRails);
        collector_->OnTick(delta);
    }

    std::shared_ptr<const Snapshot> ExternalMapBuildingSession::GetSnapshot()
    {
        if(!active_ || !collector_)return {};
        const auto* native=MiniMapNative::Get();
        if(!native || !native->player.isGameThread())return {};
        SDK::UWorld* world=nullptr;uint64_t worldGeneration=0;
        if(!IsCurrentAdmission(world,worldGeneration) || world!=admittedWorld_ || worldGeneration!=admittedWorldGeneration_)
        {
            InvalidateOnWorldEnd();
            return {};
        }
        return collector_->GetSnapshot();
    }

    SDK::UTexture2D* ExternalMapBuildingSession::SynchronizeNativeAtlasTexture()
    {
        SDK::UWorld* world=nullptr;uint64_t worldGeneration=0;
        if(!IsCurrentAdmission(world,worldGeneration))return nullptr;
        const auto* native=MiniMapNative::Get();
        if(!native || !native->asset.retainObject || !native->asset.releaseObject)return nullptr;

        uint64_t atlasSession=0,atlasRevision=0;
        {
            std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
            MiniMapBuildingAtlas::ObservePublication(atlasSession,atlasRevision);
        }
        if(!atlasSession || !atlasRevision)return nullptr;
        if(rejectedAtlasSession_==atlasSession && rejectedAtlasRevision_==atlasRevision)return nullptr;
        const auto image=MiniMapBuildingAtlas::PublishedImage();
        if(!image || image->Revision!=atlasRevision)return nullptr;
        if(nativeAtlasTexture_ && nativeAtlasSession_==atlasSession && nativeAtlasRevision_==atlasRevision)
            return nativeAtlasTexture_;

        // Latch before validation/encoding/import so every failure, including an
        // allocation exception, is attempted and reported only once per publication.
        // This never advances the successfully uploaded identity or returns an old
        // texture for new UVs. A later published session/revision may try again.
        rejectedAtlasSession_=atlasSession;
        rejectedAtlasRevision_=atlasRevision;
        try
        {
            const auto width=static_cast<int32_t>(image->Width);
            const auto height=static_cast<int32_t>(image->Height);
            if(width<=0 || height<=0 || width>4096 || height>4096 ||
                image->Rgba.size()!=size_t(width)*size_t(height)*4)
            {
                LOG_ERROR("MiniMap: native atlas published image invalid: session=%llu revision=%llu size=%dx%d",
                    static_cast<unsigned long long>(atlasSession),static_cast<unsigned long long>(atlasRevision),width,height);
                return nullptr;
            }

            std::vector<uint8_t> encoded;
            const auto encode=MiniMapCapturePng::Encode(static_cast<UINT>(width),static_cast<UINT>(height),image->Rgba,false,encoded);
            if(!encode.Ok || encoded.empty() || encoded.size()>static_cast<size_t>((std::numeric_limits<int32_t>::max)()))
            {
                LOG_ERROR("MiniMap: native atlas PNG failed: %s (0x%08lx)",encode.Stage,static_cast<unsigned long>(encode.Error));
                return nullptr;
            }

            SDK::TArray<SDK::uint8> buffer(encoded.data(),static_cast<int32_t>(encoded.size()),static_cast<int32_t>(encoded.size()));
            SDK::UTexture2D* replacement=SDK::UKismetRenderingLibrary::ImportBufferAsTexture2D(world,buffer);
            if(!replacement)
            { LOG_ERROR("MiniMap: native atlas PNG import failed");return nullptr; }
            const int32_t actualWidth=replacement->Blueprint_GetSizeX(),actualHeight=replacement->Blueprint_GetSizeY();
            // TF_Default uses the texture LOD group's sampler; it is not an
            // invalid filter. The importer creates a single mip. Atlas content
            // UVs lie inside replicated gutters, so wrap and clamp are usable.
            const bool compatibleFilter=replacement->Filter==SDK::ETextureFilter::TF_Default ||
                replacement->Filter==SDK::ETextureFilter::TF_Bilinear || replacement->Filter==SDK::ETextureFilter::TF_Trilinear;
            const auto compatibleAddress=[](SDK::ETextureAddress address)
            {return address==SDK::ETextureAddress::TA_Wrap || address==SDK::ETextureAddress::TA_Clamp;};
            if(actualWidth!=width || actualHeight!=height || !replacement->SRGB || !compatibleFilter ||
                !compatibleAddress(replacement->AddressX) || !compatibleAddress(replacement->AddressY))
            {
                LOG_ERROR("MiniMap: native atlas incompatible: size=%dx%d expected=%dx%d sRGB=%d filter=%u address=%u/%u",
                    actualWidth,actualHeight,width,height,int(replacement->SRGB),unsigned(replacement->Filter),
                    unsigned(replacement->AddressX),unsigned(replacement->AddressY));
                return nullptr;
            }

            // Keep the initialized importer resource and sampler unchanged.
            native->asset.retainObject(reinterpret_cast<SDK::UObject*>(replacement));
            auto* previous=nativeAtlasTexture_;
            nativeAtlasTexture_=replacement;
            nativeAtlasSession_=atlasSession;
            nativeAtlasRevision_=atlasRevision;
            nativeAtlasWidth_=width;
            nativeAtlasHeight_=height;
            rejectedAtlasSession_=rejectedAtlasRevision_=0;
            if(previous)native->asset.releaseObject(reinterpret_cast<SDK::UObject*>(previous));
            return nativeAtlasTexture_;
        }
        catch(...)
        {
            LOG_ERROR("MiniMap: native atlas synchronization failed: session=%llu revision=%llu",
                static_cast<unsigned long long>(atlasSession),static_cast<unsigned long long>(atlasRevision));
            return nullptr;
        }
    }

    ExternalMapSessionDiagnostics ExternalMapBuildingSession::GetDiagnostics() const
    {
        ExternalMapSessionDiagnostics result{};
        result.SessionGeneration=sessionGeneration_;
        result.Active=active_ && collector_!=nullptr;
        SDK::UWorld* world=nullptr;uint64_t worldGeneration=0;
        result.Admitted=result.Active && IsCurrentAdmission(world,worldGeneration) &&
            world==admittedWorld_ && worldGeneration==admittedWorldGeneration_;
        result.WorldInvalidated=worldInvalidated_;
        if(result.Admitted && collector_)result.Collector=collector_->GetDiagnostics();
        return result;
    }

    bool ExternalMapBuildingSession::CloseSession()
    {
        if(!collector_ && !nativeAtlasTexture_)return true;
        // World-end closes gameplay admission before teardown. Quiet GT/GC
        // admission, independent of GameplayReady, is required for native release.
        if(!MiniMapResidentMetadata::Available())return false;
        if(collector_) {collector_->ShutdownResources();collector_.reset();}
        if(nativeAtlasTexture_)
        {
            MiniMapNative::Get()->asset.releaseObject(reinterpret_cast<SDK::UObject*>(nativeAtlasTexture_));
            nativeAtlasTexture_=nullptr;
        }
        visuals_.reset();renderedSnapshot_.reset();renderedDestination_=nullptr;rendered_=false;
        nativeAtlasSession_=nativeAtlasRevision_=renderedAtlasSession_=renderedAtlasRevision_=0;
        nativeAtlasWidth_=nativeAtlasHeight_=0;
        rejectedAtlasSession_=rejectedAtlasRevision_=0;
        admittedWorld_=nullptr;admittedWorldGeneration_=0;
        if(active_)++sessionGeneration_;
        active_=false;
        return true;
    }

    MiniMapBuildingAbiResult ExternalMapBuildingSession::Render(const MiniMapBuildingRenderRequest& request,
        MiniMapBuildingRenderStatus& status)
    {
        const auto& r=request.Rectangle;
        const Bounds rectangle{r.MinX,r.MinY,r.MaxX,r.MaxY,true};
        if(!ValidWorldRectangle(rectangle) || !std::isfinite(request.DeltaSeconds) || request.DeltaSeconds<0 ||
            !request.World || !request.Destination || !request.Canvas || !request.SpriteMaterial ||
            !request.SolidMaterial || request.SpriteMaterial==request.SolidMaterial)
            return MINIMAP_BUILDING_ABI_INVALID_ARGUMENT;
        SDK::UWorld* world=nullptr;uint64_t generation=0;
        if(!IsCurrentAdmission(world,generation) || request.World!=world)return MINIMAP_BUILDING_ABI_UNAVAILABLE;
        if(active_ && (worldInvalidated_ || admittedWorld_!=world || admittedWorldGeneration_!=generation))
        {if(!CloseSession())return MINIMAP_BUILDING_ABI_UNAVAILABLE;}
        if(!active_ && !OpenSession(rectangle))return MINIMAP_BUILDING_ABI_REJECTED;
        status.Flags=MINIMAP_BUILDING_STATUS_ACTIVE;
        if(!UpdateWorldRectangle(rectangle))return MINIMAP_BUILDING_ABI_UNAVAILABLE;
        Service(request.DeltaSeconds);
        auto input=GetSnapshot();
        if(!active_)return MINIMAP_BUILDING_ABI_UNAVAILABLE;
        const auto diagnostics=GetDiagnostics();
        status.PendingAcquisitionCount=diagnostics.Collector.PendingAcquisitionCount;
        status.PublishedInRegionCount=diagnostics.Collector.PublishedInRegionCount;
        if(diagnostics.Collector.MembershipComplete)status.Flags|=MINIMAP_BUILDING_STATUS_MEMBERSHIP_COMPLETE;
        // The existing scheduler services completed draw batches, cache reads,
        // capture feedback and CPU/UI atlas publication. No new capture owner.
        MiniMapBuildingScheduler::Service();
        const auto settings=MiniMapConfig::GetRenderSettings().Buildings;
        const float width=static_cast<float>(request.Destination->SizeX),height=static_cast<float>(request.Destination->SizeY);
        if(width<=0 || height<=0)return MINIMAP_BUILDING_ABI_INVALID_ARGUMENT;
        MiniMapMap::Transform transform{};
        transform.Valid=true;transform.Width=width;transform.Height=height;
        transform.CenterX=width/2;transform.CenterY=height/2;
        transform.TrackingWorldX=r.MinX+(r.MaxX-r.MinX)/2;
        transform.TrackingWorldY=r.MinY+(r.MaxY-r.MinY)/2;
        transform.PixelsPerWorldUnit=width/(r.MaxX-r.MinX);
        transform.PixelsPerWorldUnitY=height/(r.MaxY-r.MinY);
        uint64_t atlasSession=0,atlasRevision=0;
        {
            std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
            MiniMapBuildingAtlas::ObservePublication(atlasSession,atlasRevision);
        }
        const bool same=rendered_ && renderedSnapshot_==input && renderedDestination_==request.Destination &&
            renderedAtlasSession_==atlasSession && renderedAtlasRevision_==atlasRevision &&
            renderedRectangle_.MinX==r.MinX && renderedRectangle_.MinY==r.MinY &&
            renderedRectangle_.MaxX==r.MaxX && renderedRectangle_.MaxY==r.MaxY &&
            renderedShowLayer_==settings.ShowLayer && renderedDroneRails_==settings.RenderDroneRails;
        // Keep bounded scheduler discovery moving even on an unchanged image;
        // omit native geometry recording, clearing and submission in that case.
        NativeCanvas canvas({width,height},!same);
        if(input)
        {
            MiniMapBuildingVisuals::Prepare(*visuals_,input);
            MiniMapBuildingVisuals::RenderSelected(*visuals_,input,canvas,transform,settings);
        }
        if(canvas.Failed())
        { LOG_ERROR("MiniMap: native building geometry allocation failed");return MINIMAP_BUILDING_ABI_REJECTED; }
        if(!atlasRevision)status.Flags|=MINIMAP_BUILDING_STATUS_ATLAS_PENDING;
        if(same){status.Flags|=MINIMAP_BUILDING_STATUS_UNCHANGED;return MINIMAP_BUILDING_ABI_OK;}
        if(canvas.NeedsAtlas())
        {
            auto* texture=SynchronizeNativeAtlasTexture();
            if(!texture){status.Flags|=MINIMAP_BUILDING_STATUS_ATLAS_PENDING;return MINIMAP_BUILDING_ABI_UNAVAILABLE;}
            const auto name=SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(L"SpriteAtlas"));
            request.SpriteMaterial->SetTextureParameterValue(name,texture);
        }
        // Begin has queued deferred target preparation; no native draw has been
        // submitted yet. Queue the clear before Submit's immediate canvas draws.
        // MainMap retains ownership of the matching Begin/End pass.
        SDK::UKismetRenderingLibrary::ClearRenderTarget2D(world,request.Destination,{0,0,0,1});
        canvas.Submit(request);
        renderedSnapshot_=input;renderedRectangle_=rectangle;renderedDestination_=request.Destination;
        renderedAtlasSession_=atlasSession;renderedAtlasRevision_=atlasRevision;
        renderedShowLayer_=settings.ShowLayer;renderedDroneRails_=settings.RenderDroneRails;rendered_=true;
        status.Flags|=MINIMAP_BUILDING_STATUS_DRAWN;
        return MINIMAP_BUILDING_ABI_OK;
    }

    void ExternalMapBuildingSession::InvalidateOnWorldEnd(){worldInvalidated_=active_;CloseSession();}
    void ExternalMapBuildingSession::Shutdown(){stopping_=true;if(!CloseSession())LOG_ERROR("MiniMap: external building cleanup not admitted; ownership retained");}
}
#endif
