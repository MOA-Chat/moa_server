#include "pch.h"
#include "ChatService.h"
#include "ServerSession.h"
#include "PacketDispatcher.h"
#include "UserManager.h"
#include "../DB/MessageRepository.h"
#include "../DB/UserRepository.h"
#include "../DB/GroupRepository.h"
#include "../DB/FcmTokenRepository.h"
#include "../DB/BlockRepository.h"
#include "../DB/PollRepository.h"
#include "../DB/FileRepository.h"
#include "../Cloud/CloudStorage.h"
#include "../Cloud/FcmClient.h"

#include "../CoreGlobal.h"
#include <random>
#include "json.hpp"

using namespace Protocol;





bool ChatService::SendGroupDM(sessionPtr& senderSession, uint64 reqId, const string& groupId, const string& receiverId, const Protocol::C_Chat& pkt)
{
    auto session = static_pointer_cast<ServerSession>(senderSession);
    const string senderId = session->GetUserId();
    string senderName = GetUserNameWithId(senderId);

    if (!GroupRepository::IsMember(groupId, senderId)) {
        PacketDispatcher::DispatchError(senderSession, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }
    if (senderId != receiverId && !GroupRepository::IsMember(groupId, receiverId)) {
        PacketDispatcher::DispatchError(senderSession, reqId, ERR_UNAUTHORIZED, "수신자가 그룹 멤버가 아닙니다.");
        return false;
    }

    // 차단 관계 확인
    if (BlockRepository::IsBlockedEither(senderId, receiverId)) {
        PacketDispatcher::DispatchError(senderSession, reqId, ERR_NOT_A_FRIEND, "차단된 사용자입니다.");
        return false;
    }

    const string convId = MessageRepository::CreateGroupDMConvId(groupId, senderId, receiverId);
    MessageRepository::CreateOrGetConversation(convId, "groupdm", senderId, receiverId);

    // 파일 경로 검증 (path traversal 방지)
    string filePath = pkt.gcs_path();
    if (!filePath.empty()) {
        if (filePath.find("..") != string::npos || filePath.find("//") != string::npos ||
            filePath.length() > 512) {
            filePath = "";
        }
    }

    int64 fileSize = 0;
    string fileType;
    int64 fileRetentionExpiresAt = 0;
    auto pkt_s_chat = Build_S_Chat(convId, senderId, fileSize, fileType, fileRetentionExpiresAt, pkt);
    string mentionJson;
    int mentionCount = min(pkt.mentioned_user_ids_size(), 50);
    if (mentionCount > 0) {
        mentionJson = "[";
        bool first = true;
        for (int i = 0; i < mentionCount; ++i) {
            const auto& uid = pkt.mentioned_user_ids(i);
            if (uid.length() > 100) continue;
            if (!first) mentionJson += ",";
            nlohmann::json uidJson = uid;
            mentionJson += uidJson.dump();
            first = false;
        }
        mentionJson += "]";
    }

    int64 msgSeq = MessageRepository::SaveMessage(convId, senderId, pkt_s_chat,
                                                   filePath, fileRetentionExpiresAt, mentionJson);

    // server_msg_id를 실제 msg_seq로 설정
    if (msgSeq >= 0) {
        pkt_s_chat.set_msg_seq(msgSeq);
        MessageRepository::UpdateReadStatus(senderId, convId, msgSeq);

        // UpdateReadStatus 이후: 발신자가 이미 읽은 상태 → 나머지 참여자 수 = 미읽음 수
        int unreadCount = MessageRepository::GetMsgUnreadCount(convId, msgSeq);
        pkt_s_chat.set_unread_count(unreadCount);

        if (fileSize > 0) {
            // GroupDM 파일은 그룹 스토리지 차감 (업로드 시 GROUP_CHAT 타입으로 체크됨)
            GroupRepository::SaveGroupAsset(groupId, senderId, msgSeq, fileSize, fileType);
        }
    }

    PushEnvelope(senderSession, reqId, pkt_s_chat); /* Ack to self */

    /* 나와의 채팅이면 Ack만 보내고 끝 (중복 전송 방지) */
    if (senderId == receiverId) {
        return true;
    }

    /* To receiver */
    if (msgSeq >= 0) {
        // 메시지 미리보기 (TCP/FCM 공용)
        string dmPreview = "[메시지]";
        if (pkt_s_chat.has_payload() && pkt_s_chat.payload().has_text()) {
            dmPreview = pkt_s_chat.payload().text().message();
            if (dmPreview.length() > 50) dmPreview = dmPreview.substr(0, 50) + "...";
        } else if (pkt_s_chat.has_payload() && pkt_s_chat.payload().has_image()) {
            dmPreview = "[사진]";
        } else if (pkt_s_chat.has_payload() && pkt_s_chat.payload().has_file()) {
            dmPreview = "[파일]";
        } else if (pkt_s_chat.has_payload() && pkt_s_chat.payload().has_audio()) {
            dmPreview = "[음성 메시지]";
        }
        map<string, string> dmData = {
            {"type", "groupdm"}, {"conv_id", convId},
            {"group_id", groupId}, {"sender_id", senderId}
        };

        if (auto target = _userManager.FindSession(receiverId)) {
            // 온라인 - 소켓으로 전송
            PushEnvelope(target, 0, pkt_s_chat);
            // 전 플랫폼 FCM 병행 발송 (iOS: 앱 서스펜드, Android: 배터리최적화 JS동결 대응)
            if (GFcmClient) {
                auto tokens = FcmTokenRepository::GetUserTokens(receiverId);
                for (const auto& tokenInfo : tokens) {
                    bool invalid = false;
                    GFcmClient->SendPush(tokenInfo.fcmToken, senderName, dmPreview, dmData, &invalid);
                    if (invalid) FcmTokenRepository::DeleteToken(receiverId, tokenInfo.fcmToken);
                }
            }
        }
        else if (GFcmClient) {
            // 오프라인 - FCM 푸시 발송
            LOG_INFO("[ChatService] SendGroupDM FCM: sender={}, receiver={}, group={}", senderId, receiverId, groupId);
            auto tokens = FcmTokenRepository::GetUserTokens(receiverId);
            LOG_INFO("[ChatService] Found {} FCM tokens for {}", tokens.size(), receiverId);
            for (const auto& tokenInfo : tokens) {
                LOG_INFO("[ChatService] Sending FCM to userId={}, token={}...", tokenInfo.userId, tokenInfo.fcmToken.substr(0, 20));
                bool invalidToken = false;
                bool ok = GFcmClient->SendPush(tokenInfo.fcmToken, senderName, dmPreview, dmData, &invalidToken);
                if (!ok && invalidToken) {
                    LOG_INFO("[ChatService] Deleting stale FCM token for {}", tokenInfo.userId);
                    FcmTokenRepository::DeleteToken(tokenInfo.userId, tokenInfo.fcmToken);
                }
            }
        }
    }
    return true;
}






bool ChatService::SendGroup(sessionPtr& senderSession, uint64 reqId, const string& groupId, const Protocol::C_Chat& pkt)
{
    auto session = static_pointer_cast<ServerSession>(senderSession);
    const string senderId = session->GetUserId();
    string senderName = GetUserNameWithId(senderId);

    if (!GroupRepository::IsMember(groupId, senderId)) {
        HandleErr(senderSession, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }

    const string convId = "group:" + groupId;
    MessageRepository::CreateOrGetConversation(convId, "group");

    // 파일 경로 검증 (path traversal 방지)
    string filePath = pkt.gcs_path();
    if (!filePath.empty()) {
        if (filePath.find("..") != string::npos || filePath.find("//") != string::npos ||
            filePath.length() > 512) {
            filePath = "";
        }
    }

    int64 fileSize = 0;
    string fileType;
    int64 fileRetentionExpiresAt = 0;
    auto pkt_s_chat = Build_S_Chat(convId, senderId, fileSize, fileType, fileRetentionExpiresAt, pkt);

    auto members = GroupRepository::GetGroupMembers(groupId);

    string mentionJsonGroup;
    int mentionCountGroup = min(pkt.mentioned_user_ids_size(), 50);
    if (mentionCountGroup > 0) {
        mentionJsonGroup = "[";
        bool first = true;
        for (int i = 0; i < mentionCountGroup; ++i) {
            const auto& uid = pkt.mentioned_user_ids(i);
            if (uid.length() > 100) continue;
            if (!first) mentionJsonGroup += ",";
            nlohmann::json uidJson = uid;
            mentionJsonGroup += uidJson.dump();
            first = false;
        }
        mentionJsonGroup += "]";
    }

    int64 msgSeq = MessageRepository::SaveMessage(convId, senderId, pkt_s_chat,
                                                   filePath, fileRetentionExpiresAt, mentionJsonGroup);
    if (msgSeq >= 0) {
        pkt_s_chat.set_msg_seq(msgSeq);

        MessageRepository::UpdateReadStatus(senderId, convId, msgSeq);

        // 그룹 미읽음 = 전체 멤버 수 - 1 (발신자)
        int unreadCount = (int)members.size() - 1;
        if (unreadCount < 0) unreadCount = 0;
        pkt_s_chat.set_unread_count(unreadCount);

        if (fileSize > 0) {
            GroupRepository::SaveGroupAsset(groupId, senderId, msgSeq, fileSize, fileType);
        }
    }

    PushEnvelope(senderSession, reqId, pkt_s_chat); /* Ack to self */

    // 그룹 이름 가져오기
    cGroupInfo groupInfo;
    GroupRepository::GetGroupInfoById(groupId, groupInfo);
    string groupName = groupInfo.groupName.empty() ? "그룹" : groupInfo.groupName;

    // 메시지 미리보기 준비
    string msgPreview = "[메시지]";
    if (pkt_s_chat.has_payload() && pkt_s_chat.payload().has_text()) {
        msgPreview = pkt_s_chat.payload().text().message();
        if (msgPreview.length() > 50) {
            msgPreview = msgPreview.substr(0, 50) + "...";
        }
    }
    else if (pkt_s_chat.has_payload() && pkt_s_chat.payload().has_image()) {
        msgPreview = "[사진]";
    }
    else if (pkt_s_chat.has_payload() && pkt_s_chat.payload().has_file()) {
        msgPreview = "[파일]";
    }
    else if (pkt_s_chat.has_payload() && pkt_s_chat.payload().has_audio()) {
        msgPreview = "[음성 메시지]";
    }

    string fcmTitle = groupName + " - " + senderName;
    map<string, string> fcmData = {
        {"type", "chat"}, {"conv_id", convId},
        {"sender_id", senderId}, {"group_id", groupId}
    };

    for (const auto& member : members) {
        if (member.userId == senderId) continue;

        if (auto target = _userManager.FindSession(member.userId)) {
            // 온라인 - 소켓으로 전송
            PushEnvelope(target, 0, pkt_s_chat);
            // 전 플랫폼 FCM 병행 발송 (iOS: 앱 서스펜드, Android: 배터리최적화 JS동결 대응)
            if (GFcmClient) {
                auto tokens = FcmTokenRepository::GetUserTokens(member.userId);
                for (const auto& tokenInfo : tokens) {
                    bool invalid = false;
                    GFcmClient->SendPush(tokenInfo.fcmToken, fcmTitle, msgPreview, fcmData, &invalid);
                    if (invalid) FcmTokenRepository::DeleteToken(member.userId, tokenInfo.fcmToken);
                }
            }
        }
        else if (GFcmClient) {
            // 오프라인 - FCM 푸시 발송 (전 플랫폼)
            auto tokens = FcmTokenRepository::GetUserTokens(member.userId);
            if (!tokens.empty()) {
                for (const auto& tokenInfo : tokens) {
                    bool invalidToken = false;
                    bool ok = GFcmClient->SendPush(tokenInfo.fcmToken, fcmTitle, msgPreview, fcmData, &invalidToken);
                    if (!ok && invalidToken) {
                        LOG_INFO("[ChatService] Deleting stale FCM token for {}", tokenInfo.userId);
                        FcmTokenRepository::DeleteToken(tokenInfo.userId, tokenInfo.fcmToken);
                    }
                }
            }
        }
    }

    return true;
}





bool ChatService::SendSystemMessage(const string& groupId, const string& message)
{
    auto members = GroupRepository::GetGroupMembers(groupId);
    if (members.empty()) return false;

    Protocol::S_Chat sendPkt;
    sendPkt.set_conv_id("group:" + groupId);
    sendPkt.set_sender_id("SYSTEM");
    sendPkt.set_sender_name("System"); // 클라이언트가 이름으로 쓸 것
    sendPkt.set_ts_server(Nowts());

    auto* sysPayload = sendPkt.mutable_payload()->mutable_system();
    sysPayload->set_message(message);
    sysPayload->set_type(0);

    int64 msgSeq = MessageRepository::SaveMessage("group:" + groupId, "SYSTEM", sendPkt);
    if (msgSeq > 0) sendPkt.set_msg_seq(msgSeq);

    Protocol::Envelope env;
    env.set_version(GProtoVersion);
    env.set_request_id(0);
    *env.mutable_s_chat() = sendPkt;

    for (const auto& member : members) {
        if (auto target = _userManager.FindSession(member.userId)) {
            PushEnvelope(target, 0, sendPkt);
        }
    }

    LOG_INFO("[ChatService] System Msg to Group({}): {}", groupId, message);
    return true;
}





bool ChatService::HandleAck(sessionPtr& session, uint64 reqId, PacketDispatcher::ConvType type, const string& targetId, int64 msg_seq)
{
    auto serverSession = static_pointer_cast<ServerSession>(session);
    const string userId = serverSession->GetUserId();
    if (userId.empty()) return false;

    string dbConvId;

    if (type == PacketDispatcher::ConvType::GroupDM) {
        // targetId = "groupId:receiverId"
        auto sep = targetId.find(':');
        if (sep == string::npos) return false;
        string groupId    = targetId.substr(0, sep);
        string receiverId = targetId.substr(sep + 1);
        dbConvId = MessageRepository::CreateGroupDMConvId(groupId, userId, receiverId);
    }
    else {
        dbConvId = "group:" + targetId;
    }



    bool success = MessageRepository::UpdateReadStatus(userId, dbConvId, msg_seq);
    if (!success) {
        LOG_ERROR("[ChatService] DB Update 실패");
        return false;
    }
    /* TODO 상대가 읽음 처리 */

    LOG_INFO("[ChatService] Ack 처리: User={} ReadUpTo={}", userId, msg_seq);

    return true;
}







bool ChatService::HandleReqHistory(sessionPtr& session, uint64 reqId, const Protocol::C_ReqHistory& pkt)
{
    auto serverSession = static_pointer_cast<ServerSession>(session);
    const string userId = serverSession->GetUserId();
    if (userId.empty()) { HandleErr(session, reqId, ERR_UNAUTHORIZED); return false; }

    string convId = pkt.conv_id();
    if (convId.empty()) { HandleErr(session, reqId, ERR_INVALID_PACKET, "conv_id 필요"); return false; }

    // 멤버십 확인
    auto dmPos = convId.find(":dm:");
    if (dmPos != string::npos) {
        string groupId  = convId.substr(0, dmPos);
        string pairPart = convId.substr(dmPos + 4);
        auto underPos   = pairPart.find('_');
        if (underPos != string::npos) {
            // 정규화된 형식 (userA_userB) — 본인이 포함된 DM인지 확인
            string userA = pairPart.substr(0, underPos);
            string userB = pairPart.substr(underPos + 1);
            if (userId != userA && userId != userB) {
                HandleErr(session, reqId, ERR_UNAUTHORIZED, "접근 권한이 없습니다.");
                return false;
            }
        } else {
            // 미정규화 형식 — 그룹 멤버십으로 대체 검증
            if (!GroupRepository::IsMember(groupId, userId)) {
                HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
                return false;
            }
        }
    } else if (convId.find("group:") == 0) {
        string groupId = convId.substr(6);
        if (!GroupRepository::IsMember(groupId, userId)) {
            HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
            return false;
        }
    } else {
        HandleErr(session, reqId, ERR_INVALID_PACKET, "잘못된 conv_id");
        return false;
    }

    int64 lastSeq = pkt.last_msg_seq();
    int limit = pkt.limit();

    if (limit <= 0) limit = 30;
    if (limit > 100) limit = 100;

    auto history = MessageRepository::GetHistoryMessages(convId, lastSeq, limit);

    Protocol::S_ReqHistory res;
    res.set_conv_id(convId);
    res.set_is_end(history.size() < limit);

    for (const auto& msgInfo : history) {
        Protocol::S_Chat sChat;

        if (sChat.ParseFromString(msgInfo.messageData)) {
            // 시퀀스 번호 재확인
            sChat.set_msg_seq(msgInfo.msgSeq);
            *res.add_messages() = sChat;
        }
    }

    Protocol::Envelope env;
    env.set_version(GProtoVersion);
    env.set_request_id(reqId); 

    env.mutable_s_req_history()->CopyFrom(res);
    PacketDispatcher::SendEnvelope(session, env);

    return true;
}




Protocol::S_Chat ChatService::Build_S_Chat(const string& convId, const string& senderId,
                                           int64& OUT fileSize, string& OUT fileType,
                                           int64& OUT fileRetentionExpiresAt,
                                           const Protocol::C_Chat& pkt)
{
    string senderName = GetUserNameWithId(senderId);

    Protocol::S_Chat pkt_s_chat;
    pkt_s_chat.set_conv_id(convId);
    pkt_s_chat.set_client_msg_id(pkt.client_msg_id());
    pkt_s_chat.set_sender_id(senderId);
    pkt_s_chat.set_sender_name(senderName);
    pkt_s_chat.set_ts_server(Nowts());

    // 답장 정보 복사
    if (pkt.reply_to_seq() > 0) {
        pkt_s_chat.set_reply_to_seq(pkt.reply_to_seq());
        auto replyInfo = MessageRepository::GetMessageBySeq(convId, pkt.reply_to_seq());
        if (replyInfo.found) {
            pkt_s_chat.set_reply_to_sender_name(replyInfo.senderName);
            pkt_s_chat.set_reply_to_text(replyInfo.text);
        }
    }

    // 멘션 정보 복사
    for (const auto& uid : pkt.mentioned_user_ids()) {
        pkt_s_chat.add_mentioned_user_ids(uid);
    }

    if (pkt.has_payload()) {
        auto* payload = pkt_s_chat.mutable_payload();
        if (pkt.payload().has_text()) {
            payload->mutable_text()->set_message(pkt.payload().text().message());
        }
        else if (pkt.payload().has_image()) {
            payload->mutable_image()->set_url(pkt.payload().image().url());
            payload->mutable_image()->set_thumbnail(pkt.payload().image().thumbnail());
            payload->mutable_image()->set_size(pkt.payload().image().size());
            fileSize = pkt.payload().image().size();
            fileType = "image";
        }
        else if (pkt.payload().has_video()) {
            payload->mutable_video()->set_url(pkt.payload().video().url());
            payload->mutable_video()->set_thumbnail(pkt.payload().video().thumbnail());
            payload->mutable_video()->set_size(pkt.payload().video().size());
            fileSize = pkt.payload().video().size();
            fileType = "video";
        }
        else if (pkt.payload().has_file()) {
            payload->mutable_file()->set_url(pkt.payload().file().url());
            payload->mutable_file()->set_filename(pkt.payload().file().filename());
            payload->mutable_file()->set_size(pkt.payload().file().size());
            fileSize = pkt.payload().file().size();
            fileType = "file";
        }
        else if (pkt.payload().has_audio()) {
            payload->mutable_audio()->set_url(pkt.payload().audio().url());
            payload->mutable_audio()->set_duration_sec(pkt.payload().audio().duration_sec());
            payload->mutable_audio()->set_size(pkt.payload().audio().size());
            fileSize = pkt.payload().audio().size();
            fileType = "audio";
        }
    }

    // 파일 보관 만료: subGrade 기반 재계산 + file_uploads 상태 completed 업데이트
    if (!pkt.gcs_path().empty() && fileSize > 0) {
        UserRepository::StorageInfo storInfo;
        if (UserRepository::GetStorageInfo(senderId, storInfo)) {
            if (storInfo.subGrade <= 0) {
                const int64 FREE_RETENTION_MS = 7LL * 24 * 3600 * 1000;
                fileRetentionExpiresAt = pkt_s_chat.ts_server() + FREE_RETENTION_MS;
            }
        }
        pkt_s_chat.set_file_expires_at(fileRetentionExpiresAt);
        pkt_s_chat.set_file_status("active");

        FileRepository::MarkCompletedByFilePath(pkt.gcs_path());
    }

    return pkt_s_chat;
}

void ChatService::PushEnvelope(sessionPtr& session, uint64 reqId, const Protocol::S_Chat& pkt_s_chat)
{
    Protocol::Envelope env;
    env.set_version(GProtoVersion);
    env.set_request_id(reqId);
    env.mutable_s_chat()->CopyFrom(pkt_s_chat);

    PacketDispatcher::SendEnvelope(session, env);
}



string ChatService::GetUserNameWithId(const string& userId)
{
    string name;
    if (UserRepository::GetUserNameWithId(userId, name)) {
        return name;
    }
    return userId;
}


bool ChatService::HandleDeleteMessage(sessionPtr& session, uint64 reqId, const Protocol::C_DeleteMessage& pkt)
{
    auto serverSession = static_pointer_cast<ServerSession>(session);
    const string senderId = serverSession->GetUserId();
    if (senderId.empty()) return false;

    const string clientConvId = pkt.conv_id();
    int64 msgSeq = pkt.msg_seq();

    if (clientConvId.empty() || msgSeq <= 0) {
        HandleErr(session, reqId, ERR_INVALID_PACKET, "잘못된 삭제 요청입니다.");
        return false;
    }

    // groupId:dm:targetId → groupId:dm:userA_userB (정렬)
    string convId = clientConvId;
    auto dmPos_del = clientConvId.find(":dm:");
    if (dmPos_del != string::npos) {
        string groupId  = clientConvId.substr(0, dmPos_del);
        string targetId = clientConvId.substr(dmPos_del + 4);
        if (!GroupRepository::IsMember(groupId, senderId)) {
            HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
            return false;
        }
        convId = MessageRepository::CreateGroupDMConvId(groupId, senderId, targetId);
    } else if (clientConvId.find("group:") == 0) {
        string groupId = clientConvId.substr(6);
        if (!GroupRepository::IsMember(groupId, senderId)) {
            HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
            return false;
        }
    }

    bool success = MessageRepository::SoftDeleteMessage(convId, msgSeq, senderId);
    if (!success) {
        HandleErr(session, reqId, ERR_INVALID_PACKET, "메시지 삭제에 실패했습니다.");
        return false;
    }

    // 발신자에게 응답
    Protocol::S_DeleteMessage pkt_res;
    pkt_res.set_success(success);
    pkt_res.set_conv_id(convId);
    pkt_res.set_msg_seq(msgSeq);

    Protocol::Envelope envSender;
    envSender.set_version(GProtoVersion);
    envSender.set_request_id(reqId);
    *envSender.mutable_s_delete_message() = pkt_res;
    PacketDispatcher::SendEnvelope(session, envSender);

    if (!success) return false;

    // 상대방/그룹 멤버에게 브로드캐스트
    Protocol::Envelope envPush;
    envPush.set_version(GProtoVersion);
    envPush.set_request_id(0);
    *envPush.mutable_s_delete_message() = pkt_res;

    auto dmPos_delB = convId.find(":dm:");
    if (dmPos_delB != string::npos) {
        // groupId:dm:userA_userB — 상대방에게 전송
        string pair = convId.substr(dmPos_delB + 4);
        auto underPos = pair.find('_');
        if (underPos != string::npos) {
            string userA = pair.substr(0, underPos);
            string userB = pair.substr(underPos + 1);
            string receiverId = (userA == senderId) ? userB : userA;
            if (auto target = _userManager.FindSession(receiverId)) {
                PacketDispatcher::SendEnvelope(target, envPush);
            }
        }
    }
    else if (convId.find("group:") == 0) {
        string groupId = convId.substr(6);
        auto members = GroupRepository::GetGroupMembers(groupId);
        for (const auto& m : members) {
            if (m.userId == senderId) continue;
            if (auto target = _userManager.FindSession(m.userId)) {
                PacketDispatcher::SendEnvelope(target, envPush);
            }
        }
    }

    LOG_INFO("[ChatService] DeleteMessage: conv={} seq={}", convId, msgSeq);
    return true;
}


bool ChatService::HandleEditMessage(sessionPtr& session, uint64 reqId, const Protocol::C_EditMessage& pkt)
{
    auto serverSession = static_pointer_cast<ServerSession>(session);
    const string senderId = serverSession->GetUserId();
    if (senderId.empty()) return false;

    const string clientConvId = pkt.conv_id();
    int64 msgSeq = pkt.msg_seq();
    const string newText = pkt.new_text();

    if (clientConvId.empty() || msgSeq <= 0 || newText.empty()) {
        HandleErr(session, reqId, ERR_INVALID_PACKET, "잘못된 수정 요청입니다.");
        return false;
    }

    string convId = clientConvId;
    auto dmPos_edit = clientConvId.find(":dm:");
    if (dmPos_edit != string::npos) {
        string groupId  = clientConvId.substr(0, dmPos_edit);
        string targetId = clientConvId.substr(dmPos_edit + 4);
        if (!GroupRepository::IsMember(groupId, senderId)) {
            HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
            return false;
        }
        convId = MessageRepository::CreateGroupDMConvId(groupId, senderId, targetId);
    } else if (clientConvId.find("group:") == 0) {
        string groupId = clientConvId.substr(6);
        if (!GroupRepository::IsMember(groupId, senderId)) {
            HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
            return false;
        }
    }

    int64 editedAt = Nowts();

    bool success = MessageRepository::EditMessage(convId, msgSeq, senderId, newText, editedAt);
    if (!success) {
        HandleErr(session, reqId, ERR_INVALID_PACKET, "메시지 수정에 실패했습니다.");
        return false;
    }

    // 발신자에게 응답
    Protocol::S_EditMessage pkt_res;
    pkt_res.set_success(success);
    pkt_res.set_conv_id(convId);
    pkt_res.set_msg_seq(msgSeq);
    pkt_res.set_new_text(newText);
    pkt_res.set_edited_at(editedAt);

    Protocol::Envelope envSender;
    envSender.set_version(GProtoVersion);
    envSender.set_request_id(reqId);
    *envSender.mutable_s_edit_message() = pkt_res;
    PacketDispatcher::SendEnvelope(session, envSender);

    if (!success) return false;

    // 상대방/그룹 멤버에게 브로드캐스트
    Protocol::Envelope envPush;
    envPush.set_version(GProtoVersion);
    envPush.set_request_id(0);
    *envPush.mutable_s_edit_message() = pkt_res;

    auto dmPos_editB = convId.find(":dm:");
    if (dmPos_editB != string::npos) {
        string pair = convId.substr(dmPos_editB + 4);
        auto underPos = pair.find('_');
        if (underPos != string::npos) {
            string userA = pair.substr(0, underPos);
            string userB = pair.substr(underPos + 1);
            string receiverId = (userA == senderId) ? userB : userA;
            if (auto target = _userManager.FindSession(receiverId)) {
                PacketDispatcher::SendEnvelope(target, envPush);
            }
        }
    }
    else if (convId.find("group:") == 0) {
        string groupId = convId.substr(6);
        auto members = GroupRepository::GetGroupMembers(groupId);
        for (const auto& m : members) {
            if (m.userId == senderId) continue;
            if (auto target = _userManager.FindSession(m.userId)) {
                PacketDispatcher::SendEnvelope(target, envPush);
            }
        }
    }

    LOG_INFO("[ChatService] EditMessage: conv={} seq={} newText=\"{}\"", convId, msgSeq, newText);
    return true;
}


bool ChatService::HandleReadReceipt(sessionPtr& session, uint64 reqId, const Protocol::C_ReadReceipt& pkt)
{
    auto serverSession = static_pointer_cast<ServerSession>(session);
    const string userId = serverSession->GetUserId();
    if (userId.empty()) return false;

    string convId = pkt.conv_id();
    int64 lastReadSeq = pkt.last_read_seq();
    if (convId.empty() || lastReadSeq <= 0) return false;

    // groupId:dm:targetId → groupId:dm:userA_userB (DB 정렬 형식)
    auto dmPos_rr0 = convId.find(":dm:");
    if (dmPos_rr0 != string::npos) {
        string groupId  = convId.substr(0, dmPos_rr0);
        string targetId = convId.substr(dmPos_rr0 + 4);
        if (targetId.find('_') == string::npos) {
            convId = MessageRepository::CreateGroupDMConvId(groupId, userId, targetId);
        }
    }

    // 업데이트 전 기존 last_read_seq 조회 (unread 범위 계산용)
    int64 prevReadSeq = MessageRepository::GetPrevReadSeq(userId, convId);

    // DB 업데이트
    bool ok = MessageRepository::UpdateReadStatus(userId, convId, lastReadSeq);
    if (!ok) return false;

    LOG_INFO("[ChatService] ReadReceipt: user={} conv={} seq={} prevSeq={}", userId, convId, lastReadSeq, prevReadSeq);

    // 범위 내 각 메시지의 정확한 unread_count 계산
    auto unreadUpdates = MessageRepository::GetBatchMsgUnreadCounts(convId, prevReadSeq, lastReadSeq);
    for (const auto& [seq, unread] : unreadUpdates) {
        LOG_INFO("[ChatService] ReadReceipt unread: seq={} unread={}", seq, unread);
    }

    // S_ReadReceipt 구성 (unread_updates 포함)
    Protocol::S_ReadReceipt sReadReceipt;
    sReadReceipt.set_conv_id(convId);
    sReadReceipt.set_reader_id(userId);
    sReadReceipt.set_last_read_seq(lastReadSeq);
    for (const auto& [seq, unread] : unreadUpdates) {
        auto* update = sReadReceipt.add_unread_updates();
        update->set_msg_seq(seq);
        update->set_unread_count(unread);
    }

    Protocol::Envelope env;
    env.set_version(GProtoVersion);
    env.set_request_id(0); // 서버 푸시
    env.mutable_s_read_receipt()->CopyFrom(sReadReceipt);

    auto dmPos_rr = convId.find(":dm:");
    if (dmPos_rr != string::npos) {
        // groupId:dm:userA_userB — 본인 + 상대방에게 전송
        string pair = convId.substr(dmPos_rr + 4);
        auto underPos = pair.find('_');
        if (underPos != string::npos) {
            string userA = pair.substr(0, underPos);
            string userB = pair.substr(underPos + 1);
            string targetId = (userA == userId) ? userB : userA;
            // 본인에게도 전송 (UI 갱신용)
            if (auto mySession = _userManager.FindSession(userId)) {
                PacketDispatcher::SendEnvelope(mySession, env);
            }
            if (auto targetSession = _userManager.FindSession(targetId)) {
                PacketDispatcher::SendEnvelope(targetSession, env);
            }
        }
    }
    else if (convId.find("group:") == 0) {
        // 그룹 채팅: 본인 포함 모든 온라인 멤버에게 전송
        string groupId = convId.substr(6);
        auto members = GroupRepository::GetGroupMembers(groupId);
        for (const auto& member : members) {
            auto memberSession = _userManager.FindSession(member.userId);
            if (memberSession) {
                PacketDispatcher::SendEnvelope(memberSession, env);
            }
        }
    }

    return true;
}


bool ChatService::HandleAddReaction(sessionPtr& session, uint64 reqId, const Protocol::C_AddReaction& pkt)
{
    auto serverSession = static_pointer_cast<ServerSession>(session);
    const string userId = serverSession->GetUserId();
    if (userId.empty()) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED);
        return false;
    }

    const string clientConvId = pkt.conv_id();
    int64 msgSeq = pkt.msg_seq();
    const string& emoji = pkt.emoji();

    string convId = clientConvId;
    string dmTargetId;
    string dmGroupId;
    auto dmPos_react = clientConvId.find(":dm:");
    if (dmPos_react != string::npos) {
        dmGroupId  = clientConvId.substr(0, dmPos_react);
        dmTargetId = clientConvId.substr(dmPos_react + 4);
        if (!GroupRepository::IsMember(dmGroupId, userId)) {
            HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
            return false;
        }
        convId = MessageRepository::CreateGroupDMConvId(dmGroupId, userId, dmTargetId);
    } else if (clientConvId.find("group:") == 0) {
        string groupId = clientConvId.substr(6);
        if (!GroupRepository::IsMember(groupId, userId)) {
            HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
            return false;
        }
    }

    bool outRemoved = false;
    if (!MessageRepository::ToggleReaction(convId, msgSeq, userId, emoji, outRemoved)) {
        HandleErr(session, reqId, ERR_SERVER_INTERNAL, "반응 처리 실패");
        return false;
    }

    // S_AddReaction 구성 (convId는 클라이언트 원본 형식으로 전달)
    Protocol::S_AddReaction pkt_res;
    pkt_res.set_conv_id(clientConvId);
    pkt_res.set_msg_seq(msgSeq);
    pkt_res.set_user_id(userId);
    pkt_res.set_emoji(emoji);
    pkt_res.set_removed(outRemoved);

    // 브로드캐스트
    if (clientConvId.find("group:") == 0) {
        // 그룹 채팅
        string groupId = clientConvId.substr(6); // "group:" 제거
        auto members = GroupRepository::GetGroupMembers(groupId);
        for (const auto& member : members) {
            if (auto memberSession = _userManager.FindSession(member.userId)) {
                Protocol::Envelope env;
                env.set_version(GProtoVersion);
                env.set_request_id(member.userId == userId ? reqId : 0);
                *env.mutable_s_add_reaction() = pkt_res;
                PacketDispatcher::SendEnvelope(memberSession, env);
            }
        }
    } else if (!dmTargetId.empty()) {
        // GroupDM: 발신자와 수신자 각각에게 전송
        for (const auto& pid : {userId, dmTargetId}) {
            if (pid.empty()) continue;
            if (auto peerSession = _userManager.FindSession(pid)) {
                Protocol::Envelope env;
                env.set_version(GProtoVersion);
                env.set_request_id(pid == userId ? reqId : 0);
                *env.mutable_s_add_reaction() = pkt_res;
                PacketDispatcher::SendEnvelope(peerSession, env);
            }
        }
    }

    return true;
}


