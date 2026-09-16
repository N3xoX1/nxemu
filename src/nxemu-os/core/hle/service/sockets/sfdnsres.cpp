// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string_view>
#include <utility>
#include <vector>

#include "yuzu_common/settings.h"
#include "yuzu_common/string_util.h"
#include "yuzu_common/swap.h"
#include "core/core.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/sockets/sfdnsres.h"
#include "core/hle/service/sockets/sockets.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/network.h"
#include "core/memory.h"

namespace Service::Sockets {

namespace {

bool IsInternetAccessEnabled() {
    return Settings::values.network_access_enabled.GetValue() &&
           !Settings::values.airplane_mode.GetValue();
}

} // namespace

SFDNSRES::SFDNSRES(Core::System& system_) : ServiceFramework{system_, "sfdnsres"} {
    static const FunctionInfo functions[] = {
        {0, nullptr, "SetDnsAddressesPrivateRequest"},
        {1, nullptr, "GetDnsAddressPrivateRequest"},
        {2, &SFDNSRES::GetHostByNameRequest, "GetHostByNameRequest"},
        {3, nullptr, "GetHostByAddrRequest"},
        {4, nullptr, "GetHostStringErrorRequest"},
        {5, &SFDNSRES::GetGaiStringErrorRequest, "GetGaiStringErrorRequest"},
        {6, &SFDNSRES::GetAddrInfoRequest, "GetAddrInfoRequest"},
        {7, nullptr, "GetNameInfoRequest"},
        {8, nullptr, "RequestCancelHandleRequest"},
        {9, nullptr, "CancelRequest"},
        {10, &SFDNSRES::GetHostByNameRequestWithOptions, "GetHostByNameRequestWithOptions"},
        {11, nullptr, "GetHostByAddrRequestWithOptions"},
        {12, &SFDNSRES::GetAddrInfoRequestWithOptions, "GetAddrInfoRequestWithOptions"},
        {13, nullptr, "GetNameInfoRequestWithOptions"},
        {14, &SFDNSRES::ResolverSetOptionRequest, "ResolverSetOptionRequest"},
        {15, nullptr, "ResolverGetOptionRequest"},
    };
    RegisterHandlers(functions);
}

SFDNSRES::~SFDNSRES() = default;

enum class NetDbError : s32 {
    Internal = -1,
    Success = 0,
    HostNotFound = 1,
    TryAgain = 2,
    NoRecovery = 3,
    NoData = 4,
};

static NetDbError GetAddrInfoErrorToNetDbError(GetAddrInfoError result) {
    // These combinations have been verified on console (but are not
    // exhaustive).
    switch (result) {
    case GetAddrInfoError::SUCCESS:
        return NetDbError::Success;
    case GetAddrInfoError::AGAIN:
        return NetDbError::TryAgain;
    case GetAddrInfoError::NODATA:
        return NetDbError::HostNotFound;
    case GetAddrInfoError::SERVICE:
        return NetDbError::Success;
    default:
        return NetDbError::HostNotFound;
    }
}

static Errno GetAddrInfoErrorToErrno(GetAddrInfoError result) {
    // These combinations have been verified on console (but are not
    // exhaustive).
    switch (result) {
    case GetAddrInfoError::SUCCESS:
        // Note: Sometimes a successful lookup sets errno to EADDRNOTAVAIL for
        // some reason, but that doesn't seem useful to implement.
        return Errno::SUCCESS;
    case GetAddrInfoError::AGAIN:
        return Errno::SUCCESS;
    case GetAddrInfoError::NODATA:
        return Errno::SUCCESS;
    case GetAddrInfoError::SERVICE:
        return Errno::INVAL;
    default:
        return Errno::SUCCESS;
    }
}

template <typename T>
static void Append(std::vector<u8>& vec, T t) {
    const size_t offset = vec.size();
    vec.resize(offset + sizeof(T));
    std::memcpy(vec.data() + offset, &t, sizeof(T));
}

static void AppendNulTerminated(std::vector<u8>& vec, std::string_view str) {
    const size_t offset = vec.size();
    vec.resize(offset + str.size() + 1);
    std::memmove(vec.data() + offset, str.data(), str.size());
}

// We implement gethostbyname using the host's getaddrinfo rather than the
// host's gethostbyname, because it simplifies portability: e.g., getaddrinfo
// behaves the same on Unix and Windows, unlike gethostbyname where Windows
// doesn't implement h_errno.
static std::vector<u8> SerializeAddrInfoAsHostEnt(const std::vector<Network::AddrInfo>& vec,
                                                  std::string_view host) {

    std::vector<u8> data;
    // h_name: use the input hostname (append nul-terminated)
    AppendNulTerminated(data, host);
    // h_aliases: leave empty

    Append<u32_be>(data, 0); // count of h_aliases
    // (If the count were nonzero, the aliases would be appended as nul-terminated here.)
    Append<u16_be>(data, static_cast<u16>(Domain::INET)); // h_addrtype
    Append<u16_be>(data, sizeof(Network::IPv4Address));   // h_length
    // h_addr_list:
    size_t count = vec.size();
    ASSERT(count <= UINT32_MAX);
    Append<u32_be>(data, static_cast<uint32_t>(count));
    for (const Network::AddrInfo& addrinfo : vec) {
        // On the Switch, this is passed through htonl despite already being
        // big-endian, so it ends up as little-endian.
        Append<u32_le>(data, Network::IPv4AddressToInteger(addrinfo.addr.ip));

        LOG_INFO(Service, "Resolved host '{}' to IPv4 address {}", host,
                 Network::IPv4AddressToString(addrinfo.addr.ip));
    }
    return data;
}

static std::pair<u32, GetAddrInfoError> GetHostByNameRequestImpl(HLERequestContext& ctx) {
    struct InputParameters {
        u8 use_nsd_resolve;
        u32 cancel_handle;
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();

    LOG_WARNING(
        Service,
        "called with ignored parameters: use_nsd_resolve={}, cancel_handle={}, process_id={}",
        parameters.use_nsd_resolve, parameters.cancel_handle, parameters.process_id);

    const auto host_buffer = ctx.ReadBuffer(0);
    const std::string host = Common::StringFromBuffer(host_buffer);
    // For now, ignore options, which are in input buffer 1 for GetHostByNameRequestWithOptions.

    if (!IsInternetAccessEnabled() && host != "localhost") {
        LOG_DEBUG(Network, "Guest Internet access is disabled, refusing DNS lookup for {}", host);
        return {0, GetAddrInfoError::NODATA};
    }

    // Prevent resolution of Nintendo servers
    if (host.find("srv.nintendo.net") != std::string::npos) {
        LOG_WARNING(Network, "Resolution of hostname {} requested, returning EAI_AGAIN", host);
        return {0, GetAddrInfoError::AGAIN};
    }

    auto res = Network::GetAddressInfo(host, /*service*/ std::nullopt);
    if (!res.has_value()) {
        return {0, Translate(res.error())};
    }

    const std::vector<u8> data = SerializeAddrInfoAsHostEnt(res.value(), host);
    const u32 data_size = static_cast<u32>(data.size());
    ctx.WriteBuffer(data, 0);

    return {data_size, GetAddrInfoError::SUCCESS};
}

void SFDNSRES::GetHostByNameRequest(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetHostByNameRequestImpl(ctx);

    struct OutputParameters {
        NetDbError netdb_error;
        Errno bsd_errno;
        u32 data_size;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
        .data_size = data_size,
    });
}

void SFDNSRES::GetHostByNameRequestWithOptions(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetHostByNameRequestImpl(ctx);

    struct OutputParameters {
        u32 data_size;
        NetDbError netdb_error;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .data_size = data_size,
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
    });
}

