#pragma once

#include "CloudStorage.h"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

// Cloudflare R2 스토리지 (S3-compatible API + AWS Signature V4)
// 다운로드 트래픽 무료 — GCS 대비 비용 대폭 절감
class CloudStorageR2 : public CloudStorage
{
public:
    CloudStorageR2(
        const string& accountId,
        const string& bucketName,
        const string& accessKeyId,
        const string& secretAccessKey,
        const string& publicUrlPrefix   // "https://pub-xxx.r2.dev" 또는 커스텀 도메인
    );

    bool Initialize() override;

    string GenerateUploadUrl(const string& fileId, const string& path,
                             const string& mimeType, int64 expiresInSeconds = 3600) override;
    string GenerateDownloadUrl(const string& path, int64 expiresInSeconds = 31536000) override;

    bool UploadFile(const string& path, const vector<uint8>& data, const string& mimeType) override;
    bool RemoveFile(const string& path) override;
    bool FileExists(const string& path) override;
    string GetBucketName() const override { return _bucketName; }
    string GetPublicUrlPrefix() const override { return _publicUrlPrefix; }

private:
    // AWS Sig V4 헬퍼
    string       GetTimestamp(string& outDateStr) const;
    vector<uint8> HmacSha256Bytes(const vector<uint8>& key, const string& data) const;
    string       HmacSha256Hex(const vector<uint8>& key, const string& data) const;
    string       Sha256Hex(const string& data) const;
    string       HexEncode(const vector<uint8>& data) const;
    string       UrlEncode(const string& str, bool encodeSlash = true) const;
    vector<uint8> DeriveSigningKey(const string& dateStr) const;

    // Pre-signed URL 생성 (클라이언트 직접 업로드용)
    string BuildPresignedUrl(const string& cleanPath, const string& mimeType,
                              int64 expiresInSeconds) const;

    // Authorization 헤더 빌드 (서버 직접 호출용)
    httplib::Headers BuildAuthHeaders(const string& method, const string& objectPath,
                                       const string& bodySha256,
                                       const string& contentType = "") const;

    inline void HandleErr(const string& op, const string& msg = "") const {
        if (msg.empty()) LOG_ERROR("[CloudStorageR2] {} Failed", op);
        else             LOG_ERROR("[CloudStorageR2] {} Failed: {}", op, msg);
    }

private:
    string _accountId;
    string _bucketName;
    string _accessKeyId;
    string _secretAccessKey;
    string _publicUrlPrefix;
    string _host;   // "<accountId>.r2.cloudflarestorage.com"

    unique_ptr<httplib::Client> _httpClient;

    static constexpr const char* REGION      = "auto";
    static constexpr const char* SERVICE     = "s3";
    static constexpr const char* EMPTY_SHA256 =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
};
