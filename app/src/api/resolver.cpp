/*
    Shared embed resolver for AnimeFin — see api/media.hpp.

    Turns a chosen streaming server (an embed page url) into a direct,
    mpv-playable stream. Host-agnostic: it routes ok.ru / Mixdrop / Fembed to
    dedicated readers and falls back to a generic packed-JS / jwplayer sniffer
    that covers StreamWish, Filemoon, Voe, mp4upload and friends. The packer
    unpacker and url sniffers are pure and exercised by the offline test harness.

    The scraping primitives were ported from a reference Switch anime scraper and
    generalised so any provider can share them.
*/

#include "api/media.hpp"
#include "api/diag.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <regex>

namespace anime {

namespace {

// Generic request options: pose as a desktop browser and send the embed's own
// origin as the referrer (most embed hosts gate on a plausible referrer).
HTTP::Header browserHeaders(const std::string& referer) {
    return {"User-Agent: " + USER_AGENT, "Referer: " + referer};
}

std::string originOf(const std::string& url) {
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) return "";
    size_t slash = url.find('/', scheme + 3);
    return slash == std::string::npos ? url : url.substr(0, slash);
}

std::string httpGet(const std::string& url) {
    try {
        std::string ref = originOf(url);
        std::string body = HTTP::get(url, browserHeaders(ref.empty() ? url : ref + "/"), HTTP::Timeout{8000});
        diag::log("resolve GET " + url + " -> " + std::to_string(body.size()) + " bytes");
        return body;
    } catch (const std::exception& e) {
        diag::log("resolve GET " + url + " -> ERROR: " + e.what());
        throw;
    }
}

void replaceAll(std::string& subject, const std::string& search, const std::string& repl) {
    if (search.empty()) return;
    size_t pos = 0;
    while ((pos = subject.find(search, pos)) != std::string::npos) {
        subject.replace(pos, search.length(), repl);
        pos += repl.length();
    }
}

/// Substring from `get` to `delim` (auto-detecting a matching closer when delim
/// is empty), mirroring the reference scrapElement.
std::string scrapElement(const std::string& content, const std::string& get, const std::string& delim = "") {
    if (content.empty()) return "";
    size_t val1 = content.find(get);
    if (val1 == std::string::npos) return "";
    std::string marker = delim;
    if (marker.empty()) {
        marker = content.substr(val1 == 0 ? 0 : val1 - 1, 1);
        replaceAll(marker, ">", "<");
        replaceAll(marker, "{", "}");
        replaceAll(marker, "[", "]");
        replaceAll(marker, "(", ")");
    }
    size_t val2 = content.find(marker, val1 + get.length() + 1);
    if (val2 == std::string::npos) return content.substr(val1);
    return content.substr(val1, val2 - val1);
}

std::vector<std::string> split(const std::string& s, const std::string& delimiter) {
    std::vector<std::string> res;
    size_t start = 0, end, len = delimiter.length();
    while ((end = s.find(delimiter, start)) != std::string::npos) {
        if (end > start) res.push_back(s.substr(start, end - start));
        start = end + len;
    }
    if (start < s.size()) res.push_back(s.substr(start));
    return res;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

/// Value of a single base-N "digit" in the packer's 0-9a-zA-Z alphabet.
int packDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 36;
    return -1;
}

/// Decode a whole token to its keyword index (base = radix).
long packDecode(const std::string& token, int radix) {
    long v = 0;
    for (char c : token) {
        int d = packDigit(c);
        if (d < 0 || d >= radix) return -1;
        v = v * radix + d;
    }
    return v;
}

}  // namespace

std::string Stream::mpvExtra() const {
    std::ostringstream ss;
    ss << ",user-agent=\"" << USER_AGENT << "\"";
    if (!referer.empty()) ss << ",referrer=\"" << referer << "\"";
    return ss.str();
}

// ---- Dean-Edwards p.a.c.k.e.r unpacker + stream sniffing (pure) -----------

