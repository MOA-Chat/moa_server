#include "pch.h"
#include "CoreGlobal.h"

#include "Service/UserManager.h"
#include "Service/ChatService.h"
#include "Service/FriendService.h"
#include "Service/GroupService.h"

#include "Service/VerificationManager.h"

#include "Service/AuthService.h"
#include "Service/FileService.h"
#include "Service/NotificationService.h"
#include "Cloud/CloudStorageR2.h"
#include "Cloud/FcmClient.h"
#include "Service/PaymentService.h"
#include "Service/BlockService.h"
#include "Service/ExpirationBatchJob.h"
#include "Service/AlbumService.h"
#include "Service/LocationService.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <filesystem>

// ===== 전역 변수/상수 선언 =====
const int32 GProtoVersion = 1;

const int64 TIMEOUT_MS = 120 * 1000;
const int32 MAX_GROUP_MEMBERS = 12;  // 그룹 최대 인원 수

// ─── 저장공간 기본 제공량 ───
const int64 DEFAULT_PERSONAL_STORAGE = 1LL * 1024 * 1024 * 1024;  // 1GB (무료 플랜 기본값)
const int64 DEFAULT_GROUP_STORAGE    = 1LL * 1024 * 1024 * 1024;  // 1GB
const int64 DEFAULT_MAX_FILE_SIZE    = 300LL * 1024 * 1024;       // 300MB

UserManager* GUserManager = nullptr;
ChatService* GChatService = nullptr;
FriendService* GFriendService = nullptr;
GroupService* GGroupService = nullptr;
VerificationManager* GVerificationManager = nullptr;

AuthService* GAuthService = nullptr;
FileService* GFileService = nullptr;
CloudStorage* GStorage = nullptr;
NotificationService* GNotificationService = nullptr;
BlockService* GBlockService = nullptr;
ExpirationBatchJob* GExpirationBatchJob = nullptr;
AlbumService*    GAlbumService    = nullptr;
LocationService* GLocationService = nullptr;

CoreGlobal GCoreGlobal;


// ===== 현재 시간 =====
int64 Nowts()
{
    using namespace ::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// ===== Cloudflare R2 설정 파일 읽기 =====
// config/r2_config.txt 형식:
//   account_id=<Cloudflare Account ID>
//   access_key_id=<R2 API 토큰 Access Key>
//   secret_access_key=<R2 API 토큰 Secret Key>
//   bucket_name=<버킷 이름>
//   public_url=<공개 다운로드 URL 접두사, 예: https://pub-xxx.r2.dev>
static bool LoadR2Config(string& accountId, string& bucketName,
                          string& accessKeyId, string& secretAccessKey,
                          string& publicUrl)
{
    try {
        ::filesystem::path configFile = ::filesystem::path("config") / "r2_config.txt";
        ifstream file(configFile);
        if (!file.is_open()) {
            LOG_ERROR("[CoreGlobal] R2 설정 파일을 찾을 수 없습니다: {}", configFile.string());
            return false;
        }

        auto trim = [](string& s) {
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            if (!s.empty()) s.erase(s.find_last_not_of(" \t\r\n") + 1);
        };

        string line;
        while (getline(file, line)) {
            trim(line);
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq == string::npos) continue;
            string key   = line.substr(0, eq);
            string value = line.substr(eq + 1);
            trim(key); trim(value);

            if      (key == "account_id")        accountId        = value;
            else if (key == "access_key_id")     accessKeyId      = value;
            else if (key == "secret_access_key") secretAccessKey  = value;
            else if (key == "bucket_name")       bucketName       = value;
            else if (key == "public_url")        publicUrl        = value;
        }
        file.close();

        if (accountId.empty() || bucketName.empty() ||
            accessKeyId.empty() || secretAccessKey.empty())
        {
            LOG_ERROR("[CoreGlobal] r2_config.txt 필수 항목 누락 (account_id / bucket_name / keys)");
            return false;
        }

        LOG_INFO("[CoreGlobal] R2 Config loaded:");
        LOG_INFO(" - Account ID : {}", accountId);
        LOG_INFO(" - Bucket     : {}", bucketName);
        LOG_INFO(" - Public URL : {}", publicUrl.empty() ? "(not set)" : publicUrl);

        return true;
    }
    catch (const exception& e) {
        LOG_ERROR("[CoreGlobal] LoadR2Config error: {}", e.what());
        return false;
    }
}




