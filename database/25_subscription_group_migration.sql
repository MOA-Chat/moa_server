-- 25_subscription_group_migration.sql
-- 구독 시스템을 user 기반 → group 기반으로 전환
-- 실행 전 기존 subscriptions 데이터 백업 권장

USE chat_server;

-- 1. subscriptions: user_id → group_id 컬럼 변경
ALTER TABLE subscriptions CHANGE COLUMN user_id group_id VARCHAR(64) NOT NULL;

-- 2. subscriptions: status에 'grace' 추가
ALTER TABLE subscriptions MODIFY COLUMN status ENUM('active','cancelled','expired','grace') DEFAULT 'active';

-- 3. subscriptions: expires_at TIMESTAMP → BIGINT (밀리초)
ALTER TABLE subscriptions MODIFY COLUMN expires_at BIGINT NULL;

-- 4. subscriptions: grace_expires_at 컬럼 추가
ALTER TABLE subscriptions ADD COLUMN grace_expires_at BIGINT NULL;

-- 5. subscriptions: group_id에 UNIQUE 키 추가 (ON DUPLICATE KEY UPDATE 사용)
ALTER TABLE subscriptions ADD UNIQUE KEY uq_group (group_id);

-- 6. subscription_plans: 그룹 구독 플랜으로 교체
SET FOREIGN_KEY_CHECKS = 0;
TRUNCATE TABLE subscription_plans;
SET FOREIGN_KEY_CHECKS = 1;
INSERT INTO subscription_plans (plan_type, name, grade, storage_bytes, max_file_size, monthly_price, is_active) VALUES
('group', 'Free',  0,    1073741824,    20971520,     0, 1),
('group', 'Basic', 1,  10737418240,   104857600,  1100, 1),
('group', 'Plus',  2,  53687091200,   524288000,  3300, 1),
('group', 'Max',   3, 161061273600, 1073741824,  11000, 1);

-- 7. payment_transactions: group_id 컬럼 추가
ALTER TABLE payment_transactions ADD COLUMN group_id VARCHAR(64) NULL;
ALTER TABLE payment_transactions ADD INDEX idx_pt_group (group_id);
ALTER TABLE payment_transactions ADD INDEX idx_pt_token (purchase_token(255));
