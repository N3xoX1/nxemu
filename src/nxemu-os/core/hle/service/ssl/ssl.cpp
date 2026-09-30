// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <optional>
#include <vector>

#include "yuzu_common/string_util.h"

#include "core/core.h"
#include "core/hle/result.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"
#include "core/hle/service/sockets/bsd.h"
#include "core/hle/service/ssl/cert_store.h"
#include "core/hle/service/ssl/ssl.h"
#include "core/hle/service/ssl/ssl_backend.h"
#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"

namespace Service::SSL {

// This is nn::ssl::sf::CertificateFormat
enum class CertificateFormat : u32 {
    Pem = 1,
    Der = 2,
};

// This is nn::ssl::sf::ContextOption
enum class ContextOption : u32 {
    None = 0,
    CrlImportDateCheckEnable = 1,
};

// This is nn::ssl::Connection::IoMode
enum class IoMode : u32 {
    Blocking = 1,
    NonBlocking = 2,
};

// This is nn::ssl::sf::OptionType
enum class OptionType : u32 {
    DoNotCloseSocket = 0,
    GetServerCertChain = 1,
    SkipDefaultVerify = 2,
    EnableAlpn = 3,
};

// This is nn::ssl::sf::SslVersion
struct SslVersion {
    union {
        u32 raw{};

        BitField<0, 1, u32> tls_auto;
        BitField<3, 1, u32> tls_v10;
        BitField<4, 1, u32> tls_v11;
        BitField<5, 1, u32> tls_v12;
        BitField<6, 1, u32> tls_v13;
        BitField<24, 7, u32> api_version;
    };
};

struct SslContextSharedData {
    u32 connection_count = 0;
};

std::optional<std::vector<CaCertificateId>> ReadCertificateIds(HLERequestContext& ctx) {
    const auto bytes = ctx.ReadBuffer();
    if (bytes.empty() || bytes.size() % sizeof(CaCertificateId) != 0) {
        return std::nullopt;
    }

    std::vector<CaCertificateId> ids(bytes.size() / sizeof(CaCertificateId));
    std::memcpy(ids.data(), bytes.data(), bytes.size());
    return ids;
}

class ISslConnection final : public ServiceFramework<ISslConnection> {
public:
    explicit ISslConnection(Core::System& system_in, SslVersion ssl_version_in,
                            std::shared_ptr<SslContextSharedData>& shared_data_in,
                            std::unique_ptr<SSLConnectionBackend>&& backend_in)
        : ServiceFramework{system_in, "ISslConnection"}, ssl_version{ssl_version_in},
          shared_data{shared_data_in}, backend{std::move(backend_in)} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &ISslConnection::SetSocketDescriptor, "SetSocketDescriptor"},
            {1, &ISslConnection::SetHostName, "SetHostName"},
            {2, &ISslConnection::SetVerifyOption, "SetVerifyOption"},
            {3, &ISslConnection::SetIoMode, "SetIoMode"},
            {4, &ISslConnection::GetSocketDescriptor, "GetSocketDescriptor"},
            {5, &ISslConnection::GetHostName, "GetHostName"},
            {6, &ISslConnection::GetVerifyOption, "GetVerifyOption"},
            {7, &ISslConnection::GetIoMode, "GetIoMode"},
            {8, &ISslConnection::DoHandshake, "DoHandshake"},
            {9, &ISslConnection::DoHandshakeGetServerCert, "DoHandshakeGetServerCert"},
            {10, &ISslConnection::Read, "Read"},
            {11, &ISslConnection::Write, "Write"},
            {12, &ISslConnection::Pending, "Pending"},
            {13, nullptr, "Peek"},
            {14, nullptr, "Poll"},
            {15, nullptr, "GetVerifyCertError"},
            {16, nullptr, "GetNeededServerCertBufferSize"},
            {17, &ISslConnection::SetSessionCacheMode, "SetSessionCacheMode"},
            {18, &ISslConnection::GetSessionCacheMode, "GetSessionCacheMode"},
            {19, nullptr, "FlushSessionCache"},
            {20, nullptr, "SetRenegotiationMode"},
            {21, nullptr, "GetRenegotiationMode"},
            {22, &ISslConnection::SetOption, "SetOption"},
            {23, &ISslConnection::GetOption, "GetOption"},
            {24, nullptr, "GetVerifyCertErrors"},
            {25, nullptr, "GetCipherInfo"},
            {26, nullptr, "SetNextAlpnProto"},
            {27, nullptr, "GetNextAlpnProto"},
            {28, nullptr, "SetDtlsSocketDescriptor"},
            {29, nullptr, "GetDtlsHandshakeTimeout"},
            {30, nullptr, "SetPrivateOption"},
            {31, nullptr, "SetSrtpCiphers"},
            {32, nullptr, "GetSrtpCipher"},
            {33, nullptr, "ExportKeyingMaterial"},
            {34, nullptr, "SetIoTimeout"},
            {35, nullptr, "GetIoTimeout"},
            {36, nullptr, "GetSessionTicket"},
            {37, nullptr, "SetSessionTicket"},
        };
        // clang-format on

        RegisterHandlers(functions);

        backend->SetVerifyOption(verify_option);
        shared_data->connection_count++;
    }

    ~ISslConnection() {
        shared_data->connection_count--;
        if (fd_to_close.has_value()) {
            auto bsd = system.ServiceManager().GetService<Service::Sockets::BSD>("bsd:u");
            if (bsd) {
                const auto err = bsd->CloseImpl(*fd_to_close);
                if (err != Service::Sockets::Errno::SUCCESS) {
                    LOG_ERROR(Service_SSL, "Failed to close internal duplicated socket: {}", err);
                }
            }
        }
    }

