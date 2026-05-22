#include "pch.h"
#include "ExpirationBatchJob.h"
#include "Cloud/CloudStorage.h"
#include "ChatService.h"
#include "../DB/FileRepository.h"
#include "../DB/MessageRepository.h"
#include "../DB/GroupRepository.h"
#include "../DB/UserRepository.h"
#include "../DB/AlbumRepository.h"
#include "../DB/SubscriptionRepository.h"
#include "../DB/PollRepository.h"
#include "../CoreGlobal.h"

#include <iostream>
#include <chrono>

using namespace std;

ExpirationBatchJob::ExpirationBatchJob(CloudStorage* cloudStorage, int checkIntervalSec)
    : _cloudStorage(cloudStorage), _intervalSec(checkIntervalSec)
{
}

ExpirationBatchJob::~ExpirationBatchJob()
{
    Stop();
}

void ExpirationBatchJob::Start()
{
    if (_running.exchange(true)) return;

    _thread = thread([this]() { RunLoop(); });
    LOG_INFO("[ExpirationBatchJob] Started (interval: {}s)", _intervalSec);
}

void ExpirationBatchJob::Stop()
{
    if (!_running.exchange(false)) return;
    if (_thread.joinable())
        _thread.join();
    LOG_INFO("[ExpirationBatchJob] Stopped");
}

void ExpirationBatchJob::RunOnce()
{
    ProcessExpiredFiles();
    ProcessSoftDeletedAlbumPhotos();
    ProcessExpiredSubscriptions();
    ProcessExpiredPolls();
}

void ExpirationBatchJob::RunLoop()
{
    // Wait 60s after server start before first run
    this_thread::sleep_for(chrono::seconds(60));

    while (_running.load()) {
        ProcessExpiredFiles();
        ProcessSoftDeletedAlbumPhotos();
        ProcessExpiredSubscriptions();
        ProcessExpiredPolls();

        // Sleep in 1s chunks; check polls every 60s within the main 6-hour cycle
        int pollTick = 0;
        for (int i = 0; i < _intervalSec && _running.load(); ++i) {
            this_thread::sleep_for(chrono::seconds(1));
            if (++pollTick >= 60) {
                ProcessExpiredPolls();
                pollTick = 0;
            }
        }
    }
}

void ExpirationBatchJob::ProcessExpiredFiles()
{
    if (!_cloudStorage) {
        LOG_ERROR("[ExpirationBatchJob] CloudStorage not initialized - skip");
        return;
    }

    int64 now = Nowts();
    LOG_INFO("[ExpirationBatchJob] Processing expired files (now={})", now);

    auto expiredFiles = FileRepository::GetExpiredFiles(now);
    if (expiredFiles.empty()) {
        LOG_INFO("[ExpirationBatchJob] No expired files");
        return;
    }

    LOG_INFO("[ExpirationBatchJob] Found {} expired file(s)", expiredFiles.size());

    int deleted = 0, failed = 0;
    for (const auto& fi : expiredFiles) {
        bool gcsOk = true;

        // 1) Delete original GCS file
        if (!fi.gcsPath.empty()) {
            gcsOk = _cloudStorage->RemoveFile(fi.gcsPath);
            if (!gcsOk) {
                LOG_ERROR("[ExpirationBatchJob] GCS delete failed: {}", fi.gcsPath);
            }
        }

        // 2) Delete thumbnail (if exists)
        if (!fi.thumbnailGcsPath.empty()) {
            _cloudStorage->RemoveFile(fi.thumbnailGcsPath);
        }

        // 3) Update file_uploads status
        FileRepository::UpdateUploadStatus(fi.fileId, "deleted");

        // 4) Update messages: file_status = 'expired', invalidate URLs
        if (!fi.gcsPath.empty()) {
            MessageRepository::UpdateFileStatusByGcsPath(fi.gcsPath, "expired");
        }

        // 5) Release storage quota (group or personal)
        if (!fi.gcsPath.empty()) {
            if (!fi.groupId.empty()) {
                GroupRepository::DeleteGroupAssetByGcsPath(fi.gcsPath);
            } else {
                UserRepository::DeleteUserAssetByGcsPath(fi.gcsPath);
            }
        }

        if (gcsOk) {
            ++deleted;
            LOG_INFO("[ExpirationBatchJob] Deleted: {}", fi.gcsPath);
        } else {
            ++failed;
        }
    }

    LOG_INFO("[ExpirationBatchJob] Done: deleted={} failed={}", deleted, failed);
}

