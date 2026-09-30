// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "yuzu_common/alignment.h"
#include "yuzu_common/fs/filesystem_interfaces.h"
#include "core/core.h"
#include "core/hle/service/ssl/ssl_backend.h"
#include "core/hle/service/ssl/cert_store.h"

namespace Service::SSL {

// https://switchbrew.org/wiki/SSL_services#CertStore

CertStore::CertStore(Core::System& system) {
    constexpr u64 CertStoreDataId = 0x0100000000000800ULL;

    auto& loader = system.GetSystemloader();
    auto& fsc = loader.FileSystemController();
    auto& nand = fsc.GetSystemNANDContents();

    FileSysNCAPtr nca{nand.GetEntry(CertStoreDataId, LoaderContentRecordType::Data)};
    if (!nca) {
        LOG_WARNING(Service_SSL, "CertStore system data is not installed");
        return;
    }

    IVirtualFilePtr romfs{nca->GetRomFS()};
    if (!romfs) {
        LOG_ERROR(Service_SSL, "CertStore NCA has no RomFS");
        return;
    }

    IVirtualDirectoryPtr extracted{romfs->ExtractRomFS()};
    if (!extracted) {
        LOG_ERROR(Service_SSL, "CertStore could not be extracted, corrupt RomFS?");
        return;
    }

    IVirtualFilePtr cert_store_file{extracted->GetFile("ssl_TrustedCerts.bdf")};
    if (!cert_store_file) {
        // Firmware 1.0.0-2.3.0 used the old .tcf name.
        cert_store_file = IVirtualFilePtr{extracted->GetFile("ssl_TrustedCerts.tcf")};
    }
    if (!cert_store_file) {
        LOG_ERROR(Service_SSL, "Failed to find trusted certificates in CertStore");
        return;
    }

    CertStoreHeader header{};
    if (cert_store_file->ReadBytes(reinterpret_cast<u8*>(&header), sizeof(header), 0) !=
        sizeof(header)) {
        LOG_ERROR(Service_SSL, "Failed to read CertStore header");
        return;
    }

    if (header.magic != Common::MakeMagic('s', 's', 'l', 'T')) {
        LOG_ERROR(Service_SSL, "Invalid certificate store magic");
        return;
    }

    const u64 file_size = cert_store_file->GetSize();
    const u64 entries_size = static_cast<u64>(sizeof(CertStoreEntry)) * header.num_entries;
    if (header.num_entries != 0 && entries_size / sizeof(CertStoreEntry) != header.num_entries) {
        LOG_ERROR(Service_SSL, "CertStore entry count overflow");
        return;
    }
    if (entries_size > file_size || sizeof(header) > file_size - entries_size) {
        LOG_ERROR(Service_SSL, "CertStore entry table exceeds file size");
        return;
    }

    std::vector<CertStoreEntry> entries(header.num_entries);
    if (!entries.empty() &&
        cert_store_file->ReadBytes(reinterpret_cast<u8*>(entries.data()), entries_size,
                                   sizeof(header)) != entries_size) {
        LOG_ERROR(Service_SSL, "Failed to read CertStore entries");
        return;
    }

    for (const auto& entry : entries) {
        // Data offsets in ssl_TrustedCerts are relative to the first byte after the 0x8-byte header.
        const u64 data_offset = sizeof(header) + static_cast<u64>(entry.der_offset);
        const u64 data_size = entry.der_size;
        if (data_offset > file_size || data_size > file_size - data_offset) {
            LOG_ERROR(Service_SSL,
                      "CertStore entry {} points outside file (offset={}, size={}, file={})",
                      static_cast<s32>(entry.certificate_id), data_offset, data_size, file_size);
            return;
        }

        std::vector<u8> der_data(data_size);
        if (data_size != 0 &&
            cert_store_file->ReadBytes(der_data.data(), data_size, data_offset) != data_size) {
            LOG_ERROR(Service_SSL, "Failed to read certificate {}",
                      static_cast<s32>(entry.certificate_id));
            return;
        }

        m_certs.emplace(entry.certificate_id,
                        Certificate{.status = entry.certificate_status,
                                    .der_data = std::move(der_data)});
    }

    LOG_INFO(Service_SSL, "Loaded {} built-in SSL certificates", m_certs.size());
}

CertStore::~CertStore() = default;

template <typename F>
void CertStore::ForEachCertificate(std::span<const CaCertificateId> certificate_ids, F&& f) {
    if (certificate_ids.size() == 1 && certificate_ids.front() == CaCertificateId::All) {
        for (const auto& entry : m_certs) {
            f(entry);
        }
    } else {
        for (const auto certificate_id : certificate_ids) {
            const auto entry = m_certs.find(certificate_id);
            if (entry == m_certs.end()) {
                continue;
            }
            f(*entry);
        }
    }
}

Result CertStore::GetCertificates(u32* out_num_entries, std::span<u8> out_data,
                                  std::span<const CaCertificateId> certificate_ids) {
    u32 required_size{};
    R_TRY(this->GetCertificateBufSize(std::addressof(required_size), out_num_entries,
                                      certificate_ids));
    R_UNLESS(out_data.size_bytes() >= required_size, ResultInvalidCertBufSize);

    const bool include_terminator =
        certificate_ids.size() == 1 && certificate_ids.front() == CaCertificateId::All;
    const u32 info_count = *out_num_entries + (include_terminator ? 1U : 0U);
    u32 cur_der_offset = info_count * sizeof(BuiltInCertificateInfo);

    std::vector<BuiltInCertificateInfo> cert_infos;
    cert_infos.reserve(info_count);
    std::vector<u8> der_datas;
    der_datas.reserve(required_size - cur_der_offset);

    this->ForEachCertificate(certificate_ids, [&](const auto& entry) {
        const auto& [status, cur_der_data] = entry.second;
        cert_infos.push_back(BuiltInCertificateInfo{
            .cert_id = entry.first,
            .status = status,
            .der_size = cur_der_data.size(),
            .der_offset = cur_der_offset,
        });

        der_datas.insert(der_datas.end(), cur_der_data.begin(), cur_der_data.end());
        cur_der_offset += static_cast<u32>(cur_der_data.size());
    });

    if (include_terminator) {
        cert_infos.push_back(BuiltInCertificateInfo{
            .cert_id = CaCertificateId::All,
            .status = TrustedCertStatus::Invalid,
            .der_size = 0,
            .der_offset = 0,
        });
    }

    if (!cert_infos.empty()) {
        std::memcpy(out_data.data(), cert_infos.data(),
                    cert_infos.size() * sizeof(BuiltInCertificateInfo));
    }
    if (!der_datas.empty()) {
        std::memcpy(out_data.data() + info_count * sizeof(BuiltInCertificateInfo),
                    der_datas.data(), der_datas.size());
    }

    R_SUCCEED();
}

Result CertStore::GetCertificateBufSize(u32* out_size, u32* out_num_entries,
                                        std::span<const CaCertificateId> certificate_ids) {
    const bool include_terminator =
        certificate_ids.size() == 1 && certificate_ids.front() == CaCertificateId::All;

    *out_size = include_terminator ? sizeof(BuiltInCertificateInfo) : 0;
    *out_num_entries = 0;

    this->ForEachCertificate(certificate_ids, [&](const auto& entry) {
        *out_size += sizeof(BuiltInCertificateInfo);
        *out_size += Common::AlignUp(static_cast<u32>(entry.second.der_data.size()), 4);
        (*out_num_entries)++;
    });

    R_SUCCEED();
}

} // namespace Service::SSL
