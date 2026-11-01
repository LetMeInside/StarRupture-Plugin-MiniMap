#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingComplexAbi.h"
#include "BuildingComplexApiInternal.h"
#include "BuildingComplexWorld.h"
#include "Native/TexturePublication.h"
#include "Native/NativeApi.h"
#include "Native/MassLifecycle.h"
#include "Native/ResidentMetadata.h"
#include "Map/Map.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <type_traits>
#include <vector>

#define BC_LAYOUT(T, S) static_assert(sizeof(T)==S); static_assert(alignof(T)==8)
BC_LAYOUT(BC_Key,16); BC_LAYOUT(BC_Rect,32); BC_LAYOUT(BC_Ref,24); BC_LAYOUT(BC_Open,64);
BC_LAYOUT(BC_Grid,96); BC_LAYOUT(BC_Demand,64); BC_LAYOUT(BC_Status,208);
BC_LAYOUT(BC_Lease,40); BC_LAYOUT(BC_Policy,64); BC_LAYOUT(BC_Api,104);
#undef BC_LAYOUT
static_assert(offsetof(BC_Key,X)==0 && offsetof(BC_Key,Y)==8);
static_assert(offsetof(BC_Rect,MinX)==0 && offsetof(BC_Rect,MinY)==8 && offsetof(BC_Rect,MaxX)==16 && offsetof(BC_Rect,MaxY)==24);
static_assert(offsetof(BC_Ref,StructSize)==0 && offsetof(BC_Ref,Reserved)==4 && offsetof(BC_Ref,Session)==8 && offsetof(BC_Ref,Id)==16);
static_assert(offsetof(BC_Open,StructSize)==0 && offsetof(BC_Open,Reserved)==4 && offsetof(BC_Open,World)==8 && offsetof(BC_Open,ClientWorldEpoch)==16 && offsetof(BC_Open,OriginX)==24 && offsetof(BC_Open,OriginY)==32 && offsetof(BC_Open,CellSizeCm)==40 && offsetof(BC_Open,CellPixels)==48 && offsetof(BC_Open,GroupWidth)==52 && offsetof(BC_Open,GridSchema)==56);
static_assert(offsetof(BC_Grid,StructSize)==0 && offsetof(BC_Grid,Version)==4 && offsetof(BC_Grid,ContractId)==8 && offsetof(BC_Grid,AdmissionFlags)==12 && offsetof(BC_Grid,Session)==16 && offsetof(BC_Grid,WorldGeneration)==24 && offsetof(BC_Grid,RestoreEpoch)==32 && offsetof(BC_Grid,GridId)==40 && offsetof(BC_Grid,OriginX)==48 && offsetof(BC_Grid,OriginY)==56 && offsetof(BC_Grid,CellSizeCm)==64 && offsetof(BC_Grid,TileSizeCm)==72 && offsetof(BC_Grid,CellPixels)==80 && offsetof(BC_Grid,GroupWidth)==84 && offsetof(BC_Grid,MaxInteriorSide)==88 && offsetof(BC_Grid,PixelConvention)==92);
static_assert(offsetof(BC_Demand,StructSize)==0 && offsetof(BC_Demand,InteriorSide)==4 && offsetof(BC_Demand,Session)==8 && offsetof(BC_Demand,ClientDemandId)==16 && offsetof(BC_Demand,TileX)==24 && offsetof(BC_Demand,TileY)==32 && offsetof(BC_Demand,Visible)==40 && offsetof(BC_Demand,Reserved)==44 && offsetof(BC_Demand,GridId)==48 && offsetof(BC_Demand,DetailEpoch)==56);
static_assert(offsetof(BC_Status,StructSize)==0 && offsetof(BC_Status,State)==4 && offsetof(BC_Status,Flags)==8 && offsetof(BC_Status,Reason)==12 && offsetof(BC_Status,RequestId)==16 && offsetof(BC_Status,PublicationId)==24 && offsetof(BC_Status,WorldGeneration)==32 && offsetof(BC_Status,RestoreEpoch)==40 && offsetof(BC_Status,GridId)==48 && offsetof(BC_Status,CertificateId)==56 && offsetof(BC_Status,DetailEpoch)==64 && offsetof(BC_Status,TileX)==72 && offsetof(BC_Status,TileY)==80 && offsetof(BC_Status,WorldBounds)==88 && offsetof(BC_Status,InteriorSide)==120 && offsetof(BC_Status,TextureWidth)==124 && offsetof(BC_Status,TextureHeight)==128 && offsetof(BC_Status,Gutter)==132 && offsetof(BC_Status,UVScaleX)==136 && offsetof(BC_Status,UVScaleY)==144 && offsetof(BC_Status,UVBiasX)==152 && offsetof(BC_Status,UVBiasY)==160 && offsetof(BC_Status,ReadinessRevision)==168 && offsetof(BC_Status,RequiredCells)==176 && offsetof(BC_Status,ReadyCells)==184 && offsetof(BC_Status,PendingCells)==192 && offsetof(BC_Status,FailedCells)==200);
static_assert(offsetof(BC_Lease,StructSize)==0 && offsetof(BC_Lease,Reserved)==4 && offsetof(BC_Lease,Session)==8 && offsetof(BC_Lease,LeaseId)==16 && offsetof(BC_Lease,PublicationId)==24 && offsetof(BC_Lease,Texture)==32);
static_assert(offsetof(BC_Policy,StructSize)==0 && offsetof(BC_Policy,PolicyId)==4 && offsetof(BC_Policy,ClassCount)==8 && offsetof(BC_Policy,Gutter)==12 && offsetof(BC_Policy,Sides)==16 && offsetof(BC_Policy,DowngradeFraction)==48 && offsetof(BC_Policy,MaxInteriorSide)==56 && offsetof(BC_Policy,Flags)==60);
static_assert(offsetof(BC_Api,StructSize)==0 && offsetof(BC_Api,Version)==4 && offsetof(BC_Api,ContractId)==8 && offsetof(BC_Api,Reserved)==12 && offsetof(BC_Api,Open)==16 && offsetof(BC_Api,InspectSession)==24 && offsetof(BC_Api,Submit)==32 && offsetof(BC_Api,InspectRequest)==40 && offsetof(BC_Api,InspectPublication)==48 && offsetof(BC_Api,Acquire)==56 && offsetof(BC_Api,Release)==64 && offsetof(BC_Api,Withdraw)==72 && offsetof(BC_Api,WithdrawAll)==80 && offsetof(BC_Api,Close)==88 && offsetof(BC_Api,GetPolicy)==96);
using BC_GetterSignature=uint32_t (__cdecl *)(uint32_t,BC_Api*);
static_assert(std::is_same_v<decltype(&MiniMap_GetBuildingComplexApi),BC_GetterSignature>);
static_assert(std::is_same_v<decltype(BC_Api::Open),BC_OpenFn> && std::is_same_v<decltype(BC_Api::InspectSession),BC_InspectSessionFn> && std::is_same_v<decltype(BC_Api::Submit),BC_SubmitFn> && std::is_same_v<decltype(BC_Api::InspectRequest),BC_InspectRequestFn> && std::is_same_v<decltype(BC_Api::InspectPublication),BC_InspectPublicationFn> && std::is_same_v<decltype(BC_Api::Acquire),BC_AcquireFn> && std::is_same_v<decltype(BC_Api::Release),BC_ReleaseFn> && std::is_same_v<decltype(BC_Api::Withdraw),BC_WithdrawFn> && std::is_same_v<decltype(BC_Api::WithdrawAll),BC_WithdrawAllFn> && std::is_same_v<decltype(BC_Api::Close),BC_CloseFn> && std::is_same_v<decltype(BC_Api::GetPolicy),BC_GetPolicyFn>);
static_assert(sizeof(void*)==8 && sizeof(BC_World*)==8 && sizeof(BC_Texture*)==8);
static_assert(sizeof(BC_OpenFn)==8 && sizeof(BC_InspectSessionFn)==8 && sizeof(BC_SubmitFn)==8 &&
    sizeof(BC_InspectRequestFn)==8 && sizeof(BC_InspectPublicationFn)==8 && sizeof(BC_AcquireFn)==8 &&
    sizeof(BC_ReleaseFn)==8 && sizeof(BC_WithdrawFn)==8 && sizeof(BC_WithdrawAllFn)==8 &&
    sizeof(BC_CloseFn)==8 && sizeof(BC_GetPolicyFn)==8);

