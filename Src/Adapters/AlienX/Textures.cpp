#include "Textures.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "Bootstrap.h"
#include "Native/NativeApi.h"
#include <objbase.h>
#include <cstring>
#include <limits>
#include <stdexcept>

#pragma comment(lib, "ole32.lib")

namespace MiniMap::Adapters::AlienX
{
    namespace S = Services;
    thread_local unsigned TextureRenderScope::Depth = 0;
    thread_local Textures::Operation* Textures::Operation::Current = nullptr;

    namespace
    {
        bool GameThread() noexcept
        {
            if (TextureRenderScope::Active()) return false;
            const auto* api = MiniMapNative::Get();
            try { return api && api->player.isGameThread && api->player.isGameThread(); }
            catch (...) { return false; }
        }
        bool ValidSize(S::TextureSize size) noexcept
        {
            return size.width && size.height && size.width <= std::uint32_t((std::numeric_limits<int>::max)()) &&
                size.height <= std::uint32_t((std::numeric_limits<int>::max)());
        }
    }

    std::uint64_t Textures::IssueIdentity() noexcept
    {
        auto id = NextIdentity.load(std::memory_order_relaxed);
        do
        {
            if (!id) return 0;
        } while (!NextIdentity.compare_exchange_weak(id,
            id == (std::numeric_limits<std::uint64_t>::max)() ? 0 : id + 1, std::memory_order_relaxed));
        return id;
    }

    void Textures::Initialize(IPluginSelf* self) noexcept
    {
        std::lock_guard lock(Gate);
        // Never resurrect identities against an unproven backend incarnation.
        // Ordinary world transitions do not call this method.
        if (Initialized) return;
        Initialized = true;
        Native = self && self->hooks ? self->hooks->ImGuiTextures : nullptr;
        if (Native) Functions = {Native->LoadFromRGBA, Native->LoadFromUTexture2D,
            Native->GetSize, Native->FreeTexture, Native->GetFreeSlotCount};
        GUID guid{};
        if (SUCCEEDED(CoCreateGuid(&guid)))
        {
            constexpr char hex[] = "0123456789ABCDEF";
            const auto* bytes = reinterpret_cast<const unsigned char*>(&guid);
            for (std::size_t i = 0; i < sizeof(guid); ++i)
            {
                InstanceName[i * 2] = hex[bytes[i] >> 4];
                InstanceName[i * 2 + 1] = hex[bytes[i] & 15];
            }
            NamesReady = true;
        }
        BackendOpen = OperationsOpen = AcquisitionsOpen = true;
    }

    void Textures::WithdrawAll() const noexcept
    {
        std::lock_guard lock(LedgerMutex);
        for (const auto& record : Ledger) Withdraw(record.Receipt);
    }

    bool Textures::Connected() const noexcept
    {
        if (!BackendOpen || BackendLost || !Native) return false;
        const auto* self = GetSelf();
        const auto* current = self && self->hooks ? self->hooks->ImGuiTextures : nullptr;
        // An observed loss/change permanently closes this instance. Pointer
        // equality alone is not a registry/device-reset recovery mechanism.
        if (current != Native || current->LoadFromRGBA != Functions.rgba ||
            current->LoadFromUTexture2D != Functions.copy || current->GetSize != Functions.size ||
            current->FreeTexture != Functions.free || current->GetFreeSlotCount != Functions.slots)
        {
            BackendLost = true;
            WithdrawAll();
            return false;
        }
        return Functions.rgba && Functions.free && Functions.size && Functions.slots;
    }

    bool Textures::Available() const noexcept
    {
        std::lock_guard lock(Gate);
        return AcquisitionsOpen && Connected();
    }

    Textures::Operation::Operation(Textures& owner, Kind kind) noexcept : Owner(owner)
    {
        std::lock_guard lock(Owner.Gate);
        if (!Owner.OperationsOpen || (kind == Kind::Acquire && !Owner.AcquisitionsOpen) || !Owner.Connected()) return;
        Functions = Owner.Functions;
        ++Owner.InFlight; // Before any native or game-thread predicate call.
        Admitted = true;
        Previous = Current; Current = this;
    }
    Textures::Operation::~Operation()
    {
        if (!Admitted) return;
        Current = Previous;
        { std::lock_guard lock(Owner.Gate); --Owner.InFlight; }
        Owner.Drained.notify_all();
    }
    bool Textures::Operation::Contains(const Textures& owner) noexcept
    {
        for (auto* op = Current; op; op = op->Previous) if (&op->Owner == &owner) return true;
        return false;
    }

