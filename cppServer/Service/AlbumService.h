#pragma once
#include "pch.h"
#include "Session.h"
#include "../protocol.pb.h"

using sessionPtr = shared_ptr<Session>;
class UserManager;

class AlbumService
{
public:
	explicit AlbumService(UserManager& usermanager) : _userManager(usermanager) { }
    ~AlbumService() = default;

    /* 앨범 */
    bool HandleCreateAlbum      (sessionPtr& session, uint64 reqId, const Protocol::C_CreateAlbum&       pkt);
    bool HandleGetAlbums        (sessionPtr& session, uint64 reqId, const Protocol::C_GetAlbums&         pkt);
    bool HandleDeleteAlbum      (sessionPtr& session, uint64 reqId, const Protocol::C_DeleteAlbum&       pkt);
    bool HandleEditAlbum        (sessionPtr& session, uint64 reqId, const Protocol::C_EditAlbum&         pkt);

    /* 사진 */
    bool HandleAddPhotoToAlbum  (sessionPtr& session, uint64 reqId, const Protocol::C_AddPhotoToAlbum&   pkt);
    bool HandleGetAlbumPhotos   (sessionPtr& session, uint64 reqId, const Protocol::C_GetAlbumPhotos&    pkt);
    bool HandleDeleteAlbumPhoto (sessionPtr& session, uint64 reqId, const Protocol::C_DeleteAlbumPhoto&  pkt);
    bool HandleMovePhoto        (sessionPtr& session, uint64 reqId, const Protocol::C_MovePhoto&          pkt);

    /* 댓글 */
    bool HandleAddPhotoComment   (sessionPtr& session, uint64 reqId, const Protocol::C_AddPhotoComment&   pkt);
    bool HandleGetPhotoComments  (sessionPtr& session, uint64 reqId, const Protocol::C_GetPhotoComments&  pkt);
    bool HandleDeletePhotoComment(sessionPtr& session, uint64 reqId, const Protocol::C_DeletePhotoComment& pkt);

private:
    UserManager& _userManager;
    void HandleErr(sessionPtr& session, uint64 reqId, Protocol::ErrorCode errorCode, const string& errMessage = "");
};