namespace {
    struct Request {
        uint64_t Id=0, DetailEpoch=0, Certificate=0, ReadinessRevision=0;
        uint64_t NativeRequest=0, PublicationId=0;
        int64_t X=0,Y=0; uint32_t Side=0; bool Visible=false, Withdrawn=false, CpuComplete=false;
        bool Processed=false;
        bool RetryEligible=true,RetryOnRevision=false,TerminalFailure=false;
        uint64_t RetrySourceRevision=0,RetryDependencyRevision=0;
        uint64_t RequiredCells=0,ReadyCells=0,PendingCells=0,FailedCells=0;
        uint32_t State=BC_PENDING, Reason=BC_SOURCE_PENDING;
        std::optional<MiniMapBuildingComplexWorld::TileIdentity> CpuIdentity;
    };
    struct DemandDescriptor { int64_t X=0,Y=0; uint32_t Side=0,Visible=0; uint64_t GridId=0,Epoch=0,Request=0; };
    struct Session {
        uint64_t Id=0,GridId=0,WorldGeneration=0,RestoreEpoch=0,LastDemandId=0,RestoreToken=0;
        BC_World* World=nullptr; double OriginX=0,OriginY=0; bool Closing=false,Closed=false,Revoked=false;
        std::map<uint64_t,Request> Requests;
        std::map<uint64_t,DemandDescriptor> DemandIds;
        std::set<uint64_t> WithdrawTombstones;
        std::map<uint64_t,uint64_t> LeasePublications;
        std::set<uint64_t> ReleasedLeases;
    };
    struct PublicationRecord {
        uint64_t Session=0, Request=0;
        BC_Status Snapshot{};
    };
    std::map<uint64_t,Session> Sessions;
    std::map<uint64_t,PublicationRecord> Publications;
    std::map<uint64_t,uint64_t> LeaseSessions;
    uint64_t NextSession=1,NextGrid=1,NextRequest=1,NextCertificate=1;
    constexpr uint32_t kSides[]={128,256,512,1024};

