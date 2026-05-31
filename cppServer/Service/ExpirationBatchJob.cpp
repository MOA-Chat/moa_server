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
        bool deleteOk = true;

        if (!fi.filePath.empty()) {
            deleteOk = _cloudStorage->RemoveFile(fi.filePath);
            if (!deleteOk) {
                LOG_ERROR("[ExpirationBatchJob] R2 delete failed: {}", fi.filePath);
            }
        }

        if (!fi.thumbnailPath.empty()) {
            _cloudStorage->RemoveFile(fi.thumbnailPath);
        }

        FileRepository::UpdateUploadStatus(fi.fileId, "deleted");

        if (!fi.filePath.empty()) {
            MessageRepository::UpdateFileStatusByFilePath(fi.filePath, "expired");
        }

        if (!fi.filePath.empty()) {
            if (!fi.groupId.empty()) {
                GroupRepository::DeleteGroupAssetByFilePath(fi.filePath);
            } else {
                UserRepository::DeleteUserAssetByFilePath(fi.filePath);
            }
        }

        if (deleteOk) {
            ++deleted;
            LOG_INFO("[ExpirationBatchJob] Deleted: {}", fi.filePath);
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
        if (!p.filePath.empty()) {
            _cloudStorage->RemoveFile(p.filePath);
            auto dot = p.filePath.find_last_of('.');
            string thumbPath = (dot != string::npos)
                ? p.filePath.substr(0, dot) + "_thumb.jpg"
                : p.filePath + "_thumb.jpg";
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

    // 1) active 상태인데 expires_at 지난 구독 → grace로 전환 + 그룹 알림
    auto expiredGroups = SubscriptionRepository::GetExpiredActiveGroupIds(now);
    for (const auto& groupId : expiredGroups) {
        int64 graceExpiresAt = now + GRACE_MS;
        SubscriptionRepository::SetGrace(groupId, graceExpiresAt);
        if (GChatService) {
            GChatService->SendSystemMessage(groupId,
                "[구독 알림] 구독 기간이 만료되었습니다. 14일 내 재구독하지 않으면 앨범 사진이 삭제됩니다.");
        }
        LOG_INFO("[ExpirationBatchJob] 구독 grace 전환: group={} graceExpires={}", groupId, graceExpiresAt);
    }

    // 2) grace 상태인데 grace_expires_at 지난 구독 → 1GB 초과 오래된 사진 소프트딜리트 + 용량 초기화
    // SetExpired는 두 작업이 모두 성공한 경우에만 호출 (원자성 보장)
    const int64 FREE_PLAN_QUOTA = 1073741824LL; // 1GB
    auto graceExpiredGroups = SubscriptionRepository::GetExpiredGraceGroupIds(now);
    for (const auto& groupId : graceExpiredGroups) {
        int64 deletedSize = AlbumRepository::SoftDeleteOverQuotaPhotosByGroup(groupId, FREE_PLAN_QUOTA, now);
        if (deletedSize < 0) {
            LOG_ERROR("[ExpirationBatchJob] 구독 만료 처리 실패 (사진 소프트딜리트): group={}", groupId);
            continue;
        }
        bool capacityOk = GroupRepository::SetStorageCapacityOnly(groupId, FREE_PLAN_QUOTA);
        if (deletedSize > 0) GroupRepository::DeductGroupStorage(groupId, deletedSize);
        if (capacityOk) {
            SubscriptionRepository::SetExpired(groupId);
            LOG_INFO("[ExpirationBatchJob] 구독 만료 처리 완료: group={} deletedBytes={}", groupId, deletedSize);
        } else {
            LOG_ERROR("[ExpirationBatchJob] 구독 만료 처리 실패 (용량 초기화): group={}", groupId);
        }
    }

    if (!expiredGroups.empty() || !graceExpiredGroups.empty()) {
        LOG_INFO("[ExpirationBatchJob] 구독 처리: grace전환={}건 만료={}건",
                 expiredGroups.size(), graceExpiredGroups.size());
    }
}
