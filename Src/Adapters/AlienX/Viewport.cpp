#include "Viewport.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "Bootstrap.h"
#include "Textures.h"
#include "Services/Log.h"
#include <stdexcept>

namespace
{
    namespace S = MiniMap::Services;
    thread_local const void* g_renderingEntry = nullptr;

    class Canvas final : public S::ICanvas
    {
        IModLoaderImGui& Ui;
        std::array<std::size_t, 2> Clips{};
        PluginDrawList List(S::DrawLayer layer) const
        { return layer == S::DrawLayer::Window ? Ui.GetWindowDrawList() : Ui.GetForegroundDrawList(); }
    public:
        explicit Canvas(IModLoaderImGui& ui) : Ui(ui)
        {
            // The current host initializes this complete table before dispatch.
            // A partial table cannot satisfy the approved canvas operation set.
            if (!Ui.GetWindowPos || !Ui.GetWindowSize || !Ui.GetContentRegionAvail || !Ui.Dummy ||
                !Ui.CalcTextSize || !Ui.GetWindowDrawList || !Ui.GetForegroundDrawList ||
                !Ui.DL_PushClipRect || !Ui.DL_PopClipRect || !Ui.DL_AddImage || !Ui.DL_AddImageQuad ||
                !Ui.DL_AddRectFilled || !Ui.DL_AddRect || !Ui.DL_AddCircleFilled ||
                !Ui.DL_AddTriangle || !Ui.DL_AddTriangleFilled || !Ui.DL_AddText)
                throw std::runtime_error("MiniMap: incomplete host canvas");
        }
        ~Canvas() override
        {
            // Normally zero: retain the existing explicit push/pop order.
            // Also restore known pushes if a core draw throws before its pop.
            for (std::size_t i = 0; i < Clips.size(); ++i)
                while (Clips[i])
                {
                    --Clips[i];
                    try { Ui.DL_PopClipRect(List(i == 0 ? S::DrawLayer::Window : S::DrawLayer::Foreground)); }
                    catch (...) { break; }
                }
        }
        S::Vec2 WindowPosition() const override
        { S::Vec2 v{}; Ui.GetWindowPos(&v.x, &v.y); return v; }
        S::Vec2 WindowSize() const override
        { S::Vec2 v{}; Ui.GetWindowSize(&v.x, &v.y); return v; }
        S::Vec2 ContentAvailable() const override
        { S::Vec2 v{}; Ui.GetContentRegionAvail(&v.x, &v.y); return v; }
        void ReserveContent(S::Vec2 v) override { Ui.Dummy(v.x, v.y); }
        S::Vec2 MeasureText(std::string_view text, bool hide, float wrap) const override
        { const std::string copy(text); S::Vec2 v{}; Ui.CalcTextSize(copy.c_str(), &v.x, &v.y, hide, wrap); return v; }
        void PushClip(S::DrawLayer l, S::Vec2 a, S::Vec2 b, bool intersect) override
        { Ui.DL_PushClipRect(List(l), a.x, a.y, b.x, b.y, intersect); ++Clips[l == S::DrawLayer::Window ? 0 : 1]; }
        void PopClip(S::DrawLayer l) override
        { Ui.DL_PopClipRect(List(l)); auto& count = Clips[l == S::DrawLayer::Window ? 0 : 1]; if (count) --count; }
        void Image(S::DrawLayer l, S::Texture t, S::Vec2 a, S::Vec2 b,
            S::Vec2 u, S::Vec2 v, S::Color c) override
        {
            MiniMap::Adapters::AlienX::GetTextures().Image(Ui, List(l), t, a, b, u, v, c);
        }
        void ImageQuad(S::DrawLayer l, S::Texture t, const S::Quad& p, const S::Quad& u, S::Color c) override
        {
            MiniMap::Adapters::AlienX::GetTextures().ImageQuad(Ui, List(l), t, p, u, c);
        }
        void FilledRect(S::DrawLayer l, S::Vec2 a, S::Vec2 b, S::Color c, float r) override
        { Ui.DL_AddRectFilled(List(l), a.x,a.y,b.x,b.y,c,r,PluginDrawFlags_RoundCornersAll); }
        void Rect(S::DrawLayer l, S::Vec2 a, S::Vec2 b, S::Color c, float r, float thickness) override
        { Ui.DL_AddRect(List(l), a.x,a.y,b.x,b.y,c,r,PluginDrawFlags_RoundCornersAll,thickness); }
        void FilledCircle(S::DrawLayer l, S::Vec2 a, float r, S::Color c, int segments) override
        { Ui.DL_AddCircleFilled(List(l),a.x,a.y,r,c,segments); }
        void Triangle(S::DrawLayer l, const std::array<S::Vec2,3>& p, S::Color c, float thickness) override
        { Ui.DL_AddTriangle(List(l),p[0].x,p[0].y,p[1].x,p[1].y,p[2].x,p[2].y,c,thickness); }
        void FilledTriangle(S::DrawLayer l, const std::array<S::Vec2,3>& p, S::Color c) override
        { Ui.DL_AddTriangleFilled(List(l),p[0].x,p[0].y,p[1].x,p[1].y,p[2].x,p[2].y,c); }
        void Text(S::DrawLayer l, S::Vec2 p, S::Color c, std::string_view text) override
        { const std::string copy(text); Ui.DL_AddText(List(l),p.x,p.y,c,copy.c_str()); }
    };
}

