#include "logger.h"
#include "CrashReport.h"

#include <cassert>
#ifdef __APPLE__
#include <os/log.h>
#else
#include <REND.h>
#endif

#include "chrono"
#include "core/FileSystem.h"
#include <sstream>
#include <ctime>
#include <iomanip>
#include <cstdio>

using namespace SFE::LogsModule;

std::wstring to_wstring_utf8(const std::string& str)
{
	int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
		(int)str.size(), NULL, 0);

	std::wstring wstr(size_needed, 0);

	MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
		(int)str.size(), &wstr[0], size_needed);

	return wstr;
}

void Logger::logMessage(const eLogLevel level, const char* msg) {
	auto t = time(nullptr);
	
	std::ostringstream oss;
	tm buf;
#ifdef __APPLE__
	localtime_r(&t ,&buf);
	uint8_t logType;
#else
	localtime_s(&buf, &t);
#endif

	oss << std::put_time(&buf, "%d/%m/%y %H:%M:%S ");

	switch (level) {
	case eLogLevel::WARNING:
#ifdef __APPLE__
		logType = OS_LOG_TYPE_ERROR;
#endif
		oss << "WARNING:  ";
		break;
	case eLogLevel::ERROR_:
#ifdef __APPLE__
		logType = OS_LOG_TYPE_ERROR;
#endif
		oss << "ERROR:  ";
		break;
	case eLogLevel::INFO:
#ifdef __APPLE__
		logType = OS_LOG_TYPE_INFO;
#endif
		break;
	case eLogLevel::FATAL:
#ifdef __APPLE__
		logType = OS_LOG_TYPE_FAULT;
#endif
		oss << "FATAL:  ";
	}
	oss << msg << "\n";
	CrashReport::remember(oss.str().c_str());
#ifdef __APPLE__
	os_log_with_type(OS_LOG_DEFAULT, os_log_type_t(logType), "%s", oss.str().c_str());
#else
	OutputDebugString(to_wstring_utf8(oss.str()).c_str());
#endif
	if (level == eLogLevel::FATAL || level == eLogLevel::ERROR_) {
		// Under the application's root, as every write is: never into the engine's folder, where a run of the engine's own
		// tests leaves a log of this name.
		if (FILE* logFile = fopen((SFE::FileSystem::root() / "sfe_fatal.log").string().c_str(), "a")) {
			fputs(oss.str().c_str(), logFile);
			fclose(logFile);
		}
	}
	if (level == eLogLevel::FATAL) {
		assert(false && msg);
	}
}
