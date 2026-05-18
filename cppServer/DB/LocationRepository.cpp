#include "pch.h"
#include "LocationRepository.h"

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
        장소 CRUD
=======================*/

string LocationRepository::CreatePlace(const cLocationPlace& place)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        string id  = GenerateUUID();
        int64  now = NowMs();
        session.sql(
            "INSERT INTO location_places "
            "(id, group_id, name, address, category, lat, lng, created_by, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
        ).bind(id, place.groupId, place.name, place.address,
               place.category, place.lat, place.lng,
               place.createdBy, now).execute();
        return id;
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[LocationRepository] CreatePlace 실패: {}", e.what());
        return "";
    }
}

bool LocationRepository::DeletePlace(const string& placeId, const string& userId)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();

        auto check = session.sql(
            "SELECT created_by FROM location_places WHERE id = ?"
        ).bind(placeId).execute();
        if (check.count() == 0) return false;
        auto row = check.fetchOne();
        if (row[0].get<string>() != userId) return false;

        session.sql("DELETE FROM location_places WHERE id = ?").bind(placeId).execute();
        return true;
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[LocationRepository] DeletePlace 실패: {}", e.what());
        return false;
    }
}

bool LocationRepository::UpdateVisit(const string& visitId, const string& userId,
                                     int32 rating, const string& review,
                                     int64 visitedAt, const vector<string>& photoUrls,
                                     const string& placeId)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();

        // 본인 검증
        auto check = session.sql(
            "SELECT user_id, group_id FROM location_visits WHERE id = ?"
        ).bind(visitId).execute();
        if (check.count() == 0) return false;
        auto checkRow = check.fetchOne();
        if (checkRow[0].get<string>() != userId) return false;
        string groupId = checkRow[1].get<string>();

        session.sql(
            "UPDATE location_visits SET rating=?, review=?, visited_at=? WHERE id=?"
        ).bind(rating, review, visitedAt, visitId).execute();

        // 사진 교체: 기존 삭제 후 재삽입
        session.sql("DELETE FROM location_photos WHERE visit_id=?").bind(visitId).execute();
        int64 now = (int64)time(nullptr) * 1000LL;
        for (const auto& url : photoUrls) {
            string photoId = [&]() -> string {
                auto r = session.sql("SELECT UUID()").execute();
                return r.fetchOne()[0].get<string>();
            }();
            session.sql(
                "INSERT INTO location_photos (id, visit_id, place_id, group_id, photo_url, created_at) VALUES (?,?,?,?,?,?)"
            ).bind(photoId, visitId, placeId, groupId, url, now).execute();
        }

        UpdatePlaceStats(placeId);
        return true;
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[LocationRepository] UpdateVisit 실패: {}", e.what());
        return false;
    }
}

pair<bool,bool> LocationRepository::DeleteVisit(const string& visitId, const string& userId,
                                                 const string& placeId)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();

        // 본인 검증
        auto check = session.sql(
            "SELECT user_id FROM location_visits WHERE id = ?"
        ).bind(visitId).execute();
        if (check.count() == 0) return {false, false};
        if (check.fetchOne()[0].get<string>() != userId) return {false, false};

        // 이 장소의 방문 수 확인
        auto cntRow = session.sql(
            "SELECT COUNT(*) FROM location_visits WHERE place_id = ?"
        ).bind(placeId).execute().fetchOne();
        int visitCount = (int)cntRow[0].get<int64_t>();

        // 방문 삭제 (location_photos는 FK CASCADE로 자동 삭제됨)
        session.sql("DELETE FROM location_visits WHERE id=?").bind(visitId).execute();

        bool placeDeleted = false;
        if (visitCount <= 1) {
            // 마지막 방문이었으면 장소도 삭제
            session.sql("DELETE FROM location_places WHERE id=?").bind(placeId).execute();
            placeDeleted = true;
        } else {
            UpdatePlaceStats(placeId);
        }

        return {true, placeDeleted};
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[LocationRepository] DeleteVisit 실패: {}", e.what());
        return {false, false};
    }
}

vector<cLocationPlace> LocationRepository::GetPlacesByGroup(const string& groupId)
{
    vector<cLocationPlace> result;
    try {
        auto& session = DBManager::GetInstance().GetSession();
        auto rows = session.sql(
            "SELECT id, group_id, name, address, category, lat, lng, "
            "       created_by, created_at, visit_count, avg_rating, "
            "       COALESCE(recent_photo_url, '') AS recent_photo_url "
            "FROM location_places WHERE group_id = ? ORDER BY created_at DESC"
        ).bind(groupId).execute();

        for (auto row : rows) {
            cLocationPlace p;
            p.id             = row[0].get<string>();
            p.groupId        = row[1].get<string>();
            p.name           = row[2].get<string>();
            p.address        = row[3].get<string>();
            p.category       = row[4].get<string>();
            p.lat            = row[5].get<double>();
            p.lng            = row[6].get<double>();
            p.createdBy      = row[7].get<string>();
            p.createdAt      = row[8].get<int64>();
            p.visitCount     = row[9].get<int>();
            p.avgRating      = static_cast<float>(row[10].get<double>());
            p.recentPhotoUrl = row[11].get<string>();
            result.push_back(move(p));
        }
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[LocationRepository] GetPlacesByGroup 실패: {}", e.what());
    }
    return result;
}

