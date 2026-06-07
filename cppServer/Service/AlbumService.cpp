#include "pch.h"
#include "AlbumService.h"

#include "UserManager.h"
#include "ServerSession.h"
#include "../PacketDispatcher.h"
#include "../CoreGlobal.h"
#include "../DB/AlbumRepository.h"
#include "../DB/GroupRepository.h"
#include "../DB/MessageRepository.h"
#include "../DB/FileRepository.h"
#include "../DB/UserRepository.h"

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

void FillAlbumProto(Protocol::Album* proto, const AlbumInfo& info)
{
    proto->set_id(info.id);
    proto->set_group_id(info.groupId);
    proto->set_name(info.name);
    proto->set_cover_photo_url(info.coverPhotoUrl);
    proto->set_created_by(info.createdBy);
    proto->set_created_at(info.createdAt);
    proto->set_photo_count(info.photoCount);
}

void FillPhotoProto(Protocol::AlbumPhoto* proto, const AlbumPhotoInfo& info)
{
    proto->set_id(info.id);
    proto->set_album_id(info.albumId);
    proto->set_group_id(info.groupId);
    proto->set_msg_seq(info.msgSeq);
    proto->set_photo_url(info.photoUrl);
    proto->set_thumbnail_url(info.thumbnailUrl);
    proto->set_uploader_id(info.uploaderId);
    proto->set_source(info.source);
    proto->set_uploaded_at(info.uploadedAt);
}

void FillCommentProto(Protocol::PhotoComment* proto, const PhotoCommentInfo& info)
{
    proto->set_id(info.id);
    proto->set_photo_id(info.photoId);
    proto->set_sender_id(info.senderId);
    proto->set_sender_name(info.senderName);
    proto->set_text(info.text);
    proto->set_created_at(info.createdAt);
}

} // namespace

void AlbumService::HandleErr(sessionPtr& session, uint64 reqId, Protocol::ErrorCode errorCode, const string& errMessage)
{
    PacketDispatcher::DispatchError(session, reqId, errorCode, errMessage);
}

bool AlbumService::HandleCreateAlbum(sessionPtr& session, uint64 reqId, const C_CreateAlbum& pkt)
{
    const string userId = static_pointer_cast<ServerSession>(session)->GetUserId();
    const string& groupId = pkt.group_id();
    const string& name    = pkt.name();

    if (groupId.empty() || name.empty() || userId.empty()) {
        HandleErr(session, reqId, ERR_INVALID_PACKET);
        return false;
    }
    if (!GroupRepository::IsMember(groupId, userId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }
    const string& createdBy = userId;

    string albumId = AlbumRepository::CreateAlbum(groupId, name, createdBy);
    if (albumId.empty()) {
        HandleErr(session, reqId, ERR_SERVER_INTERNAL, "앨범 생성 실패");
        return false;
    }

    auto albums = AlbumRepository::GetAlbums(groupId);
    AlbumInfo created;
    for (const auto& a : albums) {
        if (a.id == albumId) { created = a; break; }
    }

    Envelope resp;
    resp.set_request_id(static_cast<uint32>(reqId));
    auto* s = resp.mutable_s_create_album();
    s->set_success(true);
    FillAlbumProto(s->mutable_album(), created);

    BroadcastToGroup(groupId, resp, _userManager);
    LOG_INFO("[AlbumService] 앨범 생성: group={} name={} by={}", groupId, name, createdBy);
    return true;
}

bool AlbumService::HandleGetAlbums(sessionPtr& session, uint64 reqId, const C_GetAlbums& pkt)
{
    const string userId = static_pointer_cast<ServerSession>(session)->GetUserId();
    const string& groupId = pkt.group_id();

    if (!GroupRepository::IsMember(groupId, userId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }

    auto albums = AlbumRepository::GetAlbums(groupId);

    Envelope resp;
    resp.set_request_id(static_cast<uint32>(reqId));
    auto* s = resp.mutable_s_get_albums();
    for (const auto& a : albums) {
        FillAlbumProto(s->add_albums(), a);
    }
    PacketDispatcher::SendEnvelope(session, resp);
    return true;
}

bool AlbumService::HandleDeleteAlbum(sessionPtr& session, uint64 reqId, const C_DeleteAlbum& pkt)
{
    const string userId   = static_pointer_cast<ServerSession>(session)->GetUserId();
    const string& albumId = pkt.album_id();
    const string& groupId = pkt.group_id();

    if (!GroupRepository::IsMember(groupId, userId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }

    // 앨범이 실제로 이 그룹 소속인지 확인 (크로스그룹 공격 방지)
    AlbumInfo albumMeta = AlbumRepository::GetAlbum(albumId);
    if (albumMeta.id.empty()) {
        HandleErr(session, reqId, ERR_INVALID_PACKET, "앨범을 찾을 수 없습니다.");
        return false;
    }
    if (albumMeta.groupId != groupId) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "해당 그룹의 앨범이 아닙니다.");
        return false;
    }

    bool ok = AlbumRepository::DeleteAlbum(albumId, userId);
    if (!ok) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "삭제 권한이 없거나 앨범이 존재하지 않습니다.");
        return false;
    }

    Envelope resp;
    resp.set_request_id(static_cast<uint32>(reqId));
    auto* s = resp.mutable_s_delete_album();
    s->set_success(true);
    s->set_album_id(albumId);
    s->set_group_id(groupId);

    BroadcastToGroup(groupId, resp, _userManager);
    LOG_INFO("[AlbumService] 앨범 삭제: albumId={} by={}", albumId, userId);
    return true;
}

