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

    /* 사진 */
    static string               AddPhoto(const string& albumId, const string& groupId,
                                         int64 msgSeq, const string& photoUrl,
                                         const string& thumbnailUrl, const string& uploaderId,
                                         const string& source = "chat");
    static vector<AlbumPhotoInfo> GetPhotos(const string& albumId, bool latestFirst = true);
    static bool                 DeletePhoto(const string& photoId, const string& userId); // uploader_id 체크
    static bool                 MovePhoto(const string& photoId, const string& toAlbumId); // 누구나 이동 가능

    /* 댓글 */
    static string                  AddComment(const string& photoId, const string& senderId,
                                              const string& senderName, const string& text);
    static vector<PhotoCommentInfo> GetComments(const string& photoId);
    static bool                    DeleteComment(const string& commentId, const string& userId); // sender_id 체크
};
