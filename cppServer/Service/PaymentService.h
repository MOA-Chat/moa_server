#pragma once

#include "pch.h"
#include "protocol.pb.h"
#include "Session.h"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

#include <mutex>

using sessionPtr = shared_ptr<Session>;

class UserManager;

class PaymentService
{
public:
    PaymentService(UserManager& userManager);
    ~PaymentService() = default;

    bool Initialize(const string& credentialsPath);

    bool HandleVerifyPurchase(sessionPtr& session, uint64 reqId, const Protocol::C_VerifyPurchase& pkt);
    bool HandleCancelSubscription(sessionPtr& session, uint64 reqId, const Protocol::C_CancelSubscription& pkt);

private:
    bool VerifyGooglePurchase(const string& productId, const string& purchaseToken,
                               int& outPlanId, int64& outExpiresAt);

    bool VerifyApplePurchase(const string& productId, const string& receiptData,
                              int& outPlanId, int64& outExpiresAt);

    struct UpdateResult {
        bool   success      = false;
        bool   isDowngrade  = false;
        string pendingPlanName;
        int64  currentExpiresAt = 0;
        int64  currentStorageLimit = 0;
    };
    UpdateResult UpdateSubscription(const string& groupId, const string& userId, int planId,
                                    const string& platform, const string& productId,
                                    const string& transactionId, const string& purchaseToken, int64 expiresAt);

    int GetPlanIdFromProductId(const string& productId);

    // OAuth2 (GCP 서비스 계정)
    bool LoadServiceAccountJson(const string& filePath);
    bool RefreshAccessToken();    // _tokenMutex 잠금 상태에서 호출
    bool CheckValidToken();       // 내부적으로 _tokenMutex 획득
    string CreateJWT();
    string SignString(const string& data);
    string Base64Encode(const vector<uint8>& data);

    string LoadAppleSharedSecret();

    inline void HandleErr(const string& func, const string& msg = "") {
        if (msg.empty())
            LOG_ERROR("[PaymentService] {} Failed", func);
        else
            LOG_ERROR("[PaymentService] {} Failed: {}", func, msg);
    }

private:
    UserManager& _userManager;

    // OAuth2 — _tokenMutex로 보호 (IOCP 멀티스레드 동시 접근)
    mutex  _tokenMutex;
    string _accessToken;
    int64  _tokenExpiresTime = 0;
    string _serviceAccountEmail;
    string _privateKey;

    string _packageName;
    string _appleSharedSecret;
};

extern PaymentService* GPaymentService;