    bool Textures::CloseAcquisitions() noexcept
    {
        std::unique_lock lock(Gate);
        AcquisitionsOpen = false; // Release remains admitted during core cleanup.
        if (Operation::Contains(*this)) return false;
        Drained.wait(lock, [&] { return InFlight == 0; }); // Releases Gate while waiting.
        return true;
    }
    bool Textures::Stop() noexcept
    {
        if (!CloseAcquisitions()) return false;
        {
            std::unique_lock lock(Gate);
            OperationsOpen = false; // Close final release/draw admission atomically.
            WithdrawAll();
            Drained.wait(lock, [&] { return InFlight == 0; });
            BackendOpen = false; // Already-admitted operations retained their valid table through drain.
            Native = nullptr;
            Functions = {};
        }
        // No automatic resource release or ledger/issuer reset.
        return true;
    }

    S::FreeSlots Textures::ObserveFreeSlots() noexcept
    {
        Operation operation(*this, Kind::Acquire);
        if (!operation.Admitted) return {{S::Error::Unavailable, {}}, 0};
        if (!GameThread()) return {{S::Error::WrongThread, {}}, 0};
        try
        {
            const auto count = operation.Functions.slots();
            if (count < 0) return {{S::Error::BackendFailure, {}}, 0};
            return {{}, std::uint32_t(count)};
        }
        catch (...) { return {{S::Error::BackendFailure, {}}, 0}; }
    }

    Textures::Record* Textures::Find(std::uint64_t id) noexcept
    {
        if (id) for (auto& record : Ledger) if (record.Receipt->key == id) return &record;
        return nullptr;
    }

    S::UploadState Textures::UploadRGBA(std::span<const std::uint8_t> rgba, S::TextureSize size,
        const S::TextureRequest& request, S::OwnedTexture& output) noexcept
    {
        if (!ValidSize(size)) return S::UploadState::Rejected;
        const auto maximum = (std::numeric_limits<std::size_t>::max)();
        if (std::size_t(size.width) > maximum / size.height ||
            std::size_t(size.width) * size.height > maximum / 4 ||
            rgba.size() != std::size_t(size.width) * size.height * 4 || !rgba.data())
            return S::UploadState::Rejected;
        return Acquire(rgba, size, nullptr, request, output);
    }
    S::UploadState Textures::CopyTexture2D(SDK::UTexture2D& source,
        const S::TextureRequest& request, S::OwnedTexture& output) noexcept
    {
        return Acquire({}, {}, &source, request, output); // Borrowed only through this call.
    }

