#include "pch.h"
#include "SubscriptionRepository.h"

#include <ctime>


bool SubscriptionRepository::RecordTransaction(const string& userId, int planId,
                                               const string& platform, const string& productId,
                                               const string& transactionId, const string& purchaseToken)
{
    try {
        auto& db = DBManager::GetInstance();
        auto schema = db.GetSchema();

        time_t now = ::time(nullptr);
        struct tm tmNow = {};
        localtime_s(&tmNow, &now);
        char nowStr[32];
        strftime(nowStr, sizeof(nowStr), "%Y-%m-%d %H:%M:%S", &tmNow);

        schema.getTable("payment_transactions")
            .insert("user_id", "plan_id", "platform", "product_id",
                    "transaction_id", "purchase_token", "status", "verified_at")
            .values(userId, planId, platform, productId,
                    transactionId, purchaseToken, "verified", string(nowStr))
            .execute();

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

        auto result = db.GetSchema().getTable("subscription_plans")
            .select("grade", "storage_bytes")
            .where("plan_id = :pid")
            .bind("pid", planId)
            .execute();

        auto row = result.fetchOne();
        if (!row) {
            LOG_ERROR("[SubscriptionRepository] Plan not found: {}", planId);
            return false;
        }

        info.grade        = row[0].get<int>();
        info.storageBytes = row[1].get<int64>();
        return true;
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] GetPlanInfo 실패: {}", err.what());
        return false;
    }
}


bool SubscriptionRepository::UpsertSubscription(const string& userId, int planId, int64 expiresAtMs)
{
    try {
        auto& db = DBManager::GetInstance();
        auto schema = db.GetSchema();

        time_t expSec = static_cast<time_t>(expiresAtMs / 1000);
        struct tm tmExp = {};
        gmtime_s(&tmExp, &expSec);
        char expStr[32];
        strftime(expStr, sizeof(expStr), "%Y-%m-%d %H:%M:%S", &tmExp);

        auto subsTable = schema.getTable("subscriptions");
        auto existing = subsTable.select("id")
            .where("user_id = :uid AND status = 'active'")
            .limit(1)
            .bind("uid", userId)
            .execute();

        auto row = existing.fetchOne();
        if (row) {
            subsTable.update()
                .set("plan_id", planId)
                .set("expires_at", string(expStr))
                .set("status", "active")
                .set("auto_renew", 1)
                .where("id = :id")
                .bind("id", row[0].get<int>())
                .execute();
        } else {
            subsTable.insert("user_id", "plan_id", "expires_at", "auto_renew", "status")
                .values(userId, planId, string(expStr), 1, "active")
                .execute();
        }

        return true;
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] UpsertSubscription 실패: {}", err.what());
        return false;
    }
}


bool SubscriptionRepository::CancelSubscription(const string& userId)
{
    try {
        auto& db = DBManager::GetInstance();

        db.GetSchema().getTable("subscriptions")
            .update()
            .set("status", "cancelled")
            .set("auto_renew", 0)
            .where("user_id = :uid AND status = 'active'")
            .bind("uid", userId)
            .execute();

        return true;
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] CancelSubscription 실패: {}", err.what());
        return false;
    }
}


bool SubscriptionRepository::GetActiveSubscription(const string& userId, cSubscriptionInfo& info)
{
    try {
        auto& db = DBManager::GetInstance();

        auto result = db.GetSession().sql(
            "SELECT UNIX_TIMESTAMP(expires_at) * 1000, auto_renew "
            "FROM subscriptions "
            "WHERE user_id = ? AND status = 'active' "
            "LIMIT 1"
        ).bind(userId).execute();

        auto row = result.fetchOne();
        if (!row) return false;

        info.expiresAt = row[0].isNull() ? 0LL : static_cast<int64>(row[0].get<double>());
        info.autoRenew = !row[1].isNull() && row[1].get<int>() != 0;
        return true;
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] GetActiveSubscription 실패: {}", err.what());
        return false;
    }
}


vector<PlanDetail> SubscriptionRepository::GetAvailablePlans(const string& planType)
{
    vector<PlanDetail> plans;
    try {
        auto& db = DBManager::GetInstance();

        auto result = db.GetSession().sql(
            "SELECT plan_id, plan_type, name, grade, storage_bytes, max_file_size, monthly_price "
            "FROM subscription_plans "
            "WHERE plan_type = ? "
            "ORDER BY grade ASC"
        ).bind(planType).execute();

        for (auto row = result.fetchOne(); row; row = result.fetchOne()) {
            PlanDetail p;
            p.planId       = row[0].get<int>();
            p.planType     = row[1].get<string>();
            p.name         = row[2].get<string>();
            p.grade        = row[3].get<int>();
            p.storageBytes = row[4].isNull() ? 0LL : static_cast<int64>(row[4].get<double>());
            p.maxFileSize  = row[5].isNull() ? 0LL : static_cast<int64>(row[5].get<double>());
            p.monthlyPrice = row[6].isNull() ? 0.0 : row[6].get<double>();

            // 등급별 기능 설명 (DB에 features 컬럼 없으므로 하드코딩)
            if (p.grade == 0) {
                p.features = { "기본 채팅", "파일 7일 보관", "1GB 저장공간" };
            } else if (p.grade == 1) {
                p.features = { "기본 채팅", "파일 영구 보관", "10GB 저장공간", "우선 지원" };
            } else {
                p.features = { "기본 채팅", "파일 영구 보관", "100GB 저장공간", "우선 지원", "전용 기능" };
            }

            plans.push_back(std::move(p));
        }
    } catch (const mysqlx::Error& err) {
        LOG_ERROR("[SubscriptionRepository] GetAvailablePlans 실패: {}", err.what());
    }
    return plans;
}