private:
    SslVersion ssl_version;
    std::shared_ptr<SslContextSharedData> shared_data;
    std::unique_ptr<SSLConnectionBackend> backend;
    std::optional<s32> fd_to_close;
    std::optional<s32> input_fd;
    std::string host_name;
    u32 verify_option = 3;
    IoMode io_mode = IoMode::Blocking;
    u32 session_cache_mode = 0;
    bool do_not_close_socket = false;
    bool get_server_cert_chain = false;
    bool skip_default_verify = false;
    bool enable_alpn = false;
    std::shared_ptr<Network::SocketBase> socket;
    bool did_handshake = false;

    Result SetSocketDescriptorImpl(s32* out_fd, s32 fd) {
        LOG_DEBUG(Service_SSL, "called, fd={}", fd);
        if (input_fd.has_value()) {
            return ResultAlreadyInUse;
        }
        if (did_handshake) {
            return ResultAlreadyInUse;
        }

        auto bsd = system.ServiceManager().GetService<Service::Sockets::BSD>("bsd:u");
        ASSERT_OR_EXECUTE(bsd, { return ResultInternalError; });

        // Keep an internal duplicate; return the original descriptor unless DoNotCloseSocket is set.
        auto duplicate = bsd->DuplicateSocketImpl(fd);
        if (!duplicate.has_value()) {
            LOG_ERROR(Service_SSL, "Failed to duplicate socket with fd {}", fd);
            return ResultInvalidSocket;
        }

        const s32 internal_fd = *duplicate;
        const auto sock = bsd->GetSocket(internal_fd);
        if (!sock.has_value()) {
            (void)bsd->CloseImpl(internal_fd);
            LOG_ERROR(Service_SSL, "Invalid duplicated socket fd {}", internal_fd);
            return ResultInvalidSocket;
        }

        input_fd = fd;
        fd_to_close = internal_fd;
        socket = *sock;
        backend->SetSocket(socket);
        *out_fd = do_not_close_socket ? -1 : fd;
        return ResultSuccess;
    }

    Result SetHostNameImpl(const std::string& hostname) {
        LOG_DEBUG(Service_SSL, "called. hostname={}", hostname);
        if (did_handshake) {
            return ResultAlreadyInUse;
        }
        const Result res = backend->SetHostName(hostname);
        if (res == ResultSuccess) {
            host_name = hostname;
        }
        return res;
    }

    Result SetVerifyOptionImpl(u32 option) {
        if (did_handshake) {
            return ResultAlreadyInUse;
        }
        constexpr u32 default_verify = VerifyOptionPeerCa | VerifyOptionHostName;
        // Horizon 5+ requires both checks unless the guest explicitly opts out.
        if ((option & default_verify) != default_verify && !skip_default_verify) {
            return ResultInvalidOption;
        }
        // Schannel ignores HostName when peer-CA validation is disabled.
        if ((option & VerifyOptionHostName) != 0 && (option & VerifyOptionPeerCa) == 0) {
            return ResultInvalidOption;
        }
        verify_option = option;
        backend->SetVerifyOption(option);
        LOG_DEBUG(Service_SSL, "SetVerifyOption option={}", option);
        return ResultSuccess;
    }

    Result SetIoModeImpl(u32 input_mode) {
        const auto mode = static_cast<IoMode>(input_mode);
        if (mode != IoMode::Blocking && mode != IoMode::NonBlocking) {
            return ResultInvalidOption;
        }
        ASSERT_OR_EXECUTE(socket, { return ResultNoSocket; });

        const bool non_block = mode == IoMode::NonBlocking;
        const Network::Errno error = socket->SetNonBlock(non_block);
        if (error != Network::Errno::SUCCESS) {
            LOG_ERROR(Service_SSL, "Failed to set native socket non-block flag to {}", non_block);
            return ResultInternalError;
        }
        io_mode = mode;
        return ResultSuccess;
    }

    Result SetSessionCacheModeImpl(u32 mode) {
        ASSERT_OR_EXECUTE(socket, { return ResultNoSocket; });
        if (did_handshake) {
            return ResultAlreadyInUse;
        }
        if (mode > 2) {
            return ResultInvalidOption;
        }
        session_cache_mode = mode;
        return ResultSuccess;
    }

    Result DoHandshakeImpl() {
        if (!socket) {
            return ResultNoSocket;
        }
        if (did_handshake) {
            return ResultAlreadyInUse;
        }
        if ((verify_option & VerifyOptionHostName) != 0 && host_name.empty()) {
            LOG_ERROR(Service_SSL, "Hostname verification requested without a hostname");
            return ResultInvalidOption;
        }

        Result res = backend->DoHandshake();
        did_handshake = res.IsSuccess();
        return res;
    }

    std::vector<u8> SerializeServerCerts(const std::vector<std::vector<u8>>& certs) {
        struct Header {
            u64 magic;
            u32 count;
            u32 pad;
        };
        struct EntryHeader {
            u32 size;
            u32 offset;
        };
        if (!get_server_cert_chain) {
            // Just return the first one, unencoded.
            ASSERT_OR_EXECUTE_MSG(
                !certs.empty(), { return {}; }, "Should be at least one server cert");
            return certs[0];
        }
        std::vector<u8> ret;
        Header header{0x4E4D684374726543, static_cast<u32>(certs.size()), 0};
        ret.insert(ret.end(), reinterpret_cast<u8*>(&header), reinterpret_cast<u8*>(&header + 1));
        size_t data_offset = sizeof(Header) + certs.size() * sizeof(EntryHeader);
        for (auto& cert : certs) {
            EntryHeader entry_header{static_cast<u32>(cert.size()), static_cast<u32>(data_offset)};
            data_offset += cert.size();
            ret.insert(ret.end(), reinterpret_cast<u8*>(&entry_header),
                       reinterpret_cast<u8*>(&entry_header + 1));
        }
        for (auto& cert : certs) {
            ret.insert(ret.end(), cert.begin(), cert.end());
        }
        return ret;
    }

    Result ReadImpl(std::vector<u8>* out_data) {
        ASSERT_OR_EXECUTE(did_handshake, { return ResultInternalError; });
        size_t actual_size{};
        Result res = backend->Read(&actual_size, *out_data);
        if (res != ResultSuccess) {
            return res;
        }
        out_data->resize(actual_size);
        return res;
    }

    Result WriteImpl(size_t* out_size, std::span<const u8> data) {
        ASSERT_OR_EXECUTE(did_handshake, { return ResultInternalError; });
        return backend->Write(out_size, data);
    }

    Result PendingImpl(s32* out_pending) {
        ASSERT_OR_EXECUTE(socket, { return ResultNoSocket; });
        *out_pending = backend->Pending();
        return ResultSuccess;
    }

    void SetSocketDescriptor(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const s32 in_fd = rp.Pop<s32>();
        s32 out_fd{-1};
        const Result res = SetSocketDescriptorImpl(&out_fd, in_fd);
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(res);
        rb.Push<s32>(out_fd);
    }

    void SetHostName(HLERequestContext& ctx) {
        const std::string hostname = Common::StringFromBuffer(ctx.ReadBuffer());
        const Result res = SetHostNameImpl(hostname);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(res);
    }

    void SetVerifyOption(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 option = rp.Pop<u32>();
        const Result res = SetVerifyOptionImpl(option);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(res);
    }

    void SetIoMode(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 mode = rp.Pop<u32>();
        const Result res = SetIoModeImpl(mode);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(res);
    }

    void GetSocketDescriptor(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 3};
        if (!input_fd.has_value()) {
            rb.Push(ResultNoSocket);
            rb.Push<s32>(-1);
            return;
        }
        rb.Push(ResultSuccess);
        rb.Push<s32>(*input_fd);
    }

    void GetHostName(HLERequestContext& ctx) {
        const size_t write_size = (std::min)(host_name.size(), ctx.GetWriteBufferSize());
        if (write_size != 0) {
            ctx.WriteBuffer(std::span<const u8>{reinterpret_cast<const u8*>(host_name.data()),
                                                write_size});
        }
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u32>(host_name.size()));
    }

    void GetVerifyOption(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(verify_option);
    }

    void GetIoMode(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.PushEnum(io_mode);
    }

    void DoHandshake(HLERequestContext& ctx) {
        const Result res = DoHandshakeImpl();
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(res);
    }

    void DoHandshakeGetServerCert(HLERequestContext& ctx) {
        struct OutputParameters {
            u32 certs_size;
            u32 certs_count;
        };
        static_assert(sizeof(OutputParameters) == 0x8);

        Result res = DoHandshakeImpl();
        OutputParameters out{};
        if (res == ResultSuccess && (verify_option & VerifyOptionPeerCa) != 0) {
            std::vector<std::vector<u8>> certs;
            res = backend->GetServerCerts(&certs);
            if (res == ResultSuccess) {
                const std::vector<u8> certs_buf = SerializeServerCerts(certs);
                out.certs_count = get_server_cert_chain ? static_cast<u32>(certs.size())
                                                        : static_cast<u32>(!certs.empty());
                out.certs_size = static_cast<u32>(certs_buf.size());
                if (certs_buf.size() > ctx.GetWriteBufferSize()) {
                    res = ResultCertBufferTooSmall;
                } else if (!certs_buf.empty()) {
                    ctx.WriteBuffer(certs_buf);
                }
            }
        }
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(res);
        rb.PushRaw(out);
    }

    void Read(HLERequestContext& ctx) {
        std::vector<u8> output_bytes(ctx.GetWriteBufferSize());
        const Result res = ReadImpl(&output_bytes);
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(res);
        if (res == ResultSuccess) {
            rb.Push(static_cast<u32>(output_bytes.size()));
            ctx.WriteBuffer(output_bytes);
        } else {
            rb.Push(static_cast<u32>(0));
        }
    }

    void Write(HLERequestContext& ctx) {
        size_t write_size{0};
        const Result res = WriteImpl(&write_size, ctx.ReadBuffer());
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(res);
        rb.Push(static_cast<u32>(write_size));
    }

    void Pending(HLERequestContext& ctx) {
        s32 pending_size{0};
        const Result res = PendingImpl(&pending_size);
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(res);
        rb.Push<s32>(pending_size);
    }

    void SetSessionCacheMode(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 mode = rp.Pop<u32>();
        const Result res = SetSessionCacheModeImpl(mode);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(res);
    }

    void GetSessionCacheMode(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 3};
        if (!socket) {
            rb.Push(ResultNoSocket);
            rb.Push<u32>(0);
            return;
        }
        rb.Push(ResultSuccess);
        rb.Push(session_cache_mode);
    }

    Result SetOptionImpl(OptionType option, bool value) {
        switch (option) {
        case OptionType::DoNotCloseSocket:
            if (input_fd.has_value()) {
                return ResultAlreadyInUse;
            }
            do_not_close_socket = value;
            break;
        case OptionType::GetServerCertChain:
            get_server_cert_chain = value;
            break;
        case OptionType::SkipDefaultVerify:
            skip_default_verify = value;
            break;
        case OptionType::EnableAlpn:
            enable_alpn = value;
            break;
        default:
            LOG_WARNING(Service_SSL, "Unknown SSL option={}", option);
            return ResultInvalidOption;
        }
        return ResultSuccess;
    }

    Result GetOptionImpl(OptionType option, bool* out_value) const {
        switch (option) {
        case OptionType::DoNotCloseSocket:
            *out_value = do_not_close_socket;
            break;
        case OptionType::GetServerCertChain:
            *out_value = get_server_cert_chain;
            break;
        case OptionType::SkipDefaultVerify:
            *out_value = skip_default_verify;
            break;
        case OptionType::EnableAlpn:
            *out_value = enable_alpn;
            break;
        default:
            LOG_WARNING(Service_SSL, "Unknown SSL option={}", option);
            return ResultInvalidOption;
        }
        return ResultSuccess;
    }

    void SetOption(HLERequestContext& ctx) {
        struct Parameters {
            u8 value;
            INSERT_PADDING_BYTES(3);
            OptionType option;
        };
        static_assert(sizeof(Parameters) == 0x8, "Parameters is an invalid size");

        IPC::RequestParser rp{ctx};
        const auto parameters = rp.PopRaw<Parameters>();
        const Result res = SetOptionImpl(parameters.option, parameters.value != 0);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(res);
    }

    void GetOption(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto option = rp.PopEnum<OptionType>();
        bool value = false;
        const Result res = GetOptionImpl(option, &value);

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(res);
        rb.Push<u8>(value ? 1 : 0);
    }
};