bool ChatService::HandleCreatePoll(sessionPtr& session, uint64 reqId, const Protocol::C_CreatePoll& pkt)
{
    auto serverSession = static_pointer_cast<ServerSession>(session);
    const string userId = serverSession->GetUserId();
    if (userId.empty()) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED);
        return false;
    }

    const string& clientConvId = pkt.conv_id();
    if (clientConvId.find("group:") != 0) {
        HandleErr(session, reqId, ERR_INVALID_CONV_ID, "투표는 그룹에서만 가능");
        return false;
    }

    string groupId = clientConvId.substr(6);
    string convId = clientConvId;

    if (!GroupRepository::IsMember(groupId, userId)) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
        return false;
    }

    int64 now = Nowts();
    string pollId = GroupRepository::GenerateUUID();

    // options -> JSON 배열
    std::ostringstream optionsOss;
    optionsOss << "[";
    for (int i = 0; i < pkt.options_size(); i++) {
        if (i > 0) optionsOss << ",";
        optionsOss << "\"";
        // Escape quotes in option text
        for (char c : pkt.options(i)) {
            if (c == '"') optionsOss << "\\\"";
            else if (c == '\\') optionsOss << "\\\\";
            else optionsOss << c;
        }
        optionsOss << "\"";
    }
    optionsOss << "]";
    string optionsJson = optionsOss.str();

    // msg_type=5 (POLL) 메시지로 저장
    int64 msgSeq = MessageRepository::GetNextMessageSeq(convId);
    if (msgSeq < 0) {
        HandleErr(session, reqId, ERR_SERVER_INTERNAL, "msgSeq 생성 실패");
        return false;
    }

    // polls 테이블에 저장
    if (!PollRepository::CreatePoll(pollId, convId, msgSeq, userId,
                                     pkt.question(), optionsJson,
                                     pkt.is_multi_select(), pkt.is_anonymous(),
                                     pkt.expires_at(), now)) {
        HandleErr(session, reqId, ERR_SERVER_INTERNAL, "투표 생성 실패");
        return false;
    }

    // poll JSON을 message로 저장 (msg_type=5)
    Protocol::S_Chat sChat;
    sChat.set_conv_id(convId);
    sChat.set_sender_id(userId);
    sChat.set_sender_name(GetUserNameWithId(userId));
    sChat.set_ts_server(now);
    sChat.set_msg_seq(msgSeq);

    // poll 정보를 message payload에 JSON으로 저장
    std::ostringstream pollJsonOss;
    pollJsonOss << "{\"pollId\":\"" << pollId << "\""
                << ",\"question\":\"";
    for (char c : pkt.question()) {
        if (c == '"') pollJsonOss << "\\\"";
        else if (c == '\\') pollJsonOss << "\\\\";
        else pollJsonOss << c;
    }
    pollJsonOss << "\",\"options\":" << optionsJson
                << ",\"isMultiSelect\":" << (pkt.is_multi_select() ? "true" : "false")
                << ",\"isAnonymous\":" << (pkt.is_anonymous() ? "true" : "false")
                << ",\"expiresAt\":" << pkt.expires_at()
                << ",\"creatorId\":\"" << userId << "\""
                << ",\"status\":\"active\""
                << ",\"votes\":{}}";
    string pollJson = pollJsonOss.str();
    sChat.mutable_payload()->mutable_text()->set_message(pollJson);

    MessageRepository::SaveMessage(convId, userId, sChat, "", 0, "", 5 /* POLL */);

    // S_CreatePoll 브로드캐스트
    Protocol::S_CreatePoll pkt_res;
    pkt_res.set_conv_id(clientConvId);
    pkt_res.set_poll_id(pollId);
    pkt_res.set_msg_seq(msgSeq);
    pkt_res.set_creator_id(userId);
    pkt_res.set_creator_name(GetUserNameWithId(userId));
    pkt_res.set_question(pkt.question());
    for (int i = 0; i < pkt.options_size(); i++) {
        pkt_res.add_options(pkt.options(i));
    }
    pkt_res.set_is_multi_select(pkt.is_multi_select());
    pkt_res.set_is_anonymous(pkt.is_anonymous());
    pkt_res.set_expires_at(pkt.expires_at());
    pkt_res.set_ts_server(now);

    cGroupInfo groupInfoFcm;
    GroupRepository::GetGroupInfoById(groupId, groupInfoFcm);
    string groupNameFcm = groupInfoFcm.groupName.empty() ? "그룹" : groupInfoFcm.groupName;

    auto members = GroupRepository::GetGroupMembers(groupId);
    for (const auto& member : members) {
        if (auto memberSession = _userManager.FindSession(member.userId)) {
            Protocol::Envelope env;
            env.set_version(GProtoVersion);
            env.set_request_id(member.userId == userId ? reqId : 0);
            *env.mutable_s_create_poll() = pkt_res;
            PacketDispatcher::SendEnvelope(memberSession, env);
        } else if (member.userId != userId && GFcmClient) {
            auto tokens = FcmTokenRepository::GetUserTokens(member.userId);
            if (!tokens.empty()) {
                string title = groupNameFcm + " - " + pkt_res.creator_name();
                string body = "투표: " + pkt.question();
                if (body.length() > 60) body = body.substr(0, 60) + "...";
                map<string, string> data = {
                    {"type", "poll_created"}, {"conv_id", clientConvId},
                    {"group_id", groupId}, {"poll_id", pollId}, {"sender_id", userId}
                };
                for (const auto& tokenInfo : tokens) {
                    bool invalidToken = false;
                    GFcmClient->SendPush(tokenInfo.fcmToken, title, body, data, &invalidToken);
                    if (invalidToken) FcmTokenRepository::DeleteToken(tokenInfo.userId, tokenInfo.fcmToken);
                }
            }
        }
    }

    LOG_INFO("[ChatService] CreatePoll: {} in {}", pollId, convId);
    return true;
}

