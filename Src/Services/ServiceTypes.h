#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace MiniMap::Services
{
    enum class Error
    {
        None, Unavailable, InvalidArgument, Capacity, BackendFailure,
        WrongThread, ShuttingDown, OwnershipUncertain
    };
    struct Status
    {
        Error error = Error::None;
        std::string detail;
    };
    template<class T> struct Result
    {
        std::optional<T> value;
        Status status;
    };
    struct Registration { std::uint64_t id = 0; };
    enum class Acceptance { Rejected, AcceptedUnverified, VerifiedOperational, BackendUnknown };
    template<class Token> struct RegistrationReceipt
    {
        Token token;
        Acceptance acceptance;
        Status status;
    };
    enum class RegistryRemoval { NotRequested, RequestReturned, ConfirmedAbsent, Unknown };
    enum class CoreDrain { Quiescent, Pending, Unknown };
    struct Removal
    {
        bool admissionClosed;
        RegistryRemoval registry;
        CoreDrain core;
        Status status;
    };
}
