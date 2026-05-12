#pragma once
#include "pch.h"
#include "Session.h"
#include "../protocol.pb.h"

using sessionPtr = shared_ptr<Session>;
class UserManager;

class LocationService {
public:
    explicit LocationService(UserManager& usermanager) : _userManager(usermanager) { }
    ~LocationService() = default;

    bool HandleGetPlaces  (sessionPtr& session, uint64 reqId, const Protocol::C_GetPlaces&   pkt);
    bool HandleSavePlace  (sessionPtr& session, uint64 reqId, const Protocol::C_SavePlace&   pkt);
    bool HandleSaveVisit  (sessionPtr& session, uint64 reqId, const Protocol::C_SaveVisit&   pkt);
    bool HandleDeletePlace(sessionPtr& session, uint64 reqId, const Protocol::C_DeletePlace& pkt);
    bool HandleUpdateVisit(sessionPtr& session, uint64 reqId, const Protocol::C_UpdateVisit& pkt);
    bool HandleDeleteVisit(sessionPtr& session, uint64 reqId, const Protocol::C_DeleteVisit& pkt);

private:
    UserManager& _userManager;
    void HandleErr(sessionPtr& session, uint64 reqId, Protocol::ErrorCode code, const string& msg = "");
};