bool AlbumService::HandleEditAlbum(sessionPtr& session, uint64 reqId, const C_EditAlbum& pkt)
{
    const string userId   = static_pointer_cast<ServerSession>(session)->GetUserId();
    const string& albumId = pkt.album_id();
    const string& groupId = pkt.group_id();
    const string& name    = pkt.name();

    if (name.empty() || name.size() > 50) {
        HandleErr(session, reqId, ERR_INVALID_PACKET, "앨범 이름이 유효하지 않습니다.");
        return false;
    }
    if (!GroupRepository::IsMember(groupId, userId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }
    AlbumInfo albumMeta = AlbumRepository::GetAlbum(albumId);
    if (albumMeta.id.empty() || albumMeta.groupId != groupId) {
        HandleErr(session, reqId, ERR_INVALID_PACKET, "앨범을 찾을 수 없습니다.");
        return false;
    }
    // 방장 또는 앨범 생성자만 이름 수정 가능
    string role = GroupRepository::GetMemberRole(groupId, userId);
    if (role != "owner" && albumMeta.createdBy != userId) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "이름 수정 권한이 없습니다.");
        return false;
    }

    bool ok = AlbumRepository::UpdateAlbumName(albumId, groupId, name);
    if (!ok) {
        HandleErr(session, reqId, ERR_SERVER_INTERNAL, "앨범 이름 수정 실패");
        return false;
    }

    Envelope resp;
    resp.set_request_id(static_cast<uint32>(reqId));
    auto* s = resp.mutable_s_edit_album();
    s->set_success(true);
    s->set_album_id(albumId);
    s->set_name(name);

    BroadcastToGroup(groupId, resp, _userManager);
    LOG_INFO("[AlbumService] 앨범 이름 수정: albumId={} name={} by={}", albumId, name, userId);
    return true;
}