std::string unpackPacked(const std::string& source) {
    // Locate the classic invocation tail: }('<payload>',<radix>,<count>,'<k>'.split('|')
    size_t body = source.find("}(");
    if (body == std::string::npos) return "";
    size_t pStart = source.find('\'', body);
    if (pStart == std::string::npos) return "";

    // Read the payload, honouring \' and \\ escapes.
    std::string payload;
    size_t i = pStart + 1;
    for (; i < source.size(); i++) {
        char c = source[i];
        if (c == '\\' && i + 1 < source.size()) {
            char n = source[++i];
            if (n == '\'' || n == '\\')
                payload += n;
            else {
                payload += '\\';
                payload += n;
            }
        } else if (c == '\'') {
            break;
        } else {
            payload += c;
        }
    }
    if (i >= source.size()) return "";

    // Expect: ',<radix>,<count>,'
    size_t p = source.find(',', i);
    if (p == std::string::npos) return "";
    size_t radixEnd = source.find(',', p + 1);
    size_t countEnd = source.find(',', radixEnd + 1);
    if (radixEnd == std::string::npos || countEnd == std::string::npos) return "";
    int radix = atoi(trim(source.substr(p + 1, radixEnd - p - 1)).c_str());
    if (radix < 2) radix = 62;

    size_t kOpen = source.find('\'', countEnd);
    if (kOpen == std::string::npos) return "";
    size_t kClose = source.find('\'', kOpen + 1);
    if (kClose == std::string::npos) return "";
    std::vector<std::string> keywords = split(source.substr(kOpen + 1, kClose - kOpen - 1), "|");

    // Replace each word token with keywords[decode(token)] when present.
    std::string out;
    std::string token;
    auto flush = [&]() {
        if (token.empty()) return;
        long idx = packDecode(token, radix);
        if (idx >= 0 && idx < (long)keywords.size() && !keywords[idx].empty())
            out += keywords[idx];
        else
            out += token;
        token.clear();
    };
    for (char c : payload) {
        if (std::isalnum((unsigned char)c) || c == '_') {
            token += c;
        } else {
            flush();
            out += c;
        }
    }
    flush();
    return out;
}

/// A url is a static asset (not a stream) if its path ends in one of these.
static bool isAssetUrl(const std::string& u) {
    std::string p = u.substr(0, u.find('?'));
    std::transform(p.begin(), p.end(), p.begin(), ::tolower);
    for (const std::string ext :
        {".css", ".js", ".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp", ".woff", ".woff2", ".ico", ".json"})
        if (p.size() >= ext.size() && p.compare(p.size() - ext.size(), ext.size(), ext) == 0) return true;
    return false;
}

std::string findStreamUrl(const std::string& text) {
    auto norm = [](std::string u) {
        if (u.rfind("//", 0) == 0) u = "https:" + u;
        return u;
    };
    // Authoritative first: explicit file/source/wurl fields. Iterate all matches
    // and skip static assets (a small embed page can otherwise yield a .css url).
    static const std::regex fieldRe(
        R"#((?:"file"|'file'|file|"wurl"|wurl|"source"|'source'|"hls"|hls|"videoUrl")\s*[:=]\s*["']((?:https?:)?\/\/[^"']+)["'])#");
    for (std::sregex_iterator it(text.begin(), text.end(), fieldRe), end; it != end; ++it) {
        std::string u = norm((*it).str(1));
        if (!isAssetUrl(u)) return u;
    }

    // Otherwise a bare HLS, then mp4 url anywhere in the markup.
    std::smatch m;
    static const std::regex bareRe[] = {
        std::regex(R"((?:https?:)?\/\/[^"'\\\s<>]+\.m3u8[^"'\\\s<>]*)"),
        std::regex(R"((?:https?:)?\/\/[^"'\\\s<>]+\.mp4[^"'\\\s<>]*)"),
    };
    for (const auto& re : bareRe)
        if (std::regex_search(text, m, re)) return norm(m.str(0));
    return "";
}

std::string parseOkruOptions(const std::string& html) {
    // ok.ru embeds a `data-options="<html-entity-encoded json>"` blob whose
    // flashvars.metadata is itself a JSON string holding the video list.
    size_t s = html.find("data-options=\"");
    if (s == std::string::npos) return "";
    s += 14;  // strlen("data-options=\"")
    size_t end = html.find('"', s);
    if (end == std::string::npos) return "";
    std::string json = html.substr(s, end - s);
    replaceAll(json, "&quot;", "\"");
    replaceAll(json, "&amp;", "&");
    if (!nlohmann::json::accept(json)) return "";

    nlohmann::json j = nlohmann::json::parse(json);
    std::string meta = j.value("/flashvars/metadata"_json_pointer, std::string());
    if (meta.empty() || !nlohmann::json::accept(meta)) return "";
    nlohmann::json m = nlohmann::json::parse(meta);

    static const std::vector<std::string> rank = {
        "mobile", "lowest", "low", "sd", "hd", "full", "quad", "ultra"};
    std::string best;
    int bestRank = -1;
    for (auto& v : m.value("videos", nlohmann::json::array())) {
        std::string name = v.value("name", "");
        std::string url = v.value("url", "");
        if (url.empty()) continue;
        int r = 0;
        for (size_t k = 0; k < rank.size(); k++)
            if (rank[k] == name) r = (int)k;
        if (r >= bestRank) {
            bestRank = r;
            best = url;
        }
    }
    if (best.rfind("//", 0) == 0) best = "https:" + best;
    return best;
}

// ---------------------------------------------------------------- resolvers

