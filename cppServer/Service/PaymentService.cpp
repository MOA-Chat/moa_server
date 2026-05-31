#include "pch.h"
#include "PaymentService.h"
#include "../CoreGlobal.h"
#include "../PacketDispatcher.h"
#include "../ServerSession.h"
#include "../DB/GroupRepository.h"
#include "../DB/SubscriptionRepository.h"
#include "../DB/AlbumRepository.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#include "json.hpp"
using json = nlohmann::json;

PaymentService* GPaymentService = nullptr;


PaymentService::PaymentService(UserManager& userManager)
    : _userManager(userManager)
{
    // httplib::Client는 thread-safe하지 않으므로 멤버로 보관하지 않음
    // VerifyGooglePurchase / VerifyApplePurchase에서 요청마다 로컬 생성
}


bool PaymentService::Initialize(const string& credentialsPath)
{
    try {
        LOG_INFO("[PaymentService] Initializing...");

        if (!LoadServiceAccountJson(credentialsPath)) {
            HandleErr("Initialize", "Failed to load credentials");
            return false;
        }

        if (!RefreshAccessToken()) {
            HandleErr("Initialize", "OAuth2 token refresh failed");
            return false;
        }

        // Android package name 로드
        ::filesystem::path configDir = "config";
        ::filesystem::path packageFile = configDir / "android_package.txt";
        if (::filesystem::exists(packageFile)) {
            ifstream file(packageFile);
            if (file.is_open()) {
                getline(file, _packageName);
                _packageName.erase(0, _packageName.find_first_not_of(" \t\r\n"));
                _packageName.erase(_packageName.find_last_not_of(" \t\r\n") + 1);
                file.close();
            }
        }
        if (_packageName.empty()) {
            _packageName = "com.moachat.app"; // 기본값
        }

        // Apple shared secret 로드
        _appleSharedSecret = LoadAppleSharedSecret();

        LOG_INFO("[PaymentService] Initialized: package={}", _packageName);
        return true;
    }
    catch (const exception& e) {
        HandleErr("Initialize", e.what());
        return false;
    }
}


bool PaymentService::HandleVerifyPurchase(sessionPtr& session, uint64 reqId,
                                           const Protocol::C_VerifyPurchase& pkt)
{
    auto serverSession = static_pointer_cast<ServerSession>(session);
    const string userId  = serverSession->GetUserId();
    const string groupId = pkt.group_id();

    const string& platform      = pkt.platform();
    const string& productId     = pkt.product_id();
    const string& transactionId = pkt.transaction_id();
    const string& purchaseToken = pkt.purchase_token();

    if (groupId.empty()) {
        PacketDispatcher::DispatchError(session, reqId, Protocol::ERR_INVALID_ARGUMENT, "group_id 필요");
        return false;
    }
    if (platform.length() > 10 || productId.length() > 200 ||
        transactionId.length() > 500 || purchaseToken.length() > 5000) {
        PacketDispatcher::DispatchError(session, reqId, Protocol::ERR_PAYMENT_FAILED, "잘못된 결제 정보");
        return false;
    }

    // 방장 확인
    if (GroupRepository::GetMemberRole(groupId, userId) != "owner") {
        PacketDispatcher::DispatchError(session, reqId, Protocol::ERR_NO_PERMISSION, "방장만 구독할 수 있습니다.");
        return false;
    }

    LOG_INFO("[PaymentService] VerifyPurchase: group={} user={} platform={} product={}", groupId, userId, platform, productId);

    int planId = 0;
    int64 expiresAt = 0;
    bool verified = false;

    if (platform == "android") {
        verified = VerifyGooglePurchase(productId, purchaseToken, planId, expiresAt);
    } else if (platform == "ios") {
        verified = VerifyApplePurchase(productId, purchaseToken, planId, expiresAt);
    } else {
        PacketDispatcher::DispatchError(session, reqId, Protocol::ERR_PAYMENT_FAILED, "지원하지 않는 플랫폼");
        return false;
    }

    if (!verified) {
        PacketDispatcher::DispatchError(session, reqId, Protocol::ERR_RECEIPT_INVALID, "영수증 검증 실패");
        return false;
    }

    if (!UpdateSubscription(groupId, planId, platform, productId, transactionId, purchaseToken, expiresAt)) {
        PacketDispatcher::DispatchError(session, reqId, Protocol::ERR_SERVER_INTERNAL, "구독 업데이트 실패");
        return false;
    }

    cGroupInfo gInfo;
    GroupRepository::GetGroupInfoById(groupId, gInfo);

    Protocol::S_VerifyPurchase response;
    response.set_success(true);
    response.set_message("구독이 활성화되었습니다.");
    response.set_expires_at(expiresAt);
    response.set_storage_limit_bytes(gInfo.storageLimit);
    response.set_storage_usage_bytes(gInfo.storageUsage);

    Protocol::Envelope env;
    env.set_version(GProtoVersion);
    env.set_request_id(reqId);
    *env.mutable_s_verify_purchase() = response;
    PacketDispatcher::SendEnvelope(session, env);

    LOG_INFO("[PaymentService] VerifyPurchase 성공: group={} planId={} expires={}", groupId, planId, expiresAt);
    return true;
}