void ExpirationBatchJob::ProcessSoftDeletedAlbumPhotos()
{
    if (!_cloudStorage) return;

    int64 now = Nowts();
    const int64 THIRTY_DAYS_MS = 30LL * 24 * 3600 * 1000;
    int64 cutoff = now - THIRTY_DAYS_MS;

    auto photos = AlbumRepository::GetSoftDeletedPhotosOlderThan(cutoff);
    if (photos.empty()) return;

    LOG_INFO("[ExpirationBatchJob] 소프트딜리트 앨범사진 정리: {}개", photos.size());

    for (const auto& p : photos) {
        if (!p.gcsPath.empty()) {
            _cloudStorage->RemoveFile(p.gcsPath);
            // 썸네일 경로 파생 (확장자 앞에 _thumb 삽입)
            auto dot = p.gcsPath.find_last_of('.');
            string thumbPath = (dot != string::npos)
                ? p.gcsPath.substr(0, dot) + "_thumb.jpg"
                : p.gcsPath + "_thumb.jpg";
            _cloudStorage->RemoveFile(thumbPath);
        }
        AlbumRepository::HardDeletePhoto(p.id);
        LOG_INFO("[ExpirationBatchJob] 앨범사진 하드딜리트: id={}", p.id);
    }
}

void ExpirationBatchJob::ProcessExpiredPolls()
{
    if (!GChatService) return;

    int64 now = Nowts();
    auto polls = PollRepository::GetAllExpiredActivePolls(now);
    if (polls.empty()) return;

    LOG_INFO("[ExpirationBatchJob] 만료된 투표 자동 종료: {}개", polls.size());
    for (const auto& poll : polls) {
        if (MessageRepository::PollClosedMessageExists(poll.convId, poll.pollId)) continue;
        if (PollRepository::ClosePoll(poll.pollId, poll.creatorId)) {
            GChatService->SendPollClosedNotification(poll.convId, poll.pollId, poll.creatorId, 0);
            LOG_INFO("[ExpirationBatchJob] 투표 자동 종료 완료: pollId={}", poll.pollId);
        }
    }
}


void ExpirationBatchJob::ProcessExpiredSubscriptions()
{
    int64 now = Nowts();
    const int64 GRACE_MS = 14LL * 24 * 3600 * 1000;

    // 1) active 상태인데 expires_at 지난 구독 → grace로 전환
    auto expiredGroups = SubscriptionRepository::GetExpiredActiveGroupIds(now);
    for (const auto& groupId : expiredGroups) {
        int64 graceExpiresAt = now + GRACE_MS;
        SubscriptionRepository::SetGrace(groupId, graceExpiresAt);
        LOG_INFO("[ExpirationBatchJob] 구독 grace 전환: group={} graceExpires={}", groupId, graceExpiresAt);
    }

    // 2) grace 상태인데 grace_expires_at 지난 구독 → 앨범 사진 전부 소프트딜리트 + 스토리지 초기화
    auto graceExpiredGroups = SubscriptionRepository::GetExpiredGraceGroupIds(now);
    for (const auto& groupId : graceExpiredGroups) {
        AlbumRepository::SoftDeleteAllPhotosByGroup(groupId, now);
        GroupRepository::ResetStorageToFree(groupId);
        SubscriptionRepository::SetExpired(groupId);
        LOG_INFO("[ExpirationBatchJob] 구독 만료 처리 완료: group={}", groupId);
    }

    if (!expiredGroups.empty() || !graceExpiredGroups.empty()) {
        LOG_INFO("[ExpirationBatchJob] 구독 처리: grace전환={}건 만료={}건", expiredGroups.size(), graceExpiredGroups.size());
    }
}
