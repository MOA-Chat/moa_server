#pragma once

#include "pch.h"
#include "../protocol.pb.h"
#include "DBManager.h"


struct cGroupInfo {
    string groupId;
    string groupName;
    string groupCode;
    string creatorId;
    int32 memberCount = 0;

    string description;
    string groupImageUrl;

    int64 storageUsage = 0;
    int64 storageLimit = 0;

    int64 createdAt = 0;
    int64 inviteCodeExpiresAt = 0;
};

struct cGroupMemberInfo {
    string userId;
    string name;
    string profileImageUrl;
    string statusMessage;
    string role;  // "owner", "admin", "member"

    int64 joinedAt;
};

class GroupRepository {
public:
    /* Helpers */
    static string GenerateUUID();
    static string GenerateGroupCode();

    /* Actions */
    static bool CreateGroup(const string& groupName, const string& creatorId, cGroupInfo& OUT groupInfo);
    static bool AddMember(const string& groupId, const string& userId, const string& role);
    static bool RemoveMember(const string& groupId, const string& userId);
    static bool SaveGroupAsset(const string& groupId, const string& userId, int64 msgSeq, int64 fileSize, const string& fileType);
    static bool DeleteGroupAssetByFilePath(const string& filePath);
    static bool UpdateGroupInfo(const string& groupId, const string& newName, const string& newImageUrl);
    static bool RefreshInviteCode(const string& groupId, string& OUT newCode, int64& OUT expiresAt);
    static bool DeleteGroup(const string& groupId);
    static bool TransferOwnership(const string& groupId, const string& fromUserId, const string& toUserId);

    /* Query */
    static vector<cGroupInfo> GetUserGroups(const string& userId);
    static bool GetGroupInfoById(const string& groupId, cGroupInfo& OUT info);
    static bool GetGroupInfoByCode(const string& groupCode, cGroupInfo& OUT info);

    static bool IsMember(const string& groupId, const string& userId);
    static string GetMemberRole(const string& groupId, const string& userId);
    static vector<cGroupMemberInfo> GetGroupMembers(const string& groupId);

    static void EnsureDefaultStorage(const string& groupId, int64 defaultCapacity);
    static bool UpdateStorageCapacity(const string& groupId, int64 capacityBytes);
    static bool SetStorageCapacityOnly(const string& groupId, int64 capacityBytes); // usage 건드리지 않음
    static bool ResetStorageToFree(const string& groupId);

    // 앨범 사진 저장/삭제 시 그룹 스토리지 직접 조정
    static bool AddGroupStorage(const string& groupId, int64 fileSize);
    static bool DeductGroupStorage(const string& groupId, int64 fileSize);
    // 쿼터 초과 시 false 반환 (원자적 체크+증가)
    static bool TryAddGroupStorageAtomic(const string& groupId, int64 addBytes);


private:
    static bool IsGroupCodeExists(const string& groupCode);
    static uint64 ParseTimestamp(const mysqlx::Value& value);
};

