-- ============================================================
-- MOA Chat Server — DB 전체 초기화 스크립트
-- 새 서버에서 처음 실행할 때만 사용 (기존 DB 있으면 건드리지 않음)
-- 실행: mysql -u root -p < setup.sql
-- ============================================================

-- ── 0. DB / 유저 생성 ──────────────────────────────────────

CREATE DATABASE IF NOT EXISTS chat_server
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

CREATE USER IF NOT EXISTS 'chat_server'@'localhost'
    IDENTIFIED WITH mysql_native_password BY 'hoje1095';

GRANT ALL PRIVILEGES ON chat_server.* TO 'chat_server'@'localhost';
FLUSH PRIVILEGES;

USE chat_server;

-- ── 1. users ──────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS users (
    user_id                 VARCHAR(100)  PRIMARY KEY,
    password_hash           VARCHAR(255)  NULL,                 -- 소셜 로그인은 NULL
    auth_token              VARCHAR(512),
    name                    VARCHAR(100)  NOT NULL,
    email                   VARCHAR(255)  UNIQUE,
    profile_image_url       VARCHAR(500),
    background_image_url    VARCHAR(500)  NULL,
    status_message          VARCHAR(255)  DEFAULT '',
    sub_grade               INT           DEFAULT 0,
    storage_capacity_bytes  BIGINT        DEFAULT 1073741824,   -- 1GB
    storage_usage_bytes     BIGINT        DEFAULT 0,
    is_email_verified       TINYINT(1)    DEFAULT 0,
    oauth_provider          VARCHAR(20)   DEFAULT NULL,
    oauth_provider_id       VARCHAR(255)  DEFAULT NULL,
    is_deleted              TINYINT(1)    DEFAULT 0,
    deleted_at              TIMESTAMP     NULL,
    last_seen               TIMESTAMP     NULL,
    created_at              TIMESTAMP     DEFAULT CURRENT_TIMESTAMP,
    updated_at              TIMESTAMP     DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_auth_token        (auth_token),
    INDEX idx_oauth_provider    (oauth_provider, oauth_provider_id),
    INDEX idx_is_deleted        (is_deleted)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ── 2. groups / group_members / group_assets ──────────────

CREATE TABLE IF NOT EXISTS `groups` (
    group_id                VARCHAR(100)  PRIMARY KEY,
    group_name              VARCHAR(100)  NOT NULL,
    group_code              VARCHAR(20)   NOT NULL UNIQUE,
    creator_id              VARCHAR(100)  NOT NULL,
    description             TEXT,
    group_image_url         VARCHAR(255)  DEFAULT '',
    storage_capacity_bytes  BIGINT        DEFAULT 1073741824,   -- 1GB
    storage_usage_bytes     BIGINT        DEFAULT 0,
    member_count            INT           DEFAULT 0,
    invite_code_expires_at  BIGINT        DEFAULT NULL,
    created_at              TIMESTAMP     DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_creator           (creator_id),
    INDEX idx_invite_expires    (invite_code_expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS group_members (
    id          BIGINT        AUTO_INCREMENT PRIMARY KEY,
    group_id    VARCHAR(100)  NOT NULL,
    user_id     VARCHAR(100)  NOT NULL,
    role        VARCHAR(20)   DEFAULT 'member',
    joined_at   TIMESTAMP     DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_group_user (group_id, user_id),
    CONSTRAINT fk_gm_group FOREIGN KEY (group_id) REFERENCES `groups`(group_id) ON DELETE CASCADE,
    CONSTRAINT fk_gm_user  FOREIGN KEY (user_id)  REFERENCES users(user_id)     ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS group_assets (
    asset_id    BIGINT        AUTO_INCREMENT PRIMARY KEY,
    group_id    VARCHAR(100)  NOT NULL,
    user_id     VARCHAR(100)  NOT NULL,
    msg_seq     BIGINT        NOT NULL,
    file_size   BIGINT        NOT NULL,
    file_type   VARCHAR(20),
    created_at  TIMESTAMP     DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_group_asset (group_id),
    CONSTRAINT fk_assets_group FOREIGN KEY (group_id) REFERENCES `groups`(group_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- member_count 트리거 (스토리지 트리거는 코드로 처리)
DELIMITER $$
CREATE TRIGGER trg_group_member_add
AFTER INSERT ON group_members FOR EACH ROW
BEGIN
    UPDATE `groups` SET member_count = member_count + 1 WHERE group_id = NEW.group_id;
END$$
CREATE TRIGGER trg_group_member_remove
AFTER DELETE ON group_members FOR EACH ROW
BEGIN
    UPDATE `groups` SET member_count = member_count - 1 WHERE group_id = OLD.group_id;
END$$
DELIMITER ;

-- ── 3. conversations / messages / read_status ─────────────

CREATE TABLE IF NOT EXISTS conversations (
    conv_id     VARCHAR(256)  PRIMARY KEY,
    type        VARCHAR(20)   NOT NULL,
    created_at  TIMESTAMP     DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS conversation_participants (
    conv_id     VARCHAR(256)  NOT NULL,
    user_id     VARCHAR(50)   NOT NULL,
    joined_at   TIMESTAMP     DEFAULT CURRENT_TIMESTAMP,
    role        VARCHAR(20)   DEFAULT 'member',
    PRIMARY KEY (conv_id, user_id),
    FOREIGN KEY (conv_id) REFERENCES conversations(conv_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS messages (
    id                          BIGINT        AUTO_INCREMENT PRIMARY KEY,
    conv_id                     VARCHAR(256)  NOT NULL,
    msg_seq                     BIGINT        NOT NULL,
    sender_id                   VARCHAR(50)   NOT NULL,
    msg_type                    TINYINT       DEFAULT 0,
    message                     TEXT,
    media_url                   TEXT,
    thumbnail_url               TEXT,
    file_name                   VARCHAR(255),
    file_size                   BIGINT        DEFAULT 0,
    gcs_path                    VARCHAR(512)  NULL,
    file_retention_expires_at   BIGINT        NULL,
    file_status                 VARCHAR(20)   NOT NULL DEFAULT 'active',
    mentioned_user_ids          TEXT          DEFAULT NULL,
    reply_to_seq                BIGINT        DEFAULT 0,
    is_deleted                  TINYINT(1)    DEFAULT 0,
    is_edited                   TINYINT(1)    DEFAULT 0,
    edited_at                   BIGINT        DEFAULT 0,
    timestamp                   BIGINT,
    INDEX idx_conv_seq      (conv_id, msg_seq),
    INDEX idx_msg_type      (msg_type),
    INDEX idx_timestamp     (conv_id, timestamp),
    INDEX idx_file_expires  (file_retention_expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS read_status (
    user_id         VARCHAR(50)   NOT NULL,
    conv_id         VARCHAR(256)  NOT NULL,
    last_read_seq   BIGINT        DEFAULT 0,
    updated_at      TIMESTAMP     DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (user_id, conv_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ── 4. fcm_tokens ─────────────────────────────────────────

CREATE TABLE IF NOT EXISTS fcm_tokens (
    id           BIGINT        AUTO_INCREMENT PRIMARY KEY,
    user_id      VARCHAR(100)  NOT NULL,
    fcm_token    VARCHAR(512)  NOT NULL UNIQUE,
    platform     VARCHAR(20)   DEFAULT 'android',
    device_id    VARCHAR(255),
    device_name  VARCHAR(100),
    app_version  VARCHAR(20),
    created_at   TIMESTAMP     DEFAULT CURRENT_TIMESTAMP,
    last_active  TIMESTAMP     DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_user_id   (user_id),
    INDEX idx_device_id (device_id),
    FOREIGN KEY (user_id) REFERENCES users(user_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ── 5. file_uploads ───────────────────────────────────────

CREATE TABLE IF NOT EXISTS file_uploads (
    file_id                     VARCHAR(64)   PRIMARY KEY,
    user_id                     VARCHAR(64)   NOT NULL,
    group_id                    VARCHAR(100)  NULL,
    original_filename           VARCHAR(255)  NOT NULL,
    gcs_path                    VARCHAR(512)  NOT NULL,
    file_size                   BIGINT        NOT NULL,
    mime_type                   VARCHAR(100),
    is_image                    TINYINT(1)    DEFAULT 0,
    thumbnail_gcs_path          VARCHAR(512)  NULL,
    upload_status               ENUM('pending','uploading','completed','failed') DEFAULT 'pending',
    file_retention_expires_at   BIGINT        NULL,
    created_at                  TIMESTAMP     DEFAULT CURRENT_TIMESTAMP,
    uploaded_at                 TIMESTAMP     NULL,
    INDEX idx_user_id       (user_id),
    INDEX idx_group_id      (group_id),
    INDEX idx_upload_status (upload_status),
    INDEX idx_fu_retention  (file_retention_expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ── 6. 구독 시스템 ────────────────────────────────────────

CREATE TABLE IF NOT EXISTS subscription_plans (
    plan_id       INT PRIMARY KEY AUTO_INCREMENT,
    plan_type     ENUM('personal','group') NOT NULL,
    name          VARCHAR(50)   NOT NULL,
    grade         INT           NOT NULL DEFAULT 0,
    storage_bytes BIGINT        NOT NULL,
    max_file_size BIGINT        NOT NULL,
    monthly_price DECIMAL(10,2) NOT NULL DEFAULT 0,
    is_active     TINYINT(1)    DEFAULT 1,
    created_at    TIMESTAMP     DEFAULT CURRENT_TIMESTAMP
);

INSERT INTO subscription_plans (plan_type, name, grade, storage_bytes, max_file_size, monthly_price) VALUES
('group', 'Free',  0,   1073741824,   20971520,     0),
('group', 'Basic', 1,  10737418240,  104857600,  1100),
('group', 'Plus',  2,  53687091200,  524288000,  3300),
('group', 'Max',   3, 128849018880, 1073741824, 11000);

CREATE TABLE IF NOT EXISTS subscriptions (
    id               INT PRIMARY KEY AUTO_INCREMENT,
    group_id         VARCHAR(64)   NOT NULL,
    plan_id          INT           NOT NULL,
    started_at       TIMESTAMP     DEFAULT CURRENT_TIMESTAMP,
    expires_at       BIGINT        NULL,
    grace_expires_at BIGINT        NULL,
    auto_renew       TINYINT(1)    DEFAULT 1,
    status           ENUM('active','cancelled','expired','grace') DEFAULT 'active',
    FOREIGN KEY (plan_id) REFERENCES subscription_plans(plan_id),
    UNIQUE KEY uq_group (group_id),
    INDEX idx_sub_expires (expires_at)
);

CREATE TABLE IF NOT EXISTS payment_transactions (
    id              INT PRIMARY KEY AUTO_INCREMENT,
    user_id         VARCHAR(64)   NOT NULL,
    group_id        VARCHAR(64)   NULL,
    plan_id         INT           NOT NULL,
    platform        ENUM('android','ios') NOT NULL,
    product_id      VARCHAR(100)  NOT NULL,
    transaction_id  VARCHAR(255)  NOT NULL,
    purchase_token  TEXT,
    status          ENUM('pending','verified','failed','refunded') DEFAULT 'pending',
    verified_at     TIMESTAMP     NULL,
    created_at      TIMESTAMP     DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_pt_user  (user_id),
    INDEX idx_pt_group (group_id),
    INDEX idx_pt_token (purchase_token(255)),
    UNIQUE KEY uq_transaction (platform, transaction_id)
);

-- ── 7. 채팅 부가 기능 ─────────────────────────────────────

CREATE TABLE IF NOT EXISTS message_reactions (
    id          BIGINT        AUTO_INCREMENT PRIMARY KEY,
    conv_id     VARCHAR(256)  NOT NULL,
    msg_seq     BIGINT        NOT NULL,
    user_id     VARCHAR(50)   NOT NULL,
    emoji       VARCHAR(10)   NOT NULL,
    created_at  BIGINT        NOT NULL,
    UNIQUE KEY uq_reaction (conv_id, msg_seq, user_id, emoji),
    INDEX idx_mr_conv_seq (conv_id, msg_seq)
);

CREATE TABLE IF NOT EXISTS announcements (
    conv_id     VARCHAR(256)  PRIMARY KEY,
    msg_seq     BIGINT        NOT NULL,
    text        TEXT          NOT NULL,
    sender_name VARCHAR(100)  DEFAULT '',
    setter_id   VARCHAR(100)  DEFAULT '',
    set_at      BIGINT        DEFAULT 0
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS polls (
    poll_id         VARCHAR(64)   PRIMARY KEY,
    conv_id         VARCHAR(256)  NOT NULL,
    msg_seq         BIGINT        NOT NULL,
    creator_id      VARCHAR(64)   NOT NULL,
    question        TEXT          NOT NULL,
    options_json    TEXT          NOT NULL,
    is_multi_select TINYINT       DEFAULT 0,
    is_anonymous    TINYINT       DEFAULT 0,
    expires_at      BIGINT        DEFAULT 0,
    status          VARCHAR(20)   DEFAULT 'active',
    created_at      BIGINT        NOT NULL,
    INDEX idx_poll_conv (conv_id)
);

CREATE TABLE IF NOT EXISTS poll_votes (
    poll_id      VARCHAR(64)  NOT NULL,
    user_id      VARCHAR(64)  NOT NULL,
    option_index INT          NOT NULL,
    created_at   BIGINT       NOT NULL,
    PRIMARY KEY (poll_id, user_id, option_index),
    INDEX idx_pv_poll (poll_id)
);

CREATE TABLE IF NOT EXISTS block_list (
    id          BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    user_id     VARCHAR(50)  NOT NULL,
    blocked_id  VARCHAR(50)  NOT NULL,
    created_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uq_block (user_id, blocked_id),
    INDEX idx_bl_user    (user_id),
    INDEX idx_bl_blocked (blocked_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ── 8. 앨범 ──────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS albums (
    id              VARCHAR(36)   PRIMARY KEY,
    group_id        VARCHAR(50)   NOT NULL,
    name            VARCHAR(100)  NOT NULL,
    cover_photo_url VARCHAR(512)  DEFAULT NULL,
    created_by      VARCHAR(50)   NOT NULL,
    created_at      BIGINT        NOT NULL,
    INDEX idx_albums_group (group_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS album_photos (
    id              VARCHAR(36)   PRIMARY KEY,
    album_id        VARCHAR(36)   NOT NULL,
    group_id        VARCHAR(50)   NOT NULL,
    msg_seq         BIGINT        DEFAULT NULL,
    photo_url       VARCHAR(512)  NOT NULL,
    thumbnail_url   VARCHAR(512)  DEFAULT NULL,
    uploader_id     VARCHAR(50)   NOT NULL,
    source          VARCHAR(20)   NOT NULL DEFAULT 'chat',
    gcs_path        VARCHAR(512)  DEFAULT NULL,
    file_size       BIGINT        DEFAULT 0,
    is_deleted      TINYINT(1)    NOT NULL DEFAULT 0,
    deleted_at      BIGINT        DEFAULT NULL,
    uploaded_at     BIGINT        NOT NULL,
    INDEX idx_ap_album      (album_id),
    INDEX idx_ap_group      (group_id),
    INDEX idx_ap_msg        (group_id, msg_seq),
    INDEX idx_ap_soft_delete (is_deleted, deleted_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS photo_comments (
    id          VARCHAR(36)  PRIMARY KEY,
    photo_id    VARCHAR(36)  NOT NULL,
    sender_id   VARCHAR(50)  NOT NULL,
    sender_name VARCHAR(50),
    text        TEXT         NOT NULL,
    created_at  BIGINT       NOT NULL,
    INDEX idx_pc_photo (photo_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ── 9. 위치 히스토리 ──────────────────────────────────────

CREATE TABLE IF NOT EXISTS location_places (
    id               VARCHAR(36)   NOT NULL PRIMARY KEY,
    group_id         VARCHAR(36)   NOT NULL,
    name             VARCHAR(100)  NOT NULL,
    address          VARCHAR(300)  DEFAULT '',
    category         VARCHAR(30)   NOT NULL DEFAULT 'etc',
    lat              DOUBLE        DEFAULT 0,
    lng              DOUBLE        DEFAULT 0,
    created_by       VARCHAR(36)   NOT NULL,
    created_at       BIGINT        NOT NULL,
    visit_count      INT           DEFAULT 0,
    avg_rating       FLOAT         DEFAULT 0,
    recent_photo_url VARCHAR(512)  DEFAULT NULL,
    INDEX idx_lp_group   (group_id),
    INDEX idx_lp_created (created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS location_visits (
    id          VARCHAR(36)  NOT NULL PRIMARY KEY,
    place_id    VARCHAR(36)  NOT NULL,
    group_id    VARCHAR(36)  NOT NULL,
    user_id     VARCHAR(36)  NOT NULL,
    user_name   VARCHAR(60)  NOT NULL DEFAULT '',
    history_id  VARCHAR(36)  NOT NULL DEFAULT '',
    rating      TINYINT      NOT NULL DEFAULT 0,
    review      TEXT         DEFAULT NULL,
    visited_at  BIGINT       NOT NULL,
    INDEX idx_lv_place (place_id),
    INDEX idx_lv_group (group_id),
    INDEX idx_lv_user  (user_id),
    FOREIGN KEY (place_id) REFERENCES location_places(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS location_photos (
    id             VARCHAR(36)   NOT NULL PRIMARY KEY,
    visit_id       VARCHAR(36)   NOT NULL,
    place_id       VARCHAR(36)   NOT NULL,
    group_id       VARCHAR(36)   NOT NULL,
    photo_url      VARCHAR(512)  NOT NULL,
    album_photo_id VARCHAR(36)   DEFAULT NULL,
    created_at     BIGINT        NOT NULL,
    INDEX idx_lpho_visit (visit_id),
    INDEX idx_lpho_place (place_id),
    FOREIGN KEY (visit_id) REFERENCES location_visits(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
