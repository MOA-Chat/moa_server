#pragma once
#include "pch.h"
#include "DBManager.h"

struct cLocationPlace {
    string id;
    string groupId;
    string name;
    string address;
    string category;
    double lat        = 0.0;
    double lng        = 0.0;
    string createdBy;
    int64  createdAt  = 0;
    int32  visitCount = 0;
    float  avgRating  = 0.0f;
    string recentPhotoUrl;
};

struct cLocationVisit {
    string id;
    string placeId;
    string groupId;
    string userId;
    string userName;
    int32  rating    = 0;
    string review;
    int64  visitedAt = 0;
    string historyId;
    vector<pair<string, string>> photos; // {id, url}
};

class LocationRepository {
public:
    // 장소
    static string                 CreatePlace(const cLocationPlace& place); // 성공 시 placeId, 실패 시 ""
    static bool                   DeletePlace(const string& placeId, const string& userId);
    static bool                   UpdateVisit(const string& visitId, const string& userId,
                                              int32 rating, const string& review,
                                              int64 visitedAt, const vector<string>& photoUrls,
                                              const string& placeId, const string& historyId);
    static pair<bool,bool>        DeleteVisit(const string& visitId, const string& userId,
                                              const string& placeId); // {success, placeWasDeleted}
    static vector<cLocationPlace> GetPlacesByGroup(const string& groupId);
    static bool                   PlaceExists(const string& placeId);

    // 방문
    static string                 CreateVisit(const cLocationVisit& visit); // 성공 시 visitId, 실패 시 ""
    static vector<cLocationVisit> GetVisitsByPlace(const string& placeId);
    static bool                   UpdatePlaceStats(const string& placeId);

    // 사진
    static bool AddVisitPhotos(const string& visitId, const string& placeId,
                               const string& groupId,
                               const vector<string>& photoUrls);

    // 프로필 업데이트 동기화
    static void UpdateVisitUserNames(const string& userId, const string& userName);
};
