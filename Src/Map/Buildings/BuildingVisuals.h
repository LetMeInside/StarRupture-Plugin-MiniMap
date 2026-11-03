#pragma once
#include "Services/Viewport.h"
#include "Services/ImageDestination.h"
#include "DiagnosticExports.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "plugin.h"
#include "Config/Config.h"
#include "Map/MapTransform.h"
#include "BuildingSplineEnvelope.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace MiniMapBuildingVisuals
{
    // Owned numerical ribbons may be approximate; confidence is not permission.
    inline constexpr bool SplineRenderingEnabled = true;
    struct State;
    using DestinationMapping = MiniMap::Services::ImageDestinationMapping;
    struct DrawingCoordinates
    {
        std::array<double, 3> Position{}, SignedScale{};
        std::array<double, 4> Rotation{};
        bool PlacementValid = false;
        DestinationMapping Destination;
    };
    // Owns the serialized drawing and scheduler pass. Keep it on the thread
    // that called BeginDraw and end it before submitting another pass.
    class DrawingPass final
    {
    public:
        DrawingPass() noexcept;
        ~DrawingPass();
        DrawingPass(DrawingPass&&) noexcept;
        DrawingPass& operator=(DrawingPass&&) noexcept;
        DrawingPass(const DrawingPass&) = delete;
        DrawingPass& operator=(const DrawingPass&) = delete;
        explicit operator bool() const noexcept { return bool(Owner); }
    private:
        struct Impl;
        explicit DrawingPass(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> Owner;
        friend DrawingPass BeginDraw();
        friend void EndDraw(DrawingPass&) noexcept;
    };
    DrawingPass BeginDraw();
    void EndDraw(DrawingPass&) noexcept;
    enum class DrawStatus : uint8_t
    {
        Submitted, SpritePending, SpriteUnavailable, DestinationUnavailable, InvalidPlacement, Culled, SubmissionFailure
    };
    struct OcclusionGeometry
    {
        std::array<std::array<double, 2>, 4> WorldQuad{};
        std::array<double, 3> SourceMin{}, SourceMax{};
    };
    struct DrawResult
    {
        DrawStatus Status = DrawStatus::SpriteUnavailable;
        OcclusionGeometry Evidence; // Valid only when Status is Submitted.
    };
    std::shared_ptr<State> CreateState();
    // The MiniMap renderer's persistent context. Other consumers should own
    // their own State and use the explicit overloads below.
    State* MiniMapState() noexcept;
    void Prepare(State&, const std::shared_ptr<const MiniMapBuildingCollector::Snapshot>&);
    // Call only while a DrawingPass from BeginDraw is active on this thread.
    // The caller provides placement and destination mapping; geometry and
    // atlas lookup/submission remain owned by this implementation.
    DrawResult DrawBuilding(const std::string& blueprintName, const DrawingCoordinates&,
        MiniMap::Services::IImageDestination& destination);
    void DrawBuildings(State&, const std::shared_ptr<const MiniMapBuildingCollector::Snapshot>&,
        MiniMap::Services::ICanvas&, const MiniMapMap::Transform&,
        const MiniMapConfig::BuildingRenderSettings&);
    bool InitializeSpriteProcessing();
    bool BeginSession();
    void TickSpriteCapture(float deltaTime, bool gameplayReady);
    void ServiceSpriteRequirements(bool gameplayReady, bool visible, bool gameThread);
    void EndSession();
    void ShutdownSpriteProcessing();
    namespace CollectorIntegration
    {
        struct DrawingBoundsVersion { uint64_t Session = 0, Revision = 0; };
        struct DrawingBoundsChange { bool Changed = false, SessionChanged = false, RevisionChanged = false; };
        DrawingBoundsChange ObserveDrawingBoundsVersion(DrawingBoundsVersion& previous);
        void RefreshRecordBounds(MiniMapBuildingCollector::Record&);
    }
    struct DiagnosticSnapshot
    {
        uintptr_t PrivateWorld = 0;
        uint64_t Session = 0;
        unsigned WorldPhase = 0, RequestPhase = 0;
        bool Active = false;
        char BlueprintTail[96]{};
    };
    DiagnosticSnapshot GetDiagnosticSnapshot() noexcept;
    bool Initialize(IPluginSelf* self);
    void Reset();
    void Shutdown();
    void DrawBuildings(MiniMap::Services::ICanvas& ui, const MiniMapMap::Transform& transform,
        const MiniMapConfig::BuildingRenderSettings& settings);

    // Shared with narrow submission fixtures. Called only from the existing UI
    // draw pass, under its lifecycle contract and intersecting draw-list clip.
    namespace Detail
    {
        namespace E=MiniMapBuildingSplineEnvelope;
        namespace P=MiniMapBuildingSplineProfiles;
        namespace C=MiniMapBuildingCollector;
        struct ProjectedVertex {MiniMapMap::ScreenPoint Position;bool Valid=false;};
        struct SplineSubmission {size_t Triangles=0,ProjectedVertices=0;};
        inline bool Drawable(E::State state)
        {return state==E::State::Partial || state==E::State::Complete;}
        inline bool WorldTriangle(const E::Profile& profile,size_t first)
        {
            if(first>profile.Indices.size() || profile.Indices.size()-first<3)return false;
            const auto a=profile.Indices[first],b=profile.Indices[first+1],c=profile.Indices[first+2];
            if(a>=profile.Vertices.size() || b>=profile.Vertices.size() || c>=profile.Vertices.size())return false;
            for(auto index:{a,b,c})for(double value:profile.Vertices[index])if(!std::isfinite(value))return false;
            const double area=E::Area(profile.Vertices[a],profile.Vertices[b],profile.Vertices[c]);
            return std::isfinite(area) && area!=0;
        }
        inline const E::Profile* SelectSpline(const C::Record& record,const E::Result& geometry)
        {
            if(!record.HasSpline || !Drawable(geometry.Status) ||
                !E::NumericalEquivalent(record,geometry.Sampling.get(),geometry,geometry.SyntheticThin))return nullptr;
            for(const auto& profile:geometry.Profiles)
                if(Drawable(profile.Status) && !profile.Indices.empty() && profile.Indices.size()%3==0)
                    for(size_t first=0;first<profile.Indices.size();first+=3)
                        if(WorldTriangle(profile,first))return &profile;
            return nullptr;
        }
        inline SplineSubmission SubmitSpline(MiniMap::Services::ICanvas& ui,MiniMap::Services::DrawLayer list,const MiniMapMap::Transform& transform,
            const C::Record& record,const E::Result& geometry,const C::Bounds& viewport,uint32_t color,
            std::vector<ProjectedVertex>& scratch)
        {
            SplineSubmission submitted;
            if(!transform.Valid || !(color>>24) || !viewport.Valid)return submitted;
            if(!std::isfinite(viewport.MinX) || !std::isfinite(viewport.MinY) || !std::isfinite(viewport.MaxX) ||
                !std::isfinite(viewport.MaxY) || viewport.MinX>=viewport.MaxX || viewport.MinY>=viewport.MaxY)return submitted;
            const auto* profile=SelectSpline(record,geometry);if(!profile)return submitted;
            // Scratch belongs to the UI thread; only capacity is reused. No
            // projection cache, native reads, or allocations per triangle.
            try{scratch.resize(profile->Vertices.size());}catch(...){return submitted;}
            for(size_t i=0;i<profile->Vertices.size();++i)
            {
                auto& vertex=scratch[i];const auto& world=profile->Vertices[i];
                vertex.Valid=transform.WorldToScreen(world[0],world[1],vertex.Position) &&
                    std::isfinite(vertex.Position.X) && std::isfinite(vertex.Position.Y);
            }
            submitted.ProjectedVertices=profile->Vertices.size();
            for(size_t first=0;first<profile->Indices.size();first+=3)
            {
                const auto a=profile->Indices[first],b=profile->Indices[first+1],c=profile->Indices[first+2];
                if(a>=scratch.size() || b>=scratch.size() || c>=scratch.size() ||
                    !scratch[a].Valid || !scratch[b].Valid || !scratch[c].Valid)continue;
                auto p=scratch[a].Position,q=scratch[b].Position,r=scratch[c].Position;
                const double area=(double(q.X)-p.X)*(double(r.Y)-p.Y)-(double(q.Y)-p.Y)*(double(r.X)-p.X);
                if(!std::isfinite(area) || area==0)continue;
                // Cull triangles by their actual surface, never the centreline
                // or collector proxy. Crossing triangles reach the clip scissor.
                if((std::max)({p.X,q.X,r.X})<viewport.MinX || (std::min)({p.X,q.X,r.X})>viewport.MaxX ||
                    (std::max)({p.Y,q.Y,r.Y})<viewport.MinY || (std::min)({p.Y,q.Y,r.Y})>viewport.MaxY)continue;
                if(area<0)std::swap(q,r);
                try{ui.FilledTriangle(list, {{{p.X, p.Y}, {q.X, q.Y}, {r.X, r.Y}}}, color);}
                catch(...){return submitted;} // Preserve normal outer clip/lifecycle retirement; never retry this call.
                ++submitted.Triangles;
            }
            return submitted; // Submission only; no GPU completion claim.
        }
    }
}
#endif