// ===== GCP 설정 파일 읽기 (FCM + PaymentService 전용) =====
static bool LoadGCPConfig(string& projectId, string& credentialsPath)
{
    try {
        ::filesystem::path configDir = "config";
        ::filesystem::path adminFile = configDir / "googleCloud_admin.txt";
        ifstream file(adminFile);
        if (!file.is_open()) {
            LOG_WARN("[CoreGlobal] GCP 설정 파일 없음 (FCM/Payment 비활성화): {}", adminFile.string());
            return false;
        }

        string bucketIgnored;
        if (!getline(file, projectId) || !getline(file, bucketIgnored)) {
            LOG_WARN("[CoreGlobal] googleCloud_admin.txt 파일 비어있음");
            return false;
        }
        file.close();

        auto trim = [](string& s) {
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            if (!s.empty()) s.erase(s.find_last_not_of(" \t\r\n") + 1);
        };
        trim(projectId);

        ::filesystem::path keyFile = configDir / "gcp_key.json";
        if (!::filesystem::exists(keyFile)) {
            LOG_WARN("[CoreGlobal] GCP 인증 키 없음 (FCM/Payment 비활성화): {}", keyFile.string());
            return false;
        }

        credentialsPath = keyFile.string();
        LOG_INFO("[CoreGlobal] GCP Config loaded (FCM/Payment): projectId={}", projectId);
        return true;
    }
    catch (const exception& e) {
        LOG_WARN("[CoreGlobal] LoadGCPConfig error: {}", e.what());
        return false;
    }
}


CoreGlobal::CoreGlobal()
{
    _userManager = make_unique<UserManager>();
    _chatService = make_unique<ChatService>(*_userManager);
    _friendService = make_unique<FriendService>(*_userManager);
    _groupService = make_unique<GroupService>(*_userManager);
    _verificationManager = make_unique<VerificationManager>(*_userManager);


    _authService = make_unique<AuthService>(*_userManager);
    _notificationService = make_unique<NotificationService>(*_userManager);
    _blockService    = make_unique<BlockService>(*_userManager);
    _albumService    = make_unique<AlbumService>(*_userManager);
    _locationService = make_unique<LocationService>(*_userManager);

    // Cloudflare R2 스토리지 초기화
    {
        string accountId, bucketName, accessKeyId, secretAccessKey, publicUrl;
        if (!LoadR2Config(accountId, bucketName, accessKeyId, secretAccessKey, publicUrl)) {
            LOG_ERROR("[CoreGlobal] Failed to load R2 config — file upload disabled.");
            _cloudStorage = nullptr;
            _fileService  = nullptr;
        }
        else {
            _cloudStorage = make_unique<CloudStorageR2>(
                accountId, bucketName, accessKeyId, secretAccessKey, publicUrl);

            if (!_cloudStorage->Initialize()) {
                LOG_ERROR("[CoreGlobal] CloudStorageR2 Initialize Failed");
            }

            _fileService = make_unique<FileService>(_cloudStorage.get());

            _expirationBatchJob = make_unique<ExpirationBatchJob>(_cloudStorage.get());
            _expirationBatchJob->Start();
        }
    }

    // FCM + PaymentService 초기화 (GCP credentials)
    {
        string projectId, credentialsPath;
        if (LoadGCPConfig(projectId, credentialsPath)) {
            _fcmClient = make_unique<FcmClient>(projectId, credentialsPath);
            if (!_fcmClient->Initialize()) {
                LOG_ERROR("[CoreGlobal] FcmClient Initialize Failed");
                _fcmClient = nullptr;
            }

            _paymentService = make_unique<PaymentService>(*_userManager);
            if (!_paymentService->Initialize(credentialsPath)) {
                LOG_ERROR("[CoreGlobal] PaymentService Initialize Failed");
                _paymentService = nullptr;
            }
        }
        else {
            LOG_WARN("[CoreGlobal] GCP config missing — FCM/Payment disabled");
        }
    }

    GUserManager = _userManager.get();
    GChatService = _chatService.get();
    GFriendService = _friendService.get();
    GGroupService = _groupService.get();
    GVerificationManager = _verificationManager.get();

    GAuthService = _authService.get();
    GFileService = _fileService.get();
    GStorage = _cloudStorage.get();
    GNotificationService = _notificationService.get();
    GPaymentService = _paymentService.get();
    GBlockService = _blockService.get();
    GFcmClient = _fcmClient.get();
    GExpirationBatchJob = _expirationBatchJob.get();
    GAlbumService    = _albumService.get();
    GLocationService = _locationService.get();
}