bool PaymentService::VerifyGooglePurchase(const string& productId,
                                            const string& purchaseToken, int& outPlanId,
                                            int64& outExpiresAt)
{
    if (!CheckValidToken()) {
        HandleErr("VerifyGooglePurchase", "Token validation failed");
        return false;
    }

    // 토큰을 복사한 뒤 뮤텍스 해제 후 HTTP 호출 (I/O 중 lock 보유 방지)
    string accessToken;
    {
        lock_guard<mutex> lock(_tokenMutex);
        accessToken = _accessToken;
    }

    try {
        // 요청마다 로컬 클라이언트 생성 (httplib::Client는 thread-safe 아님)
        httplib::Client googleClient("https://androidpublisher.googleapis.com");
        googleClient.set_connection_timeout(10, 0);
        googleClient.set_read_timeout(30, 0);

        string url = "/androidpublisher/v3/applications/" + _packageName +
                     "/purchases/subscriptionsv2/tokens/" + purchaseToken;

        httplib::Headers headers = {
            {"Authorization", "Bearer " + accessToken},
            {"Content-Type", "application/json"}
        };

        auto res = googleClient.Get(url.c_str(), headers);

        if (!res) {
            HandleErr("VerifyGooglePurchase", "HTTP request failed");
            return false;
        }

        if (res->status != 200) {
            HandleErr("VerifyGooglePurchase", "HTTP " + to_string(res->status) + ": " + res->body);
            return false;
        }

        json response = json::parse(res->body);

        // subscriptionState 확인
        string state = response.value("subscriptionState", "");
        if (state != "SUBSCRIPTION_STATE_ACTIVE" && state != "SUBSCRIPTION_STATE_IN_GRACE_PERIOD") {
            HandleErr("VerifyGooglePurchase", "Invalid subscription state: " + state);
            return false;
        }

        // lineItems에서 expiryTime 추출
        if (response.contains("lineItems") && response["lineItems"].is_array() &&
            !response["lineItems"].empty()) {
            auto& lineItem = response["lineItems"][0];
            if (lineItem.contains("expiryTime")) {
                string expiryTimeStr = lineItem["expiryTime"].get<string>();
                // ISO 8601 → Unix timestamp ms (간단 파싱)
                // 형식: "2026-03-16T18:00:00.000Z"
                struct tm tm = {};
                int parsed = sscanf(expiryTimeStr.c_str(), "%d-%d-%dT%d:%d:%d",
                                    &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
                if (parsed < 6) {
                    HandleErr("VerifyGooglePurchase", "expiryTime 파싱 실패: " + expiryTimeStr);
                    return false;
                }
                tm.tm_year -= 1900;
                tm.tm_mon -= 1;
                time_t t = _mkgmtime(&tm);
                outExpiresAt = static_cast<int64>(t) * 1000;
            }
        }

        if (outExpiresAt == 0) {
            HandleErr("VerifyGooglePurchase", "expiryTime 없음 — 만료일 불명확");
            return false;
        }

        outPlanId = GetPlanIdFromProductId(productId);

        LOG_INFO("[PaymentService] Google 검증 성공: product={} expires={}", productId, outExpiresAt);
        return true;
    }
    catch (const exception& e) {
        HandleErr("VerifyGooglePurchase", e.what());
        return false;
    }
}


bool PaymentService::VerifyApplePurchase(const string& productId,
                                           const string& receiptData, int& outPlanId,
                                           int64& outExpiresAt)
{
    try {
        json requestBody = {
            {"receipt-data", receiptData},
            {"password", _appleSharedSecret},
            {"exclude-old-transactions", true}
        };

        string jsonStr = requestBody.dump();

        // 요청마다 로컬 클라이언트 생성 (httplib::Client는 thread-safe 아님)
        httplib::Client appleClient("https://buy.itunes.apple.com");
        appleClient.set_connection_timeout(10, 0);
        appleClient.set_read_timeout(30, 0);

        auto res = appleClient.Post("/verifyReceipt", jsonStr, "application/json");

        if (!res) {
            HandleErr("VerifyApplePurchase", "HTTP request failed");
            return false;
        }

        json response = json::parse(res->body);
        int status = response.value("status", -1);

        // status 21007 → sandbox 영수증이므로 sandbox 서버로 재요청
        if (status == 21007) {
            httplib::Client sandboxClient("https://sandbox.itunes.apple.com");
            sandboxClient.set_connection_timeout(10, 0);
            sandboxClient.set_read_timeout(30, 0);
            res = sandboxClient.Post("/verifyReceipt", jsonStr, "application/json");
            if (!res) {
                HandleErr("VerifyApplePurchase", "Sandbox HTTP request failed");
                return false;
            }
            response = json::parse(res->body);
            status = response.value("status", -1);
        }

        if (status != 0) {
            HandleErr("VerifyApplePurchase", "Invalid status: " + to_string(status));
            return false;
        }

        // latest_receipt_info에서 만료일 추출
        if (response.contains("latest_receipt_info") &&
            response["latest_receipt_info"].is_array() &&
            !response["latest_receipt_info"].empty()) {

            auto& latestReceipt = response["latest_receipt_info"].back();
            if (latestReceipt.contains("expires_date_ms")) {
                string expiresMs = latestReceipt["expires_date_ms"].get<string>();
                outExpiresAt = stoll(expiresMs);
            }
        }

        if (outExpiresAt == 0) {
            HandleErr("VerifyApplePurchase", "expires_date_ms 없음 — 만료일 불명확");
            return false;
        }

        outPlanId = GetPlanIdFromProductId(productId);

        LOG_INFO("[PaymentService] Apple 검증 성공: product={} expires={}", productId, outExpiresAt);
        return true;
    }
    catch (const exception& e) {
        HandleErr("VerifyApplePurchase", e.what());
        return false;
    }
}


bool PaymentService::UpdateSubscription(const string& groupId, int planId,
                                          const string& platform, const string& productId,
                                          const string& transactionId,
                                          const string& purchaseToken, int64 expiresAt)
{
    try {
        if (!SubscriptionRepository::RecordTransaction(groupId, planId, platform, productId, transactionId, purchaseToken)) {
            HandleErr("UpdateSubscription", "RecordTransaction failed");
            return false;
        }

        SubscriptionPlanInfo planInfo;
        if (!SubscriptionRepository::GetPlanInfo(planId, planInfo)) {
            HandleErr("UpdateSubscription", "GetPlanInfo failed: planId=" + to_string(planId));
            return false;
        }

        if (!SubscriptionRepository::UpsertSubscription(groupId, planId, expiresAt)) {
            HandleErr("UpdateSubscription", "UpsertSubscription failed");
            return false;
        }

        if (!GroupRepository::UpdateStorageCapacity(groupId, planInfo.storageBytes)) {
            HandleErr("UpdateSubscription", "UpdateStorageCapacity failed");
            return false;
        }

        // 플랜 변경 후 쿼터 초과 사진 정리 (다운그레이드 시 오래된 사진 소프트딜리트)
        // 업그레이드 시에는 deletedSize=0 으로 무해하게 처리됨
        int64 nowMs = chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()).count();
        int64 deletedSize = AlbumRepository::SoftDeleteOverQuotaPhotosByGroup(
            groupId, planInfo.storageBytes, nowMs);
        if (deletedSize > 0) {
            GroupRepository::DeductGroupStorage(groupId, deletedSize);
            LOG_INFO("[PaymentService] 플랜 변경 후 초과 사진 정리: group={} deletedBytes={}", groupId, deletedSize);
        }

        LOG_INFO("[PaymentService] 구독 업데이트 완료: group={} planId={} storage={}", groupId, planId, planInfo.storageBytes);
        return true;
    }
    catch (const exception& e) {
        HandleErr("UpdateSubscription", e.what());
        return false;
    }
}


