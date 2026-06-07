#pragma once

#include "pch.h"
#include "DBManager.h"

struct SubscriptionPlanInfo {
    int    planId       = 0;
    int    grade        = 0;
    int64  storageBytes = 0;
    string name;
};

struct cSubscriptionInfo {
    string status;
    int    planId            = 0;
    int64  expiresAt         = 0;
    int64  graceExpiresAt    = 0;
    bool   autoRenew         = false;
    bool   cancelAtPeriodEnd = false;
    int    pendingPlanId     = 0;
};

struct PlanDetail {
    int             planId       = 0;
    string          name;
    int64           storageBytes = 0;
    int64           maxFileSize  = 0;
    double          monthlyPrice = 0.0;
};

class SubscriptionRepository {
public:
    static bool RecordTransaction(const string& groupId, const string& userId, int planId,
                                  const string& platform, const string& productId,
                                  const string& transactionId, const string& purchaseToken);

    static bool GetPlanInfo(int planId, SubscriptionPlanInfo& OUT info);
    static bool GetPlanInfoByProductId(const string& productId, SubscriptionPlanInfo& OUT info);

    // 구독 상태 변경
    static bool UpsertSubscription(const string& groupId, int planId, int64 expiresAtMs);
    static bool SetGrace(const string& groupId, int64 graceExpiresAtMs);
    static bool SetGraceForCancel(const string& groupId, int64 graceExpiresAtMs); // plan_id→1 + grace
    static bool SetExpired(const string& groupId);
    static bool SetExpiredFromActive(const string& groupId); // cancel + usage≤1GB → 바로 만료
    static bool CancelSubscription(const string& groupId);

    // 해지 예정 / 다운그레이드 예정
    static bool SetCancelAtPeriodEnd(const string& groupId);
    static bool SetPendingPlan(const string& groupId, int pendingPlanId);
    static bool ApplyPendingPlan(const string& groupId, int planId); // plan_id=planId, pending_plan_id=NULL

    static bool GetActiveSubscription(const string& groupId, cSubscriptionInfo& OUT info);

    static vector<PlanDetail> GetAvailablePlans();

    // 배치잡용
    static vector<string> GetExpiredActiveGroupIds(int64 nowMs); // 모든 만료된 active
    static vector<string> GetExpiredGraceGroupIds(int64 nowMs);
};
