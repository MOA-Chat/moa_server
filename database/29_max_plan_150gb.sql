-- 29_max_plan_150gb.sql
-- Max 플랜 용량 120GB(128849018880) → 150GB(161061273600) 변경
-- 2026-05-28

UPDATE subscription_plans
SET storage_bytes = 161061273600
WHERE name = 'Max' AND plan_type = 'group';