bool AlbumService::HandleAddPhotoToAlbum(sessionPtr& session, uint64 reqId, const C_AddPhotoToAlbum& pkt)
{
    const string uploaderId = static_pointer_cast<ServerSession>(session)->GetUserId();
    const string& albumId   = pkt.album_id();
    const string& groupId   = pkt.group_id();
    int64  msgSeq           = pkt.msg_seq();
    const string& photoUrl  = pkt.photo_url();
    const string& thumbUrl  = pkt.thumbnail_url();

    if (albumId.empty() || photoUrl.empty() || uploaderId.empty()) {
        HandleErr(session, reqId, ERR_INVALID_PACKET);
        return false;
    }
    if (!GroupRepository::IsMember(groupId, uploaderId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }

    // 채팅 메시지에서 file_path + file_size 조회
    string filePath;
    int64  fileSize = 0;
    if (msgSeq > 0) {
        string convId = "group:" + groupId;
        MessageRepository::GetFilePathAndFileSizeByMsgSeq(convId, msgSeq, filePath, fileSize);
    }

    // 그룹 스토리지 쿼터 원자적 체크 + 증가
    if (fileSize > 0) {
        GroupRepository::EnsureDefaultStorage(groupId, DEFAULT_GROUP_STORAGE);
        if (!GroupRepository::TryAddGroupStorageAtomic(groupId, fileSize)) {
            HandleErr(session, reqId, ERR_STORAGE_EXCEEDED, "그룹 저장 용량이 부족합니다.");
            return false;
        }
    }

    string photoId = AlbumRepository::AddPhoto(albumId, groupId, msgSeq, photoUrl, thumbUrl,
                                               uploaderId, "chat", filePath, fileSize);
    if (photoId.empty()) {
        if (fileSize > 0) GroupRepository::DeductGroupStorage(groupId, fileSize);
        HandleErr(session, reqId, ERR_SERVER_INTERNAL, "사진 추가 실패");
        return false;
    }

    if (!filePath.empty()) {
        FileRepository::ClearRetentionByFilePath(filePath);
    }

    AlbumPhotoInfo info;
    info.id           = photoId;
    info.albumId      = albumId;
    info.groupId      = groupId;
    info.msgSeq       = msgSeq;
    info.photoUrl     = photoUrl;
    info.thumbnailUrl = thumbUrl;
    info.uploaderId   = uploaderId;
    info.source       = "chat";
    info.uploadedAt   = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    AlbumInfo updatedAlbum = AlbumRepository::GetAlbum(albumId);

    Envelope resp;
    resp.set_request_id(static_cast<uint32>(reqId));
    auto* s = resp.mutable_s_add_photo_to_album();
    s->set_success(true);
    s->set_album_id(albumId);
    FillPhotoProto(s->mutable_photo(), info);
    if (!updatedAlbum.id.empty()) FillAlbumProto(s->mutable_album(), updatedAlbum);

    BroadcastToGroup(groupId, resp, _userManager);
    return true;
}

bool AlbumService::HandleGetAlbumPhotos(sessionPtr& session, uint64 reqId, const C_GetAlbumPhotos& pkt)
{
    const string userId   = static_pointer_cast<ServerSession>(session)->GetUserId();
    const string& albumId = pkt.album_id();

    AlbumInfo albumMeta = AlbumRepository::GetAlbum(albumId);
    if (!albumMeta.id.empty() && !GroupRepository::IsMember(albumMeta.groupId, userId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }
    bool latestFirst = pkt.latest_first();

    auto photos = AlbumRepository::GetPhotos(albumId, latestFirst);

    Envelope resp;
    resp.set_request_id(static_cast<uint32>(reqId));
    auto* s = resp.mutable_s_get_album_photos();
    s->set_album_id(albumId);
    for (const auto& p : photos) {
        FillPhotoProto(s->add_photos(), p);
    }
    PacketDispatcher::SendEnvelope(session, resp);
    return true;
}

bool AlbumService::HandleDeleteAlbumPhoto(sessionPtr& session, uint64 reqId, const C_DeleteAlbumPhoto& pkt)
{
    const string userId   = static_pointer_cast<ServerSession>(session)->GetUserId();
    const string& photoId = pkt.photo_id();
    const string& groupId = pkt.group_id();
    const string& albumId = pkt.album_id();

    if (!GroupRepository::IsMember(groupId, userId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }

    bool ok = AlbumRepository::DeletePhoto(photoId, userId, groupId);
    if (!ok) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "삭제 권한이 없거나 사진이 존재하지 않습니다.");
        return false;
    }

    AlbumInfo updatedAlbum = AlbumRepository::GetAlbum(albumId);

    Envelope resp;
    resp.set_request_id(static_cast<uint32>(reqId));
    auto* s = resp.mutable_s_delete_album_photo();
    s->set_success(true);
    s->set_photo_id(photoId);
    s->set_album_id(albumId);
    s->set_group_id(groupId);
    if (!updatedAlbum.id.empty()) FillAlbumProto(s->mutable_album(), updatedAlbum);

    BroadcastToGroup(groupId, resp, _userManager);
    return true;
}

bool AlbumService::HandleMovePhoto(sessionPtr& session, uint64 reqId, const C_MovePhoto& pkt)
{
    const string userId       = static_pointer_cast<ServerSession>(session)->GetUserId();
    const string& photoId     = pkt.photo_id();
    const string& fromAlbumId = pkt.from_album_id();
    const string& toAlbumId   = pkt.to_album_id();
    const string& groupId     = pkt.group_id();

    if (!GroupRepository::IsMember(groupId, userId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }

    // 크로스그룹 이동 방지: fromAlbum과 toAlbum 모두 이 그룹 소속이어야 함
    AlbumInfo fromAlbumCheck = AlbumRepository::GetAlbum(fromAlbumId);
    AlbumInfo toAlbumCheck   = AlbumRepository::GetAlbum(toAlbumId);
    if (fromAlbumCheck.id.empty() || toAlbumCheck.id.empty()) {
        HandleErr(session, reqId, ERR_INVALID_PACKET, "앨범을 찾을 수 없습니다.");
        return false;
    }
    if (fromAlbumCheck.groupId != groupId || toAlbumCheck.groupId != groupId) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "다른 그룹 앨범으로 이동할 수 없습니다.");
        return false;
    }

    bool ok = AlbumRepository::MovePhoto(photoId, toAlbumId);
    if (!ok) {
        HandleErr(session, reqId, ERR_SERVER_INTERNAL, "사진 이동 실패");
        return false;
    }

    // 커버 갱신 후 최신 album 상태 재조회
    AlbumInfo fromAlbum = AlbumRepository::GetAlbum(fromAlbumId);
    AlbumInfo toAlbum   = AlbumRepository::GetAlbum(toAlbumId);

    Envelope resp;
    resp.set_request_id(static_cast<uint32>(reqId));
    auto* s = resp.mutable_s_move_photo();
    s->set_success(true);
    s->set_photo_id(photoId);
    s->set_from_album_id(fromAlbumId);
    s->set_to_album_id(toAlbumId);
    s->set_group_id(groupId);
    if (!fromAlbum.id.empty()) FillAlbumProto(s->mutable_from_album(), fromAlbum);
    if (!toAlbum.id.empty())   FillAlbumProto(s->mutable_to_album(), toAlbum);

    BroadcastToGroup(groupId, resp, _userManager);
    return true;
}

