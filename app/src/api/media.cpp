/*
    anime::Stream helpers — see api/media.hpp.
*/

#include "api/media.hpp"

#include <sstream>

namespace anime {

std::string Stream::mpvExtra() const {
    // mpv exposes user-agent/referrer as plain string options; return them with
    // a leading comma so they append to the base option string setUrl() builds.
    std::ostringstream ss;
    ss << ",user-agent=\"" << USER_AGENT << "\"";
    if (!referer.empty()) ss << ",referrer=\"" << referer << "\"";
    return ss.str();
}

}  // namespace anime
