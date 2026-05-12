#include "pch.h"
#include "AlbumService.h"

#include "UserManager.h"
#include "../PacketDispatcher.h"
#include "../CoreGlobal.h"
#include "../DB/AlbumRepository.h"
#include "../DB/GroupRepository.h"

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

/*======================
        앨범 핸들러
=======================*/

bool AlbumService::HandleCreateAlbum(sessionPtr& session, uint64 reqId, const C_CreateAlbum& pkt)
{
    const string& groupId   = pkt.group_id();
    const string& name      = pkt.name();
    const string& createdBy = pkt.created_by();

    if (groupId.empty() || name.empty() || createdBy.empty()) {
        HandleErr(session, reqId, ERR_INVALID_PACKET);
        return false;
    }

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
    const string& groupId = pkt.group_id();

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
    const string& albumId = pkt.album_id();
    const string& userId  = pkt.user_id();
    const string& groupId = pkt.group_id();

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

/*======================
        사진 핸들러
=======================*/

bool AlbumService::HandleAddPhotoToAlbum(sessionPtr& session, uint64 reqId, const C_AddPhotoToAlbum& pkt)
{
    const string& albumId    = pkt.album_id();
    const string& groupId    = pkt.group_id();
    int64  msgSeq            = pkt.msg_seq();
    const string& photoUrl   = pkt.photo_url();
    const string& thumbUrl   = pkt.thumbnail_url();
    const string& uploaderId = pkt.uploader_id();

    if (albumId.empty() || photoUrl.empty() || uploaderId.empty()) {
        HandleErr(session, reqId, ERR_INVALID_PACKET);
        return false;
    }

    string photoId = AlbumRepository::AddPhoto(albumId, groupId, msgSeq, photoUrl, thumbUrl, uploaderId, "chat");
    if (photoId.empty()) {
        HandleErr(session, reqId, ERR_SERVER_INTERNAL, "사진 추가 실패");
        return false;
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
    const string& albumId = pkt.album_id();
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
    const string& photoId = pkt.photo_id();
    const string& userId  = pkt.user_id();
    const string& groupId = pkt.group_id();
    const string& albumId = pkt.album_id();

    bool ok = AlbumRepository::DeletePhoto(photoId, userId);
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
    const string& photoId     = pkt.photo_id();
    const string& fromAlbumId = pkt.from_album_id();
    const string& toAlbumId   = pkt.to_album_id();
    const string& groupId     = pkt.group_id();

    bool ok = AlbumRepository::MovePhoto(photoId, toAlbumId);
    if (!ok) {
        HandleErr(session, reqId, ERR_SERVER_INTERNAL, "사진 이동 실패");
        return false;
    }

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

/*======================
        댓글 핸들러
=======================*/

bool AlbumService::HandleAddPhotoComment(sessionPtr& session, uint64 reqId, const C_AddPhotoComment& pkt)
{
    const string& photoId    = pkt.photo_id();
    const string& senderId   = pkt.sender_id();
    const string& senderName = pkt.sender_name();
    const string& text       = pkt.text();
    const string& groupId    = pkt.group_id();

    if (photoId.empty() || senderId.empty() || text.empty()) {
        HandleErr(session, reqId, ERR_INVALID_PACKET);
        return false;
    }

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
    const string& photoId = pkt.photo_id();

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
    const string& commentId = pkt.comment_id();
    const string& userId    = pkt.user_id();
    const string& groupId   = pkt.group_id();
    const string& photoId   = pkt.photo_id();

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