    S::UploadState Textures::Acquire(std::span<const std::uint8_t> rgba, S::TextureSize input,
        SDK::UTexture2D* source, const S::TextureRequest& request, S::OwnedTexture& output) noexcept
    {
        if (output.HasRecord() || (request.requiredSize && !ValidSize(*request.requiredSize)) ||
            request.sharingName.size() > 63 || request.sharingName.find('\0') != std::string_view::npos)
            return S::UploadState::Rejected;
        if (!source && request.requiredSize && (request.requiredSize->width != input.width ||
            request.requiredSize->height != input.height)) return S::UploadState::Rejected;
        Operation operation(*this, Kind::Acquire);
        if (!operation.Admitted || (source && !operation.Functions.copy)) return S::UploadState::Unavailable;
        if (!GameThread()) return S::UploadState::Rejected;

        Record* record = nullptr;
        try
        {
            const auto id = IssueIdentity();
            if (!id || (request.sharingName.empty() && !NamesReady)) return S::UploadState::Rejected;
            std::string name(request.sharingName);
            if (name.empty())
            {
                // 5 + 32 GUID + 1 + 16 identity + NUL = 55, inside native name[64].
                std::array<char, 55> fresh{};
                std::memcpy(fresh.data(), "MMTX:", 5);
                std::memcpy(fresh.data() + 5, InstanceName.data(), 32);
                fresh[37] = ':';
                constexpr char hex[] = "0123456789ABCDEF";
                auto serial = id;
                for (std::size_t i = 0; i < 16; ++i) { fresh[53 - i] = hex[serial & 15]; serial >>= 4; }
                name = fresh.data();
            }
            auto receipt = PrepareReceipt(id);
            std::lock_guard lock(LedgerMutex);
            // Retain unknown name obligations without guessing whether another
            // same-key load would repeat an interrupted reference acquisition.
            for (const auto& prior : Ledger)
                if ((prior.Ownership == State::UploadUncertain || prior.Ownership == State::ReleaseUncertain) &&
                    _stricmp(prior.Name.c_str(), name.c_str()) == 0) return S::UploadState::OwnershipUncertain;
            Ledger.emplace_back(); // Last fallible storage step, BEFORE native entry.
            record = &Ledger.back();
            record->Receipt = std::move(receipt);
            record->Name.swap(name);
            record->Required = request.requiredSize;
            record->Input = input; record->EngineCopy = source != nullptr;
            record->Functions = operation.Functions;
            Attach(output, record->Receipt); // Preallocated, nonthrowing output linkage.
        }
        catch (...) { return S::UploadState::Rejected; } // No Load call entered.

        PluginTextureHandle handle = nullptr;
        try
        {
            handle = source ? operation.Functions.copy(source, record->Name.c_str()) :
                operation.Functions.rgba(rgba.data(), int(input.width), int(input.height), record->Name.c_str());
        }
        catch (const std::out_of_range&)
        {
            std::lock_guard lock(LedgerMutex);
            record->Ownership = State::Released; record->Busy = false;
            record->Failure = Evidence::SlotRejected; Clear(output);
            return S::UploadState::Rejected; // v70 verified pre-acquisition capacity exception.
        }
        catch (...)
        {
            std::lock_guard lock(LedgerMutex);
            record->Ownership = State::UploadUncertain; record->Busy = false;
            record->Failure = Evidence::LoadException;
            return S::UploadState::OwnershipUncertain; // No identifiable binding to free.
        }
        {
            std::lock_guard lock(LedgerMutex);
            record->Handle = handle; // First work after return; no fallible insertion.
            record->Ownership = handle ? State::Acquired : State::Released;
            if (!handle)
            {
                record->Busy = false; record->Failure = Evidence::NullReturn; Clear(output);
                return S::UploadState::Unavailable;
            }
        }
        int width = 0, height = 0;
        Evidence failure = Evidence::None;
        {
            std::lock_guard gate(Gate);
            if (!Connected())
            {
                std::lock_guard lock(LedgerMutex);
                record->Ownership = State::Withdrawing; record->Busy = false;
                record->Failure = Evidence::InterfaceUnavailable;
                return S::UploadState::OwnershipUncertain; // Known binding retained; no stale size call.
            }
        }
        try { operation.Functions.size(handle, &width, &height); }
        catch (...) { failure = Evidence::SizeException; }
        if (failure == Evidence::None && (width <= 0 || height <= 0)) failure = Evidence::InvalidSize;
        if (failure == Evidence::None && record->Required &&
            (std::uint32_t(width) != record->Required->width || std::uint32_t(height) != record->Required->height))
            failure = Evidence::RequiredSizeMismatch;
        {
            // Gate precedes LedgerMutex everywhere they are both needed. A
            // concurrent observed interface loss cannot be followed by re-admission.
            std::lock_guard gate(Gate);
            const bool connected = Connected();
            std::lock_guard lock(LedgerMutex);
            record->ObservedWidth = width; record->ObservedHeight = height;
            record->Failure = failure;
            if (!connected)
            {
                record->Ownership = State::Withdrawing; record->Busy = false;
                if (failure == Evidence::None) record->Failure = Evidence::InterfaceUnavailable;
                return S::UploadState::OwnershipUncertain;
            }
            if (failure == Evidence::None)
            {
                record->ValidatedSize = {std::uint32_t(width), std::uint32_t(height)};
                record->Ownership = State::Validated; record->Busy = false;
                Admit(record->Receipt, record->ValidatedSize);
                return S::UploadState::Complete;
            }
            // Remain Busy through the one controlled validation cleanup.
        }
        const auto released = ReturnRecord(*record, output);
        return released == S::ReleaseState::Returned ? S::UploadState::Rejected : S::UploadState::OwnershipUncertain;
    }