static std::vector<u8> SerializeAddrInfo(const std::vector<Network::AddrInfo>& vec,
                                         std::string_view host) {
    // Adapted from
    // https://github.com/switchbrew/libnx/blob/c5a9a909a91657a9818a3b7e18c9b91ff0cbb6e3/nx/source/runtime/resolver.c#L190
    std::vector<u8> data;

    for (const Network::AddrInfo& addrinfo : vec) {
        // serialized addrinfo:
        Append<u32_be>(data, 0xBEEFCAFE);                                        // magic
        Append<u32_be>(data, 0);                                                 // ai_flags
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.family)));      // ai_family
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.socket_type))); // ai_socktype
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.protocol)));    // ai_protocol
        Append<u32_be>(data, sizeof(SockAddrIn));                                // ai_addrlen
        // ^ *not* sizeof(SerializedSockAddrIn), not that it matters since they're the same size

        // ai_addr:
        Append<u16_be>(data, static_cast<u16>(Translate(addrinfo.addr.family))); // sin_family
        // On the Switch, the following fields are passed through htonl despite
        // already being big-endian, so they end up as little-endian.
        Append<u16_le>(data, addrinfo.addr.portno);                            // sin_port
        Append<u32_le>(data, Network::IPv4AddressToInteger(addrinfo.addr.ip)); // sin_addr
        data.resize(data.size() + 8, 0);                                       // sin_zero

        if (addrinfo.canon_name.has_value()) {
            AppendNulTerminated(data, *addrinfo.canon_name);
        } else {
            data.push_back(0);
        }

        LOG_INFO(Service, "Resolved host '{}' to IPv4 address {}", host,
                 Network::IPv4AddressToString(addrinfo.addr.ip));
    }

    data.resize(data.size() + 4, 0); // 4-byte sentinel value

    return data;
}