class ISslContext final : public ServiceFramework<ISslContext> {
public:
    explicit ISslContext(Core::System& system_, SslVersion version)
        : ServiceFramework{system_, "ISslContext"}, ssl_version{version},
          shared_data{std::make_shared<SslContextSharedData>()} {
        static const FunctionInfo functions[] = {
            {0, &ISslContext::SetOption, "SetOption"},
            {1, nullptr, "GetOption"},
            {2, &ISslContext::CreateConnection, "CreateConnection"},
            {3, &ISslContext::GetConnectionCount, "GetConnectionCount"},
            {4, &ISslContext::ImportServerPki, "ImportServerPki"},
            {5, &ISslContext::ImportClientPki, "ImportClientPki"},
            {6, nullptr, "RemoveServerPki"},
            {7, nullptr, "RemoveClientPki"},
            {8, nullptr, "RegisterInternalPki"},
            {9, nullptr, "AddPolicyOid"},
            {10, nullptr, "ImportCrl"},
            {11, nullptr, "RemoveCrl"},
            {12, nullptr, "ImportClientCertKeyPki"},
            {13, nullptr, "GeneratePrivateKeyAndCert"},
        };
        RegisterHandlers(functions);
    }

private:
    SslVersion ssl_version;
    std::shared_ptr<SslContextSharedData> shared_data;