bool PaymentService::HandleCancelSubscription(sessionPtr& session, uint64 reqId,
                                               const Protocol::C_CancelSubscription& pkt)
{
    auto serverSession = static_pointer_cast<ServerSession>(session);
    const string userId  = serverSession->GetUserId();
    const string groupId = pkt.group_id();

    if (groupId.empty()) {
        PacketDispatcher::DispatchError(session, reqId, Protocol::ERR_INVALID_ARGUMENT, "group_id 필요");
        return false;
    }

    if (GroupRepository::GetMemberRole(groupId, userId) != "owner") {
        PacketDispatcher::DispatchError(session, reqId, Protocol::ERR_NO_PERMISSION, "방장만 구독을 해지할 수 있습니다.");
        return false;
    }

    try {
        cSubscriptionInfo subInfo;
        if (!SubscriptionRepository::GetActiveSubscription(groupId, subInfo)) {
            Protocol::S_CancelSubscription resp;
            resp.set_success(false);
            resp.set_message("구독 중인 플랜이 없습니다.");
            resp.set_grace_expires_at(0);
            Protocol::Envelope env;
            env.set_version(GProtoVersion);
            env.set_request_id(reqId);
            *env.mutable_s_cancel_subscription() = resp;
            PacketDispatcher::SendEnvelope(session, env);
            return true;
        }

        const int64 GRACE_PERIOD_MS = 14LL * 24 * 3600 * 1000;
        int64 graceExpiresAt = Nowts() + GRACE_PERIOD_MS;

        // SetGrace: status='active' → 'grace' + grace_expires_at 세팅
        // CancelSubscription을 먼저 호출하면 status='cancelled'가 되어 SetGrace의 WHERE status='active' 조건에 안 걸림
        SubscriptionRepository::SetGrace(groupId, graceExpiresAt);

        LOG_INFO("[PaymentService] 구독 해지: group={} 유예만료={}", groupId, graceExpiresAt);

        Protocol::S_CancelSubscription resp;
        resp.set_success(true);
        resp.set_message("구독이 해지되었습니다. 14일 후 앨범 사진이 삭제됩니다.");
        resp.set_grace_expires_at(graceExpiresAt);

        Protocol::Envelope env;
        env.set_version(GProtoVersion);
        env.set_request_id(reqId);
        *env.mutable_s_cancel_subscription() = resp;
        PacketDispatcher::SendEnvelope(session, env);

        return true;
    }
    catch (const exception& e) {
        HandleErr("HandleCancelSubscription", e.what());
        PacketDispatcher::DispatchError(session, reqId, Protocol::ERR_SERVER_INTERNAL, e.what());
        return false;
    }
}


