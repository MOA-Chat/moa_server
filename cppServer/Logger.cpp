#include "pch.h"
#include "Logger.h"

#include <filesystem>

std::shared_ptr<spdlog::logger> Logger::s_serverLogger;
std::shared_ptr<spdlog::logger> Logger::s_auditLogger;
std::mutex Logger::s_convMutex;
std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> Logger::s_convLoggers;

void Logger::Init()
{
    std::filesystem::create_directories("logs/server");
    std::filesystem::create_directories("logs/audit");
    std::filesystem::create_directories("logs/conversations");

    // Server logger: console + daily rotation (30 days)
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto serverFileSink = std::make_shared<spdlog::sinks::daily_file_sink_mt>("logs/server/server", 0, 0, false, 30);

    s_serverLogger = std::make_shared<spdlog::logger>("server",
        spdlog::sinks_init_list{ consoleSink, serverFileSink });
    s_serverLogger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    s_serverLogger->set_level(spdlog::level::trace);
    s_serverLogger->flush_on(spdlog::level::warn);

    // Audit logger: daily rotation only (90 days), no console
    auto auditFileSink = std::make_shared<spdlog::sinks::daily_file_sink_mt>("logs/audit/audit", 0, 0, false, 90);

    s_auditLogger = std::make_shared<spdlog::logger>("audit", auditFileSink);
    s_auditLogger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
    s_auditLogger->set_level(spdlog::level::info);
    s_auditLogger->flush_on(spdlog::level::info);

    spdlog::register_logger(s_serverLogger);
    spdlog::register_logger(s_auditLogger);

    s_serverLogger->info("Logger initialized");
}

void Logger::Shutdown()
{
    if (s_serverLogger) s_serverLogger->info("Logger shutting down");
    spdlog::shutdown();
}

std::shared_ptr<spdlog::logger>& Logger::GetServerLogger()
{
    // CoreGlobal 등 전역 객체가 main() 전에 생성될 때 Init() 미호출 상태 대비
    if (!s_serverLogger) {
        auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        s_serverLogger = std::make_shared<spdlog::logger>("server_early", consoleSink);
        s_serverLogger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        s_serverLogger->set_level(spdlog::level::trace);
    }
    return s_serverLogger;
}

std::shared_ptr<spdlog::logger>& Logger::GetAuditLogger()
{
    if (!s_auditLogger) {
        auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        s_auditLogger = std::make_shared<spdlog::logger>("audit_early", consoleSink);
        s_auditLogger->set_level(spdlog::level::info);
    }
    return s_auditLogger;
}

std::shared_ptr<spdlog::logger> Logger::GetConvLogger(const std::string& logKey)
{
    std::lock_guard<std::mutex> lock(s_convMutex);

    auto it = s_convLoggers.find(logKey);
    if (it != s_convLoggers.end()) {
        return it->second;
    }

    // logKey 예: "groupId/chat"  또는  "groupId/dm_userA_userB"
    // 파일 경로: logs/conversations/groupId/chat_YYYY-MM-DD.log
    std::string fullPath = "logs/conversations/" + logKey;

    // 부모 디렉토리 자동 생성 (groupId 폴더)
    std::filesystem::path fsPath(fullPath);
    std::filesystem::create_directories(fsPath.parent_path());

    // 대화방별 daily rotation (365일 보관, 콘솔 출력 없음)
    std::string path = fullPath;
    auto sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(path, 0, 0, false, 365);

    auto logger = std::make_shared<spdlog::logger>("conv_" + logKey, sink);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S] %v");
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);

    s_convLoggers[logKey] = logger;
    return logger;
}
