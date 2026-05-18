#pragma once

#include "pch.h"
#include "DBManager.h"

struct SubscriptionPlanInfo {
    int   grade        = 0;
    int64 storageBytes = 0;
};

struct cSubscriptionInfo {
    int64 expiresAt = 0;    // ms timestamp, 0 = 영구
    bool  autoRenew = false;
};

struct PlanDetail {
    int             planId       = 0;
    string          planType;
    string          name;
    int             grade        = 0;
    int64           storageBytes = 0;
    int64           maxFileSize  = 0;
    double          monthlyPrice = 0.0;
    vector<string>  features;
};

class SubscriptionRepository {
public:
    static bool RecordTransaction(const string& userId, int planId, const string& platform,
                                  const string& productId, const string& transactionId,
                                  const string& purchaseToken);

    static bool GetPlanInfo(int planId, SubscriptionPlanInfo& OUT info);

    static bool UpsertSubscription(const string& userId, int planId, int64 expiresAtMs);

    static bool CancelSubscription(const string& userId);

    static bool GetActiveSubscription(const string& userId, cSubscriptionInfo& OUT info);

    static vector<PlanDetail> GetAvailablePlans(const string& planType);
};
