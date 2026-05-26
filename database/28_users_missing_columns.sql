-- 28_users_missing_columns.sql
-- users 테이블에 누락된 컬럼 추가 (setup.sql이 이전 버전으로 생성된 DB용)
-- 실행: mysql -u root -p chat_server < 28_users_missing_columns.sql

USE chat_server;

ALTER TABLE users
    ADD COLUMN IF NOT EXISTS background_image_url    VARCHAR(500)  NULL               AFTER profile_image_url,
    ADD COLUMN IF NOT EXISTS sub_grade               INT           DEFAULT 0          AFTER status_message,
    ADD COLUMN IF NOT EXISTS storage_capacity_bytes  BIGINT        DEFAULT 1073741824  AFTER sub_grade,
    ADD COLUMN IF NOT EXISTS storage_usage_bytes     BIGINT        DEFAULT 0          AFTER storage_capacity_bytes;
