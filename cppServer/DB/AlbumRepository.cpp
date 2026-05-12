#include "pch.h"
#include "AlbumRepository.h"

#include <sstream>
#include <iomanip>
#include <random>
#include <chrono>

namespace {

string GenerateUUID()
{
    static thread_local mt19937_64 rng(
        chrono::high_resolution_clock::now().time_since_epoch().count()
    );
    uniform_int_distribution<uint64_t> dist;
    uint64_t a = dist(rng), b = dist(rng);
    // RFC 4122 v4 UUID
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    ostringstream oss;
    oss << hex << setfill('0')
        << setw(8) << (a >> 32)
        << '-' << setw(4) << ((a >> 16) & 0xFFFF)
        << '-' << setw(4) << (a & 0xFFFF)
        << '-' << setw(4) << (b >> 48)
        << '-' << setw(12) << (b & 0x0000FFFFFFFFFFFFULL);
    return oss.str();
}

int64 NowMs()
{
    return chrono::duration_cast<chrono::milliseconds>(
        chrono::system_clock::now().time_since_epoch()
    ).count();
}

} // namespace

/*======================
        앨범 CRUD
=======================*/

string AlbumRepository::CreateAlbum(const string& groupId, const string& name, const string& createdBy)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        string id = GenerateUUID();
        int64 now = NowMs();
        session.sql(
            "INSERT INTO albums (id, group_id, name, cover_photo_url, created_by, created_at) "
            "VALUES (?, ?, ?, NULL, ?, ?)"
        ).bind(id, groupId, name, createdBy, now).execute();
        return id;
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[AlbumRepository] CreateAlbum 실패: {}", e.what());
        return "";
    }
}

vector<AlbumInfo> AlbumRepository::GetAlbums(const string& groupId)
{
    vector<AlbumInfo> result;
    try {
        auto& session = DBManager::GetInstance().GetSession();
        auto rows = session.sql(
            "SELECT a.id, a.group_id, a.name, COALESCE(a.cover_photo_url, '') AS cover_photo_url, "
            "       a.created_by, a.created_at, "
            "       (SELECT COUNT(*) FROM album_photos WHERE album_id = a.id) AS photo_count "
            "FROM albums a "
            "WHERE a.group_id = ? "
            "ORDER BY a.created_at ASC"
        ).bind(groupId).execute();

        for (auto row : rows) {
            AlbumInfo info;
            info.id             = row[0].get<string>();
            info.groupId        = row[1].get<string>();
            info.name           = row[2].get<string>();
            info.coverPhotoUrl  = row[3].get<string>();
            info.createdBy      = row[4].get<string>();
            info.createdAt      = row[5].get<int64>();
            info.photoCount     = row[6].get<int>();
            result.push_back(info);
        }
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[AlbumRepository] GetAlbums 실패: {}", e.what());
    }
    return result;
}

AlbumInfo AlbumRepository::GetAlbum(const string& albumId)
{
    AlbumInfo info;
    try {
        auto& session = DBManager::GetInstance().GetSession();
        auto rows = session.sql(
            "SELECT a.id, a.group_id, a.name, COALESCE(a.cover_photo_url, '') AS cover_photo_url, "
            "       a.created_by, a.created_at, "
            "       (SELECT COUNT(*) FROM album_photos WHERE album_id = a.id) AS photo_count "
            "FROM albums a WHERE a.id = ? LIMIT 1"
        ).bind(albumId).execute();
        if (auto row = rows.fetchOne()) {
            info.id            = row[0].get<string>();
            info.groupId       = row[1].get<string>();
            info.name          = row[2].get<string>();
            info.coverPhotoUrl = row[3].get<string>();
            info.createdBy     = row[4].get<string>();
            info.createdAt     = row[5].get<int64>();
            info.photoCount    = row[6].get<int>();
        }
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[AlbumRepository] GetAlbum 실패: {}", e.what());
    }
    return info;
}

