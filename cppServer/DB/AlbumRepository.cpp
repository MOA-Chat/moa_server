#include "pch.h"
#include "AlbumRepository.h"
#include "GroupRepository.h"

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
            "       (SELECT COUNT(*) FROM album_photos WHERE album_id = a.id AND is_deleted = 0) AS photo_count "
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
            "       (SELECT COUNT(*) FROM album_photos WHERE album_id = a.id AND is_deleted = 0) AS photo_count "
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

string AlbumRepository::GetPhotoGroupId(const string& photoId)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        auto rows = session.sql(
            "SELECT group_id FROM album_photos WHERE id = ? LIMIT 1"
        ).bind(photoId).execute();
        if (auto row = rows.fetchOne()) {
            return row[0].get<string>();
        }
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[AlbumRepository] GetPhotoGroupId 실패: {}", e.what());
    }
    return "";
}

bool AlbumRepository::DeleteAlbum(const string& albumId, const string& userId)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        session.startTransaction();

        // 앨범 정보 + 권한 확인
        auto allCheck = session.sql(
            "SELECT created_by, group_id FROM albums WHERE id = ?"
        ).bind(albumId).execute().fetchAll();
        if (allCheck.empty()) { session.rollback(); return false; }
        string createdBy = allCheck[0][0].get<string>();
        string groupId   = allCheck[0][1].get<string>();

        // 생성자 또는 그룹 방장만 삭제 가능 (role 기반)
        bool isOwner = false;
        {
            auto roleRow = session.sql(
                "SELECT role FROM group_members WHERE group_id = ? AND user_id = ? LIMIT 1"
            ).bind(groupId, userId).execute().fetchOne();
            if (roleRow) isOwner = (roleRow[0].get<string>() == "owner");
        }
        if (createdBy != userId && !isOwner) { session.rollback(); return false; }

        // 삭제할 사진들의 파일 크기 합산 (스토리지 차감용)
        int64 totalFileSize = 0;
        {
            auto sizeRow = session.sql(
                "SELECT COALESCE(SUM(file_size), 0) FROM album_photos WHERE album_id = ? AND is_deleted = 0"
            ).bind(albumId).execute().fetchOne();
            if (sizeRow && !sizeRow[0].isNull())
                totalFileSize = static_cast<int64>(sizeRow[0].get<double>());
        }

        int64 now = NowMs();

        // 사진 댓글 삭제
        session.sql(
            "DELETE pc FROM photo_comments pc "
            "INNER JOIN album_photos ap ON pc.photo_id = ap.id "
            "WHERE ap.album_id = ?"
        ).bind(albumId).execute();

        // 사진 소프트 딜리트 (배치잡이 30일 후 R2 하드딜리트)
        session.sql(
            "UPDATE album_photos SET is_deleted = 1, deleted_at = ? WHERE album_id = ? AND is_deleted = 0"
        ).bind(now, albumId).execute();

        // 앨범 레코드 삭제
        session.sql("DELETE FROM albums WHERE id = ?").bind(albumId).execute();

        session.commit();

        // 스토리지 차감 (트랜잭션 외부, 실패해도 롤백 불필요)
        if (totalFileSize > 0) {
            GroupRepository::DeductGroupStorage(groupId, totalFileSize);
        }

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
        string coverUrl;
        {
            auto allRows = session.sql(
                "SELECT COALESCE(thumbnail_url, photo_url) FROM album_photos "
                "WHERE album_id = ? AND is_deleted = 0 ORDER BY uploaded_at ASC LIMIT 1"
            ).bind(albumId).execute().fetchAll();
            if (!allRows.empty())
                coverUrl = allRows[0][0].get<string>();
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
                                  const string& source, const string& filePath, int64 fileSize)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        string id = GenerateUUID();
        int64 now = NowMs();

        if (msgSeq > 0) {
            session.sql(
                "INSERT INTO album_photos "
                "(id, album_id, group_id, msg_seq, photo_url, thumbnail_url, uploader_id, source, uploaded_at, gcs_path, file_size) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
            ).bind(id, albumId, groupId, msgSeq, photoUrl, thumbnailUrl, uploaderId, source, now, filePath, fileSize).execute();
        } else {
            session.sql(
                "INSERT INTO album_photos "
                "(id, album_id, group_id, msg_seq, photo_url, thumbnail_url, uploader_id, source, uploaded_at, gcs_path, file_size) "
                "VALUES (?, ?, ?, NULL, ?, ?, ?, ?, ?, ?, ?)"
            ).bind(id, albumId, groupId, photoUrl, thumbnailUrl, uploaderId, source, now, filePath, fileSize).execute();
        }

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
            "       photo_url, COALESCE(thumbnail_url, ''), uploader_id, source, uploaded_at, "
            "       COALESCE(gcs_path, ''), COALESCE(file_size, 0) "
            "FROM album_photos WHERE album_id = ? AND is_deleted = 0 "
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
            info.filePath     = row[9].get<string>();
            info.fileSize     = row[10].get<int64>();
            result.push_back(info);
        }
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[AlbumRepository] GetPhotos 실패: {}", e.what());
    }
    return result;
}