bool ChatService::HandleVote(sessionPtr& session, uint64 reqId, const Protocol::C_Vote& pkt)
{
    auto serverSession = static_pointer_cast<ServerSession>(session);
    const string userId = serverSession->GetUserId();
    if (userId.empty()) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED);
        return false;
    }

    const string& pollId = pkt.poll_id();

    // 그룹 멤버십 확인
    {
        string clientConvId = pkt.conv_id();
        if (clientConvId.find("group:") == 0) {
            string groupId = clientConvId.substr(6);
            if (!GroupRepository::IsMember(groupId, userId)) {
                HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
                return false;
            }
        }
    }

    // 투표 존재 + 상태 확인
    PollInfo pollInfo = PollRepository::GetPollInfo(pollId);
    if (pollInfo.pollId.empty()) {
        HandleErr(session, reqId, ERR_POLL_NOT_FOUND, "투표를 찾을 수 없음");
        return false;
    }
    if (pollInfo.status != "active") {
        HandleErr(session, reqId, ERR_POLL_CLOSED, "이미 종료된 투표");
        return false;
    }

    // 자동 만료 체크
    if (pollInfo.expiresAt > 0 && Nowts() > pollInfo.expiresAt) {
        if (PollRepository::ClosePoll(pollId, pollInfo.creatorId)) {
            // 최초 만료 종료 시 한 번만 알림 메시지 전송
            SendPollClosedNotification(pkt.conv_id(), pollId, pollInfo.creatorId, 0);
        }
        HandleErr(session, reqId, ERR_POLL_CLOSED, "만료된 투표");
        return false;
    }

    // 선택지 유효성 검사
    vector<int> selectedOptions;
    for (int i = 0; i < pkt.selected_options_size(); i++) {
        selectedOptions.push_back(pkt.selected_options(i));
    }

    // 투표 저장
    if (!PollRepository::Vote(pollId, userId, selectedOptions)) {
        HandleErr(session, reqId, ERR_SERVER_INTERNAL, "투표 처리 실패");
        return false;
    }

    // 최신 투표 현황
    string votesJson = PollRepository::GetPollVotes(pollId);

    // messages 테이블의 poll JSON 업데이트 (오프라인 B가 history fetch 시 최신 상태 수신)
    MessageRepository::UpdatePollVotesInMessage(pkt.conv_id(), pkt.msg_seq(), votesJson);

    // S_Vote 브로드캐스트
    Protocol::S_Vote pkt_res;
    pkt_res.set_conv_id(pkt.conv_id());
    pkt_res.set_poll_id(pollId);
    pkt_res.set_msg_seq(pkt.msg_seq());
    pkt_res.set_voter_id(userId);
    pkt_res.set_voter_name(GetUserNameWithId(userId));
    for (int opt : selectedOptions) {
        pkt_res.add_selected_options(opt);
    }
    pkt_res.set_votes_json(votesJson);

    string clientConvId = pkt.conv_id();
    if (clientConvId.find("group:") == 0) {
        string groupId = clientConvId.substr(6);
        auto members = GroupRepository::GetGroupMembers(groupId);
        for (const auto& member : members) {
            if (auto memberSession = _userManager.FindSession(member.userId)) {
                Protocol::Envelope env;
                env.set_version(GProtoVersion);
                env.set_request_id(member.userId == userId ? reqId : 0);
                *env.mutable_s_vote() = pkt_res;
                PacketDispatcher::SendEnvelope(memberSession, env);
            }
        }
    }

    LOG_INFO("[ChatService] Vote: {} by {}", pollId, userId);
    return true;
}

