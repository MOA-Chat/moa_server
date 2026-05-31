-- 30_fix_member_count.sql
-- 버그: trg_group_member_add/remove 트리거가 누락된 채 AddMember/RemoveMember에
--       명시적 UPDATE까지 남아있어 JOIN 1회당 member_count +2 발생
--       (10_drop_storage_triggers.sql에서 스토리지 트리거만 삭제, group 트리거 미삭제)
--
-- 수정:
--   1. group_members 트리거 DROP (코드에서 직접 처리로 일원화)
--   2. 기존 그룹 member_count를 실제 group_members 행 수로 재계산

USE chat_server;

-- 1. 잔존 트리거 제거
DROP TRIGGER IF EXISTS trg_group_member_add;
DROP TRIGGER IF EXISTS trg_group_member_remove;

-- 2. 기존 데이터 수정 (이중증가로 오염된 member_count 리셋)
SET SQL_SAFE_UPDATES = 0;

UPDATE `groups` g
SET member_count = (
    SELECT COUNT(*) FROM group_members gm WHERE gm.group_id = g.group_id
);

SET SQL_SAFE_UPDATES = 1;
