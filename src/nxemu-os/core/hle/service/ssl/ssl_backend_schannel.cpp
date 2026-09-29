// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>

#include "yuzu_common/error.h"
#include "yuzu_common/fs/file.h"
#include "yuzu_common/hex_util.h"
#include "yuzu_common/string_util.h"

#include "core/hle/service/ssl/ssl_backend.h"
#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"

namespace {

// These includes are inside the namespace to avoid a conflict on MinGW where
// the headers define an enum containing Network and Service as enumerators
// (which clash with the correspondingly named namespaces).
#define SECURITY_WIN32
#include <schnlsp.h>
#include <security.h>
#include <wincrypt.h>

std::once_flag one_time_init_flag;
bool one_time_init_success = false;

SCHANNEL_CRED schannel_cred{};
CredHandle cred_handle{};

static void OneTimeInit() {
    schannel_cred.dwVersion = SCHANNEL_CRED_VERSION;
    schannel_cred.dwFlags =
        SCH_USE_STRONG_CRYPTO | SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;

    const SECURITY_STATUS ret =
        AcquireCredentialsHandle(nullptr, const_cast<LPTSTR>(UNISP_NAME), SECPKG_CRED_OUTBOUND,
                                 nullptr, &schannel_cred, nullptr, nullptr, &cred_handle, nullptr);
    if (ret != SEC_E_OK) {
        LOG_ERROR(Service_SSL, "AcquireCredentialsHandle failed: {}",
                  Common::NativeErrorToString(ret));
        return;
    }

    if (getenv("SSLKEYLOGFILE")) {
        LOG_CRITICAL(Service_SSL,
                     "SSLKEYLOGFILE was set but Schannel does not support exporting keys; not "
                     "logging keys!");
    }

    one_time_init_success = true;
}

} // namespace

namespace Service::SSL {

class SSLConnectionBackendSchannel final : public SSLConnectionBackend {
public:
    Result Init() {
        std::call_once(one_time_init_flag, OneTimeInit);

        if (!one_time_init_success) {
            LOG_ERROR(Service_SSL,
                      "Can't create SSL connection because Schannel one-time initialization failed");
            return ResultInternalError;
        }

        return ResultSuccess;
    }

    void SetSocket(std::shared_ptr<Network::SocketBase> socket_in) override {
        socket = std::move(socket_in);
    }

    Result SetHostName(const std::string& hostname_in) override {
        hostname = hostname_in;
        return ResultSuccess;
    }

    void SetVerifyOption(u32 option) override {
        verify_option = option;
    }

    Result DoHandshake() override {
        while (true) {
            Result r;
            switch (handshake_state) {
            case HandshakeState::Initial:
                if ((r = FlushCiphertextWriteBuf()) != ResultSuccess ||
                    (r = CallInitializeSecurityContext()) != ResultSuccess) {
                    return r;
                }
                continue;
            case HandshakeState::ContinueNeeded:
            case HandshakeState::IncompleteMessage:
                if ((r = FlushCiphertextWriteBuf()) != ResultSuccess ||
                    (r = FillCiphertextReadBuf()) != ResultSuccess) {
                    return r;
                }
                if (ciphertext_read_buf.empty()) {
                    LOG_ERROR(Service_SSL, "SSL handshake failed because server hung up");
                    return ResultInternalError;
                }
                if ((r = CallInitializeSecurityContext()) != ResultSuccess) {
                    return r;
                }
                continue;
            case HandshakeState::DoneAfterFlush:
                if ((r = FlushCiphertextWriteBuf()) != ResultSuccess) {
                    return r;
                }
                handshake_state = HandshakeState::Connected;
                return ResultSuccess;
            case HandshakeState::Connected:
                LOG_ERROR(Service_SSL, "Called DoHandshake but we already handshook");
                return ResultInternalError;
            case HandshakeState::Error:
                return ResultInternalError;
            }
        }
    }