namespace {

/// Unpack every Dean-Edwards packed block on a page (players are often preceded
/// by packed ad scripts, so the first block isn't necessarily the right one).
std::string unpackAll(const std::string& page) {
    std::string all;
    size_t pos = 0;
    while ((pos = page.find("}(", pos)) != std::string::npos) {
        std::string seg = unpackPacked(page.substr(pos));
        if (!seg.empty()) all += seg + "\n";
        pos += 2;
    }
    return all;
}

/// Fetch an embed page and dig out its direct stream: try the raw markup, then
/// the unpacked script(s), then follow one level of iframe. Dumps the page when
/// nothing is found so the real format can be inspected from the device logs.
Stream resolveGeneric(const std::string& embedUrl, int depth = 1) {
    Stream out;
    std::string page = httpGet(embedUrl);
    if (page.empty()) return out;

    std::string url = findStreamUrl(page);
    if (url.empty()) {
        std::string unpacked = unpackAll(page);
        if (!unpacked.empty()) url = findStreamUrl(unpacked);
    }
    if (url.empty() && depth > 0) {
        std::string frame = scrapElement(page, "<iframe", "</iframe");
        std::string src = scrapElement(frame, "src=\"", "\"");
        replaceAll(src, "src=\"", "");
        if (!src.empty()) {
            if (src.rfind("//", 0) == 0)
                src = "https:" + src;
            else if (src.rfind("/", 0) == 0)
                src = originOf(embedUrl) + src;
            // only follow a real http(s) frame (avoids about:blank, data:, javascript:)
            if (src.rfind("http", 0) == 0 && src != embedUrl) return resolveGeneric(src, depth - 1);
        }
    }
    if (!url.empty()) {
        out.url = url;
        out.referer = originOf(embedUrl) + "/";
    } else {
        diag::dump("resolve_generic", embedUrl, page);
    }
    return out;
}

/// Mixdrop: unpack the embed's packed script and read MDCore.wurl.
Stream resolveMixdrop(const std::string& embedUrl) {
    Stream out;
    std::string page = httpGet(embedUrl);
    std::string unpacked = unpackAll(page);
    std::string url = findStreamUrl(unpacked.empty() ? page : unpacked);
    if (url.empty()) url = findStreamUrl(page);
    if (!url.empty()) {
        out.url = url;
        out.referer = originOf(embedUrl) + "/";
    } else {
        diag::dump("resolve_mixdrop", embedUrl, unpacked.empty() ? page : unpacked);
    }
    return out;
}

/// ok.ru: read the metadata blob and pick the best mp4.
Stream resolveOkru(const std::string& embedUrl) {
    Stream out;
    std::string page = httpGet(embedUrl);
    std::string url = parseOkruOptions(page);
    if (!url.empty()) {
        out.url = url;
        out.referer = "https://ok.ru/";
    }
    return out;
}

/// Fembed-style API: POST to /api/source/<id>, take the top quality.
Stream resolveFembed(const std::string& embedUrl) {
    Stream out;
    std::string api = embedUrl;
    replaceAll(api, "/v/", "/api/source/");
    replaceAll(api, "/f/", "/api/source/");
    std::string resp = HTTP::post(api, "", browserHeaders(originOf(embedUrl) + "/"), HTTP::Timeout{8000});
    if (nlohmann::json::accept(resp)) {
        nlohmann::json j = nlohmann::json::parse(resp);
        auto data = j.value("data", nlohmann::json::array());
        if (data.is_array() && !data.empty()) {
            out.url = data.back().value("file", "");
            out.referer = originOf(embedUrl) + "/";
        }
    }
    return out;
}

}  // namespace

Stream resolveEmbed(const std::string& serverName, const std::string& url) {
    // Some sources (e.g. Monoschinos) hand out a host's file/download page rather
    // than its embed page. Normalise the common ones to their embed form, which is
    // what the readers below expect.
    std::string u = url;
    if (u.find("mp4upload.com") != std::string::npos && u.find("embed-") == std::string::npos) {
        size_t last = u.find_last_of('/');
        std::string code = last == std::string::npos ? "" : u.substr(last + 1);
        size_t dot = code.find('.');
        if (dot != std::string::npos) code = code.substr(0, dot);
        if (!code.empty()) u = "https://www.mp4upload.com/embed-" + code + ".html";
    }
    if (u.find("mixdrop") != std::string::npos) {
        size_t f = u.find("/f/");
        if (f != std::string::npos) u.replace(f, 3, "/e/");
    }

    // Mega streams are end-to-end encrypted (the key lives in the url fragment
    // and is decrypted in-browser); they can't be turned into a plain url here.
    if (u.find("mega.nz") != std::string::npos || u.find("mega.co.nz") != std::string::npos) return {};

    std::string name = serverName;
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    auto has = [&](const char* s) { return name.find(s) != std::string::npos || u.find(s) != std::string::npos; };

    if (has("okru") || has("ok.ru")) return resolveOkru(u);
    if (has("mixdrop") || has("mdfx") || has("mdbekjwqa") || has("mdy48tn97")) return resolveMixdrop(u);
    if (has("fembed") || has("/api/source/")) return resolveFembed(u);
    // StreamWish / Filemoon / Voe / mp4upload / sw and friends.
    return resolveGeneric(u);
}

}  // namespace anime
