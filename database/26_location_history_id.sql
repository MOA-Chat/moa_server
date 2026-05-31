-- location_visits에 history_id 컬럼 추가
-- history_id: 크로스그룹 공유 시 동일 UUID 공유 → 방문기록 중복 제거에 사용
-- 수정(CUpdateVisit) 시 새 UUID로 교체 → 독립 기록으로 분리됨
ALTER TABLE location_visits ADD COLUMN history_id VARCHAR(36) NOT NULL DEFAULT '';

-- 기존 데이터: history_id = 자기 자신의 id (각자 독립, 중복 없음)
  SET SQL_SAFE_UPDATES = 0;
  UPDATE location_visits SET history_id = id WHERE history_id = '';
  SET SQL_SAFE_UPDATES = 1;