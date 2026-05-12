-- ============================================
-- 21_groupdm_schema.sql
-- GroupDM 지원: conv_id 길이 확장 + type 컬럼 VARCHAR 보장
--
-- 문제 1: conversations.type 컬럼이 ENUM('direct','group')으로 생성된 경우
--         'groupdm' 삽입 시 "Data truncated" 에러 발생
-- 문제 2: conv_id VARCHAR(100) — GroupDM conv_id가 100자 초과 가능
--         (groupdm:UUID32:UUID32_UUID32 = 106자)
-- ============================================

USE chat_server;

-- conversations 테이블
ALTER TABLE conversations
    MODIFY COLUMN conv_id VARCHAR(256) NOT NULL,
    MODIFY COLUMN type    VARCHAR(20)  NOT NULL;

-- messages 테이블
ALTER TABLE messages
    MODIFY COLUMN conv_id VARCHAR(256) NOT NULL;

-- read_status 테이블
ALTER TABLE read_status
    MODIFY COLUMN conv_id VARCHAR(256) NOT NULL;

-- announcements 테이블 (존재하는 경우)
ALTER TABLE announcements
    MODIFY COLUMN conv_id VARCHAR(256) NOT NULL;
