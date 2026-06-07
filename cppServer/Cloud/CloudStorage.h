#pragma once

#include "pch.h"

// 클라우드 스토리지 추상화 인터페이스 (현재 구현: Cloudflare R2)
class CloudStorage {
public:
    virtual ~CloudStorage() = default;

    virtual bool Initialize() = 0;

    // Presigned Upload URL 생성 (클라이언트가 직접 업로드)
    virtual string GenerateUploadUrl(
        const string& fileId,
        const string& path,
        const string& mimeType,
        int64 expiresInSeconds = 3600
    ) = 0;

    // Public/Presigned Download URL 생성
    virtual string GenerateDownloadUrl(
        const string& path,
        int64 expiresInSeconds = 31536000
    ) = 0;

    // 서버에서 직접 업로드 (썸네일 등)
    virtual bool UploadFile(const string& path, const vector<uint8>& data, const string& mimeType) = 0;

    // 파일 삭제
    virtual bool RemoveFile(const string& path) = 0;
    
    // 파일 존재 확인
    virtual bool FileExists(const string& path) = 0;

    // 버킷명 반환
    virtual string GetBucketName() const = 0;

    // Public URL 접두사 반환 (gcs_path 역추출용)
    virtual string GetPublicUrlPrefix() const = 0;
};