int PaymentService::GetPlanIdFromProductId(const string& productId)
{
    if (productId == "basic_monthly") return 2;
    if (productId == "plus_monthly")  return 3;
    if (productId == "max_monthly")   return 4;
    LOG_WARN("[PaymentService] Unknown productId: {}", productId);
    return 1;
}


bool PaymentService::LoadServiceAccountJson(const string& filePath)
{
    try {
        ifstream file(filePath);
        if (!file.is_open()) {
            HandleErr("LoadServiceAccountJson", "File open failed: " + filePath);
            return false;
        }

        json j;
        file >> j;
        file.close();

        if (j.contains("client_email")) {
            _serviceAccountEmail = j["client_email"].get<string>();
        } else {
            HandleErr("LoadServiceAccountJson", "client_email not found");
            return false;
        }

        if (j.contains("private_key")) {
            _privateKey = j["private_key"].get<string>();
            size_t pos = 0;
            while ((pos = _privateKey.find("\\n", pos)) != string::npos) {
                _privateKey.replace(pos, 2, "\n");
                pos += 1;
            }
        } else {
            HandleErr("LoadServiceAccountJson", "private_key not found");
            return false;
        }

        return true;
    }
    catch (const exception& e) {
        HandleErr("LoadServiceAccountJson", e.what());
        return false;
    }
}


bool PaymentService::RefreshAccessToken()
{
    try {
        string jwt = CreateJWT();
        if (jwt.empty()) {
            HandleErr("RefreshAccessToken", "JWT creation failed");
            return false;
        }

        httplib::Client oauthClient("https://oauth2.googleapis.com");
        httplib::Params params = {
            {"grant_type", "urn:ietf:params:oauth:grant-type:jwt-bearer"},
            {"assertion", jwt}
        };

        auto res = oauthClient.Post("/token", params);

        if (!res || res->status != 200) {
            HandleErr("RefreshAccessToken", res ? to_string(res->status) : "HTTP request failed");
            return false;
        }

        json response = json::parse(res->body);

        if (response.contains("access_token")) {
            _accessToken = response["access_token"].get<string>();
            int64 expiresIn = response.value("expires_in", (int64)3600);
            _tokenExpiresTime = Nowts() + (expiresIn * 1000);
            return true;
        }

        HandleErr("RefreshAccessToken", "access_token not found");
        return false;
    }
    catch (const exception& e) {
        HandleErr("RefreshAccessToken", e.what());
        return false;
    }
}


