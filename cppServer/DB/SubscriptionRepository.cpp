#include "pch.h"
#include "SubscriptionRepository.h"
#include "../CoreGlobal.h"

bool SubscriptionRepository::RecordTransaction(const string& groupId, const string& userId, int planId,
                                               const string& platform, const string& productId,
                                               const string& transactionId, const string& purchaseToken)
{
    try {
        auto& db = DBManager::GetInstance();
        db.GetSession().sql(
            "INSERT INTO payment_transactions "
            "(group_id, user_id, plan_id, platform, product_id, transaction_id, purchase_token, status, verified_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, 'verified', NOW()) "
            "ON DUPLICATE KEY UPDATE status='verified', verified_at=NOW()"
        ).bind(groupId, userId, planId, platform, productId, transactionId, purchaseToken).execute();
        return true;
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] RecordTransaction 실패: {}", err.what());
        return false;
    }
}

bool SubscriptionRepository::GetPlanInfo(int planId, SubscriptionPlanInfo& info)
{
    try {
        auto& db = DBManager::GetInstance();
        auto result = db.GetSession().sql(
            "SELECT plan_id, grade, storage_bytes, name FROM subscription_plans WHERE plan_id = ? LIMIT 1"
        ).bind(planId).execute();
        auto row = result.fetchOne();
        if (!row) return false;
        info.planId       = row[0].get<int>();
        info.grade        = row[1].get<int>();
        info.storageBytes = row[2].isNull() ? 0LL : static_cast<int64>(row[2].get<double>());
        info.name         = row[3].isNull() ? "" : row[3].get<string>();
        return true;
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] GetPlanInfo 실패: {}", err.what());
        return false;
    }
}

bool SubscriptionRepository::GetPlanInfoByProductId(const string& productId, SubscriptionPlanInfo& info)
{
    int planId = 1;
    if      (productId == "basic_monthly") planId = 2;
    else if (productId == "plus_monthly")  planId = 3;
    else if (productId == "max_monthly")   planId = 4;
    return GetPlanInfo(planId, info);
}

bool SubscriptionRepository::UpsertSubscription(const string& groupId, int planId, int64 expiresAtMs)
{
    try {
        auto& db = DBManager::GetInstance();
        db.GetSession().sql(
            "INSERT INTO subscriptions (group_id, plan_id, expires_at, auto_renew, status, cancel_at_period_end, pending_plan_id) "
            "VALUES (?, ?, ?, 1, 'active', 0, NULL) "
            "ON DUPLICATE KEY UPDATE plan_id=?, expires_at=?, auto_renew=1, status='active', "
            "grace_expires_at=NULL, cancel_at_period_end=0, pending_plan_id=NULL"
        ).bind(groupId, planId, expiresAtMs, planId, expiresAtMs).execute();
        return true;
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] UpsertSubscription 실패: {}", err.what());
        return false;
    }
}

bool SubscriptionRepository::SetGrace(const string& groupId, int64 graceExpiresAtMs)
{
    try {
        auto& db = DBManager::GetInstance();
        db.GetSession().sql(
            "UPDATE subscriptions SET status='grace', grace_expires_at=? "
            "WHERE group_id=? AND status='active'"
        ).bind(graceExpiresAtMs, groupId).execute();
        return true;
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] SetGrace 실패: {}", err.what());
        return false;
    }
}

bool SubscriptionRepository::SetGraceForCancel(const string& groupId, int64 graceExpiresAtMs)
{
    // cancel 만료 후 용량 초과 → plan_id=1(Free)로 변경 + grace 전환
    try {
        auto& db = DBManager::GetInstance();
        db.GetSession().sql(
            "UPDATE subscriptions SET status='grace', plan_id=1, grace_expires_at=?, "
            "cancel_at_period_end=0 WHERE group_id=? AND status='active'"
        ).bind(graceExpiresAtMs, groupId).execute();
        return true;
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] SetGraceForCancel 실패: {}", err.what());
        return false;
    }
}

bool SubscriptionRepository::SetExpired(const string& groupId)
{
    try {
        auto& db = DBManager::GetInstance();
        db.GetSession().sql(
            "UPDATE subscriptions SET status='expired' WHERE group_id=? AND status='grace'"
        ).bind(groupId).execute();
        return true;
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] SetExpired 실패: {}", err.what());
        return false;
    }
}

bool SubscriptionRepository::SetExpiredFromActive(const string& groupId)
{
    // cancel_at_period_end + 용량 1GB 이하 → 바로 만료 (grace 불필요)
    try {
        auto& db = DBManager::GetInstance();
        db.GetSession().sql(
            "UPDATE subscriptions SET status='expired', plan_id=1, cancel_at_period_end=0 "
            "WHERE group_id=? AND status='active'"
        ).bind(groupId).execute();
        return true;
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] SetExpiredFromActive 실패: {}", err.what());
        return false;
    }
}

bool SubscriptionRepository::CancelSubscription(const string& groupId)
{
    try {
        auto& db = DBManager::GetInstance();
        db.GetSession().sql(
            "UPDATE subscriptions SET status='cancelled', auto_renew=0 "
            "WHERE group_id=? AND status IN ('active','grace')"
        ).bind(groupId).execute();
        return true;
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] CancelSubscription 실패: {}", err.what());
        return false;
    }
}

