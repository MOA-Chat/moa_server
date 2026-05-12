-- 23_location_places.sql
-- 위치 히스토리: 장소(places), 방문 기록(visits), 사진 참조(photos)

CREATE TABLE IF NOT EXISTS location_places (
    id            VARCHAR(36)  NOT NULL PRIMARY KEY,
    group_id      VARCHAR(36)  NOT NULL,
    name          VARCHAR(100) NOT NULL,
    address       VARCHAR(300) DEFAULT '',
    category      VARCHAR(30)  NOT NULL DEFAULT 'etc',
    lat           DOUBLE       DEFAULT 0,
    lng           DOUBLE       DEFAULT 0,
    created_by    VARCHAR(36)  NOT NULL,
    created_at    BIGINT       NOT NULL,
    visit_count   INT          DEFAULT 0,
    avg_rating    FLOAT        DEFAULT 0,
    recent_photo_url VARCHAR(512) DEFAULT NULL,
    INDEX idx_lp_group (group_id),
    INDEX idx_lp_created (created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS location_visits (
    id            VARCHAR(36)  NOT NULL PRIMARY KEY,
    place_id      VARCHAR(36)  NOT NULL,
    group_id      VARCHAR(36)  NOT NULL,
    user_id       VARCHAR(36)  NOT NULL,
    user_name     VARCHAR(60)  NOT NULL DEFAULT '',
    rating        TINYINT      NOT NULL DEFAULT 0,
    review        TEXT         DEFAULT NULL,
    visited_at    BIGINT       NOT NULL,
    INDEX idx_lv_place (place_id),
    INDEX idx_lv_group (group_id),
    INDEX idx_lv_user  (user_id),
    FOREIGN KEY (place_id) REFERENCES location_places(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS location_photos (
    id            VARCHAR(36)  NOT NULL PRIMARY KEY,
    visit_id      VARCHAR(36)  NOT NULL,
    place_id      VARCHAR(36)  NOT NULL,
    group_id      VARCHAR(36)  NOT NULL,
    photo_url     VARCHAR(512) NOT NULL,
    album_photo_id VARCHAR(36) DEFAULT NULL,
    created_at    BIGINT       NOT NULL,
    INDEX idx_lpho_visit  (visit_id),
    INDEX idx_lpho_place  (place_id),
    FOREIGN KEY (visit_id) REFERENCES location_visits(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