namespace MiniMap::Adapters::AlienX
{
    S::RegistrationReceipt<S::Widget> Viewport::CreateOverlay(const S::OverlaySpec& spec,
        S::RenderFn render, void* user)
    {
        auto* self = GetSelf();
        auto* ui = self && self->hooks ? self->hooks->UI : nullptr;
        if (!render || spec.persistentName.empty() || spec.persistentName.find('\0') != std::string_view::npos)
            return {{}, S::Acceptance::Rejected, {S::Error::InvalidArgument, "Invalid overlay descriptor"}};
        if (!ui || !ui->RegisterWidget || !ui->UnregisterWidget || !ui->SetWidgetVisible)
            return {{}, S::Acceptance::Rejected, {S::Error::Unavailable, "Widget operations unavailable"}};
        Entry* entry;
        {
            std::lock_guard lock(Mutex);
            if (!NextToken || (Current && (Current->Admitting || Current->InFlight ||
                Current->Registry != S::RegistryRemoval::RequestReturned ||
                Current->Render != render || Current->User != user || Current->Name != spec.persistentName)))
                return {{}, S::Acceptance::Rejected, {S::Error::Capacity, "MiniMap widget endpoint already assigned"}};
            Entries.emplace_back(); entry = &Entries.back();
            entry->Token = {NextToken++}; entry->Name = spec.persistentName;
            entry->Render = render; entry->User = user;
            entry->Hints = {spec.firstUseSize.x, spec.firstUseSize.y,
                spec.firstUsePosition ? spec.firstUsePosition->x : -1.0f,
                spec.firstUsePosition ? spec.firstUsePosition->y : -1.0f,
                0.0f, 0.0f, 4, 4,
                PluginWindowFlags_NoTitleBar | PluginWindowFlags_NoScrollbar | PluginWindowFlags_NoBackground};
            entry->Description = {entry->Name.c_str(), &RenderThunk, &entry->Hints};
            entry->Detach = ui->UnregisterWidget; entry->Visible = ui->SetWidgetVisible;
            Current = entry; // All callback-visible state prepared before host submission.
        }
        try
        {
            const auto handle = ui->RegisterWidget(&entry->Description);
            std::lock_guard lock(Mutex); entry->Handle = handle;
            if (!handle)
            {
                entry->Admitting = false; Current = nullptr;
                entry->Registry = S::RegistryRemoval::ConfirmedAbsent;
                return {{}, S::Acceptance::Rejected, {S::Error::BackendFailure, "Widget rejected"}};
            }
            return {entry->Token, S::Acceptance::AcceptedUnverified, {}};
        }
        catch (...)
        {
            std::lock_guard lock(Mutex); entry->Admitting = false;
            entry->Registry = S::RegistryRemoval::Unknown;
            return {entry->Token, S::Acceptance::BackendUnknown, {S::Error::BackendFailure, "Widget submission interrupted"}};
        }
    }
    S::Status Viewport::SetVisible(S::Widget token, bool visible)
    {
        WidgetHandle handle = nullptr; void (*set)(WidgetHandle,bool) = nullptr;
        {
            std::lock_guard lock(Mutex);
            for (auto& entry : Entries) if (entry.Token.id == token.id && entry.Admitting)
            { handle = entry.Handle; set = entry.Visible; break; }
        }
        if (!handle || !set) return {S::Error::Unavailable, "Widget unavailable"};
        try { set(handle, visible); return {}; }
        catch (...) { return {S::Error::BackendFailure, "Widget visibility submission interrupted"}; }
    }
    S::Removal Viewport::Remove(S::Widget token)
    {
        Entry* entry = nullptr; WidgetHandle handle = nullptr; void (*detach)(WidgetHandle) = nullptr;
        {
            std::lock_guard lock(Mutex);
            if (!token.id) return {true,S::RegistryRemoval::NotRequested,S::CoreDrain::Quiescent,{}};
            for (auto& e : Entries) if (token.id && e.Token.id == token.id) { entry = &e; break; }
            if (!entry) return {false,S::RegistryRemoval::NotRequested,S::CoreDrain::Unknown,{S::Error::InvalidArgument,{}}};
            entry->Admitting = false;
            if (!entry->RemovalRequested)
            {
                entry->RemovalRequested = true; handle = entry->Handle; detach = entry->Detach;
                if (entry->Registry != S::RegistryRemoval::ConfirmedAbsent) entry->Registry = S::RegistryRemoval::Unknown;
                entry->RemovalError = S::Error::OwnershipUncertain;
            }
        }
        if (handle && detach)
        {
            try
            {
                detach(handle);
                std::lock_guard lock(Mutex);
                entry->Registry = S::RegistryRemoval::RequestReturned;
                entry->RemovalError = S::Error::None;
            }
            catch (...)
            {
                std::lock_guard lock(Mutex); entry->RemovalError = S::Error::BackendFailure;
            } // Unknown, never blindly resubmit an interrupted unregister.
        }
        std::unique_lock lock(Mutex);
        if (g_renderingEntry == entry)
            return {true,entry->Registry,S::CoreDrain::Pending,{entry->RemovalError,{}}};
        Drained.wait(lock, [&]{return entry->InFlight == 0;});
        // Only the same core owner can subsequently use this native endpoint.
        return {true,entry->Registry,S::CoreDrain::Quiescent,{entry->RemovalError,{}}};
    }
    void Viewport::RenderThunk(IModLoaderImGui* ui) noexcept { GetViewport().Dispatch(ui); }
    void Viewport::Dispatch(IModLoaderImGui* ui) noexcept
    {
        Entry* entry;
        {
            std::lock_guard lock(Mutex); entry = Current;
            if (!ui || !entry || !entry->Admitting) return;
            ++entry->InFlight;
        }
        const auto* previous = g_renderingEntry; g_renderingEntry = entry;
        try { TextureRenderScope rendering; Canvas frame(*ui); entry->Render(entry->User,frame); }
        catch (...)
        {
            if (auto* log = S::TryLog())
                log->Write(S::Level::Error, "MiniMap: viewport callback failed; frame not retried");
        }
        g_renderingEntry = previous;
        { std::lock_guard lock(Mutex); --entry->InFlight; }
        Drained.notify_all();
    }
}
#endif
