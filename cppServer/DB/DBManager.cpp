#include "pch.h"
#include "DBManager.h"

// thread_local 세션을 파일 스코프에 선언 → ResetCurrentSession에서 접근 가능
static thread_local std::unique_ptr<mysqlx::Session> tls_session;
static thread_local bool tls_needReset = false;

DBManager& DBManager::GetInstance() {
    static DBManager instance;
    return instance;
}

bool DBManager::Initialize(const string& host, int port,
    const string& user, const string& password,
    const string& database) {
    try {
        // 메인 스레드에서 연결 테스트
        mysqlx::SessionSettings settings(
            mysqlx::SessionOption::HOST, host,
            mysqlx::SessionOption::PORT, port,
            mysqlx::SessionOption::USER, user,
            mysqlx::SessionOption::PWD, password,
            mysqlx::SessionOption::DB, database
        );

        auto testSession = mysqlx::Session(settings);
        testSession.close();

        // 설정 저장 (스레드별 세션 생성용)
        _host = host;
        _port = port;
        _user = user;
        _password = password;
        _database = database;
        _initialized = true;

        LOG_INFO("[DBManager] MySQL 연결 성공: {}:{}", host, port);
        return true;
    }
    catch (const mysqlx::Error& err) {
        LOG_ERROR("[DBManager] 연결 실패: {}", err.what());
        return false;
    }
}

mysqlx::Session& DBManager::GetSession() {
    if (!_initialized) {
        throw std::runtime_error("[DBManager] Database not initialized");
    }

    // 에러 후 리셋 요청된 경우 기존 세션 파기
    if (tls_needReset && tls_session) {
        LOG_WARN("[DBManager] 세션 리셋 (CDK 에러 복구) thread={}",
                 std::hash<std::thread::id>{}(std::this_thread::get_id()));
        try { tls_session->close(); } catch (...) {}
        tls_session.reset();
        tls_needReset = false;
    }

    if (!tls_session) {
        mysqlx::SessionSettings settings(
            mysqlx::SessionOption::HOST, _host,
            mysqlx::SessionOption::PORT, _port,
            mysqlx::SessionOption::USER, _user,
            mysqlx::SessionOption::PWD, _password,
            mysqlx::SessionOption::DB, _database
        );
        tls_session = std::make_unique<mysqlx::Session>(settings);
        LOG_INFO("[DBManager] 스레드별 DB 세션 생성 (thread_id={})",
                 std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }

    return *tls_session;
}

mysqlx::Schema DBManager::GetSchema() {
    return GetSession().getSchema(_database);
}

// CDK "Can't write message while another one is being written" 에러 후 호출
// 현재 스레드의 세션에 리셋 플래그 설정 → 다음 GetSession()에서 재생성
void DBManager::ResetCurrentSession() {
    tls_needReset = true;
}
