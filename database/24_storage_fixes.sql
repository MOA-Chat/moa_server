-- ============================================================
-- 24_storage_fixes.sql
-- 그룹 스토리지 트리거 제거 + 용량 보정 마이그레이션
-- 1) group_assets 트리거 모두 DROP (애플리케이션 레벨로 전환)
-- 2) 기존 그룹 capacity 보정 (DEFAULT 10GB → 1GB)
-- 3) user_assets: msg_seq 중복 방지 UNIQUE KEY 추가
-- ============================================================

USE chat_server;

-- ── 1. group_assets 트리거 모두 제거 ──
--   storage_usage_bytes 증감은 GroupRepository 코드에서 직접 처리
DROP TRIGGER IF EXISTS trg_group_storage_add;
DROP TRIGGER IF EXISTS trg_group_storage_remove;


-- ── 2. capacity=0 또는 DEFAULT 10GB인 기존 그룹을 1GB로 보정 ──
--   (GroupRepository::CreateGroup은 처음부터 1GB를 삽입하므로 새 그룹은 영향 없음)
SET SQL_SAFE_UPDATES = 0;
UPDATE `groups`
SET storage_capacity_bytes = 1073741824   -- 1GB
WHERE storage_capacity_bytes = 0
   OR storage_capacity_bytes = 10737418240;  -- 10GB (old schema default)
SET SQL_SAFE_UPDATES = 1;


-- ── 3. user_assets: 동일 msg_seq 이중 INSERT 방지 ──
ALTER TABLE user_assets
    ADD CONSTRAINT uq_ua_msg_seq UNIQUE (msg_seq);