bool LocationRepository::PlaceExists(const string& placeId)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        auto rows = session.sql(
            "SELECT 1 FROM location_places WHERE id = ? LIMIT 1"
        ).bind(placeId).execute();
        return rows.count() > 0;
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[LocationRepository] PlaceExists 실패: {}", e.what());
        return false;
    }
}

/*======================
      방문 기록 CRUD
=======================*/

string LocationRepository::CreateVisit(const cLocationVisit& visit)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        string id = GenerateUUID();
        session.sql(
            "INSERT INTO location_visits "
            "(id, place_id, group_id, user_id, user_name, rating, review, visited_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"
        ).bind(id, visit.placeId, visit.groupId, visit.userId,
               visit.userName, visit.rating, visit.review,
               visit.visitedAt).execute();
        return id;
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[LocationRepository] CreateVisit 실패: {}", e.what());
        return "";
    }
}

vector<cLocationVisit> LocationRepository::GetVisitsByPlace(const string& placeId)
{
    vector<cLocationVisit> result;
    try {
        auto& session = DBManager::GetInstance().GetSession();
        auto rows = session.sql(
            "SELECT v.id, v.place_id, v.group_id, v.user_id, v.user_name, "
            "       v.rating, v.review, v.visited_at, "
            "       COALESCE(p.id, '')        AS photo_id, "
            "       COALESCE(p.photo_url, '') AS photo_url "
            "FROM location_visits v "
            "LEFT JOIN location_photos p ON p.visit_id = v.id "
            "WHERE v.place_id = ? ORDER BY v.visited_at DESC"
        ).bind(placeId).execute();

        string lastId;
        for (auto row : rows) {
            string vid = row[0].get<string>();
            if (vid != lastId) {
                cLocationVisit v;
                v.id        = vid;
                v.placeId   = row[1].get<string>();
                v.groupId   = row[2].get<string>();
                v.userId    = row[3].get<string>();
                v.userName  = row[4].get<string>();
                v.rating    = row[5].get<int>();
                v.review    = row[6].get<string>();
                v.visitedAt = row[7].get<int64>();
                result.push_back(move(v));
                lastId = vid;
            }
            string photoId = row[8].get<string>();
            if (!photoId.empty()) {
                result.back().photos.emplace_back(photoId, row[9].get<string>());
            }
        }
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[LocationRepository] GetVisitsByPlace 실패: {}", e.what());
    }
    return result;
}

bool LocationRepository::UpdatePlaceStats(const string& placeId)
{
    try {
        auto& session = DBManager::GetInstance().GetSession();
        session.sql(
            "UPDATE location_places "
            "SET visit_count = (SELECT COUNT(*) FROM location_visits WHERE place_id = ?), "
            "    avg_rating  = COALESCE((SELECT AVG(rating) FROM location_visits WHERE place_id = ? AND rating > 0), 0), "
            "    recent_photo_url = ("
            "        SELECT p.photo_url FROM location_photos p "
            "        JOIN location_visits v ON v.id = p.visit_id "
            "        WHERE v.place_id = ? ORDER BY p.created_at DESC LIMIT 1"
            "    ) "
            "WHERE id = ?"
        ).bind(placeId, placeId, placeId, placeId).execute();
        return true;
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[LocationRepository] UpdatePlaceStats 실패: {}", e.what());
        return false;
    }
}

bool LocationRepository::AddVisitPhotos(const string& visitId, const string& placeId,
                                         const string& groupId,
                                         const vector<string>& photoUrls)
{
    if (photoUrls.empty()) return true;
    try {
        auto& session = DBManager::GetInstance().GetSession();
        int64 now = NowMs();
        for (const auto& url : photoUrls) {
            session.sql(
                "INSERT INTO location_photos (id, visit_id, place_id, group_id, photo_url, created_at) "
                "VALUES (?, ?, ?, ?, ?, ?)"
            ).bind(GenerateUUID(), visitId, placeId, groupId, url, now).execute();
        }
        return true;
    } catch (const mysqlx::Error& e) {
        LOG_ERROR("[LocationRepository] AddVisitPhotos 실패: {}", e.what());
        return false;
    }
}