    Result FillCiphertextReadBuf() {
        const size_t fill_size = read_buf_fill_size ? read_buf_fill_size : 4096;
        read_buf_fill_size = 0;
        const size_t offset = ciphertext_read_buf.size();
        ASSERT_OR_EXECUTE(offset + fill_size >= offset, { return ResultInternalError; });
        ciphertext_read_buf.resize(offset + fill_size, 0);
        const auto read_span = std::span(ciphertext_read_buf).subspan(offset, fill_size);
        const auto [actual, err] = socket->Recv(0, read_span);
        switch (err) {
        case Network::Errno::SUCCESS:
            ASSERT(static_cast<size_t>(actual) <= fill_size);
            ciphertext_read_buf.resize(offset + actual);
            if (actual == 0 && offset != 0) {
                // EOF in the middle of a TLS record cannot be recovered by another read.
                LOG_ERROR(Service_SSL, "TLS record truncated by socket EOF");
                return ResultConnectionAbort;
            }
            return ResultSuccess;
        case Network::Errno::AGAIN:
            ciphertext_read_buf.resize(offset);
            return ResultWouldBlock;
        case Network::Errno::TIMEDOUT:
            ciphertext_read_buf.resize(offset);
            return ResultTimeout;
        case Network::Errno::CONNRESET:
            ciphertext_read_buf.resize(offset);
            return ResultConnectionReset;
        case Network::Errno::CONNABORTED:
            ciphertext_read_buf.resize(offset);
            return ResultConnectionAbort;
        default:
            ciphertext_read_buf.resize(offset);
            LOG_ERROR(Service_SSL, "Socket recv returned Network::Errno {}", err);
            return ResultInternalError;
        }
    }

    // Returns success if the write buffer has been completely emptied.
    Result FlushCiphertextWriteBuf() {
        while (!ciphertext_write_buf.empty()) {
            const auto [actual, err] = socket->Send(ciphertext_write_buf, 0);
            switch (err) {
            case Network::Errno::SUCCESS:
                if (actual <= 0) {
                    LOG_ERROR(Service_SSL, "Socket send made no progress");
                    return ResultInternalError;
                }
                ASSERT(static_cast<size_t>(actual) <= ciphertext_write_buf.size());
                ciphertext_write_buf.erase(ciphertext_write_buf.begin(),
                                           ciphertext_write_buf.begin() + actual);
                break;
            case Network::Errno::AGAIN:
                return ResultWouldBlock;
            case Network::Errno::TIMEDOUT:
                return ResultTimeout;
            case Network::Errno::CONNRESET:
                return ResultConnectionReset;
            case Network::Errno::CONNABORTED:
                return ResultConnectionAbort;
            default:
                LOG_ERROR(Service_SSL, "Socket send returned Network::Errno {}", err);
                return ResultInternalError;
            }
        }
        return ResultSuccess;
    }