bool AlbumRepository::DeleteAlbum(const string& albumId, const string& userId)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        session.startTransaction();

        // 권한 확인
        auto check = session.sql(
            "SELECT created_by FROM albums WHERE id = ?"
        ).bind(albumId).execute();
        if (check.count() == 0) { session.rollback(); return false; }
        auto row = check.fetchOne();
        if (row[0].get<string>() != userId) { session.rollback(); return false; }

        // 댓글 → 사진 → 앨범 순 삭제
        session.sql(
            "DELETE pc FROM photo_comments pc "
            "INNER JOIN album_photos ap ON pc.photo_id = ap.id "
            "WHERE ap.album_id = ?"
        ).bind(albumId).execute();
        session.sql("DELETE FROM album_photos WHERE album_id = ?").bind(albumId).execute();
        session.sql("DELETE FROM albums WHERE id = ?").bind(albumId).execute();

        session.commit();
        return true;
    } catch (const mysqlx::Error& e) {
        try { DBManager::GetInstance().GetSession().rollback(); } catch (...) {}
        LOG_ERROR("[AlbumRepository] DeleteAlbum 실패: {}", e.what());
        return false;
    }
}

bool AlbumRepository::UpdateCoverPhoto(const string& albumId)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        auto rows = session.sql(
            "SELECT COALESCE(thumbnail_url, photo_url) FROM album_photos "
            "WHERE album_id = ? ORDER BY uploaded_at ASC LIMIT 1"
        ).bind(albumId).execute();

        string coverUrl;
        if (rows.count() > 0) {
            auto row = rows.fetchOne();
            coverUrl = row[0].get<string>();
        }
        session.sql("UPDATE albums SET cover_photo_url = ? WHERE id = ?")
            .bind(coverUrl.empty() ? mysqlx::nullvalue : mysqlx::Value(coverUrl), albumId)
            .execute();
        return true;
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[AlbumRepository] UpdateCoverPhoto 실패: {}", e.what());
        return false;
    }
}

/*======================
        사진 CRUD
=======================*/

string AlbumRepository::AddPhoto(const string& albumId, const string& groupId,
                                  int64 msgSeq, const string& photoUrl,
                                  const string& thumbnailUrl, const string& uploaderId,
                                  const string& source)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        string id = GenerateUUID();
        int64 now = NowMs();

        if (msgSeq > 0) {
            session.sql(
                "INSERT INTO album_photos "
                "(id, album_id, group_id, msg_seq, photo_url, thumbnail_url, uploader_id, source, uploaded_at) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
            ).bind(id, albumId, groupId, msgSeq, photoUrl, thumbnailUrl, uploaderId, source, now).execute();
        } else {
            session.sql(
                "INSERT INTO album_photos "
                "(id, album_id, group_id, msg_seq, photo_url, thumbnail_url, uploader_id, source, uploaded_at) "
                "VALUES (?, ?, ?, NULL, ?, ?, ?, ?, ?)"
            ).bind(id, albumId, groupId, photoUrl, thumbnailUrl, uploaderId, source, now).execute();
        }

        // 커버 사진 자동 갱신 (첫 사진인 경우)
        UpdateCoverPhoto(albumId);
        return id;
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[AlbumRepository] AddPhoto 실패: {}", e.what());
        return "";
    }
}

vector<AlbumPhotoInfo> AlbumRepository::GetPhotos(const string& albumId, bool latestFirst)
{
    vector<AlbumPhotoInfo> result;
    try {
        auto& session = DBManager::GetInstance().GetSession();
        string order = latestFirst ? "DESC" : "ASC";
        auto rows = session.sql(
            "SELECT id, album_id, group_id, COALESCE(msg_seq, 0), "
            "       photo_url, COALESCE(thumbnail_url, ''), uploader_id, source, uploaded_at "
            "FROM album_photos WHERE album_id = ? "
            "ORDER BY uploaded_at " + order
        ).bind(albumId).execute();

        for (auto row : rows) {
            AlbumPhotoInfo info;
            info.id           = row[0].get<string>();
            info.albumId      = row[1].get<string>();
            info.groupId      = row[2].get<string>();
            info.msgSeq       = row[3].get<int64>();
            info.photoUrl     = row[4].get<string>();
            info.thumbnailUrl = row[5].get<string>();
            info.uploaderId   = row[6].get<string>();
            info.source       = row[7].get<string>();
            info.uploadedAt   = row[8].get<int64>();
            result.push_back(info);
        }
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[AlbumRepository] GetPhotos 실패: {}", e.what());
    }
    return result;
}

