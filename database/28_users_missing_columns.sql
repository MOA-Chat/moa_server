-- 28_users_missing_columns.sql
-- setup.sql로 생성된 DB에 background_image_url 컬럼 추가
-- MySQL 5.x 호환 (IF NOT EXISTS 미지원)
-- 실행: mysql -u root -p chat_server < 28_users_missing_columns.sql

USE chat_server;

ALTER TABLE users ADD COLUMN background_image_url VARCHAR(500) NULL AFTER profile_image_url;
