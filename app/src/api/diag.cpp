#include "api/diag.hpp"
#include "utils/config.hpp"  // AppConfig + AppVersion

#include <borealis/core/logger.hpp>
#include <fstream>
#include <mutex>
#include <ctime>
#include <cctype>
#include <algorithm>

namespace diag {

namespace {

std::mutex mutex;
bool headerWritten = false;
// Keep the log bounded so it can't fill a Switch SD card over a long session.
constexpr long MAX_LOG_BYTES = 4 * 1024 * 1024;
constexpr size_t MAX_DUMP_BYTES = 800 * 1024;

std::string dir() { return AppConfig::instance().configDir(); }

std::string stamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

/// Truncate the log if it has grown past the cap (called while locked).
void rotateIfBig(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (f && f.tellg() > MAX_LOG_BYTES) {
        std::ofstream(path, std::ios::trunc);  // wipe
        headerWritten = false;
    }
}

std::string sanitize(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum((unsigned char)c))
            out += c;
        else if (out.empty() || out.back() != '_')
            out += '_';
        if (out.size() >= 60) break;
    }
    return out.empty() ? "x" : out;
}

}  // namespace

std::string logPath() { return dir() + "/anime_diag.log"; }

void log(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex);
    const std::string path = logPath();
    rotateIfBig(path);

    std::ofstream f(path, std::ios::app);
    if (!f) return;
    if (!headerWritten) {
        f << "==== AnimeFin diagnostics — " << AppVersion::getVersion() << " ====\n";
        headerWritten = true;
    }
    f << "[" << stamp() << "] " << line << "\n";
    // Mirror to the borealis log too (visible on desktop / nxlink).
    brls::Logger::info("[diag] {}", line);
}

std::string dump(const std::string& tag, const std::string& url, const std::string& body) {
    std::string path = dir() + "/diag_" + sanitize(tag) + "_" + sanitize(url) + ".txt";
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (f) f.write(body.data(), std::min(body.size(), MAX_DUMP_BYTES));
    }
    log("  saved raw response (" + std::to_string(body.size()) + " bytes) to " + path);
    return path;
}

bool looksBlocked(const std::string& body) {
    // Common Cloudflare / challenge / rate-limit fingerprints.
    static const char* markers[] = {
        "Just a moment",
        "cf-browser-verification",
        "Checking your browser",
        "Attention Required",
        "__cf_chl",
        "cf-challenge",
        "Enable JavaScript and cookies",
        "Access denied",
        "Error 1015",  // rate limited
    };
    for (const char* m : markers)
        if (body.find(m) != std::string::npos) return true;
    // A near-empty body from a site that should return a full page is suspicious.
    return body.size() > 0 && body.size() < 256 && body.find("<html") != std::string::npos;
}

}  // namespace diag