    Result CallInitializeSecurityContext() {
        unsigned long req = ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_CONFIDENTIALITY |
                            ISC_REQ_INTEGRITY | ISC_REQ_REPLAY_DETECT |
                            ISC_REQ_SEQUENCE_DETECT | ISC_REQ_STREAM |
                            ISC_REQ_USE_SUPPLIED_CREDS;
        if ((verify_option & VerifyOptionPeerCa) == 0) {
            req |= ISC_REQ_MANUAL_CRED_VALIDATION;
        }
        unsigned long attr{};

        std::array<SecBuffer, 2> input_buffers{{
            {
                .cbBuffer = static_cast<unsigned long>(ciphertext_read_buf.size()),
                .BufferType = SECBUFFER_TOKEN,
                .pvBuffer = ciphertext_read_buf.data(),
            },
            {
                .cbBuffer = 0,
                .BufferType = SECBUFFER_EMPTY,
                .pvBuffer = nullptr,
            },
        }};
        std::array<SecBuffer, 2> output_buffers{{
            {
                .cbBuffer = 0,
                .BufferType = SECBUFFER_TOKEN,
                .pvBuffer = nullptr,
            },
            {
                .cbBuffer = 0,
                .BufferType = SECBUFFER_ALERT,
                .pvBuffer = nullptr,
            },
        }};
        SecBufferDesc input_desc{
            .ulVersion = SECBUFFER_VERSION,
            .cBuffers = static_cast<unsigned long>(input_buffers.size()),
            .pBuffers = input_buffers.data(),
        };
        SecBufferDesc output_desc{
            .ulVersion = SECBUFFER_VERSION,
            .cBuffers = static_cast<unsigned long>(output_buffers.size()),
            .pBuffers = output_buffers.data(),
        };
        ASSERT_OR_EXECUTE_MSG(input_buffers[0].cbBuffer == ciphertext_read_buf.size(),
                              { return ResultInternalError; }, "read buffer too large");

        const bool initial_call_done = handshake_state != HandshakeState::Initial;
        if (initial_call_done) {
            LOG_DEBUG(Service_SSL, "Passing {} bytes into InitializeSecurityContext",
                      ciphertext_read_buf.size());
        }

        char* target_name =
            (verify_option & VerifyOptionHostName) != 0 && hostname
                ? const_cast<char*>(hostname->c_str())
                : nullptr;
        const SECURITY_STATUS ret = InitializeSecurityContextA(
            &cred_handle, initial_call_done ? &ctxt : nullptr, target_name, req, 0, 0,
            initial_call_done ? &input_desc : nullptr, 0, &ctxt,
            &output_desc, &attr, nullptr);

        if (output_buffers[0].pvBuffer) {
            const std::span span(static_cast<u8*>(output_buffers[0].pvBuffer),
                                 output_buffers[0].cbBuffer);
            ciphertext_write_buf.insert(ciphertext_write_buf.end(), span.begin(), span.end());
            FreeContextBuffer(output_buffers[0].pvBuffer);
        }

        if (output_buffers[1].pvBuffer) {
            const std::span span(static_cast<u8*>(output_buffers[1].pvBuffer),
                                 output_buffers[1].cbBuffer);
            LOG_DEBUG(Service_SSL, "Got a {}-byte alert buffer: {}", span.size(),
                      Common::HexToString(span));
            FreeContextBuffer(output_buffers[1].pvBuffer);
        }

        auto preserve_extra_input = [&] {
            if (input_buffers[1].BufferType == SECBUFFER_EXTRA) {
                ASSERT(input_buffers[1].cbBuffer <= ciphertext_read_buf.size());
                ciphertext_read_buf.erase(ciphertext_read_buf.begin(),
                                           ciphertext_read_buf.end() - input_buffers[1].cbBuffer);
            } else {
                ASSERT(input_buffers[1].BufferType == SECBUFFER_EMPTY);
                ciphertext_read_buf.clear();
            }
        };

        switch (ret) {
        case SEC_I_CONTINUE_NEEDED:
            LOG_DEBUG(Service_SSL, "InitializeSecurityContext => SEC_I_CONTINUE_NEEDED");
            preserve_extra_input();
            handshake_state = HandshakeState::ContinueNeeded;
            return ResultSuccess;
        case SEC_E_INCOMPLETE_MESSAGE:
            LOG_DEBUG(Service_SSL, "InitializeSecurityContext => SEC_E_INCOMPLETE_MESSAGE");
            ASSERT(input_buffers[1].BufferType == SECBUFFER_MISSING);
            read_buf_fill_size = input_buffers[1].cbBuffer;
            handshake_state = HandshakeState::IncompleteMessage;
            return ResultSuccess;
        case SEC_E_OK:
            LOG_DEBUG(Service_SSL, "InitializeSecurityContext => SEC_E_OK");
            if ((attr & (ISC_RET_CONFIDENTIALITY | ISC_RET_INTEGRITY)) !=
                (ISC_RET_CONFIDENTIALITY | ISC_RET_INTEGRITY)) {
                LOG_ERROR(Service_SSL, "TLS context did not negotiate confidentiality and integrity");
                handshake_state = HandshakeState::Error;
                return ResultInternalError;
            }
            if (initial_call_done) {
                preserve_extra_input();
            } else {
                ciphertext_read_buf.clear();
            }
            handshake_state = HandshakeState::DoneAfterFlush;
            return GrabStreamSizes();
        default:
            LOG_ERROR(Service_SSL,
                      "InitializeSecurityContext failed (probably certificate/protocol issue): {}",
                      Common::NativeErrorToString(ret));
            handshake_state = HandshakeState::Error;
            return ResultInternalError;
        }
    }