bool PaymentService::CheckValidToken()
{
    lock_guard<mutex> lock(_tokenMutex);
    // 5분 버퍼: 만료 5분 전부터 갱신 (토큰 만료 직전 요청 실패 방지)
    const int64 BUFFER_MS = 5LL * 60 * 1000;
    if (_accessToken.empty() || Nowts() >= (_tokenExpiresTime - BUFFER_MS)) {
        return RefreshAccessToken();
    }
    return true;
}


string PaymentService::CreateJWT()
{
    json header = { {"alg", "RS256"}, {"typ", "JWT"} };

    int64 now = Nowts() / 1000;

    json payload = {
        {"iss", _serviceAccountEmail},
        {"scope", "https://www.googleapis.com/auth/androidpublisher"},
        {"aud", "https://oauth2.googleapis.com/token"},
        {"exp", now + 3600},
        {"iat", now}
    };

    string headerStr = header.dump();
    string payloadStr = payload.dump();

    vector<uint8> headerBytes(headerStr.begin(), headerStr.end());
    vector<uint8> payloadBytes(payloadStr.begin(), payloadStr.end());

    string encodedHeader = Base64Encode(headerBytes);
    string encodedPayload = Base64Encode(payloadBytes);

    replace(encodedHeader.begin(), encodedHeader.end(), '+', '-');
    replace(encodedHeader.begin(), encodedHeader.end(), '/', '_');
    replace(encodedPayload.begin(), encodedPayload.end(), '+', '-');
    replace(encodedPayload.begin(), encodedPayload.end(), '/', '_');

    encodedHeader.erase(remove(encodedHeader.begin(), encodedHeader.end(), '='), encodedHeader.end());
    encodedPayload.erase(remove(encodedPayload.begin(), encodedPayload.end(), '='), encodedPayload.end());

    string toSign = encodedHeader + "." + encodedPayload;

    string signature = SignString(toSign);
    if (signature.empty()) return "";

    replace(signature.begin(), signature.end(), '+', '-');
    replace(signature.begin(), signature.end(), '/', '_');
    signature.erase(remove(signature.begin(), signature.end(), '='), signature.end());

    return toSign + "." + signature;
}


string PaymentService::SignString(const string& data)
{
    BIO* bio = BIO_new_mem_buf(_privateKey.c_str(), static_cast<int>(_privateKey.length()));
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);

    if (!pkey) {
        HandleErr("SignString", "Failed to load private key");
        return "";
    }

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(mdctx, nullptr, EVP_sha256(), nullptr, pkey);
    EVP_DigestSignUpdate(mdctx, data.c_str(), data.length());

    size_t sigLen = 0;
    EVP_DigestSignFinal(mdctx, nullptr, &sigLen);

    vector<uint8> sig(sigLen);
    EVP_DigestSignFinal(mdctx, sig.data(), &sigLen);

    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);

    return Base64Encode(sig);
}


string PaymentService::Base64Encode(const vector<uint8>& data)
{
    BIO* bio = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    bio = BIO_push(b64, bio);

    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, data.data(), static_cast<int>(data.size()));
    BIO_flush(bio);

    BUF_MEM* bufferPtr;
    BIO_get_mem_ptr(bio, &bufferPtr);

    string result(bufferPtr->data, bufferPtr->length);
    BIO_free_all(bio);

    return result;
}


string PaymentService::LoadAppleSharedSecret()
{
    try {
        ::filesystem::path secretFile = ::filesystem::path("config") / "apple_iap_secret.txt";
        if (!::filesystem::exists(secretFile)) {
            LOG_WARN("[PaymentService] Apple shared secret 파일 없음");
            return "";
        }

        ifstream file(secretFile);
        if (!file.is_open()) return "";

        string secret;
        getline(file, secret);
        secret.erase(0, secret.find_first_not_of(" \t\r\n"));
        secret.erase(secret.find_last_not_of(" \t\r\n") + 1);
        file.close();

        LOG_INFO("[PaymentService] Apple shared secret loaded");
        return secret;
    }
    catch (...) {
        return "";
    }
}
