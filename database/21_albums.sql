-- 21_albums.sql
-- 앨범 시스템 테이블 (chat upload + direct upload 확장성 포함)

USE chat_server;

CREATE TABLE IF NOT EXISTS albums (
  id            VARCHAR(36)   PRIMARY KEY,
  group_id      VARCHAR(50)   NOT NULL,
  name          VARCHAR(100)  NOT NULL,
  cover_photo_url VARCHAR(512) DEFAULT NULL,
  created_by    VARCHAR(50)   NOT NULL,
  created_at    BIGINT        NOT NULL,
  INDEX idx_albums_group (group_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- album_photos:
--   msg_seq  : 채팅 업로드 시 해당 메시지 seq, 앨범 직접 업로드(future) 시 NULL
--   group_id : 앨범 조인 없이 그룹 전체 사진 조회 가능 (확장성)
--   source   : 'chat' | 'direct' (현재는 chat만, 추후 직접 업로드 지원)
--   uploader_id: 올린 사람 (삭제 권한 체크용)
CREATE TABLE IF NOT EXISTS album_photos (
  id            VARCHAR(36)   PRIMARY KEY,
  album_id      VARCHAR(36)   NOT NULL,
  group_id      VARCHAR(50)   NOT NULL,
  msg_seq       BIGINT        DEFAULT NULL,
  photo_url     VARCHAR(512)  NOT NULL,
  thumbnail_url VARCHAR(512)  DEFAULT NULL,
  uploader_id   VARCHAR(50)   NOT NULL,
  source        VARCHAR(20)   NOT NULL DEFAULT 'chat',
  uploaded_at   BIGINT        NOT NULL,
  INDEX idx_ap_album   (album_id),
  INDEX idx_ap_group   (group_id),
  INDEX idx_ap_msg     (group_id, msg_seq)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- photo_comments:
--   photo_id : album_photos.id 참조 (사진 이동해도 댓글 유지, UUID 안 변함)
CREATE TABLE IF NOT EXISTS photo_comments (
  id          VARCHAR(36)  PRIMARY KEY,
  photo_id    VARCHAR(36)  NOT NULL,
  sender_id   VARCHAR(50)  NOT NULL,
  sender_name VARCHAR(50),
  text        TEXT         NOT NULL,
  created_at  BIGINT       NOT NULL,
  INDEX idx_pc_photo (photo_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