    Result GrabStreamSizes() {
        const SECURITY_STATUS ret =
            QueryContextAttributes(&ctxt, SECPKG_ATTR_STREAM_SIZES, &stream_sizes);
        if (ret != SEC_E_OK) {
            LOG_ERROR(Service_SSL, "QueryContextAttributes(SECPKG_ATTR_STREAM_SIZES) failed: {}",
                      Common::NativeErrorToString(ret));
            handshake_state = HandshakeState::Error;
            return ResultInternalError;
        }
        return ResultSuccess;
    }

    Result Read(size_t* out_size, std::span<u8> data) override {
        *out_size = 0;
        if (handshake_state != HandshakeState::Connected) {
            LOG_ERROR(Service_SSL, "Called Read but we did not successfully handshake");
            return ResultInternalError;
        }
        if (data.empty() || got_read_eof) {
            return ResultSuccess;
        }

        while (true) {
            if (!cleartext_read_buf.empty()) {
                const size_t read_size = (std::min)(cleartext_read_buf.size(), data.size());
                std::memcpy(data.data(), cleartext_read_buf.data(), read_size);
                cleartext_read_buf.erase(cleartext_read_buf.begin(),
                                         cleartext_read_buf.begin() + read_size);
                *out_size = read_size;
                return ResultSuccess;
            }

            if (!ciphertext_read_buf.empty()) {
                const SecBuffer empty{
                    .cbBuffer = 0,
                    .BufferType = SECBUFFER_EMPTY,
                    .pvBuffer = nullptr,
                };
                std::array<SecBuffer, 4> buffers{{
                    {
                        .cbBuffer = static_cast<unsigned long>(ciphertext_read_buf.size()),
                        .BufferType = SECBUFFER_DATA,
                        .pvBuffer = ciphertext_read_buf.data(),
                    },
                    empty,
                    empty,
                    empty,
                }};
                ASSERT_OR_EXECUTE_MSG(buffers[0].cbBuffer == ciphertext_read_buf.size(),
                                      { return ResultInternalError; }, "read buffer too large");
                SecBufferDesc desc{
                    .ulVersion = SECBUFFER_VERSION,
                    .cBuffers = static_cast<unsigned long>(buffers.size()),
                    .pBuffers = buffers.data(),
                };

                const SECURITY_STATUS ret = DecryptMessage(&ctxt, &desc, 0, nullptr);
                switch (ret) {
                case SEC_E_OK:
                    ASSERT_OR_EXECUTE(buffers[0].BufferType == SECBUFFER_STREAM_HEADER,
                                      { return ResultInternalError; });
                    ASSERT_OR_EXECUTE(buffers[1].BufferType == SECBUFFER_DATA,
                                      { return ResultInternalError; });
                    ASSERT_OR_EXECUTE(buffers[2].BufferType == SECBUFFER_STREAM_TRAILER,
                                      { return ResultInternalError; });
                    cleartext_read_buf.assign(static_cast<u8*>(buffers[1].pvBuffer),
                                              static_cast<u8*>(buffers[1].pvBuffer) +
                                                  buffers[1].cbBuffer);
                    if (buffers[3].BufferType == SECBUFFER_EXTRA) {
                        ASSERT(buffers[3].cbBuffer <= ciphertext_read_buf.size());
                        ciphertext_read_buf.erase(
                            ciphertext_read_buf.begin(),
                            ciphertext_read_buf.end() - buffers[3].cbBuffer);
                    } else {
                        ASSERT(buffers[3].BufferType == SECBUFFER_EMPTY);
                        ciphertext_read_buf.clear();
                    }
                    continue;
                case SEC_E_INCOMPLETE_MESSAGE:
                    break;
                case SEC_I_CONTEXT_EXPIRED:
                    got_read_eof = true;
                    return ResultSuccess;
                default:
                    LOG_ERROR(Service_SSL, "DecryptMessage failed: {}",
                              Common::NativeErrorToString(ret));
                    return ResultInternalError;
                }
            }

            const Result r = FillCiphertextReadBuf();
            if (r != ResultSuccess) {
                return r;
            }
            if (ciphertext_read_buf.empty()) {
                got_read_eof = true;
                return ResultSuccess;
            }
        }
    }