    template<class T> bool Shape(T* p) noexcept { return p && reinterpret_cast<uintptr_t>(p)%alignof(T)==0 && p->StructSize==sizeof(T); }
    template<class T> void Init(T* p) noexcept { std::memset(p,0,sizeof(T)); p->StructSize=sizeof(T); }
    bool OnGameThread() noexcept { const auto* n=MiniMapNative::Get(); return n&&n->player.isGameThread&&n->player.isGameThread(); }
    bool Admitted() noexcept { return OnGameThread()&&MiniMapResidentMetadata::Available(); }
    Session* Find(uint64_t id) noexcept { auto i=Sessions.find(id); return i==Sessions.end()?nullptr:&i->second; }
    void DeferRetryUntilRevision(Request& q,bool retryable=true) noexcept {
        q.RetryEligible=false;q.RetryOnRevision=retryable;
        if(retryable) {
            const auto& index=MiniMapBuildingComplexWorld::GetIndex();
            q.RetrySourceRevision=index.SourceRevision();q.RetryDependencyRevision=index.DependencyRevision();
        }
    }
    void RefreshRetryEligibility(Request& q) noexcept {
        if(!q.RetryEligible&&q.RetryOnRevision) {
            const auto& index=MiniMapBuildingComplexWorld::GetIndex();
            if(index.SourceRevision()!=q.RetrySourceRevision||index.DependencyRevision()!=q.RetryDependencyRevision) {
                q.RetryEligible=true;q.RetryOnRevision=false;
            }
        }
    }
    void MarkRequestStale(Request& q) noexcept {
        q.CpuIdentity.reset();q.CpuComplete=false;q.State=BC_STALE;q.Reason=BC_STALE_OR_REVOKED_IDENTITY;
        q.NativeRequest=0;q.PublicationId=0;
        q.RequiredCells=q.ReadyCells=q.PendingCells=q.FailedCells=0;q.Certificate=0;
        q.RetryEligible=true;q.RetryOnRevision=false;q.TerminalFailure=false;
        if(q.ReadinessRevision!=(std::numeric_limits<uint64_t>::max)())++q.ReadinessRevision;
    }
    bool TileBounds(const Session& s,int64_t x,int64_t y,BC_Rect& out) noexcept {
        constexpr int64_t low=(std::numeric_limits<int64_t>::min)()/4;
        constexpr int64_t high=((std::numeric_limits<int64_t>::max)()-3)/4;
        if(x<low||x>high||y<low||y>high)return false;
        const long double minX=static_cast<long double>(s.OriginX)+static_cast<long double>(x)*50000.0L;
        const long double minY=static_cast<long double>(s.OriginY)+static_cast<long double>(y)*50000.0L;
        const long double maxX=minX+50000.0L,maxY=minY+50000.0L;
        out={static_cast<double>(minX),static_cast<double>(minY),static_cast<double>(maxX),static_cast<double>(maxY)};
        return std::isfinite(minX)&&std::isfinite(minY)&&std::isfinite(maxX)&&std::isfinite(maxY)&&
            std::isfinite(out.MinX)&&std::isfinite(out.MinY)&&std::isfinite(out.MaxX)&&std::isfinite(out.MaxY)&&
            out.MinX<out.MaxX&&out.MinY<out.MaxY;
    }
    bool Current(Session& s) noexcept {
        if(s.Closed||s.Closing)return false;
        const auto w=MiniMapBuildingComplexWorld::GetStatus();
        const auto life=MiniMapMassLifecycle::Status();
        MiniMapBuildingComplexIndex::CellSnapshot snapshot;
        if(MiniMapMap::GetWorld()!=s.World || MiniMapMap::GetWorldGeneration()!=s.WorldGeneration ||
           w.WorldGeneration!=s.WorldGeneration || w.RestoreEpoch!=s.RestoreEpoch ||
           life.WorldGeneration!=s.WorldGeneration || life.RestoreToken!=s.RestoreToken ||
           !MiniMapBuildingComplexWorld::GetIndex().TrySnapshot({0,0},snapshot) || !snapshot.Origin.Valid ||
           snapshot.WorldGeneration!=s.WorldGeneration ||
           snapshot.Origin.X!=s.OriginX || snapshot.Origin.Y!=s.OriginY) { s.Revoked=true; return false; }
        return !s.Revoked;
    }
    void FillGrid(const Session& s,BC_Grid& g) noexcept {
        g.Version=BC_VERSION;g.ContractId=BC_CONTRACT_ID;g.AdmissionFlags=s.Revoked?8u:(s.Closing?4u:1u);
        g.Session=s.Id;g.WorldGeneration=s.WorldGeneration;g.RestoreEpoch=s.RestoreEpoch;g.GridId=s.GridId;
        g.OriginX=s.OriginX;g.OriginY=s.OriginY;g.CellSizeCm=12500.0;g.TileSizeCm=50000.0;
        g.CellPixels=256;g.GroupWidth=4;g.MaxInteriorSide=1024;g.PixelConvention=BC_PIXEL_CONVENTION;
        if(MiniMapBuildingComplexWorld::GetStatus().State==MiniMapBuildingComplexWorld::MembershipState::MembershipEstablished) g.AdmissionFlags|=2u;
    }
    uint32_t FailedStatus(uint32_t result,BC_Status& out) noexcept {
        out.State=(result==BC_STALE_SESSION||result==BC_CLOSED)?BC_STALE:BC_FAILED;
        out.Reason=(result==BC_STALE_SESSION||result==BC_CLOSED)?BC_STALE_OR_REVOKED_IDENTITY:
            result==BC_RESOURCE_FAILURE?BC_RESOURCE_FAILURE_REASON:BC_REASON_NONE;
        return result;
    }
    uint32_t SessionCheck(uint64_t id,Session*& s) noexcept {
        s=Find(id);if(!s)return BC_STALE_SESSION;
        if(s->Closed)return BC_CLOSED;
        if(s->Closing)return BC_BUSY;
        if(!Current(*s))return BC_STALE_SESSION;
        const auto life=MiniMapMassLifecycle::Status();
        if(!life.Admitted||life.Cancelled)return BC_REJECTED_THREAD_OR_ADMISSION;
        return BC_RESULT_OK;
    }
    bool InputHeader(const void* p,uint32_t size,uint32_t expected) noexcept {
        return p && reinterpret_cast<uintptr_t>(p)%alignof(uint32_t)==0 && *static_cast<const uint32_t*>(p)==expected && size==expected;
    }
    bool ValidRef(const BC_Ref* ref,bool needsId) noexcept {
        return ref&&reinterpret_cast<uintptr_t>(ref)%alignof(BC_Ref)==0&&
            InputHeader(ref,ref->StructSize,sizeof(BC_Ref))&&ref->Reserved==0&&ref->Session!=0&&
            (needsId?ref->Id!=0:ref->Id==0);
    }
    uint32_t BC_CALL Open(const BC_Open* in,BC_Grid* out) {
        if(!Shape(out))return BC_INVALID_ARGUMENT;Init(out);
        if(!in||reinterpret_cast<uintptr_t>(in)%alignof(BC_Open)||!InputHeader(in,in->StructSize,sizeof(BC_Open)))return BC_INVALID_ARGUMENT;
        if(in->Reserved||!in->World||!std::isfinite(in->OriginX)||!std::isfinite(in->OriginY)||!std::isfinite(in->CellSizeCm)||
           in->CellSizeCm!=12500.0||in->CellPixels!=256||in->GroupWidth!=4||in->GridSchema!=BC_SCHEMA)return BC_INVALID_ARGUMENT;
        if(!OnGameThread())return BC_REJECTED_THREAD_OR_ADMISSION;
        if(!MiniMapResidentMetadata::Available())return BC_REJECTED_THREAD_OR_ADMISSION;
        if(!MiniMapMap::GameplayReady())return BC_UNAVAILABLE;
        const auto life=MiniMapMassLifecycle::Status();
        const auto world=MiniMapBuildingComplexWorld::GetStatus();
        if(!life.Admitted||life.Cancelled||!life.WorldGeneration||!world.WorldGeneration||
            life.WorldGeneration!=MiniMapMap::GetWorldGeneration()||MiniMapMap::GetWorld()!=in->World||
            MiniMapMap::GetWorldGeneration()!=world.WorldGeneration)return BC_UNAVAILABLE;
        auto& index=MiniMapBuildingComplexWorld::GetIndex();MiniMapBuildingComplexIndex::CellSnapshot snap;
        if(!index.TrySnapshot({0,0},snap)||!snap.Origin.Valid)return BC_UNAVAILABLE;
        if(snap.Origin.X!=in->OriginX||snap.Origin.Y!=in->OriginY)return BC_UNAVAILABLE;
        for(const auto& [id,session]:Sessions){(void)id;if(!session.Closed)return BC_BUSY;}
        if(!NextSession||NextSession==(std::numeric_limits<uint64_t>::max)()||!NextGrid||NextGrid==(std::numeric_limits<uint64_t>::max)())return BC_RESOURCE_FAILURE;
        try { Session s;s.Id=NextSession++;s.GridId=NextGrid++;s.WorldGeneration=world.WorldGeneration;s.RestoreEpoch=world.RestoreEpoch;s.RestoreToken=life.RestoreToken;s.World=in->World;s.OriginX=snap.Origin.X;s.OriginY=snap.Origin.Y;auto [it,ok]=Sessions.emplace(s.Id,std::move(s));if(!ok)return BC_INTERNAL_FAILURE;FillGrid(it->second,*out);return BC_RESULT_OK; }
        catch(...) { return BC_RESOURCE_FAILURE; }
    }
    uint32_t BC_CALL InspectSession(const BC_Ref* in,BC_Grid* out) {
        if(!Shape(out))return BC_INVALID_ARGUMENT;Init(out);
        if(!ValidRef(in,false))return BC_INVALID_ARGUMENT;
        if(!Admitted())return BC_REJECTED_THREAD_OR_ADMISSION;Session* s=nullptr;const auto r=SessionCheck(in->Session,s);if(r!=BC_RESULT_OK)return r;FillGrid(*s,*out);return BC_RESULT_OK;
    }
    uint32_t BC_CALL Submit(const BC_Demand* in,uint64_t* out) {
        if(!out||reinterpret_cast<uintptr_t>(out)%alignof(uint64_t))return BC_INVALID_ARGUMENT;*out=0;
        if(!in||reinterpret_cast<uintptr_t>(in)%alignof(BC_Demand)||!InputHeader(in,in->StructSize,sizeof(BC_Demand)))return BC_INVALID_ARGUMENT;
        if(in->Reserved||in->Visible>1||!in->Session||!in->ClientDemandId||!in->GridId||std::find(std::begin(kSides),std::end(kSides),in->InteriorSide)==std::end(kSides))return BC_INVALID_ARGUMENT;
        if(!Admitted())return BC_REJECTED_THREAD_OR_ADMISSION;Session* s=nullptr;auto r=SessionCheck(in->Session,s);if(r!=BC_RESULT_OK)return r;
        if(s->Closing||in->GridId!=s->GridId)return s->Closing?BC_BUSY:BC_INVALID_ARGUMENT;
        BC_Rect checkedBounds{};if(!TileBounds(*s,in->TileX,in->TileY,checkedBounds))return BC_INVALID_ARGUMENT;
        auto existing=s->DemandIds.find(in->ClientDemandId);
        if(existing!=s->DemandIds.end()) { const auto& d=existing->second;if(d.X!=in->TileX||d.Y!=in->TileY||d.Side!=in->InteriorSide||d.Visible!=in->Visible||d.GridId!=in->GridId||d.Epoch!=in->DetailEpoch)return BC_INVALID_ARGUMENT;*out=d.Request;return BC_RESULT_OK; }
        if(in->ClientDemandId<=s->LastDemandId)return BC_INVALID_ARGUMENT;
        try {
        for(auto& [id,q]:s->Requests)if(!q.Withdrawn&&q.X==in->TileX&&q.Y==in->TileY&&q.Side==in->InteriorSide&&q.DetailEpoch==in->DetailEpoch){
            auto inserted=s->DemandIds.emplace(in->ClientDemandId,DemandDescriptor{in->TileX,in->TileY,in->InteriorSide,in->Visible,in->GridId,in->DetailEpoch,id});
            if(!inserted.second)return BC_INTERNAL_FAILURE;
            const bool becameVisible=!q.Visible&&in->Visible!=0;
            q.Visible=in->Visible!=0;
            if(becameVisible&&!q.TerminalFailure&&!q.CpuComplete&&(q.State==BC_FAILED||q.State==BC_STALE)) {
                q.RetryEligible=true;q.RetryOnRevision=false;
            }
            s->LastDemandId=in->ClientDemandId;*out=id;return BC_RESULT_OK;
        }
        if(!NextRequest||NextRequest==(std::numeric_limits<uint64_t>::max)())return BC_RESOURCE_FAILURE;
        Request q;q.Id=NextRequest++;q.X=in->TileX;q.Y=in->TileY;q.Side=in->InteriorSide;q.Visible=in->Visible!=0;q.DetailEpoch=in->DetailEpoch;const auto id=q.Id;
        auto request=s->Requests.emplace(id,std::move(q));if(!request.second)return BC_INTERNAL_FAILURE;
        try { auto demand=s->DemandIds.emplace(in->ClientDemandId,DemandDescriptor{in->TileX,in->TileY,in->InteriorSide,in->Visible,in->GridId,in->DetailEpoch,id});if(!demand.second){s->Requests.erase(request.first);return BC_INTERNAL_FAILURE;} }
        catch(...) { s->Requests.erase(request.first);throw; }
        s->LastDemandId=in->ClientDemandId;*out=id;return BC_RESULT_OK;
        } catch(...) { return BC_RESOURCE_FAILURE; }
    }
    void FillRequest(const Session& s,const Request& q,BC_Status& o) noexcept {
        o.State=q.State;o.Reason=q.Reason;o.RequestId=q.Id;o.WorldGeneration=s.WorldGeneration;
        o.RestoreEpoch=s.RestoreEpoch;o.GridId=s.GridId;o.CertificateId=q.Certificate;o.DetailEpoch=q.DetailEpoch;
        o.TileX=q.X;o.TileY=q.Y;o.InteriorSide=q.Side;o.ReadinessRevision=q.ReadinessRevision;
        TileBounds(s,q.X,q.Y,o.WorldBounds);
        if(q.CpuIdentity&&q.State!=BC_STALE)o.Flags|=BC_CURRENT;
        if(q.CpuComplete)o.Flags|=BC_COMPLETE;
        if(q.State==BC_FAILED)o.Flags|=BC_HAS_FAILURE;
        if(q.RetryEligible)o.Flags|=BC_RETRY_ELIGIBLE;
        if(q.State==BC_PENDING&&q.CpuComplete) {
            for(const auto& [id,other]:s.Requests) if(id!=q.Id&&!other.Withdrawn&&
                other.X==q.X&&other.Y==q.Y&&other.Side!=q.Side&&other.State==BC_READY) {
                o.Flags|=BC_DETAIL_PENDING;break;
            }
        }
        if(MiniMapBuildingComplexWorld::GetStatus().State!=MiniMapBuildingComplexWorld::MembershipState::MembershipEstablished)o.Flags|=BC_ADMISSION_PENDING;
        o.RequiredCells=q.RequiredCells;o.ReadyCells=q.ReadyCells;o.PendingCells=q.PendingCells;o.FailedCells=q.FailedCells;
        if(q.State==BC_READY&&q.PublicationId) {
            o.PublicationId=q.PublicationId;o.Flags|=BC_NATIVE_READY;
            o.TextureWidth=o.TextureHeight=q.Side+2;o.Gutter=1;
            o.UVScaleX=o.UVScaleY=double(q.Side)/double(q.Side+2);
            o.UVBiasX=o.UVBiasY=1.0/double(q.Side+2);
        }
        else if(q.State==BC_EMPTY) { o.Gutter=1; }
    }
    void RefreshNative(Session& s,Request& q) noexcept {
        if(!q.NativeRequest||q.Withdrawn)return;
        const auto native=MiniMapTexturePublication::InspectGroupedRequest(q.NativeRequest);
        if(native.CertificateId)q.Certificate=native.CertificateId;
        if(native.State==MiniMapTexturePublication::RequestState::Pending) {
            if(q.CpuComplete&&q.CpuIdentity&&q.CpuIdentity->State==MiniMapBuildingComplexWorld::TileState::Complete) {
                q.State=BC_PENDING;q.Reason=BC_UPLOAD_PENDING;
            }
            return;
        }
        if(native.State==MiniMapTexturePublication::RequestState::Failed) {
            if(q.State!=BC_FAILED)if(q.ReadinessRevision!=(std::numeric_limits<uint64_t>::max)())++q.ReadinessRevision;
            q.State=BC_FAILED;q.Reason=BC_RESOURCE_FAILURE_REASON;q.TerminalFailure=true;
            return;
        }
        if(native.State==MiniMapTexturePublication::RequestState::Stale) {
            if(q.CpuIdentity&&!MiniMapBuildingComplexWorld::ValidateTile(*q.CpuIdentity))MarkRequestStale(q);
            else { q.State=BC_PENDING;q.Reason=BC_SOURCE_PENDING;q.NativeRequest=0;q.PublicationId=0;
                q.CpuComplete=false;q.RetryEligible=true;q.RetryOnRevision=false; }
            return;
        }
        if(native.State!=MiniMapTexturePublication::RequestState::Ready||!native.PublicationId)return;
        if(q.State!=BC_READY||q.PublicationId!=native.PublicationId)
            if(q.ReadinessRevision!=(std::numeric_limits<uint64_t>::max)())++q.ReadinessRevision;
        q.PublicationId=native.PublicationId;q.State=BC_READY;q.Reason=BC_REASON_NONE;
        q.CpuComplete=true;
        BC_Status snapshot{};Init(&snapshot);FillRequest(s,q,snapshot);
        try {
            PublicationRecord record;record.Session=s.Id;record.Request=q.Id;record.Snapshot=snapshot;
            Publications[native.PublicationId]=record;
        } catch(...) {
            MiniMapTexturePublication::WithdrawGroupedPublication(native.PublicationId);
            q.State=BC_FAILED;q.Reason=BC_RESOURCE_FAILURE_REASON;q.PublicationId=0;
        }
    }
    uint32_t Inspect(uint64_t sid,uint64_t id,BC_Status& out) {
        if(!Admitted())return FailedStatus(BC_REJECTED_THREAD_OR_ADMISSION,out);Session* s=nullptr;auto r=SessionCheck(sid,s);if(r!=BC_RESULT_OK)return FailedStatus(r,out);
        if(!id)return FailedStatus(BC_INVALID_ARGUMENT,out);
        auto it=s->Requests.find(id);if(it==s->Requests.end())return FailedStatus(BC_INVALID_ARGUMENT,out);
        auto& q=it->second;if(q.Withdrawn)return FailedStatus(BC_CLOSED,out);
        if(q.CpuIdentity&&!MiniMapBuildingComplexWorld::ValidateTile(*q.CpuIdentity))MarkRequestStale(q);
        RefreshRetryEligibility(q);
        RefreshNative(*s,q);
        FillRequest(*s,q,out);return BC_RESULT_OK;
    }
    uint32_t BC_CALL InspectRequest(const BC_Ref* in,BC_Status* out) {
        if(!Shape(out))return BC_INVALID_ARGUMENT;
        Init(out);
        if(!ValidRef(in,true))return FailedStatus(BC_INVALID_ARGUMENT,*out);
        return Inspect(in->Session,in->Id,*out);
    }
    uint32_t BC_CALL InspectPublication(const BC_Ref* in,BC_Status* out) {
        if(!Shape(out))return BC_INVALID_ARGUMENT;
        Init(out);
        if(!ValidRef(in,true))return FailedStatus(BC_INVALID_ARGUMENT,*out);
        if(!Admitted())return FailedStatus(BC_REJECTED_THREAD_OR_ADMISSION,*out);
        Session* s=nullptr;
        const auto result=SessionCheck(in->Session,s);
        if(result!=BC_RESULT_OK)return FailedStatus(result,*out);
        const auto found=Publications.find(in->Id);
        if(found==Publications.end()||found->second.Session!=s->Id)return FailedStatus(BC_INVALID_ARGUMENT,*out);
        auto native=MiniMapTexturePublication::InspectGroupedPublication(in->Id);
        if(native.State==MiniMapTexturePublication::RequestState::Unknown)
            return FailedStatus(BC_INTERNAL_FAILURE,*out);
        *out=found->second.Snapshot;
        if(native.State==MiniMapTexturePublication::RequestState::Ready) {
            out->State=BC_READY;out->Reason=BC_REASON_NONE;out->Flags|=BC_CURRENT|BC_NATIVE_READY|BC_COMPLETE;
        } else {
            out->State=native.State==MiniMapTexturePublication::RequestState::Pending?BC_PENDING:
                native.State==MiniMapTexturePublication::RequestState::Failed?BC_FAILED:BC_STALE;
            out->Reason=native.State==MiniMapTexturePublication::RequestState::Pending?BC_UPLOAD_PENDING:
                native.State==MiniMapTexturePublication::RequestState::Failed?BC_RESOURCE_FAILURE_REASON:BC_STALE_OR_REVOKED_IDENTITY;
            out->Flags&=~(BC_CURRENT|BC_NATIVE_READY);
            if(out->State==BC_FAILED)out->Flags|=BC_HAS_FAILURE;
        }
        return BC_RESULT_OK;
    }
    uint32_t BC_CALL Acquire(const BC_Ref* in,BC_Lease* out) {
        if(!Shape(out))return BC_INVALID_ARGUMENT;
        Init(out);
        if(!ValidRef(in,true))return BC_INVALID_ARGUMENT;
        if(!Admitted())return BC_REJECTED_THREAD_OR_ADMISSION;
        Session* s=nullptr;const auto result=SessionCheck(in->Session,s);
        if(result!=BC_RESULT_OK)return result;
        const auto publication=Publications.find(in->Id);
        if(publication==Publications.end()||publication->second.Session!=s->Id)return BC_INVALID_ARGUMENT;
        if(publication->second.Snapshot.State!=BC_READY||
            MiniMapTexturePublication::InspectGroupedPublication(in->Id).State!=MiniMapTexturePublication::RequestState::Ready)
            return BC_UNAVAILABLE;
        MiniMapTexturePublication::Lease nativeLease;
        if(!MiniMapTexturePublication::AcquireGrouped(in->Id,nativeLease))return BC_UNAVAILABLE;
        try {
            const auto inserted=s->LeasePublications.emplace(nativeLease.LeaseId,in->Id);
            if(!inserted.second) { MiniMapTexturePublication::ReleaseGrouped(nativeLease.LeaseId);return BC_INTERNAL_FAILURE; }
            LeaseSessions.emplace(nativeLease.LeaseId,s->Id);
        } catch(...) {
            s->LeasePublications.erase(nativeLease.LeaseId);
            MiniMapTexturePublication::ReleaseGrouped(nativeLease.LeaseId);
            return BC_RESOURCE_FAILURE;
        }
        out->Session=s->Id;out->LeaseId=nativeLease.LeaseId;out->PublicationId=in->Id;out->Texture=nativeLease.Texture;
        return BC_RESULT_OK;
    }
    uint32_t BC_CALL Release(const BC_Ref* in) {
        if(!ValidRef(in,true))return BC_INVALID_ARGUMENT;
        if(!OnGameThread())return BC_REJECTED_THREAD_OR_ADMISSION;auto* s=Find(in->Session);if(!s)return BC_STALE_SESSION;if(s->Closed)return BC_CLOSED;
        const auto owner=LeaseSessions.find(in->Id);
        if(owner==LeaseSessions.end()||owner->second!=s->Id)return BC_INVALID_ARGUMENT;
        if(s->ReleasedLeases.contains(in->Id))return BC_RESULT_OK;
        if(!s->LeasePublications.contains(in->Id))return BC_INVALID_ARGUMENT;
        try { s->ReleasedLeases.insert(in->Id); }
        catch(...) { return BC_RESOURCE_FAILURE; }
        if(!MiniMapTexturePublication::ReleaseGrouped(in->Id)) { s->ReleasedLeases.erase(in->Id);return BC_INTERNAL_FAILURE; }
        s->LeasePublications.erase(in->Id);
        return BC_RESULT_OK;
    }
    uint32_t BC_CALL Withdraw(const BC_Ref* in) {
        if(!ValidRef(in,true))return BC_INVALID_ARGUMENT;
        if(!OnGameThread())return BC_REJECTED_THREAD_OR_ADMISSION;auto* s=Find(in->Session);if(!s)return BC_STALE_SESSION;if(s->Closed)return BC_CLOSED;
        auto it=s->Requests.find(in->Id);if(it==s->Requests.end())return s->WithdrawTombstones.contains(in->Id)?BC_RESULT_OK:BC_INVALID_ARGUMENT;
        if(it->second.Withdrawn)return BC_RESULT_OK;
        try { s->WithdrawTombstones.insert(in->Id); }
        catch(...) { return BC_RESOURCE_FAILURE; }
        const uint64_t nativeRequest=it->second.NativeRequest;
        const bool shared= nativeRequest && std::any_of(s->Requests.begin(),s->Requests.end(),[&](const auto& pair) {
            return pair.first!=in->Id&&!pair.second.Withdrawn&&pair.second.NativeRequest==nativeRequest;
        });
        if(!shared)MiniMapTexturePublication::WithdrawGroupedRequest(nativeRequest);
        const uint64_t publication=it->second.PublicationId;
        const bool publicationShared=publication&&std::any_of(s->Requests.begin(),s->Requests.end(),[&](const auto& pair) {
            return pair.first!=in->Id&&!pair.second.Withdrawn&&pair.second.PublicationId==publication;
        });
        if(publication&&!publicationShared)MiniMapTexturePublication::WithdrawGroupedPublication(publication);
        it->second.Withdrawn=true;it->second.Visible=false;it->second.CpuIdentity.reset();it->second.CpuComplete=false;
        return BC_RESULT_OK;
    }
    uint32_t BC_CALL WithdrawAll(const BC_Ref* in) {
        if(!ValidRef(in,false))return BC_INVALID_ARGUMENT;
        if(!OnGameThread())return BC_REJECTED_THREAD_OR_ADMISSION;auto* s=Find(in->Session);if(!s)return BC_STALE_SESSION;if(s->Closed)return BC_CLOSED;
        try { for(const auto& [id,q]:s->Requests){(void)q;s->WithdrawTombstones.insert(id);} }
        catch(...) { return BC_RESOURCE_FAILURE; }
        for(auto& [id,q]:s->Requests){(void)id;MiniMapTexturePublication::WithdrawGroupedRequest(q.NativeRequest);
            if(q.PublicationId)MiniMapTexturePublication::WithdrawGroupedPublication(q.PublicationId);
            q.Withdrawn=true;q.Visible=false;q.CpuIdentity.reset();q.CpuComplete=false;}
        return BC_RESULT_OK;
    }
    uint32_t BC_CALL Close(const BC_Ref* in) {
        if(!ValidRef(in,false))return BC_INVALID_ARGUMENT;
        if(!OnGameThread())return BC_REJECTED_THREAD_OR_ADMISSION;auto* s=Find(in->Session);if(!s)return BC_STALE_SESSION;if(s->Closed)return BC_RESULT_OK;
        s->Closing=true;
        // Stop all request activity and release CPU image ownership before any
        // fallible bookkeeping. A failed Close is not acknowledged: the
        // request/demand records and tombstones remain for a later retry.
        for(auto& [id,q]:s->Requests){(void)id;MiniMapTexturePublication::WithdrawGroupedRequest(q.NativeRequest);
            if(q.PublicationId)MiniMapTexturePublication::WithdrawGroupedPublication(q.PublicationId);
            q.Withdrawn=true;q.Visible=false;q.CpuIdentity.reset();q.CpuComplete=false;}
        if(!s->LeasePublications.empty())return BC_BUSY;
        try { for(const auto& [id,q]:s->Requests){(void)q;s->WithdrawTombstones.insert(id);} }
        catch(...) { return BC_RESOURCE_FAILURE; }
        // Tombstones and demand descriptors live through this successful
        // closure transition. The closed-session record remains for duplicate Close.
        s->Requests.clear();s->DemandIds.clear();s->WithdrawTombstones.clear();s->Closed=true;
        return BC_RESULT_OK;
    }
    uint32_t BC_CALL GetPolicy(const BC_Ref* in,BC_Policy* out) {
        if(!Shape(out))return BC_INVALID_ARGUMENT;Init(out);if(!ValidRef(in,false))return BC_INVALID_ARGUMENT;
        if(!Admitted())return BC_REJECTED_THREAD_OR_ADMISSION;Session* s=nullptr;auto r=SessionCheck(in->Session,s);if(r!=BC_RESULT_OK)return r;
        out->PolicyId=1;out->ClassCount=4;out->Gutter=1;std::copy(std::begin(kSides),std::end(kSides),out->Sides);out->DowngradeFraction=0.40;out->MaxInteriorSide=1024;return BC_RESULT_OK;
    }
    static_assert(std::is_same_v<decltype(&Open),BC_OpenFn>);
    static_assert(std::is_same_v<decltype(&InspectSession),BC_InspectSessionFn>);
    static_assert(std::is_same_v<decltype(&Submit),BC_SubmitFn>);
    static_assert(std::is_same_v<decltype(&InspectRequest),BC_InspectRequestFn>);
    static_assert(std::is_same_v<decltype(&InspectPublication),BC_InspectPublicationFn>);
    static_assert(std::is_same_v<decltype(&Acquire),BC_AcquireFn>);
    static_assert(std::is_same_v<decltype(&Release),BC_ReleaseFn>);
    static_assert(std::is_same_v<decltype(&Withdraw),BC_WithdrawFn>);
    static_assert(std::is_same_v<decltype(&WithdrawAll),BC_WithdrawAllFn>);
    static_assert(std::is_same_v<decltype(&Close),BC_CloseFn>);
    static_assert(std::is_same_v<decltype(&GetPolicy),BC_GetPolicyFn>);
    void FillCpuResult(Request& q,const MiniMapBuildingComplexWorld::TileResult& t) {
        q.Processed=true;
        if(t.State==MiniMapBuildingComplexWorld::TileState::Stale) {
            MarkRequestStale(q);DeferRetryUntilRevision(q);
            return;
        }
        if(t.State==MiniMapBuildingComplexWorld::TileState::Pending) {
            if(q.State!=BC_PARTIAL||!q.CpuIdentity||!MiniMapBuildingComplexWorld::ValidateTile(*q.CpuIdentity)) {
                q.CpuIdentity.reset();q.CpuComplete=false;q.State=BC_PENDING;q.Reason=BC_SOURCE_PENDING;
                q.RequiredCells=q.ReadyCells=q.PendingCells=q.FailedCells=0;q.Certificate=0;
                if(q.ReadinessRevision!=(std::numeric_limits<uint64_t>::max)())++q.ReadinessRevision;
                DeferRetryUntilRevision(q);
            }
            return;
        }
        if(t.State==MiniMapBuildingComplexWorld::TileState::Failed||!t.Image) {
            q.CpuIdentity.reset();q.CpuComplete=false;q.State=BC_FAILED;q.Reason=BC_REASON_NONE;
            q.TerminalFailure=false;
            q.RequiredCells=q.ReadyCells=q.PendingCells=q.FailedCells=0;q.Certificate=0;
            if(q.ReadinessRevision!=(std::numeric_limits<uint64_t>::max)())++q.ReadinessRevision;
            DeferRetryUntilRevision(q);
            return;
        }
        if(!MiniMapBuildingComplexWorld::ValidateTile(t.Image->Identity)) {
            MarkRequestStale(q);DeferRetryUntilRevision(q);
            return;
        }
        if(t.State==MiniMapBuildingComplexWorld::TileState::Partial&&q.CpuIdentity&&
            MiniMapBuildingComplexWorld::ValidateTile(*q.CpuIdentity))return;
        if(!NextCertificate||NextCertificate==(std::numeric_limits<uint64_t>::max)()) {
            q.CpuIdentity.reset();q.CpuComplete=false;q.State=BC_FAILED;q.Reason=BC_RESOURCE_FAILURE_REASON;
            q.TerminalFailure=true;
            q.RequiredCells=q.ReadyCells=q.PendingCells=q.FailedCells=0;q.Certificate=0;
            if(q.ReadinessRevision!=(std::numeric_limits<uint64_t>::max)())++q.ReadinessRevision;
            DeferRetryUntilRevision(q,false);
            return;
        }
        MiniMapBuildingComplexWorld::TileIdentity currentIdentity=t.Image->Identity;
        q.CpuIdentity=std::move(currentIdentity);q.Certificate=NextCertificate++;q.RequiredCells=t.Image->Identity.Inputs.size();
        q.RetryEligible=false;q.RetryOnRevision=false;q.TerminalFailure=false;
        q.ReadyCells=q.PendingCells=q.FailedCells=0;
        for(const auto& cell:t.Image->Identity.Inputs) {
            if(cell.Status==MiniMapBuildingComplexCellRasterizer::State::Complete)++q.ReadyCells;
            else ++q.PendingCells;
        }
        q.CpuComplete=t.State==MiniMapBuildingComplexWorld::TileState::Complete||t.State==MiniMapBuildingComplexWorld::TileState::Empty;
        switch(t.State) {
        case MiniMapBuildingComplexWorld::TileState::Partial:q.State=BC_PARTIAL;q.Reason=BC_GLOBAL_COMPLETENESS_UNAVAILABLE;break;
        case MiniMapBuildingComplexWorld::TileState::Complete:
            q.State=BC_PENDING;q.Reason=BC_UPLOAD_PENDING;
            q.NativeRequest=MiniMapTexturePublication::RequestGroupedTile(t.Image,q.Certificate);
            if(!q.NativeRequest) { q.State=BC_FAILED;q.Reason=BC_RESOURCE_FAILURE_REASON;q.TerminalFailure=true; }
            break;
        case MiniMapBuildingComplexWorld::TileState::Empty:q.State=BC_EMPTY;q.Reason=BC_REASON_NONE;break;
        default:q.State=BC_FAILED;q.Reason=BC_REASON_NONE;q.CpuComplete=false;DeferRetryUntilRevision(q);break;
        }
        if(q.ReadinessRevision!=(std::numeric_limits<uint64_t>::max)())++q.ReadinessRevision;
    }
}