bool AlbumRepository::DeletePhoto(const string& photoId, const string& userId)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        session.startTransaction();

        auto check = session.sql(
            "SELECT uploader_id, album_id FROM album_photos WHERE id = ?"
        ).bind(photoId).execute();
        if (check.count() == 0) { session.rollback(); return false; }
        auto row = check.fetchOne();
        if (row[0].get<string>() != userId) { session.rollback(); return false; }
        string albumId = row[1].get<string>();

        session.sql("DELETE FROM photo_comments WHERE photo_id = ?").bind(photoId).execute();
        session.sql("DELETE FROM album_photos WHERE id = ?").bind(photoId).execute();
        session.commit();

        UpdateCoverPhoto(albumId);
        return true;
    } catch (const mysqlx::Error& e) {
        try { DBManager::GetInstance().GetSession().rollback(); } catch (...) {}
        LOG_ERROR("[AlbumRepository] DeletePhoto 실패: {}", e.what());
        return false;
    }
}

bool AlbumRepository::MovePhoto(const string& photoId, const string& toAlbumId)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        // 원본 albumId 조회 (커버 갱신용)
        auto check = session.sql(
            "SELECT album_id FROM album_photos WHERE id = ?"
        ).bind(photoId).execute();
        if (check.count() == 0) return false;
        string fromAlbumId = check.fetchOne()[0].get<string>();

        session.sql("UPDATE album_photos SET album_id = ? WHERE id = ?")
            .bind(toAlbumId, photoId).execute();

        // 양쪽 앨범 커버 갱신
        UpdateCoverPhoto(fromAlbumId);
        UpdateCoverPhoto(toAlbumId);
        return true;
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[AlbumRepository] MovePhoto 실패: {}", e.what());
        return false;
    }
}

/*======================
        댓글 CRUD
=======================*/

string AlbumRepository::AddComment(const string& photoId, const string& senderId,
                                    const string& senderName, const string& text)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        string id = GenerateUUID();
        int64 now = NowMs();
        session.sql(
            "INSERT INTO photo_comments (id, photo_id, sender_id, sender_name, text, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?)"
        ).bind(id, photoId, senderId, senderName, text, now).execute();
        return id;
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[AlbumRepository] AddComment 실패: {}", e.what());
        return "";
    }
}

vector<PhotoCommentInfo> AlbumRepository::GetComments(const string& photoId)
{
    vector<PhotoCommentInfo> result;
    try {
        auto& session = DBManager::GetInstance().GetSession();
        auto rows = session.sql(
            "SELECT id, photo_id, sender_id, COALESCE(sender_name, ''), text, created_at "
            "FROM photo_comments WHERE photo_id = ? ORDER BY created_at ASC"
        ).bind(photoId).execute();

        for (auto row : rows) {
            PhotoCommentInfo info;
            info.id         = row[0].get<string>();
            info.photoId    = row[1].get<string>();
            info.senderId   = row[2].get<string>();
            info.senderName = row[3].get<string>();
            info.text       = row[4].get<string>();
            info.createdAt  = row[5].get<int64>();
            result.push_back(info);
        }
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[AlbumRepository] GetComments 실패: {}", e.what());
    }
    return result;
}

bool AlbumRepository::DeleteComment(const string& commentId, const string& userId)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        auto check = session.sql(
            "SELECT sender_id FROM photo_comments WHERE id = ?"
        ).bind(commentId).execute();
        if (check.count() == 0) return false;
        if (check.fetchOne()[0].get<string>() != userId) return false;

        session.sql("DELETE FROM photo_comments WHERE id = ?").bind(commentId).execute();
        return true;
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[AlbumRepository] DeleteComment 실패: {}", e.what());
        return false;
    }
}