bool AlbumRepository::DeletePhoto(const string& photoId, const string& userId,
                                   const string& groupId)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        session.startTransaction();

        auto allCheck = session.sql(
            "SELECT uploader_id, album_id, COALESCE(gcs_path,''), COALESCE(file_size,0) "
            "FROM album_photos WHERE id = ? AND is_deleted = 0"
        ).bind(photoId).execute().fetchAll();
        if (allCheck.empty()) { session.rollback(); return false; }

        string uploaderId = allCheck[0][0].get<string>();
        string albumId    = allCheck[0][1].get<string>();
        string filePath   = allCheck[0][2].get<string>();
        int64  fileSize   = allCheck[0][3].get<int64>();

        // 업로더 또는 그룹 방장(role 기반)만 삭제 가능
        bool isOwner = false;
        {
            auto roleRow = session.sql(
                "SELECT role FROM group_members WHERE group_id = ? AND user_id = ? LIMIT 1"
            ).bind(groupId, userId).execute().fetchOne();
            if (roleRow) isOwner = (roleRow[0].get<string>() == "owner");
        }
        if (uploaderId != userId && !isOwner) {
            session.rollback();
            return false;
        }

        // 소프트 딜리트
        int64 now = NowMs();
        session.sql(
            "UPDATE album_photos SET is_deleted = 1, deleted_at = ? WHERE id = ?"
        ).bind(now, photoId).execute();
        session.commit();

        // 즉시 스토리지 차감
        if (fileSize > 0) {
            GroupRepository::DeductGroupStorage(groupId, fileSize);
        }

        UpdateCoverPhoto(albumId);
        return true;
    } catch (const mysqlx::Error& e) {
        try { DBManager::GetInstance().GetSession().rollback(); } catch (...) {}
        LOG_ERROR("[AlbumRepository] DeletePhoto 실패: {}", e.what());
        return false;
    }
}


vector<AlbumPhotoInfo> AlbumRepository::GetSoftDeletedPhotosOlderThan(int64 cutoffMs)
{
    vector<AlbumPhotoInfo> result;
    try {
        auto& session = DBManager::GetInstance().GetSession();
        auto rows = session.sql(
            "SELECT id, group_id, COALESCE(gcs_path,''), COALESCE(file_size,0) "
            "FROM album_photos WHERE is_deleted = 1 AND deleted_at IS NOT NULL AND deleted_at < ? "
            "LIMIT 200"
        ).bind(cutoffMs).execute();

        for (auto row : rows) {
            AlbumPhotoInfo info;
            info.id       = row[0].get<string>();
            info.groupId  = row[1].get<string>();
            info.filePath = row[2].get<string>();
            info.fileSize = row[3].get<int64>();
            result.push_back(info);
        }
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[AlbumRepository] GetSoftDeletedPhotosOlderThan 실패: {}", e.what());
    }
    return result;
}

bool AlbumRepository::HardDeletePhoto(const string& photoId)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        session.startTransaction();
        session.sql("DELETE FROM photo_comments WHERE photo_id = ?").bind(photoId).execute();
        session.sql("DELETE FROM album_photos WHERE id = ?").bind(photoId).execute();
        session.commit();
        return true;
    } catch (const mysqlx::Error& e) {
        try { DBManager::GetInstance().GetSession().rollback(); } catch (...) {}
        LOG_ERROR("[AlbumRepository] HardDeletePhoto 실패: {}", e.what());
        return false;
    }
}

bool AlbumRepository::SoftDeleteAllPhotosByGroup(const string& groupId, int64 deletedAtMs)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        session.sql(
            "UPDATE album_photos SET is_deleted=1, deleted_at=? "
            "WHERE group_id=? AND is_deleted=0"
        ).bind(deletedAtMs, groupId).execute();
        return true;
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[AlbumRepository] SoftDeleteAllPhotosByGroup 실패: {}", e.what());
        return false;
    }
}

int64 AlbumRepository::SoftDeleteOverQuotaPhotosByGroup(
    const string& groupId, int64 quotaBytes, int64 deletedAtMs)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();

        // 최신순으로 사진 목록 조회 (삭제되지 않은 것만)
        auto rows = session.sql(
            "SELECT id, COALESCE(file_size, 0) FROM album_photos "
            "WHERE group_id = ? AND is_deleted = 0 "
            "ORDER BY uploaded_at DESC"
        ).bind(groupId).execute();

        vector<string> toDelete;
        int64 accumulated = 0;
        int64 totalDeletedSize = 0;

        for (auto row : rows) {
            string photoId = row[0].get<string>();
            int64 sz = row[1].get<int64>();
            if (accumulated + sz <= quotaBytes) {
                accumulated += sz;
            } else {
                toDelete.push_back(photoId);
                totalDeletedSize += sz;
            }
        }

        if (toDelete.empty()) return 0;

        // IN 절 구성 — UUID는 안전한 hex+dash 문자만 포함
        string inClause;
        for (size_t i = 0; i < toDelete.size(); ++i) {
            if (i > 0) inClause += ',';
            inClause += '\'';
            inClause += toDelete[i];
            inClause += '\'';
        }
        session.sql(
            "UPDATE album_photos SET is_deleted = 1, deleted_at = ? "
            "WHERE id IN (" + inClause + ")"
        ).bind(deletedAtMs).execute();

        return totalDeletedSize;
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[AlbumRepository] SoftDeleteOverQuotaPhotosByGroup 실패: {}", e.what());
        return -1;
    }
}

bool AlbumRepository::MovePhoto(const string& photoId, const string& toAlbumId)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        // 원본 albumId 조회 (커버 갱신용)
        string fromAlbumId;
        {
            auto allCheck = session.sql(
                "SELECT album_id FROM album_photos WHERE id = ?"
            ).bind(photoId).execute().fetchAll();
            if (allCheck.empty()) return false;
            fromAlbumId = allCheck[0][0].get<string>();
        }

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
        {
            auto allCheck = session.sql(
                "SELECT sender_id FROM photo_comments WHERE id = ?"
            ).bind(commentId).execute().fetchAll();
            if (allCheck.empty()) return false;
            if (allCheck[0][0].get<string>() != userId) return false;
        }

        session.sql("DELETE FROM photo_comments WHERE id = ?").bind(commentId).execute();
        return true;
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[AlbumRepository] DeleteComment 실패: {}", e.what());
        return false;
    }
}
