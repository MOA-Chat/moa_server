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

        // R2 삭제 성공 시에만 DB 마킹 (실패 시 다음 배치 주기에 재시도)
        if (deleteOk) {
            FileRepository::UpdateUploadStatus(fi.fileId, "deleted");
        }

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
    const int64 GRACE_MS = 7LL * 24 * 3600 * 1000;   // 7일 유예
    const int64 FREE_PLAN_QUOTA = 1073741824LL;         // 1GB (무료 플랜)

    // ── 1) active + expires_at 만료 → 케이스별 처리 ──────────────────────────
    auto expiredGroups = SubscriptionRepository::GetExpiredActiveGroupIds(now);
    int graceCount = 0, cancelCount = 0, downgradeCount = 0;

    for (const auto& groupId : expiredGroups) {
        cSubscriptionInfo subInfo;
        if (!SubscriptionRepository::GetActiveSubscription(groupId, subInfo)) continue;

        if (subInfo.cancelAtPeriodEnd) {
            // ── 해지 예약 만료 ────────────────────────────────────────────────
            cGroupInfo gInfo;
            GroupRepository::GetGroupInfoById(groupId, gInfo);
            if (gInfo.storageUsage <= FREE_PLAN_QUOTA) {
                // 1GB 이하: 바로 만료 (grace 불필요)
                SubscriptionRepository::SetExpiredFromActive(groupId);
                GroupRepository::SetStorageCapacityOnly(groupId, FREE_PLAN_QUOTA);
                LOG_INFO("[ExpirationBatchJob] 해지 만료 (용량 OK): group={}", groupId);
            } else {
                // 1GB 초과: 7일 grace 부여 (plan_id=1로 변경)
                int64 graceExpiresAt = now + GRACE_MS;
                SubscriptionRepository::SetGraceForCancel(groupId, graceExpiresAt);
                LOG_INFO("[ExpirationBatchJob] 해지 만료→grace: group={} usage={} grace={}", groupId, gInfo.storageUsage, graceExpiresAt);
            }
            ++cancelCount;

        } else if (subInfo.pendingPlanId > 0) {
            // ── 다운그레이드 예약 만료 → 새 플랜 적용 ───────────────────────
            SubscriptionPlanInfo pendingInfo;
            if (!SubscriptionRepository::GetPlanInfo(subInfo.pendingPlanId, pendingInfo)) {
                LOG_ERROR("[ExpirationBatchJob] 다운그레이드 플랜 정보 없음: group={} planId={}", groupId, subInfo.pendingPlanId);
                continue;
            }
            // 새 플랜 적용
            SubscriptionRepository::ApplyPendingPlan(groupId, subInfo.pendingPlanId);
            GroupRepository::UpdateStorageCapacity(groupId, pendingInfo.storageBytes);

            cGroupInfo gInfo;
            GroupRepository::GetGroupInfoById(groupId, gInfo);
            if (gInfo.storageUsage > pendingInfo.storageBytes) {
                // 초과: 7일 grace
                int64 graceExpiresAt = now + GRACE_MS;
                SubscriptionRepository::SetGrace(groupId, graceExpiresAt);
                LOG_INFO("[ExpirationBatchJob] 다운그레이드→grace: group={} plan={} usage={}", groupId, pendingInfo.name, gInfo.storageUsage);
            } else {
                LOG_INFO("[ExpirationBatchJob] 다운그레이드 완료: group={} plan={}", groupId, pendingInfo.name);
            }
            ++downgradeCount;

        } else {
            // ── 일반 만료 (갱신 실패 등) → grace ─────────────────────────────
            int64 graceExpiresAt = now + GRACE_MS;
            SubscriptionRepository::SetGrace(groupId, graceExpiresAt);
            LOG_INFO("[ExpirationBatchJob] 구독 grace 전환: group={} graceExpires={}", groupId, graceExpiresAt);
            ++graceCount;
        }
    }

    // ── 2) grace + grace_expires_at 만료 → 사진 정리 + 최종 만료 ───────────
    auto graceExpiredGroups = SubscriptionRepository::GetExpiredGraceGroupIds(now);
    int expiredCount = 0;

    for (const auto& groupId : graceExpiredGroups) {
        // 현재 plan_id로 적용 쿼터 결정
        // - 일반 만료/다운그레이드 grace: plan_id = 다운그레이드된 플랜
        // - 해지 grace: plan_id = 1 (SetGraceForCancel이 1로 설정함)
        cSubscriptionInfo gSub;
        int64 quota = FREE_PLAN_QUOTA;
        if (SubscriptionRepository::GetActiveSubscription(groupId, gSub) && gSub.planId > 0) {
            SubscriptionPlanInfo planInfo;
            if (SubscriptionRepository::GetPlanInfo(gSub.planId, planInfo) && planInfo.storageBytes > 0) {
                quota = planInfo.storageBytes;
            }
        }

        int64 deletedSize = AlbumRepository::SoftDeleteOverQuotaPhotosByGroup(groupId, quota, now);
        if (deletedSize < 0) {
            LOG_ERROR("[ExpirationBatchJob] grace 만료 처리 실패 (사진 정리): group={}", groupId);
            continue;
        }
        bool capacityOk = GroupRepository::SetStorageCapacityOnly(groupId, quota);
        if (deletedSize > 0) GroupRepository::DeductGroupStorage(groupId, deletedSize);
        if (capacityOk) {
            SubscriptionRepository::SetExpired(groupId);
            LOG_INFO("[ExpirationBatchJob] grace 만료: group={} quota={}B deleted={}B", groupId, quota, deletedSize);
            ++expiredCount;
        } else {
            LOG_ERROR("[ExpirationBatchJob] grace 만료 처리 실패 (용량 초기화): group={}", groupId);
        }
    }

    if (!expiredGroups.empty() || !graceExpiredGroups.empty()) {
        LOG_INFO("[ExpirationBatchJob] 구독 처리: grace={}건 cancel={}건 downgrade={}건 expired={}건",
                 graceCount, cancelCount, downgradeCount, expiredCount);
    }
}