    S::ReleaseState Textures::ReturnRecord(Record& record, S::OwnedTexture& owner) noexcept
    {
        {
            std::lock_guard gate(Gate);
            const bool connected = Connected();
            std::lock_guard lock(LedgerMutex);
            Withdraw(record.Receipt);
            record.Ownership = State::Withdrawing;
            if (!connected)
            {
                record.Busy = false;
                if (record.CleanupFailure == Evidence::None) record.CleanupFailure = Evidence::InterfaceUnavailable;
                return S::ReleaseState::UnavailableRetained;
            }
            // Set the irreversible-attempt state BEFORE crossing FreeTexture.
            record.ReleaseAttempted = true;
            record.Ownership = State::ReleaseUncertain;
        }
        try { record.Functions.free(record.Handle); } // Exact acquisition binding; no ledger lock.
        catch (...)
        {
            std::lock_guard lock(LedgerMutex);
            record.Busy = false; record.ReleaseThrew = true;
            // The attempt state remains ReleaseUncertain even if a prior
            // unavailable observation is retained as the first cleanup evidence.
            if (record.CleanupFailure == Evidence::None) record.CleanupFailure = Evidence::FreeException;
            return S::ReleaseState::OwnershipUncertain;
        }
        std::lock_guard lock(LedgerMutex);
        record.Ownership = State::Released; record.Busy = false;
        Clear(owner); // Ledger retains exact binding and evidence, without another release authority.
        return S::ReleaseState::Returned;
    }

    S::ReleaseState Textures::Release(S::OwnedTexture& owner) noexcept
    {
        if (!owner.HasRecord()) return S::ReleaseState::Empty;
        if (!Issued(owner)) return S::ReleaseState::UnavailableRetained;
        Operation operation(*this, Kind::Release);
        Record* record;
        {
            std::lock_guard lock(LedgerMutex);
            record = Find(Key(owner));
            if (!record) return S::ReleaseState::UnavailableRetained;
            Withdraw(record->Receipt); // Excludes any immediate draw using this ID.
            if (record->Ownership == State::UploadUncertain || record->Ownership == State::ReleaseUncertain)
                return S::ReleaseState::OwnershipUncertain;
            if (record->Busy) return S::ReleaseState::UnavailableRetained;
            if (record->Ownership == State::Released) { Clear(owner); return S::ReleaseState::Empty; }
            if (!operation.Admitted)
            {
                record->Ownership = State::Withdrawing;
                if (record->CleanupFailure == Evidence::None) record->CleanupFailure = Evidence::InterfaceUnavailable;
                return S::ReleaseState::UnavailableRetained;
            }
            record->Busy = true;
        }
        return ReturnRecord(*record, owner);
    }

    void Textures::Image(IModLoaderImGui& ui, PluginDrawList list, S::Texture token,
        S::Vec2 a, S::Vec2 b, S::Vec2 u, S::Vec2 v, S::Color color)
    {
        Operation operation(*this, Kind::Draw);
        if (!operation.Admitted) throw std::invalid_argument("MiniMap: texture service drawing closed");
        std::lock_guard lock(LedgerMutex);
        const auto* record = Find(token.id);
        if (!record || record->Ownership != State::Validated || !record->Receipt->view.load(std::memory_order_acquire))
            throw std::invalid_argument("MiniMap: texture identity is not draw-admitted");
        // Only immediate draw recording is allowed under this exclusion.
        ui.DL_AddImage(list, record->Handle, a.x,a.y,b.x,b.y,u.x,u.y,v.x,v.y,color);
    }
    void Textures::ImageQuad(IModLoaderImGui& ui, PluginDrawList list, S::Texture token,
        const S::Quad& p, const S::Quad& u, S::Color color)
    {
        Operation operation(*this, Kind::Draw);
        if (!operation.Admitted) throw std::invalid_argument("MiniMap: texture service drawing closed");
        std::lock_guard lock(LedgerMutex);
        const auto* record = Find(token.id);
        if (!record || record->Ownership != State::Validated || !record->Receipt->view.load(std::memory_order_acquire))
            throw std::invalid_argument("MiniMap: texture identity is not draw-admitted");
        ui.DL_AddImageQuad(list, record->Handle, p[0].x,p[0].y,p[1].x,p[1].y,p[2].x,p[2].y,p[3].x,p[3].y,
            u[0].x,u[0].y,u[1].x,u[1].y,u[2].x,u[2].y,u[3].x,u[3].y,color);
    }
}
#endif
