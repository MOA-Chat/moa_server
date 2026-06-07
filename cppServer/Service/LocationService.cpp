#include "pch.h"
#include "LocationService.h"

#include "UserManager.h"
#include "ServerSession.h"
#include "../PacketDispatcher.h"
#include "../CoreGlobal.h"
#include "../DB/LocationRepository.h"
#include "../DB/GroupRepository.h"
#include "../DB/UserRepository.h"
#include "../DB/FileRepository.h"
#include "Cloud/CloudStorage.h"

#include <chrono>

using namespace Protocol;

namespace {

void BroadcastToGroup(const string& groupId, Protocol::Envelope& env,
                      UserManager& um, const string& excludeUserId = "")
{
    auto members = GroupRepository::GetGroupMembers(groupId);
    for (const auto& member : members) {
        if (!excludeUserId.empty() && member.userId == excludeUserId) continue;
        if (auto s = um.FindSession(member.userId)) {
            PacketDispatcher::SendEnvelope(s, env);
        }
    }
}

void FillVisitProto(Protocol::LocationVisitInfo* proto, const cLocationVisit& v)
{
    proto->set_id(v.id);
    proto->set_user_id(v.userId);
    proto->set_user_name(v.userName);
    proto->set_rating(v.rating);
    proto->set_review(v.review);
    proto->set_visited_at(v.visitedAt);
    proto->set_history_id(v.historyId);
    for (const auto& [id, url] : v.photos) {
        auto* ph = proto->add_photos();
        ph->set_id(id);
        ph->set_photo_url(url);
    }
}

void FillPlaceProto(Protocol::LocationPlaceInfo* proto, const cLocationPlace& p,
                    const vector<cLocationVisit>& visits = {})
{
    proto->set_id(p.id);
    proto->set_group_id(p.groupId);
    proto->set_name(p.name);
    proto->set_address(p.address);
    proto->set_category(p.category);
    proto->set_lat(p.lat);
    proto->set_lng(p.lng);
    proto->set_created_by(p.createdBy);
    proto->set_created_at(p.createdAt);
    proto->set_visit_count(p.visitCount);
    proto->set_avg_rating(p.avgRating);
    proto->set_recent_photo_url(p.recentPhotoUrl);
    if (!p.canonicalId.empty()) {
        proto->set_canonical_id(p.canonicalId);
    }
    for (const auto& v : visits) {
        FillVisitProto(proto->add_visits(), v);
    }
}

} // namespace

void LocationService::HandleErr(sessionPtr& session, uint64 reqId, ErrorCode code, const string& msg)
{
    PacketDispatcher::DispatchError(session, reqId, code, msg);
}

// ── 장소 목록 조회 ─────────────────────────────────────────────────────────────