bool ChatService::HandleClosePoll(sessionPtr& session, uint64 reqId, const Protocol::C_ClosePoll& pkt)
{
    auto serverSession = static_pointer_cast<ServerSession>(session);
    const string userId = serverSession->GetUserId();
    if (userId.empty()) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED);
        return false;
    }

    const string& pollId = pkt.poll_id();

    // 그룹 멤버십 확인
    {
        string clientConvId = pkt.conv_id();
        if (clientConvId.find("group:") == 0) {
            string groupId = clientConvId.substr(6);
            if (!GroupRepository::IsMember(groupId, userId)) {
                HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
                return false;
            }
        }
    }

    if (!PollRepository::ClosePoll(pollId, userId)) {
        HandleErr(session, reqId, ERR_NO_PERMISSION, "투표 종료 권한 없음 또는 이미 종료됨");
        return false;
    }

    // S_ClosePoll 브로드캐스트 (PollDetailScreen 실시간 반영)
    string clientConvId = pkt.conv_id();
    Protocol::S_ClosePoll pkt_res;
    pkt_res.set_conv_id(clientConvId);
    pkt_res.set_poll_id(pollId);
    pkt_res.set_msg_seq(pkt.msg_seq());

    // 종료 알림 새 메시지 전송
    SendPollClosedNotification(clientConvId, pollId, userId, reqId);

    if (clientConvId.find("group:") == 0) {
        string groupId = clientConvId.substr(6);
        auto members = GroupRepository::GetGroupMembers(groupId);
        for (const auto& member : members) {
            if (auto memberSession = _userManager.FindSession(member.userId)) {
                Protocol::Envelope env;
                env.set_version(GProtoVersion);
                env.set_request_id(member.userId == userId ? reqId : 0);
                *env.mutable_s_close_poll() = pkt_res;
                PacketDispatcher::SendEnvelope(memberSession, env);
            }
        }
    }

    LOG_INFO("[ChatService] ClosePoll: {}", pollId);
    return true;
}

