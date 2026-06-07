#pragma once
#include "pch.h"
#include "DBManager.h"

struct AlbumInfo {
    string id;
    string groupId;
    string name;
    string coverPhotoUrl;
    string createdBy;
    int64  createdAt;
    int    photoCount;
};

struct AlbumPhotoInfo {
    string id;
    string albumId;
    string groupId;
    int64  msgSeq;        // 0 = 직접 업로드 (future)
    string photoUrl;
    string thumbnailUrl;
    string uploaderId;
    string source;        // "chat" | "direct"
    int64  uploadedAt;
    string filePath;
    int64  fileSize   = 0;
    bool   isDeleted  = false;
    int64  deletedAt  = 0;
};

struct PhotoCommentInfo {
    string id;
    string photoId;
    string senderId;
    string senderName;
    string text;
    int64  createdAt;
};

class AlbumRepository
{
public:
    /* 앨범 */
    static string             CreateAlbum(const string& groupId, const string& name, const string& createdBy);
    static vector<AlbumInfo>  GetAlbums(const string& groupId);
    static AlbumInfo          GetAlbum(const string& albumId);
    static bool               DeleteAlbum(const string& albumId, const string& userId);  // created_by 체크
    static bool               UpdateCoverPhoto(const string& albumId);                   // 첫 사진으로 커버 자동 갱신
    static bool               UpdateAlbumName(const string& albumId, const string& groupId, const string& name);

    /* 사진 */
    static string               AddPhoto(const string& albumId, const string& groupId,
                                         int64 msgSeq, const string& photoUrl,
                                         const string& thumbnailUrl, const string& uploaderId,
                                         const string& source = "chat",
                                         const string& filePath = "", int64 fileSize = 0);
    static vector<AlbumPhotoInfo> GetPhotos(const string& albumId, bool latestFirst = true);
    // uploader 또는 방장 삭제 가능 (소프트 딜리트)
    static bool                 DeletePhoto(const string& photoId, const string& userId,
                                            const string& groupId);
    static bool                 MovePhoto(const string& photoId, const string& toAlbumId); // 누구나 이동 가능

    // ExpirationBatchJob용: 30일 지난 소프트 딜리트 사진 조회 및 하드 삭제
    static vector<AlbumPhotoInfo> GetSoftDeletedPhotosOlderThan(int64 cutoffMs);
    static bool                   HardDeletePhoto(const string& photoId);

    // 구독 만료 grace 종료 시: 그룹 전체 사진 일괄 소프트딜리트
    static bool SoftDeleteAllPhotosByGroup(const string& groupId, int64 deletedAtMs);

    // 쿼터 초과 사진 소프트딜리트 (최신 보존, 오래된 것 삭제)
    // quotaBytes 이내 최신 사진 보존, 초과분 오래된 순으로 소프트딜리트
    // 반환값: 삭제된 총 파일 크기(bytes), -1=오류
    static int64 SoftDeleteOverQuotaPhotosByGroup(const string& groupId, int64 quotaBytes, int64 deletedAtMs);

    /* 댓글 */
    static string                  AddComment(const string& photoId, const string& senderId,
                                              const string& senderName, const string& text);
    static vector<PhotoCommentInfo> GetComments(const string& photoId);
    static bool                    DeleteComment(const string& commentId, const string& userId); // sender_id 체크
    static string                  GetPhotoGroupId(const string& photoId); // 멤버십 확인용
};