bool LocationService::HandleGetPlaces(sessionPtr& session, uint64 reqId, const C_GetPlaces& pkt)
{
    const string userId   = static_pointer_cast<ServerSession>(session)->GetUserId();
    const string& groupId = pkt.group_id();
    if (groupId.empty()) {
        HandleErr(session, reqId, ERR_INVALID_PACKET);
        return false;
    }
    if (!GroupRepository::IsMember(groupId, userId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }

    auto places = LocationRepository::GetPlacesByGroup(groupId);

    Envelope resp;
    resp.set_request_id(static_cast<uint32>(reqId));
    auto* s = resp.mutable_s_get_places();
    s->set_group_id(groupId);
    for (const auto& p : places) {
        auto visits = LocationRepository::GetVisitsByPlace(p.id);
        FillPlaceProto(s->add_places(), p, visits);
    }

    PacketDispatcher::SendEnvelope(session, resp);
    return true;
}

// ── 장소 저장 ─────────────────────────────────────────────────────────────────

bool LocationService::HandleSavePlace(sessionPtr& session, uint64 reqId, const C_SavePlace& pkt)
{
    const string userId = static_pointer_cast<ServerSession>(session)->GetUserId();
    if (pkt.group_id().empty() || pkt.name().empty() || userId.empty()) {
        HandleErr(session, reqId, ERR_INVALID_PACKET);
        return false;
    }
    if (!GroupRepository::IsMember(pkt.group_id(), userId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }

    cLocationPlace place;
    place.groupId   = pkt.group_id();
    place.name      = pkt.name();
    place.address   = pkt.address();
    place.category  = pkt.category().empty() ? "etc" : pkt.category();
    place.lat       = pkt.lat();
    place.lng       = pkt.lng();
    place.createdBy = userId;

    string placeId = LocationRepository::CreatePlace(place);
    if (placeId.empty()) {
        HandleErr(session, reqId, ERR_SERVER_INTERNAL, "장소 저장 실패");
        return false;
    }
    place.id = placeId;

    Envelope resp;
    resp.set_request_id(static_cast<uint32>(reqId));
    auto* s = resp.mutable_s_save_place();
    s->set_success(true);
    FillPlaceProto(s->mutable_place(), place);

    BroadcastToGroup(place.groupId, resp, _userManager);
    LOG_INFO("[LocationService] 장소 저장: group={} name={} by={}", place.groupId, place.name, place.createdBy);
    return true;
}

// ── 방문 기록 저장 ─────────────────────────────────────────────────────────────

bool LocationService::HandleSaveVisit(sessionPtr& session, uint64 reqId, const C_SaveVisit& pkt)
{
    const string userId = static_pointer_cast<ServerSession>(session)->GetUserId();
    if (pkt.place_id().empty() || pkt.group_id().empty() || userId.empty()) {
        HandleErr(session, reqId, ERR_INVALID_PACKET);
        return false;
    }
    if (!GroupRepository::IsMember(pkt.group_id(), userId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }
    if (pkt.rating() < 0 || pkt.rating() > 5) {
        HandleErr(session, reqId, ERR_INVALID_PACKET, "별점은 0~5 사이여야 합니다.");
        return false;
    }
    if (pkt.photo_urls_size() > 5) {
        HandleErr(session, reqId, ERR_INVALID_PACKET, "사진은 최대 5장까지 등록 가능합니다.");
        return false;
    }

    if (!LocationRepository::PlaceExists(pkt.place_id())) {
        HandleErr(session, reqId, ERR_INVALID_PACKET, "장소를 찾을 수 없습니다");
        return false;
    }

    string userName;
    UserRepository::GetUserNameWithId(userId, userName);

    cLocationVisit visit;
    visit.placeId   = pkt.place_id();
    visit.groupId   = pkt.group_id();
    visit.userId    = userId;
    visit.userName  = userName.empty() ? pkt.user_name() : userName;
    visit.rating    = pkt.rating();
    visit.review    = pkt.review();
    visit.historyId = pkt.history_id();
    visit.visitedAt = pkt.visited_at() > 0 ? pkt.visited_at() :
        chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()).count();

    string visitId = LocationRepository::CreateVisit(visit);
    if (visitId.empty()) {
        HandleErr(session, reqId, ERR_SERVER_INTERNAL, "방문 기록 저장 실패");
        return false;
    }
    visit.id = visitId;

    if (pkt.photo_urls_size() > 0) {
        vector<string> urls(pkt.photo_urls().begin(), pkt.photo_urls().end());
        LocationRepository::AddVisitPhotos(visit.id, visit.placeId, visit.groupId, urls);
        for (const auto& url : urls) {
            visit.photos.emplace_back("", url);
            // 위치 사진은 GROUP_CHAT으로 업로드되어 14일 retention이 걸림 → 영구 전환
            if (GStorage) {
                const string& prefix = GStorage->GetPublicUrlPrefix();
                if (!prefix.empty() && url.substr(0, prefix.size()) == prefix) {
                    string gcsPath = url.substr(prefix.size() + 1);
                    auto q = gcsPath.find('?');
                    if (q != string::npos) gcsPath = gcsPath.substr(0, q);
                    FileRepository::ClearRetentionByFilePath(gcsPath);
                }
            }
        }
    }

    LocationRepository::UpdatePlaceStats(visit.placeId);

    // 갱신된 장소 통계 조회
    cLocationPlace updatedPlace;
    for (const auto& p : LocationRepository::GetPlacesByGroup(pkt.group_id())) {
        if (p.id == pkt.place_id()) { updatedPlace = p; break; }
    }
    if (updatedPlace.id.empty()) {
        LOG_ERROR("[LocationService] 방문 저장 후 장소 조회 실패: place_id={}", pkt.place_id());
    }

    Envelope resp;
    resp.set_request_id(static_cast<uint32>(reqId));
    auto* s = resp.mutable_s_save_visit();
    s->set_success(true);
    s->set_place_id(pkt.place_id());
    FillVisitProto(s->mutable_visit(), visit);
    if (!updatedPlace.id.empty()) FillPlaceProto(s->mutable_place(), updatedPlace);

    BroadcastToGroup(visit.groupId, resp, _userManager);
    LOG_INFO("[LocationService] 방문 기록: place={} by={} rating={}", visit.placeId, visit.userId, visit.rating);
    return true;
}

// ── 장소 삭제 ─────────────────────────────────────────────────────────────────

bool LocationService::HandleDeletePlace(sessionPtr& session, uint64 reqId, const C_DeletePlace& pkt)
{
    const string userId = static_pointer_cast<ServerSession>(session)->GetUserId();
    if (pkt.place_id().empty() || userId.empty()) {
        HandleErr(session, reqId, ERR_INVALID_PACKET);
        return false;
    }
    if (!GroupRepository::IsMember(pkt.group_id(), userId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }

    if (!LocationRepository::DeletePlace(pkt.place_id(), userId)) {
        HandleErr(session, reqId, ERR_NO_PERMISSION, "삭제 권한이 없거나 장소를 찾을 수 없습니다");
        return false;
    }

    Envelope resp;
    resp.set_request_id(static_cast<uint32>(reqId));
    auto* s = resp.mutable_s_delete_place();
    s->set_success(true);
    s->set_place_id(pkt.place_id());
    s->set_group_id(pkt.group_id());

    BroadcastToGroup(pkt.group_id(), resp, _userManager);
    LOG_INFO("[LocationService] 장소 삭제: id={} by={}", pkt.place_id(), pkt.user_id());
    return true;
}

// ── 방문 기록 수정 ─────────────────────────────────────────────────────────────

bool LocationService::HandleUpdateVisit(sessionPtr& session, uint64 reqId, const C_UpdateVisit& pkt)
{
    const string userId = static_pointer_cast<ServerSession>(session)->GetUserId();
    if (pkt.visit_id().empty() || pkt.place_id().empty() || pkt.group_id().empty() || userId.empty()) {
        HandleErr(session, reqId, ERR_INVALID_PACKET);
        return false;
    }
    if (!GroupRepository::IsMember(pkt.group_id(), userId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }
    if (pkt.rating() < 0 || pkt.rating() > 5) {
        HandleErr(session, reqId, ERR_INVALID_PACKET, "별점은 0~5 사이여야 합니다.");
        return false;
    }
    if (pkt.photo_urls_size() > 5) {
        HandleErr(session, reqId, ERR_INVALID_PACKET, "사진은 최대 5장까지 등록 가능합니다.");
        return false;
    }

    int64 visitedAt = pkt.visited_at() > 0 ? pkt.visited_at() :
        chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()).count();

    vector<string> photos(pkt.photo_urls().begin(), pkt.photo_urls().end());
    bool ok = LocationRepository::UpdateVisit(
        pkt.visit_id(), userId,
        pkt.rating(), pkt.review(), visitedAt, photos, pkt.place_id(), pkt.history_id()
    );
    if (!ok) {
        HandleErr(session, reqId, ERR_NO_PERMISSION, "수정 권한이 없거나 방문 기록을 찾을 수 없습니다");
        return false;
    }

    // 위치 사진 retention 영구 전환 (UpdateVisit은 기존 사진 삭제 후 재삽입)
    if (GStorage && !photos.empty()) {
        const string& prefix = GStorage->GetPublicUrlPrefix();
        for (const auto& url : photos) {
            if (!prefix.empty() && url.substr(0, prefix.size()) == prefix) {
                string gcsPath = url.substr(prefix.size() + 1);
                auto q = gcsPath.find('?');
                if (q != string::npos) gcsPath = gcsPath.substr(0, q);
                FileRepository::ClearRetentionByFilePath(gcsPath);
            }
        }
    }

    string userName;
    UserRepository::GetUserNameWithId(userId, userName);

    // 갱신된 방문 + 장소 조회
    cLocationVisit updatedVisit;
    updatedVisit.id        = pkt.visit_id();
    updatedVisit.placeId   = pkt.place_id();
    updatedVisit.groupId   = pkt.group_id();
    updatedVisit.userId    = userId;
    updatedVisit.userName  = userName.empty() ? userId : userName;
    updatedVisit.rating    = pkt.rating();
    updatedVisit.review    = pkt.review();
    updatedVisit.visitedAt = visitedAt;
    updatedVisit.historyId = pkt.history_id();
    for (const auto& url : photos) updatedVisit.photos.emplace_back("", url);

    cLocationPlace updatedPlace;
    for (const auto& p : LocationRepository::GetPlacesByGroup(pkt.group_id())) {
        if (p.id == pkt.place_id()) { updatedPlace = p; break; }
    }
    if (updatedPlace.id.empty()) {
        LOG_ERROR("[LocationService] 방문 수정 후 장소 조회 실패: place_id={}", pkt.place_id());
    }

    Envelope resp;
    resp.set_request_id(static_cast<uint32>(reqId));
    auto* s = resp.mutable_s_update_visit();
    s->set_success(true);
    FillVisitProto(s->mutable_visit(), updatedVisit);
    if (!updatedPlace.id.empty()) FillPlaceProto(s->mutable_place(), updatedPlace);

    BroadcastToGroup(pkt.group_id(), resp, _userManager);
    LOG_INFO("[LocationService] 방문 수정: visit={} by={}", pkt.visit_id(), pkt.user_id());
    return true;
}

// ── 방문 기록 삭제 ─────────────────────────────────────────────────────────────

bool LocationService::HandleDeleteVisit(sessionPtr& session, uint64 reqId, const C_DeleteVisit& pkt)
{
    const string userId = static_pointer_cast<ServerSession>(session)->GetUserId();
    if (pkt.visit_id().empty() || pkt.place_id().empty() || pkt.group_id().empty() || userId.empty()) {
        HandleErr(session, reqId, ERR_INVALID_PACKET);
        return false;
    }
    if (!GroupRepository::IsMember(pkt.group_id(), userId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }

    auto [success, placeDeleted] = LocationRepository::DeleteVisit(pkt.visit_id(), userId, pkt.place_id());
    if (!success) {
        HandleErr(session, reqId, ERR_NO_PERMISSION, "삭제 권한이 없거나 방문 기록을 찾을 수 없습니다");
        return false;
    }

    Envelope resp;
    resp.set_request_id(static_cast<uint32>(reqId));
    auto* s = resp.mutable_s_delete_visit();
    s->set_success(true);
    s->set_visit_id(pkt.visit_id());
    s->set_place_id(pkt.place_id());
    s->set_group_id(pkt.group_id());
    s->set_place_deleted(placeDeleted);

    BroadcastToGroup(pkt.group_id(), resp, _userManager);
    LOG_INFO("[LocationService] 방문 삭제: visit={} placeDeleted={}", pkt.visit_id(), placeDeleted);
    return true;
}