    void SetOption(HLERequestContext& ctx) {
        struct Parameters {
            ContextOption option;
            s32 value;
        };
        static_assert(sizeof(Parameters) == 0x8, "Parameters is an invalid size");

        IPC::RequestParser rp{ctx};
        const auto parameters = rp.PopRaw<Parameters>();

        LOG_WARNING(Service_SSL, "(STUBBED) called. option={}, value={}", parameters.option,
                    parameters.value);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void CreateConnection(HLERequestContext& ctx) {
        LOG_WARNING(Service_SSL, "called");

        std::unique_ptr<SSLConnectionBackend> backend;
        const Result res = CreateSSLConnectionBackend(&backend);

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(res);
        if (res == ResultSuccess) {
            rb.PushIpcInterface<ISslConnection>(system, ssl_version, shared_data,
                                                std::move(backend));
        }
    }

    void GetConnectionCount(HLERequestContext& ctx) {
        LOG_DEBUG(Service_SSL, "connection_count={}", shared_data->connection_count);

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(shared_data->connection_count);
    }

    void ImportServerPki(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto certificate_format = rp.PopEnum<CertificateFormat>();
        [[maybe_unused]] const auto pkcs_12_certificates = ctx.ReadBuffer(0);

        constexpr u64 server_id = 0;

        LOG_WARNING(Service_SSL, "(STUBBED) called, certificate_format={}", certificate_format);

        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push(server_id);
    }

    void ImportClientPki(HLERequestContext& ctx) {
        [[maybe_unused]] const auto pkcs_12_certificate = ctx.ReadBuffer(0);
        [[maybe_unused]] const auto ascii_password = [&ctx] {
            if (ctx.CanReadBuffer(1)) {
                return ctx.ReadBuffer(1);
            }

            return std::span<const u8>{};
        }();

        constexpr u64 client_id = 0;

        LOG_WARNING(Service_SSL, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push(client_id);
    }
};

class ISslService final : public ServiceFramework<ISslService> {
public:
    explicit ISslService(Core::System& system_, std::shared_ptr<CertStore> cert_store_)
        : ServiceFramework{system_, "ssl"}, cert_store{std::move(cert_store_)} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &ISslService::CreateContext, "CreateContext"},
            {1, nullptr, "GetContextCount"},
            {2, &ISslService::GetCertificates, "GetCertificates"},
            {3, &ISslService::GetCertificateBufSize, "GetCertificateBufSize"},
            {4, nullptr, "DebugIoctl"},
            {5, &ISslService::SetInterfaceVersion, "SetInterfaceVersion"},
            {6, nullptr, "FlushSessionCache"},
            {7, nullptr, "SetDebugOption"},
            {8, nullptr, "GetDebugOption"},
            {9, nullptr, "ClearTls12FallbackFlag"},
            {10, nullptr, "GetCertificateByIndex"},
            {11, nullptr, "GetTrustedCertificateCount"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    std::shared_ptr<CertStore> cert_store;

    void GetCertificates(HLERequestContext& ctx) {
        const auto ids = ReadCertificateIds(ctx);
        if (!ids.has_value()) {
            IPC::ResponseBuilder rb{ctx, 3};
            rb.Push(ResultInvalidOption);
            rb.Push<u32>(0);
            return;
        }

        u32 required_size{};
        u32 num_entries{};
        Result res = cert_store->GetCertificateBufSize(&required_size, &num_entries, *ids);
        if (res == ResultSuccess && required_size > ctx.GetWriteBufferSize()) {
            res = ResultInvalidCertBufSize;
        }

        if (res == ResultSuccess) {
            std::vector<u8> output(required_size);
            res = cert_store->GetCertificates(&num_entries, output, *ids);
            if (res == ResultSuccess && !output.empty()) {
                ctx.WriteBuffer(output);
            }
        }

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(res);
        rb.Push(num_entries);
    }

    void GetCertificateBufSize(HLERequestContext& ctx) {
        const auto ids = ReadCertificateIds(ctx);
        u32 required_size{};
        u32 num_entries{};
        const Result res = ids.has_value()
                               ? cert_store->GetCertificateBufSize(&required_size, &num_entries, *ids)
                               : ResultInvalidOption;

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(res);
        rb.Push(required_size);
    }

    void CreateContext(HLERequestContext& ctx) {
        struct Parameters {
            SslVersion ssl_version;
            INSERT_PADDING_BYTES(0x4);
            u64 pid_placeholder;
        };
        static_assert(sizeof(Parameters) == 0x10, "Parameters is an invalid size");

        IPC::RequestParser rp{ctx};
        const auto parameters = rp.PopRaw<Parameters>();

        LOG_WARNING(Service_SSL, "(STUBBED) called, api_version={}, pid_placeholder={}",
                    parameters.ssl_version.api_version, parameters.pid_placeholder);

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ISslContext>(system, parameters.ssl_version);
    }

    void SetInterfaceVersion(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        u32 ssl_version = rp.Pop<u32>();

        LOG_DEBUG(Service_SSL, "called, ssl_version={}", ssl_version);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

class ISslServiceForSystem final : public ServiceFramework<ISslServiceForSystem> {
public:
    explicit ISslServiceForSystem(Core::System& system_, std::shared_ptr<CertStore> cert_store_)
        : ServiceFramework{system_, "ssl:s"}, cert_store{std::move(cert_store_)} {
        static const FunctionInfo functions[] = {
            {0, &ISslServiceForSystem::CreateContext, "CreateContext"},
            {1, nullptr, "GetContextCount"},
            {2, &ISslServiceForSystem::GetCertificates, "GetCertificates"},
            {3, &ISslServiceForSystem::GetCertificateBufSize, "GetCertificateBufSize"},
            {4, nullptr, "DebugIoctl"},
            {5, &ISslServiceForSystem::SetInterfaceVersion, "SetInterfaceVersion"},
            {6, nullptr, "FlushSessionCache"},
            {7, nullptr, "SetDebugOption"},
            {8, nullptr, "GetDebugOption"},
            {9, nullptr, "ClearTls12FallbackFlag"},
            {10, nullptr, "GetCertificateByIndex"},
            {11, nullptr, "GetTrustedCertificateCount"},
            {100, &ISslServiceForSystem::CreateContextForSystem, "CreateContextForSystem"},
            {101, nullptr, "SetThreadCoreMask"},
            {102, nullptr, "GetThreadCoreMask"},
            {103, nullptr, "VerifySignature"},
            {104, nullptr, "ResetCoverageCounters"},
            {105, nullptr, "DumpCoverageProfile"},
        };
        RegisterHandlers(functions);
    }

private:
    std::shared_ptr<CertStore> cert_store;

    void GetCertificates(HLERequestContext& ctx) {
        const auto ids = ReadCertificateIds(ctx);
        if (!ids.has_value()) {
            IPC::ResponseBuilder rb{ctx, 3};
            rb.Push(ResultInvalidOption);
            rb.Push<u32>(0);
            return;
        }

        u32 required_size{};
        u32 num_entries{};
        Result res = cert_store->GetCertificateBufSize(&required_size, &num_entries, *ids);
        if (res == ResultSuccess && required_size > ctx.GetWriteBufferSize()) {
            res = ResultInvalidCertBufSize;
        }
        if (res == ResultSuccess) {
            std::vector<u8> output(required_size);
            res = cert_store->GetCertificates(&num_entries, output, *ids);
            if (res == ResultSuccess && !output.empty()) {
                ctx.WriteBuffer(output);
            }
        }

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(res);
        rb.Push(num_entries);
    }

    void GetCertificateBufSize(HLERequestContext& ctx) {
        const auto ids = ReadCertificateIds(ctx);
        u32 required_size{};
        u32 num_entries{};
        const Result res = ids.has_value()
                               ? cert_store->GetCertificateBufSize(&required_size, &num_entries, *ids)
                               : ResultInvalidOption;
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(res);
        rb.Push(required_size);
    }

    void CreateContext(HLERequestContext& ctx) {
        struct Parameters {
            SslVersion ssl_version;
            INSERT_PADDING_BYTES(0x4);
            u64 pid_placeholder;
        };
        static_assert(sizeof(Parameters) == 0x10, "Parameters is an invalid size");

        IPC::RequestParser rp{ctx};
        const auto parameters = rp.PopRaw<Parameters>();

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ISslContext>(system, parameters.ssl_version);
    }

    void SetInterfaceVersion(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        [[maybe_unused]] const u32 ssl_version = rp.Pop<u32>();
        IPC::ResponseBuilder{ctx, 2}.Push(ResultSuccess);
    }

    void CreateContextForSystem(HLERequestContext& ctx) {
        struct Parameters {
            SslVersion ssl_version;
            INSERT_PADDING_BYTES(0x4);
            u64 pid_placeholder;
        };
        static_assert(sizeof(Parameters) == 0x10, "Parameters is an invalid size");

        IPC::RequestParser rp{ctx};
        const auto parameters = rp.PopRaw<Parameters>();

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ISslContext>(system, parameters.ssl_version);
    }
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    auto cert_store = std::make_shared<CertStore>(system);
    server_manager->RegisterNamedService("ssl",
                                         std::make_shared<ISslService>(system, cert_store));
    server_manager->RegisterNamedService(
        "ssl:s", std::make_shared<ISslServiceForSystem>(system, std::move(cert_store)));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::SSL
