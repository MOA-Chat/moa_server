-- 29_max_plan_150gb.sql
-- Max 플랜 용량 120GB(128849018880) → 150GB(161061273600) 변경
-- 2026-05-28
-- 주의: safe update mode 우회 필요 (26_location_history_id.sql 과 동일한 패턴)

SET SQL_SAFE_UPDATES = 0;

UPDATE subscription_plans
SET storage_bytes = 161061273600
WHERE name = 'Max' AND plan_type = 'group';

SET SQL_SAFE_UPDATES = 1;