void ChatService::SendPollClosedNotification(const string& clientConvId, const string& pollId,
                                              const string& closedByUserId, uint64 reqId)
{
    string convId = clientConvId;
    string groupId;
    if (clientConvId.find("group:") == 0) {
        groupId = clientConvId.substr(6);
    } else {
        return;
    }

    PollInfo info = PollRepository::GetPollInfo(pollId);
    if (info.pollId.empty()) return;

    string votesJson = PollRepository::GetPollVotes(pollId);
    int64 now = Nowts();

    // 새 메시지 seq 생성
    int64 newMsgSeq = MessageRepository::GetNextMessageSeq(convId);
    if (newMsgSeq < 0) return;

    // closed poll JSON 구성
    std::ostringstream oss;
    oss << "{\"pollId\":\"" << info.pollId << "\""
        << ",\"question\":\"";
    for (char c : info.question) {
        if (c == '"') oss << "\\\"";
        else if (c == '\\') oss << "\\\\";
        else oss << c;
    }
    oss << "\",\"options\":" << info.optionsJson
        << ",\"isMultiSelect\":" << (info.isMultiSelect ? "true" : "false")
        << ",\"isAnonymous\":" << (info.isAnonymous ? "true" : "false")
        << ",\"expiresAt\":" << info.expiresAt
        << ",\"creatorId\":\"" << info.creatorId << "\""
        << ",\"status\":\"closed\""
        << ",\"votes\":" << votesJson << "}";
    string closedPollJson = oss.str();

    // messages 테이블에 저장
    Protocol::S_Chat sChat;
    sChat.set_conv_id(convId);
    sChat.set_sender_id(info.creatorId);
    sChat.set_sender_name(GetUserNameWithId(info.creatorId));
    sChat.set_ts_server(now);
    sChat.set_msg_seq(newMsgSeq);
    sChat.mutable_payload()->mutable_text()->set_message(closedPollJson);
    MessageRepository::SaveMessage(convId, info.creatorId, sChat, "", 0, "", 5);

    // S_CreatePoll로 브로드캐스트 (status="closed")
    Protocol::S_CreatePoll notify;
    notify.set_conv_id(clientConvId);
    notify.set_poll_id(info.pollId);
    notify.set_msg_seq(newMsgSeq);
    notify.set_creator_id(info.creatorId);
    notify.set_creator_name(GetUserNameWithId(info.creatorId));
    notify.set_question(info.question);
    for (const auto& opt : nlohmann::json::parse(info.optionsJson)) {
        notify.add_options(opt.get<string>());
    }
    notify.set_is_multi_select(info.isMultiSelect);
    notify.set_is_anonymous(info.isAnonymous);
    notify.set_expires_at(info.expiresAt);
    notify.set_ts_server(now);
    notify.set_status("closed");
    notify.set_votes_json(votesJson);

    cGroupInfo groupInfoClosed;
    GroupRepository::GetGroupInfoById(groupId, groupInfoClosed);
    string groupNameClosed = groupInfoClosed.groupName.empty() ? "그룹" : groupInfoClosed.groupName;

    auto members = GroupRepository::GetGroupMembers(groupId);
    for (const auto& member : members) {
        if (auto memberSession = _userManager.FindSession(member.userId)) {
            Protocol::Envelope env;
            env.set_version(GProtoVersion);
            env.set_request_id(0);
            *env.mutable_s_create_poll() = notify;
            PacketDispatcher::SendEnvelope(memberSession, env);
        } else if (GFcmClient) {
            auto tokens = FcmTokenRepository::GetUserTokens(member.userId);
            if (!tokens.empty()) {
                string title = groupNameClosed;
                string body = "투표가 종료되었습니다: " + info.question;
                if (body.length() > 60) body = body.substr(0, 60) + "...";
                map<string, string> data = {
                    {"type", "poll_closed"}, {"conv_id", clientConvId},
                    {"group_id", groupId}, {"poll_id", pollId}
                };
                for (const auto& tokenInfo : tokens) {
                    bool invalidToken = false;
                    GFcmClient->SendPush(tokenInfo.fcmToken, title, body, data, &invalidToken);
                    if (invalidToken) FcmTokenRepository::DeleteToken(tokenInfo.userId, tokenInfo.fcmToken);
                }
            }
        }
    }
}


