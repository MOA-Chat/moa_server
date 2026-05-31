-- 27_album_photos_columns.sql
-- album_photos 테이블에 구독/쿼터/소프트딜리트 관련 컬럼 추가
-- AlbumRepository.cpp가 사용 중이나 21_albums.sql에 누락됨

USE chat_server;

ALTER TABLE album_photos
  ADD COLUMN gcs_path    VARCHAR(512) DEFAULT NULL,
  ADD COLUMN file_size   BIGINT       DEFAULT 0,
  ADD COLUMN is_deleted  TINYINT(1)   NOT NULL DEFAULT 0,
  ADD COLUMN deleted_at  BIGINT       DEFAULT NULL;

-- 소프트딜리트 조회용 인덱스
CREATE INDEX idx_ap_soft_delete ON album_photos (is_deleted, deleted_at);