bool SubscriptionRepository::SetCancelAtPeriodEnd(const string& groupId)
{
    try {
        auto& db = DBManager::GetInstance();
        db.GetSession().sql(
            "UPDATE subscriptions SET cancel_at_period_end=1, auto_renew=0, pending_plan_id=NULL "
            "WHERE group_id=? AND status='active'"
        ).bind(groupId).execute();
        return true;
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] SetCancelAtPeriodEnd 실패: {}", err.what());
        return false;
    }
}

bool SubscriptionRepository::SetPendingPlan(const string& groupId, int pendingPlanId)
{
    try {
        auto& db = DBManager::GetInstance();
        db.GetSession().sql(
            "UPDATE subscriptions SET pending_plan_id=?, cancel_at_period_end=0 "
            "WHERE group_id=? AND status='active'"
        ).bind(pendingPlanId, groupId).execute();
        return true;
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] SetPendingPlan 실패: {}", err.what());
        return false;
    }
}

bool SubscriptionRepository::ApplyPendingPlan(const string& groupId, int planId)
{
    try {
        auto& db = DBManager::GetInstance();
        db.GetSession().sql(
            "UPDATE subscriptions SET plan_id=?, pending_plan_id=NULL "
            "WHERE group_id=? AND status='active'"
        ).bind(planId, groupId).execute();
        return true;
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] ApplyPendingPlan 실패: {}", err.what());
        return false;
    }
}

bool SubscriptionRepository::GetActiveSubscription(const string& groupId, cSubscriptionInfo& info)
{
    try {
        auto& db = DBManager::GetInstance();
        auto result = db.GetSession().sql(
            "SELECT status, plan_id, expires_at, grace_expires_at, auto_renew, "
            "cancel_at_period_end, pending_plan_id "
            "FROM subscriptions "
            "WHERE group_id=? AND status IN ('active','grace') "
            "ORDER BY id DESC LIMIT 1"
        ).bind(groupId).execute();
        auto row = result.fetchOne();
        if (!row) return false;
        info.status            = row[0].isNull() ? "" : row[0].get<string>();
        info.planId            = row[1].isNull() ? 0  : row[1].get<int>();
        info.expiresAt         = row[2].isNull() ? 0LL : static_cast<int64>(row[2].get<double>());
        info.graceExpiresAt    = row[3].isNull() ? 0LL : static_cast<int64>(row[3].get<double>());
        info.autoRenew         = !row[4].isNull() && row[4].get<int>() != 0;
        info.cancelAtPeriodEnd = !row[5].isNull() && row[5].get<int>() != 0;
        info.pendingPlanId     = row[6].isNull() ? 0  : row[6].get<int>();
        return true;
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] GetActiveSubscription 실패: {}", err.what());
        return false;
    }
}

vector<PlanDetail> SubscriptionRepository::GetAvailablePlans()
{
    vector<PlanDetail> plans;
    try {
        auto& db = DBManager::GetInstance();
        auto result = db.GetSession().sql(
            "SELECT plan_id, name, storage_bytes, max_file_size, monthly_price "
            "FROM subscription_plans WHERE plan_type='group' AND is_active=1 ORDER BY grade ASC"
        ).execute();
        for (auto row = result.fetchOne(); row; row = result.fetchOne()) {
            PlanDetail p;
            p.planId       = row[0].get<int>();
            p.name         = row[1].get<string>();
            p.storageBytes = row[2].isNull() ? 0LL : static_cast<int64>(row[2].get<double>());
            p.maxFileSize  = row[3].isNull() ? 0LL : static_cast<int64>(row[3].get<double>());
            p.monthlyPrice = row[4].isNull() ? 0.0 : row[4].get<double>();
            plans.push_back(std::move(p));
        }
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] GetAvailablePlans 실패: {}", err.what());
    }
    return plans;
}

vector<string> SubscriptionRepository::GetExpiredActiveGroupIds(int64 nowMs)
{
    vector<string> groupIds;
    try {
        auto& db = DBManager::GetInstance();
        auto result = db.GetSession().sql(
            "SELECT group_id FROM subscriptions "
            "WHERE status='active' AND expires_at > 0 AND expires_at < ? LIMIT 200"
        ).bind(nowMs).execute();
        for (auto row = result.fetchOne(); row; row = result.fetchOne()) {
            groupIds.push_back(row[0].get<string>());
        }
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] GetExpiredActiveGroupIds 실패: {}", err.what());
    }
    return groupIds;
}

vector<string> SubscriptionRepository::GetExpiredGraceGroupIds(int64 nowMs)
{
    vector<string> groupIds;
    try {
        auto& db = DBManager::GetInstance();
        auto result = db.GetSession().sql(
            "SELECT group_id FROM subscriptions "
            "WHERE status='grace' AND grace_expires_at > 0 AND grace_expires_at < ? LIMIT 200"
        ).bind(nowMs).execute();
        for (auto row = result.fetchOne(); row; row = result.fetchOne()) {
            groupIds.push_back(row[0].get<string>());
        }
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] GetExpiredGraceGroupIds 실패: {}", err.what());
    }
    return groupIds;
}