bool ChatService::HandleSetAnnouncement(sessionPtr& session, uint64 reqId, const Protocol::C_SetAnnouncement& pkt)
{
    auto serverSession = static_pointer_cast<ServerSession>(session);
    const string userId = serverSession->GetUserId();
    if (userId.empty()) {
        HandleErr(session, reqId, ERR_UNAUTHORIZED);
        return false;
    }

    const string& clientConvId = pkt.conv_id();
    int64 msgSeq = pkt.msg_seq();
    const string& text = pkt.text();
    const string& senderName = pkt.sender_name();

    // 멤버십 확인
    {
        auto dmPos_ann_check = clientConvId.find(":dm:");
        if (dmPos_ann_check != string::npos) {
            string groupId = clientConvId.substr(0, dmPos_ann_check);
            if (!GroupRepository::IsMember(groupId, userId)) {
                HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
                return false;
            }
        } else if (clientConvId.find("group:") == 0) {
            string groupId = clientConvId.substr(6);
            if (!GroupRepository::IsMember(groupId, userId)) {
                HandleErr(session, reqId, ERR_UNAUTHORIZED, "그룹 멤버가 아닙니다.");
                return false;
            }
        }
    }

    string convId = clientConvId;
    auto dmPos_ann = clientConvId.find(":dm:");
    if (dmPos_ann != string::npos) {
        string groupId  = clientConvId.substr(0, dmPos_ann);
        string targetId = clientConvId.substr(dmPos_ann + 4);
        convId = MessageRepository::CreateGroupDMConvId(groupId, userId, targetId);
    }

    // DB 저장/삭제
    if (msgSeq == 0) {
        MessageRepository::ClearAnnouncement(convId);
    } else {
        MessageRepository::SetAnnouncement(convId, msgSeq, text, senderName, userId);
    }

    // S_SetAnnouncement 빌드
    Protocol::S_SetAnnouncement res;
    res.set_conv_id(clientConvId); // 클라이언트 원본 conv_id 유지
    res.set_msg_seq(msgSeq);
    res.set_text(text);
    res.set_sender_name(senderName);
    res.set_setter_id(userId);

    // 그룹이면 전체 브로드캐스트, 1:1이면 양쪽에 전송
    if (clientConvId.find("group:") == 0) {
        string groupId = clientConvId.substr(6);
        auto members = GroupRepository::GetGroupMembers(groupId);
        for (const auto& member : members) {
            if (auto memberSession = _userManager.FindSession(member.userId)) {
                Protocol::Envelope env;
                env.set_version(GProtoVersion);
                env.set_request_id(member.userId == userId ? reqId : 0);
                *env.mutable_s_set_announcement() = res;
                PacketDispatcher::SendEnvelope(memberSession, env);
            }
        }
    } else {
        // GroupDM: 요청자에게 응답 + 상대방에게 전달
        auto dmPos_annB = convId.find(":dm:");
        if (dmPos_annB != string::npos) {
            Protocol::Envelope env;
            env.set_version(GProtoVersion);
            env.set_request_id(reqId);
            *env.mutable_s_set_announcement() = res;
            PacketDispatcher::SendEnvelope(session, env);

            string pair = convId.substr(dmPos_annB + 4);
            auto underPos = pair.find('_');
            if (underPos != string::npos) {
                string userA = pair.substr(0, underPos);
                string userB = pair.substr(underPos + 1);
                string targetId = (userA == userId) ? userB : userA;
                if (auto targetSession = _userManager.FindSession(targetId)) {
                    Protocol::Envelope env2;
                    env2.set_version(GProtoVersion);
                    env2.set_request_id(0);
                    *env2.mutable_s_set_announcement() = res;
                    PacketDispatcher::SendEnvelope(targetSession, env2);
                }
            }
        }
    }

    LOG_INFO("[ChatService] SetAnnouncement: convId={}, msgSeq={}", convId, msgSeq);
    return true;
}


void ChatService::HandleErr(sessionPtr& session, uint64 reqId, ErrorCode errorCode, const string& errMessage)
{
    LOG_ERROR("[ChatService] Error: {} (code: {})", errMessage, static_cast<int>(errorCode));
    PacketDispatcher::DispatchError(session, reqId, errorCode, errMessage);
}