static std::pair<u32, GetAddrInfoError> GetAddrInfoRequestImpl(HLERequestContext& ctx) {
    struct InputParameters {
        u8 use_nsd_resolve;
        u32 cancel_handle;
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();

    LOG_WARNING(
        Service,
        "called with ignored parameters: use_nsd_resolve={}, cancel_handle={}, process_id={}",
        parameters.use_nsd_resolve, parameters.cancel_handle, parameters.process_id);

    // TODO: If use_nsd_resolve is true, pass the name through NSD::Resolve
    // before looking up.

    const auto host_buffer = ctx.ReadBuffer(0);
    const std::string host = Common::StringFromBuffer(host_buffer);

    if (!IsInternetAccessEnabled() && host != "localhost") {
        LOG_DEBUG(Network, "Guest Internet access is disabled, refusing DNS lookup for {}", host);
        return {0, GetAddrInfoError::NODATA};
    }

    // Prevent resolution of Nintendo servers
    if (host.find("srv.nintendo.net") != std::string::npos) {
        LOG_WARNING(Network, "Resolution of hostname {} requested, returning EAI_AGAIN", host);
        return {0, GetAddrInfoError::AGAIN};
    }

    std::optional<std::string> service = std::nullopt;
    if (ctx.CanReadBuffer(1)) {
        const std::span<const u8> service_buffer = ctx.ReadBuffer(1);
        service = Common::StringFromBuffer(service_buffer);
    }

    // Serialized hints are also passed in a buffer, but are ignored for now.

    auto res = Network::GetAddressInfo(host, service);
    if (!res.has_value()) {
        return {0, Translate(res.error())};
    }

    const std::vector<u8> data = SerializeAddrInfo(res.value(), host);
    const u32 data_size = static_cast<u32>(data.size());
    ctx.WriteBuffer(data, 0);

    return {data_size, GetAddrInfoError::SUCCESS};
}

void SFDNSRES::GetAddrInfoRequest(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetAddrInfoRequestImpl(ctx);

    struct OutputParameters {
        Errno bsd_errno;
        GetAddrInfoError gai_error;
        u32 data_size;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
        .gai_error = emu_gai_err,
        .data_size = data_size,
    });
}

void SFDNSRES::GetGaiStringErrorRequest(HLERequestContext& ctx) {
    struct InputParameters {
        GetAddrInfoError gai_errno;
    };
    IPC::RequestParser rp{ctx};
    auto input = rp.PopRaw<InputParameters>();

    const std::string result = Translate(input.gai_errno);
    ctx.WriteBuffer(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void SFDNSRES::GetAddrInfoRequestWithOptions(HLERequestContext& ctx) {
    // Additional options are ignored
    auto [data_size, emu_gai_err] = GetAddrInfoRequestImpl(ctx);

    struct OutputParameters {
        u32 data_size;
        GetAddrInfoError gai_error;
        NetDbError netdb_error;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0x10);

    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .data_size = data_size,
        .gai_error = emu_gai_err,
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
    });
}

void SFDNSRES::ResolverSetOptionRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 3};

    rb.Push(ResultSuccess);
    rb.Push<s32>(0); // bsd errno
}

} // namespace Service::Sockets