CoreGlobal::~CoreGlobal()
{
    GLocationService    = nullptr;
    GAlbumService       = nullptr;
    GExpirationBatchJob = nullptr;
    GFcmClient = nullptr;
    GPaymentService = nullptr;
    GBlockService = nullptr;
    GNotificationService = nullptr;
    GStorage = nullptr;
    GFileService = nullptr;
    GAuthService = nullptr;

    GVerificationManager = nullptr;
    GGroupService = nullptr;
    GFriendService = nullptr;
    GChatService = nullptr;
    GUserManager = nullptr;

    _fcmClient.reset();
    _paymentService.reset();
    _blockService.reset();
    _notificationService.reset();
    _fileService.reset();
    _cloudStorage.reset();
    _authService.reset();
    _albumService.reset();

    _verificationManager.reset();

    _groupService.reset();
    _friendService.reset();
    _chatService.reset();
    _userManager.reset();
}

void CoreGlobal::Reset()
{
    if (_expirationBatchJob) _expirationBatchJob->Stop();
    _expirationBatchJob.reset();
    _fcmClient.reset();
    _paymentService.reset();
    _blockService.reset();
    _notificationService.reset();
    _fileService.reset();
    _cloudStorage.reset();
    _authService.reset();
    _friendService.reset();
    _groupService.reset();
    _verificationManager.reset();

    _chatService.reset();
    _userManager.reset();

    GExpirationBatchJob = nullptr;
    GFcmClient = nullptr;
    GPaymentService = nullptr;
    GBlockService = nullptr;
    GNotificationService = nullptr;
    GStorage = nullptr;
    GFileService = nullptr;
    GAuthService = nullptr;
    GFriendService = nullptr;
    GGroupService = nullptr;
    GVerificationManager = nullptr;

    GChatService = nullptr;
    GUserManager = nullptr;

    _userManager = make_unique<UserManager>();
    _chatService = make_unique<ChatService>(*_userManager);
    _friendService = make_unique<FriendService>(*_userManager);
    _groupService = make_unique<GroupService>(*_userManager);
    _verificationManager = make_unique<VerificationManager>(*_userManager);

    _authService = make_unique<AuthService>(*_userManager);
    _notificationService = make_unique<NotificationService>(*_userManager);
    _blockService    = make_unique<BlockService>(*_userManager);
    _albumService    = make_unique<AlbumService>(*_userManager);
    _locationService = make_unique<LocationService>(*_userManager);

    // Cloudflare R2 스토리지 재초기화
    {
        string accountId, bucketName, accessKeyId, secretAccessKey, publicUrl;
        if (!LoadR2Config(accountId, bucketName, accessKeyId, secretAccessKey, publicUrl)) {
            LOG_ERROR("[CoreGlobal] Failed to load R2 config — file upload disabled.");
            _cloudStorage = nullptr;
            _fileService  = nullptr;
        }
        else {
            _cloudStorage = make_unique<CloudStorageR2>(
                accountId, bucketName, accessKeyId, secretAccessKey, publicUrl);

            if (!_cloudStorage->Initialize()) {
                LOG_ERROR("[CoreGlobal] CloudStorageR2 Initialize Failed");
            }

            _fileService = make_unique<FileService>(_cloudStorage.get());

            _expirationBatchJob = make_unique<ExpirationBatchJob>(_cloudStorage.get());
            _expirationBatchJob->Start();
        }
    }

    // FCM + PaymentService 재초기화 (GCP credentials)
    {
        string projectId, credentialsPath;
        if (LoadGCPConfig(projectId, credentialsPath)) {
            _fcmClient = make_unique<FcmClient>(projectId, credentialsPath);
            if (!_fcmClient->Initialize()) {
                LOG_ERROR("[CoreGlobal] FcmClient Initialize Failed");
                _fcmClient = nullptr;
            }

            _paymentService = make_unique<PaymentService>(*_userManager);
            if (!_paymentService->Initialize(credentialsPath)) {
                LOG_ERROR("[CoreGlobal] PaymentService Initialize Failed");
                _paymentService = nullptr;
            }
        }
        else {
            LOG_WARN("[CoreGlobal] GCP config missing — FCM/Payment disabled");
        }
    }

    GUserManager = _userManager.get();
    GChatService = _chatService.get();
    GFriendService = _friendService.get();
    GGroupService = _groupService.get();
    GVerificationManager = _verificationManager.get();

    GAuthService = _authService.get();
    GFileService = _fileService.get();
    GStorage = _cloudStorage.get();
    GNotificationService = _notificationService.get();
    GPaymentService = _paymentService.get();
    GBlockService = _blockService.get();
    GFcmClient = _fcmClient.get();
    GExpirationBatchJob = _expirationBatchJob.get();
    GAlbumService    = _albumService.get();
    GLocationService = _locationService.get();
}


