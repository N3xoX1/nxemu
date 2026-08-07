// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/hex_util.h"
#include "yuzu_common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/bcat/backend/backend.h"

namespace Service::BCAT {

ProgressServiceBackend::ProgressServiceBackend(Core::System& system, std::string_view event_name)
    : service_context{system, "ProgressServiceBackend"} {
    update_event = service_context.CreateEvent("ProgressServiceBackend:UpdateEvent:" +
                                               std::string(event_name));
}

ProgressServiceBackend::~ProgressServiceBackend() {
    service_context.CloseEvent(update_event);
}

Kernel::KReadableEvent& ProgressServiceBackend::GetEvent() {
    return update_event->GetReadableEvent();
}

DeliveryCacheProgressImpl& ProgressServiceBackend::GetImpl() {
    return impl;
}

void ProgressServiceBackend::SetTotalSize(u64 size) {
    impl.total_bytes = size;
    SignalUpdate();
}

void ProgressServiceBackend::StartConnecting() {
    impl.status = DeliveryCacheProgressStatus::Connecting;
    SignalUpdate();
}

void ProgressServiceBackend::StartProcessingDataList() {
    impl.status = DeliveryCacheProgressStatus::ProcessingDataList;
    SignalUpdate();
}

void ProgressServiceBackend::StartDownloadingFile(std::string_view dir_name,
                                                  std::string_view file_name, u64 file_size) {
    impl.status = DeliveryCacheProgressStatus::Downloading;
    impl.current_downloaded_bytes = 0;
    impl.current_total_bytes = file_size;
    std::memcpy(impl.current_directory.data(), dir_name.data(),
                std::min<u64>(dir_name.size(), 0x31ull));
    std::memcpy(impl.current_file.data(), file_name.data(),
                std::min<u64>(file_name.size(), 0x31ull));
    SignalUpdate();
}

void ProgressServiceBackend::UpdateFileProgress(u64 downloaded) {
    impl.current_downloaded_bytes = downloaded;
    SignalUpdate();
}

void ProgressServiceBackend::FinishDownloadingFile() {
    impl.total_downloaded_bytes += impl.current_total_bytes;
    SignalUpdate();
}

void ProgressServiceBackend::CommitDirectory(std::string_view dir_name) {
    impl.status = DeliveryCacheProgressStatus::Committing;
    impl.current_file.fill(0);
    impl.current_downloaded_bytes = 0;
    impl.current_total_bytes = 0;
    std::memcpy(impl.current_directory.data(), dir_name.data(),
                std::min<u64>(dir_name.size(), 0x31ull));
    SignalUpdate();
}

void ProgressServiceBackend::FinishDownload(Result result) {
    impl.total_downloaded_bytes = impl.total_bytes;
    impl.status = DeliveryCacheProgressStatus::Done;
    impl.result = result;
    SignalUpdate();
}

void ProgressServiceBackend::SignalUpdate() {
    update_event->Signal();
}

BcatBackend::BcatBackend() = default;

BcatBackend::~BcatBackend() = default;

bool BcatBackend::Synchronize(TitleIDVersion title, ProgressServiceBackend& progress) {
    LOG_DEBUG(Service_BCAT, "called, title_id={:016X}, build_id={:016X}", title.title_id,
              title.build_id);

    progress.FinishDownload(ResultSuccess);
    return true;
}

bool BcatBackend::SynchronizeDirectory(TitleIDVersion title, std::string name,
                                       ProgressServiceBackend& progress) {
    LOG_DEBUG(Service_BCAT, "called, title_id={:016X}, build_id={:016X}, name={}", title.title_id,
              title.build_id, name);

    progress.FinishDownload(ResultSuccess);
    return true;
}

bool BcatBackend::Clear(u64 title_id) {
    LOG_DEBUG(Service_BCAT, "called, title_id={:016X}", title_id);

    return true;
}

void BcatBackend::SetPassphrase(u64 title_id, const Passphrase& passphrase) {
    LOG_DEBUG(Service_BCAT, "called, title_id={:016X}, passphrase={}", title_id,
              Common::HexToString(passphrase));
}

std::optional<std::vector<u8>> BcatBackend::GetLaunchParameter(TitleIDVersion title) {
    LOG_DEBUG(Service_BCAT, "called, title_id={:016X}, build_id={:016X}", title.title_id,
              title.build_id);
    return std::nullopt;
}

} // namespace Service::BCAT
