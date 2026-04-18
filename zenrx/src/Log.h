#pragma once

#include <cstdio>
#include <ctime>
#include <cstdarg>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include <mutex>

namespace zenrx {

enum class LogLevel {
    None,
    Error,
    Warning,
    Info,
    Debug
};

// ANSI color codes
#define CLR_RESET   "\033[0m"
#define CLR_RED     "\033[31m"
#define CLR_GREEN   "\033[32m"
#define CLR_YELLOW  "\033[33m"
#define CLR_BLUE    "\033[34m"
#define CLR_MAGENTA "\033[35m"
#define CLR_CYAN    "\033[36m"
#define CLR_WHITE   "\033[37m"
#define CLR_BOLD    "\033[1m"

class Log {
public:
    static void init(LogLevel level = LogLevel::Info) {
        s_level = level;
    }

    static void initFile(const std::string& path) {
        if (s_logFile) fclose(s_logFile);
        s_logFile = fopen(path.c_str(), "a");
    }

    static void closeFile() {
        if (s_logFile) { fclose(s_logFile); s_logFile = nullptr; }
    }

    static void print(LogLevel level, const char* fmt, ...) {
        if (level > s_level) return;
        
        std::lock_guard<std::mutex> lock(s_mutex);
        
        // Timestamp
        time_t now = time(nullptr);
        struct tm* tm_info = localtime(&now);
        char timestamp[32];
        strftime(timestamp, sizeof(timestamp), "[%H:%M:%S]", tm_info);
        
        // Level color
        const char* color = "";
        const char* tag = "";
        switch (level) {
            case LogLevel::Error:   color = CLR_RED;    tag = "ERROR"; break;
            case LogLevel::Warning: color = CLR_YELLOW; tag = "WARN "; break;
            case LogLevel::Info:    color = CLR_GREEN;  tag = "INFO "; break;
            case LogLevel::Debug:   color = CLR_CYAN;   tag = "DEBUG"; break;
            default: break;
        }
        
        printf("%s %s%s" CLR_RESET " ", timestamp, color, tag);

        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);

        printf("\n");
        fflush(stdout);

        if (s_logFile) {
            fprintf(s_logFile, "%s %s ", timestamp, tag);
            va_list args2;
            va_start(args2, fmt);
            vfprintf(s_logFile, fmt, args2);
            va_end(args2);
            fprintf(s_logFile, "\n");
            fflush(s_logFile);
        }
    }

    // Special formatted outputs
    static void accepted(uint64_t accepted, uint64_t rejected) {
        std::lock_guard<std::mutex> lock(s_mutex);
        printTimestamp();
        printf(CLR_GREEN "SHARE" CLR_RESET " Share accepted "
               CLR_GREEN "[%lu" CLR_RESET "/" CLR_RED "%lu" CLR_RESET "]\n",
               accepted, rejected);
        fflush(stdout);
        if (s_logFile) {
            printTimestampToFile();
            fprintf(s_logFile, "SHARE Share accepted [%lu/%lu]\n", accepted, rejected);
            fflush(s_logFile);
        }
    }
    
    static void rejected(uint64_t accepted, uint64_t rejected, const char* reason = nullptr) {
        std::lock_guard<std::mutex> lock(s_mutex);
        printTimestamp();
        if (reason) {
            printf(CLR_RED "SHARE" CLR_RESET " Share rejected "
                   CLR_GREEN "[%lu" CLR_RESET "/" CLR_RED "%lu" CLR_RESET "] %s\n",
                   accepted, rejected, reason);
        } else {
            printf(CLR_RED "SHARE" CLR_RESET " Share rejected "
                   CLR_GREEN "[%lu" CLR_RESET "/" CLR_RED "%lu" CLR_RESET "]\n",
                   accepted, rejected);
        }
        fflush(stdout);
        if (s_logFile) {
            printTimestampToFile();
            if (reason) {
                fprintf(s_logFile, "SHARE Share rejected [%lu/%lu] %s\n", accepted, rejected, reason);
            } else {
                fprintf(s_logFile, "SHARE Share rejected [%lu/%lu]\n", accepted, rejected);
            }
            fflush(s_logFile);
        }
    }
    
    static void hashrate(double hr, uint64_t accepted, uint64_t rejected) {
        std::lock_guard<std::mutex> lock(s_mutex);
        printTimestamp();

        // Format hashrate (auto kH/s or H/s)
        double hrDisplay = hr;
        const char* unit = "H/s";
        if (hrDisplay >= 1000.0) {
            hrDisplay /= 1000.0;
            unit = "kH/s";
        }

        printf(CLR_CYAN "STATS" CLR_RESET " %.2f %s "
               CLR_GREEN "A:%lu" CLR_RESET " " CLR_RED "R:%lu" CLR_RESET "\n",
               hrDisplay, unit, accepted, rejected);
        fflush(stdout);
        if (s_logFile) {
            printTimestampToFile();
            fprintf(s_logFile, "STATS %.2f %s A:%lu R:%lu\n", hrDisplay, unit, accepted, rejected);
            fflush(s_logFile);
        }
    }
    
    static void share(uint64_t diff) {
        std::lock_guard<std::mutex> lock(s_mutex);
        printTimestamp();

        // Format diff (auto K/M)
        const char* unit = "";
        double d = static_cast<double>(diff);
        if (d >= 1000000.0) {
            d /= 1000000.0;
            unit = "M";
        } else if (d >= 1000.0) {
            d /= 1000.0;
            unit = "K";
        }

        printf(CLR_MAGENTA "SHARE" CLR_RESET " Share found! diff: %.1f%s\n", d, unit);
        fflush(stdout);
        if (s_logFile) {
            printTimestampToFile();
            fprintf(s_logFile, "SHARE Share found! diff: %.1f%s\n", d, unit);
            fflush(s_logFile);
        }
    }

    static void error(const char* fmt, ...) {
        if (s_level < LogLevel::Error) return;
        va_list args;
        va_start(args, fmt);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        print(LogLevel::Error, "%s", buf);
    }

    static void warn(const char* fmt, ...) {
        if (s_level < LogLevel::Warning) return;
        va_list args;
        va_start(args, fmt);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        print(LogLevel::Warning, "%s", buf);
    }

    static void info(const char* fmt, ...) {
        if (s_level < LogLevel::Info) return;
        va_list args;
        va_start(args, fmt);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        print(LogLevel::Info, "%s", buf);
    }

    static void debug(const char* fmt, ...) {
        if (s_level < LogLevel::Debug) return;
        va_list args;
        va_start(args, fmt);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        print(LogLevel::Debug, "%s", buf);
    }

private:
    static void printTimestamp() {
        time_t now = time(nullptr);
        struct tm* tm_info = localtime(&now);
        char timestamp[32];
        strftime(timestamp, sizeof(timestamp), "[%H:%M:%S]", tm_info);
        printf("%s ", timestamp);
    }

    static void printTimestampToFile() {
        time_t now = time(nullptr);
        struct tm* tm_info = localtime(&now);
        char timestamp[32];
        strftime(timestamp, sizeof(timestamp), "[%H:%M:%S]", tm_info);
        fprintf(s_logFile, "%s ", timestamp);
    }

    static inline LogLevel s_level = LogLevel::Info;
    static inline std::mutex s_mutex;
    static inline FILE* s_logFile = nullptr;
};

} // namespace zenrx
