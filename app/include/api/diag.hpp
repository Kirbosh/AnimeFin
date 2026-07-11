/*
    Runtime diagnostics for the anime providers.

    Because the sites can't be reached from the build/test environment, this
    records the whole live pipeline — every request, its status/size, Cloudflare
    detection, parse counts, resolver steps and the final stream handed to mpv —
    to a plain-text log on the device. When a parse comes back empty it also
    saves the raw response, which is exactly what's needed to fix markup drift
    remotely. Share `diag::logPath()` to hand the whole trail over.
*/

#pragma once

#include <string>

namespace diag {

/// Append one timestamped line to the diagnostics log (thread-safe).
void log(const std::string& line);

/// Save a raw response body (truncated) next to the log for later inspection;
/// returns the file path written, and logs a pointer to it.
std::string dump(const std::string& tag, const std::string& url, const std::string& body);

/// True if a response looks like a Cloudflare / JS challenge interstitial
/// rather than the real page.
bool looksBlocked(const std::string& body);

/// Absolute path of the diagnostics log file (under the app's config dir).
std::string logPath();

}  // namespace diag