    Result Write(size_t* out_size, std::span<const u8> data) override {
        *out_size = 0;
        if (handshake_state != HandshakeState::Connected) {
            LOG_ERROR(Service_SSL, "Called Write but we did not successfully handshake");
            return ResultInternalError;
        }
        if (data.empty()) {
            return ResultSuccess;
        }

        data = data.subspan(0, (std::min)(data.size(), size_t{stream_sizes.cbMaximumMessage}));
        if (!cleartext_write_buf.empty()) {
            if (data.size() != cleartext_write_buf.size() ||
                std::memcmp(data.data(), cleartext_write_buf.data(), data.size()) != 0) {
                LOG_ERROR(Service_SSL, "Called Write but buffer does not match previous buffer");
                return ResultInternalError;
            }
            return WriteAlreadyEncryptedData(out_size);
        }
        cleartext_write_buf.assign(data.begin(), data.end());

        std::vector<u8> header_buf(stream_sizes.cbHeader, 0);
        std::vector<u8> tmp_data_buf = cleartext_write_buf;
        std::vector<u8> trailer_buf(stream_sizes.cbTrailer, 0);

        std::array<SecBuffer, 4> buffers{{
            {
                .cbBuffer = stream_sizes.cbHeader,
                .BufferType = SECBUFFER_STREAM_HEADER,
                .pvBuffer = header_buf.data(),
            },
            {
                .cbBuffer = static_cast<unsigned long>(tmp_data_buf.size()),
                .BufferType = SECBUFFER_DATA,
                .pvBuffer = tmp_data_buf.data(),
            },
            {
                .cbBuffer = stream_sizes.cbTrailer,
                .BufferType = SECBUFFER_STREAM_TRAILER,
                .pvBuffer = trailer_buf.data(),
            },
            {
                .cbBuffer = 0,
                .BufferType = SECBUFFER_EMPTY,
                .pvBuffer = nullptr,
            },
        }};
        ASSERT_OR_EXECUTE_MSG(buffers[1].cbBuffer == tmp_data_buf.size(),
                              { return ResultInternalError; }, "temp buffer too large");
        SecBufferDesc desc{
            .ulVersion = SECBUFFER_VERSION,
            .cBuffers = static_cast<unsigned long>(buffers.size()),
            .pBuffers = buffers.data(),
        };

        const SECURITY_STATUS ret = EncryptMessage(&ctxt, 0, &desc, 0);
        if (ret != SEC_E_OK) {
            LOG_ERROR(Service_SSL, "EncryptMessage failed: {}", Common::NativeErrorToString(ret));
            // Do not retry the plaintext when no TLS record was produced.
            cleartext_write_buf.clear();
            return ResultInternalError;
        }

        ciphertext_write_buf.insert(ciphertext_write_buf.end(), header_buf.begin(),
                                    header_buf.begin() + buffers[0].cbBuffer);
        ciphertext_write_buf.insert(ciphertext_write_buf.end(), tmp_data_buf.begin(),
                                    tmp_data_buf.begin() + buffers[1].cbBuffer);
        ciphertext_write_buf.insert(ciphertext_write_buf.end(), trailer_buf.begin(),
                                    trailer_buf.begin() + buffers[2].cbBuffer);
        return WriteAlreadyEncryptedData(out_size);
    }