namespace MiniMapBuildingComplexApiInternal {
    void ServiceRequests() noexcept {
        if(!Admitted()||!MiniMapMassLifecycle::InAdmittedService())return;
        try { for(auto& [sid,s]:Sessions){(void)sid;if(s.Closed||!Current(s))continue;
            if(MiniMapBuildingComplexWorld::GetStatus().State!=MiniMapBuildingComplexWorld::MembershipState::MembershipEstablished)continue;
            for(auto& [id,q]:s.Requests){(void)id;if(q.Withdrawn||!q.Visible)continue;
                if(q.CpuIdentity&&!MiniMapBuildingComplexWorld::ValidateTile(*q.CpuIdentity))MarkRequestStale(q);
                RefreshRetryEligibility(q);
                if(!q.RetryEligible||q.CpuComplete)continue;
                if(q.State==BC_PARTIAL&&q.CpuIdentity)continue;
                q.RetryEligible=false;q.RetryOnRevision=false;
                try {
                    // Reuse another exact current tile result/publication before composing.
                    bool reused=false;
                    MiniMapBuildingComplexWorld::TileIdentity sharedIdentity;
                    const auto shared=MiniMapTexturePublication::FindGroupedTile(q.X,q.Y,q.Side,sharedIdentity);
                    if((shared.State==MiniMapTexturePublication::RequestState::Ready||
                        shared.State==MiniMapTexturePublication::RequestState::Pending)&&
                        MiniMapBuildingComplexWorld::ValidateTile(sharedIdentity)) {
                        q.CpuIdentity=std::move(sharedIdentity);q.Certificate=shared.CertificateId;
                        q.NativeRequest=shared.RequestId;q.PublicationId=shared.PublicationId;
                        q.CpuComplete=true;q.RequiredCells=q.CpuIdentity->Inputs.size();
                        q.ReadyCells=q.PendingCells=q.FailedCells=0;
                        for(const auto& cell:q.CpuIdentity->Inputs) {
                            if(cell.Status==MiniMapBuildingComplexCellRasterizer::State::Complete)++q.ReadyCells;
                            else ++q.PendingCells;
                        }
                        q.State=shared.State==MiniMapTexturePublication::RequestState::Ready?BC_READY:BC_PENDING;
                        q.Reason=q.State==BC_READY?BC_REASON_NONE:BC_UPLOAD_PENDING;
                        if(q.State==BC_READY)RefreshNative(s,q);
                        reused=true;
                    }
                    for(auto& [otherId,other]:s.Requests) {
                        if(reused)break;
                        RefreshNative(s,other);
                        if(otherId==q.Id||other.Withdrawn||other.X!=q.X||other.Y!=q.Y||other.Side!=q.Side||
                            !other.CpuIdentity||other.State==BC_FAILED||!MiniMapBuildingComplexWorld::ValidateTile(*other.CpuIdentity))continue;
                        q.CpuIdentity=other.CpuIdentity;q.CpuComplete=other.CpuComplete;q.Certificate=other.Certificate;
                        q.RequiredCells=other.RequiredCells;q.ReadyCells=other.ReadyCells;q.PendingCells=other.PendingCells;
                        q.FailedCells=other.FailedCells;q.State=other.State;q.Reason=other.Reason;
                        q.NativeRequest=other.NativeRequest;q.PublicationId=other.PublicationId;
                        q.RetryEligible=other.RetryEligible;q.RetryOnRevision=other.RetryOnRevision;
                        reused=true;break;
                    }
                    if(!reused) { const auto t=MiniMapBuildingComplexWorld::ComposeTile({q.X,q.Y},s.WorldGeneration,q.Side);FillCpuResult(q,t); }
                    else if(q.ReadinessRevision!=(std::numeric_limits<uint64_t>::max)())++q.ReadinessRevision;
                }
                catch(...) {
                    q.CpuIdentity.reset();q.CpuComplete=false;q.State=BC_FAILED;q.Reason=BC_RESOURCE_FAILURE_REASON;
                    q.TerminalFailure=false;
                    q.RequiredCells=q.ReadyCells=q.PendingCells=q.FailedCells=0;q.Certificate=0;
                    if(q.ReadinessRevision!=(std::numeric_limits<uint64_t>::max)())++q.ReadinessRevision;
                    DeferRetryUntilRevision(q,false);
                }
                break;}} }
        catch(...){}
    }
}
extern "C" MINIMAP_BUILDING_ABI_EXPORT uint32_t MINIMAP_BUILDING_ABI_CALL MiniMap_GetBuildingComplexApi(uint32_t requestedVersion,BC_Api* outApi) {
    if(!outApi||reinterpret_cast<uintptr_t>(outApi)%alignof(BC_Api)||outApi->StructSize!=sizeof(BC_Api))return BC_INVALID_ARGUMENT;
    std::memset(outApi,0,sizeof(*outApi));outApi->StructSize=sizeof(BC_Api);
    if(requestedVersion!=BC_VERSION)return BC_VERSION_MISMATCH;
    outApi->Version=BC_VERSION;outApi->ContractId=BC_CONTRACT_ID;outApi->Open=&Open;outApi->InspectSession=&InspectSession;outApi->Submit=&Submit;
    outApi->InspectRequest=&InspectRequest;outApi->InspectPublication=&InspectPublication;outApi->Acquire=&Acquire;outApi->Release=&Release;
    outApi->Withdraw=&Withdraw;outApi->WithdrawAll=&WithdrawAll;outApi->Close=&Close;outApi->GetPolicy=&GetPolicy;return BC_RESULT_OK;
}
#endif