bool AlbumService::HandleAddPhotoComment(sessionPtr& session, uint64 reqId, const C_AddPhotoComment& pkt)
{
    const string senderId = static_pointer_cast<ServerSession>(session)->GetUserId();
    const string& photoId  = pkt.photo_id();
    const string& text     = pkt.text();
    const string& groupId  = pkt.group_id();

    if (photoId.empty() || senderId.empty() || text.empty()) {
        HandleErr(session, reqId, ERR_INVALID_PACKET);
        return false;
    }
    if (text.size() > 100) {
        HandleErr(session, reqId, ERR_INVALID_PACKET, "댓글은 100자 이하로 입력해주세요.");
        return false;
    }
    if (!GroupRepository::IsMember(groupId, senderId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }

    string senderName;
    UserRepository::GetUserNameWithId(senderId, senderName);

    string commentId = AlbumRepository::AddComment(photoId, senderId, senderName, text);
    if (commentId.empty()) {
        HandleErr(session, reqId, ERR_SERVER_INTERNAL, "댓글 추가 실패");
        return false;
    }

    PhotoCommentInfo info;
    info.id         = commentId;
    info.photoId    = photoId;
    info.senderId   = senderId;
    info.senderName = senderName;
    info.text       = text;
    info.createdAt  = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    Envelope resp;
    resp.set_request_id(static_cast<uint32>(reqId));
    auto* s = resp.mutable_s_add_photo_comment();
    s->set_success(true);
    FillCommentProto(s->mutable_comment(), info);

    BroadcastToGroup(groupId, resp, _userManager);
    return true;
}

bool AlbumService::HandleGetPhotoComments(sessionPtr& session, uint64 reqId, const C_GetPhotoComments& pkt)
{
    const string userId   = static_pointer_cast<ServerSession>(session)->GetUserId();
    const string& photoId = pkt.photo_id();

    string groupId = AlbumRepository::GetPhotoGroupId(photoId);
    if (!groupId.empty() && !GroupRepository::IsMember(groupId, userId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }

    auto comments = AlbumRepository::GetComments(photoId);

    Envelope resp;
    resp.set_request_id(static_cast<uint32>(reqId));
    auto* s = resp.mutable_s_get_photo_comments();
    s->set_photo_id(photoId);
    for (const auto& c : comments) {
        FillCommentProto(s->add_comments(), c);
    }
    PacketDispatcher::SendEnvelope(session, resp);
    return true;
}

bool AlbumService::HandleDeletePhotoComment(sessionPtr& session, uint64 reqId, const C_DeletePhotoComment& pkt)
{
    const string userId     = static_pointer_cast<ServerSession>(session)->GetUserId();
    const string& commentId = pkt.comment_id();
    const string& groupId   = pkt.group_id();
    const string& photoId   = pkt.photo_id();

    if (!GroupRepository::IsMember(groupId, userId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }

    bool ok = AlbumRepository::DeleteComment(commentId, userId);
    if (!ok) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "삭제 권한이 없거나 댓글이 존재하지 않습니다.");
        return false;
    }

    Envelope resp;
    resp.set_request_id(static_cast<uint32>(reqId));
    auto* s = resp.mutable_s_delete_photo_comment();
    s->set_success(true);
    s->set_comment_id(commentId);
    s->set_photo_id(photoId);
    s->set_group_id(groupId);

    BroadcastToGroup(groupId, resp, _userManager);
    return true;
}