    Result WriteAlreadyEncryptedData(size_t* out_size) {
        const Result r = FlushCiphertextWriteBuf();
        if (r != ResultSuccess) {
            return r;
        }
        *out_size = cleartext_write_buf.size();
        cleartext_write_buf.clear();
        return ResultSuccess;
    }

    int Pending() const override {
        return static_cast<int>((std::min)(cleartext_read_buf.size(),
                                           static_cast<size_t>(std::numeric_limits<int>::max())));
    }

    Result GetServerCerts(std::vector<std::vector<u8>>* out_certs) override {
        PCCERT_CONTEXT returned_cert = nullptr;
        const SECURITY_STATUS ret =
            QueryContextAttributes(&ctxt, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &returned_cert);
        if (ret != SEC_E_OK) {
            LOG_ERROR(Service_SSL,
                      "QueryContextAttributes(SECPKG_ATTR_REMOTE_CERT_CONTEXT) failed: {}",
                      Common::NativeErrorToString(ret));
            return ResultInternalError;
        }

        // Locate the leaf certificate instead of assuming the store's enumeration order.
        size_t leaf_index = std::numeric_limits<size_t>::max();
        PCCERT_CONTEXT some_cert = nullptr;
        while ((some_cert = CertEnumCertificatesInStore(returned_cert->hCertStore, some_cert))) {
            if (some_cert->cbCertEncoded == returned_cert->cbCertEncoded &&
                std::memcmp(some_cert->pbCertEncoded, returned_cert->pbCertEncoded,
                            returned_cert->cbCertEncoded) == 0) {
                leaf_index = out_certs->size();
            }
            out_certs->emplace_back(static_cast<u8*>(some_cert->pbCertEncoded),
                                    static_cast<u8*>(some_cert->pbCertEncoded) +
                                        some_cert->cbCertEncoded);
        }

        if (leaf_index == std::numeric_limits<size_t>::max()) {
            // Use the server certificate when the store omits the leaf.
            out_certs->insert(out_certs->begin(),
                              std::vector<u8>{static_cast<u8*>(returned_cert->pbCertEncoded),
                                              static_cast<u8*>(returned_cert->pbCertEncoded) +
                                                  returned_cert->cbCertEncoded});
        } else if (leaf_index == out_certs->size() - 1) {
            // Reverse a root-first chain to put the leaf first.
            std::reverse(out_certs->begin(), out_certs->end());
        } else if (leaf_index != 0) {
            // Move the leaf first and preserve the order of the other certificates.
            auto leaf = std::move((*out_certs)[leaf_index]);
            out_certs->erase(out_certs->begin() + static_cast<std::ptrdiff_t>(leaf_index));
            out_certs->insert(out_certs->begin(), std::move(leaf));
        }

        CertFreeCertificateContext(returned_cert);
        return ResultSuccess;
    }

    ~SSLConnectionBackendSchannel() override {
        if (handshake_state != HandshakeState::Initial) {
            DeleteSecurityContext(&ctxt);
        }
    }

private:
    enum class HandshakeState {
        Initial,
        ContinueNeeded,
        IncompleteMessage,
        DoneAfterFlush,
        Connected,
        Error,
    } handshake_state = HandshakeState::Initial;

    CtxtHandle ctxt{};
    SecPkgContext_StreamSizes stream_sizes{};

    std::shared_ptr<Network::SocketBase> socket;
    std::optional<std::string> hostname;

    std::vector<u8> ciphertext_read_buf;
    std::vector<u8> ciphertext_write_buf;
    std::vector<u8> cleartext_read_buf;
    std::vector<u8> cleartext_write_buf;

    bool got_read_eof = false;
    u32 verify_option = VerifyOptionPeerCa | VerifyOptionHostName;
    size_t read_buf_fill_size = 0;
};

Result CreateSSLConnectionBackend(std::unique_ptr<SSLConnectionBackend>* out_backend) {
    auto conn = std::make_unique<SSLConnectionBackendSchannel>();
    R_TRY(conn->Init());
    *out_backend = std::move(conn);
    return ResultSuccess;
}

} // namespace Service::SSL
