#pragma once

#include "pch.h"
#include "DBManager.h"

struct SubscriptionPlanInfo {
    int   planId      = 0;
    int   grade       = 0;
    int64 storageBytes = 0;
};

struct cSubscriptionInfo {
    string status;          // 'active', 'grace', 'cancelled', 'expired'
    int64  expiresAt     = 0;
    int64  graceExpiresAt = 0;
    bool   autoRenew     = false;
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
    static bool RecordTransaction(const string& groupId, int planId, const string& platform,
                                  const string& productId, const string& transactionId,
                                  const string& purchaseToken);

    static bool GetPlanInfo(int planId, SubscriptionPlanInfo& OUT info);
    static bool GetPlanInfoByProductId(const string& productId, SubscriptionPlanInfo& OUT info);

    static bool UpsertSubscription(const string& groupId, int planId, int64 expiresAtMs);
    static bool SetGrace(const string& groupId, int64 graceExpiresAtMs);
    static bool SetExpired(const string& groupId);
    static bool CancelSubscription(const string& groupId);

    static bool GetActiveSubscription(const string& groupId, cSubscriptionInfo& OUT info);

    static vector<PlanDetail> GetAvailablePlans();

    // 배치잡용: 만료된 구독, grace 만료된 구독 조회
    static vector<string> GetExpiredActiveGroupIds(int64 nowMs);
    static vector<string> GetExpiredGraceGroupIds(int64 nowMs);
};
