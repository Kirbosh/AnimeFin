/*
    Native stream extractors for AnimeFin — implements anime::resolveEmbed().

    These are C++ ports of the recipes in Pigamer37/animeflv-stremio-addon
    (lib/streamParsing.js), which are themselves plain HTTP + text scraping — no
    browser, no server. Each host embed page exposes its real video URL in a
    predictable spot; we fetch the page and pull it out. All parsing is manual
    (no std::regex, which stack-overflows on large obfuscated pages).

    Ported hosts: YourUpload, MP4Upload, Voe, StreamWish, Filemoon, Vidhide,
    Hqq, Fembed, Okru, PDrain, HLS. (Mega is end-to-end encrypted — unsupported.)
*/

#include "api/media.hpp"
#include "api/http.hpp"
#include "api/diag.hpp"

#include <algorithm>
#include <cctype>

namespace anime {

namespace {

HTTP::Header headers(const std::string& referer) {
    HTTP::Header h = {"User-Agent: " + USER_AGENT,
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8", "Accept-Language: es-ES,es;q=0.9,en;q=0.8"};
    if (!referer.empty()) h.push_back("Referer: " + referer);
    return h;
}

std::string httpGet(const std::string& url, const std::string& referer = "") {
    try {
        std::string body = HTTP::get(url, headers(referer), HTTP::Timeout{8000});
        diag::log("extract GET " + url + " -> " + std::to_string(body.size()) + " bytes");
        return body;
    } catch (const std::exception& e) {
        diag::log("extract GET " + url + " -> ERROR: " + e.what());
        return "";
    }
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

/// scheme://host of a url.
std::string originOf(const std::string& url) {
    size_t s = url.find("://");
    if (s == std::string::npos) return "";
    size_t slash = url.find('/', s + 3);
    return slash == std::string::npos ? url : url.substr(0, slash);
}

/// Decode the escapes these pages use inside JS strings/attributes.
std::string normalize(std::string v) {
    auto rep = [&](const std::string& a, const std::string& b) {
        size_t p = 0;
        while ((p = v.find(a, p)) != std::string::npos) { v.replace(p, a.size(), b); p += b.size(); }
    };
    rep("\\u0026", "&");
    rep("\\/", "/");
    rep("&amp;", "&");
    rep("%3A", ":"); rep("%3a", ":");
    rep("%2F", "/"); rep("%2f", "/");
    rep("%3F", "?"); rep("%3f", "?");
    rep("%3D", "="); rep("%3d", "=");
    // trim
    size_t a = v.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = v.find_last_not_of(" \t\r\n");
    return v.substr(a, b - a + 1);
}

/// Reject analytics/asset urls that aren't the video (mirrors isLikelyVideoUrl).
bool likelyVideo(const std::string& url) {
    if (url.empty()) return false;
    std::string l = lower(url);
    for (const char* bad : {"cloudflareinsights", "google-analytics", "googletagmanager", "facebook.net",
             "beacon.min.js", ".js?", "analytics", "pixel", "bigbuckbunny", "sample-video", "placeholder"})
        if (l.find(bad) != std::string::npos) return false;
    return l.find(".mp4") != std::string::npos || l.find(".m3u8") != std::string::npos ||
        l.find("video") != std::string::npos || l.find("stream") != std::string::npos;
}

/// First quoted value of a `key: "url"` / `key = "url"` field (manual scan).
std::string fieldValue(const std::string& t, const std::string& key) {
    size_t p = 0;
    while ((p = t.find(key, p)) != std::string::npos) {
        size_t i = p + key.size();
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '"' || t[i] == '\'')) i++;
        if (i < t.size() && (t[i] == ':' || t[i] == '=')) {
            i++;
            while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) i++;
            if (i < t.size() && (t[i] == '"' || t[i] == '\'')) {
                char q = t[i++];
                size_t s = i;
                while (i < t.size() && t[i] != q) i++;
                std::string v = normalize(t.substr(s, i - s));
                if (v.rfind("//", 0) == 0) v = "https:" + v;
                if (v.rfind("http", 0) == 0) return v;
            }
        }
        p += key.size();
    }
    return "";
}

/// First bare http url containing `ext` (.m3u8 / .mp4).
std::string bareMedia(const std::string& t, const std::string& ext) {
    size_t e = 0;
    while ((e = t.find(ext, e)) != std::string::npos) {
        // url start = nearest real scheme marker before the extension. Must be
        // "https://"/"http://" (NOT bare "http", which also matches http-equiv=)
        // or a protocol-relative "//".
        size_t start = std::string::npos;
        for (const char* scheme : {"https://", "http://"}) {
            size_t p = t.rfind(scheme, e);
            if (p != std::string::npos && (start == std::string::npos || p > start)) start = p;
        }
        if (start == std::string::npos) {
            size_t d = t.rfind("//", e);
            if (d != std::string::npos) start = d;
        }
        if (start != std::string::npos && e - start < 2000) {
            size_t end = e + ext.size();
            while (end < t.size()) {
                char c = t[end];
                if (c == '"' || c == '\'' || c == '\\' || c == ' ' || c == '\n' || c == '\r' || c == '\t' ||
                    c == '<' || c == '>' || c == ')' || c == '(')
                    break;
                end++;
            }
            std::string v = normalize(t.substr(start, end - start));
            if (v.rfind("//", 0) == 0) v = "https:" + v;
            if (v.rfind("http", 0) == 0 && v.find("blob:") == std::string::npos) return v;
        }
        e += ext.size();
    }
    return "";
}

/// Generic: an explicit file/source/src field, else a bare hls/mp4 url.
std::string findVideo(const std::string& data) {
    for (const std::string key : {"file", "source", "src", "videoUrl", "hls"}) {
        std::string v = fieldValue(data, key);
        if (likelyVideo(v)) return v;
    }
    std::string v = bareMedia(data, ".m3u8");
    if (!v.empty()) return v;
    v = bareMedia(data, ".mp4");
    if (!v.empty()) return v;
    return "";
}

/// A quoted "…needle…" http string (used for MP4Upload's src:"…mp4").
std::string quotedContaining(const std::string& data, const std::string& needle) {
    size_t n = 0;
    while ((n = data.find(needle, n)) != std::string::npos) {
        size_t open = data.rfind('"', n);
        if (open != std::string::npos) {
            size_t close = data.find('"', n);
            if (close != std::string::npos) {
                std::string v = normalize(data.substr(open + 1, close - open - 1));
                if (v.rfind("http", 0) == 0) return v;
            }
        }
        n += needle.size();
    }
    return "";
}

std::string metaContent(const std::string& data, const std::string& prop) {
    size_t p = data.find(prop);
    if (p == std::string::npos) return "";
    size_t c = data.find("content=", p);
    if (c == std::string::npos) return "";
    c += 8;
    if (c < data.size() && (data[c] == '"' || data[c] == '\'')) {
        char q = data[c++];
        size_t s = c;
        while (c < data.size() && data[c] != q) c++;
        return normalize(data.substr(s, c - s));
    }
    return "";
}

// ---- per-host recipes (ported from streamParsing.js) ---------------------

/// A clean, directly-playable url: real scheme and no stray HTML/whitespace (so
/// a scrape that accidentally grabbed markup is never handed to mpv).
bool validMedia(const std::string& u) {
    if (u.rfind("https://", 0) != 0 && u.rfind("http://", 0) != 0) return false;
    if (u.size() > 2000) return false;
    for (char c : u)
        if (c == ' ' || c == '"' || c == '\'' || c == '<' || c == '>' || c == '\n' || c == '\r' || c == '\t')
            return false;
    return true;
}

Stream mk(const std::string& url, const std::string& referer) {
    Stream out;
    if (validMedia(url)) {
        out.url = url;
        out.referer = referer;
    }
    return out;
}

Stream yourUpload(const std::string& embed) {
    // <meta property="og:video" content="https://.../video.mp4">
    std::string data = httpGet(embed);
    return mk(metaContent(data, "og:video"), "https://yourupload.com");
}

Stream mp4Upload(const std::string& embed) {
    // a <script> with player src: "https://.../video.mp4"
    std::string data = httpGet(embed);
    std::string url = quotedContaining(data, ".mp4");
    if (url.empty()) url = findVideo(data);
    return mk(url, "https://a1.mp4upload.com");
}

Stream voe(const std::string& embed) {
    std::string data = httpGet(embed, originOf(embed));
    // Voe often bounces once via window.location.href='...'
    std::string redir = fieldValue(data, "location.href");
    if (redir.empty()) {
        size_t p = data.find("window.location.href");
        if (p != std::string::npos) {
            size_t q1 = data.find_first_of("'\"", p);
            if (q1 != std::string::npos) {
                size_t q2 = data.find(data[q1], q1 + 1);
                if (q2 != std::string::npos) {
                    std::string u = normalize(data.substr(q1 + 1, q2 - q1 - 1));
                    if (u.rfind("http", 0) == 0) redir = u;
                }
            }
        }
    }
    if (!redir.empty()) data = httpGet(redir, originOf(embed));
    return mk(findVideo(data), originOf(embed) + "/");
}

Stream pdrain(const std::string& embed) {
    // https://host/path/ID(?embed) -> https://host/api/file/ID
    std::string u = embed;
    size_t q = u.find('?');
    if (q != std::string::npos) u = u.substr(0, q);
    std::string origin = originOf(u);
    size_t last = u.find_last_of('/');
    if (last == std::string::npos || origin.empty()) return {};
    std::string id = u.substr(last + 1);
    if (id.empty()) return {};
    return mk(origin + "/api/file/" + id, originOf(embed) + "/");
}

Stream hls(const std::string& embed) {
    std::string u = embed;
    size_t p = u.find("/play/");
    if (p != std::string::npos) u.replace(p, 6, "/m3u8/");
    return mk(u, originOf(embed) + "/");
}

Stream generic(const std::string& embed) {
    // Voe/StreamWish/Filemoon/Vidhide/Hqq/Fembed/Okru all expose file/src or a
    // bare hls/mp4 url on the embed page.
    std::string data = httpGet(embed, originOf(embed));
    return mk(findVideo(data), originOf(embed) + "/");
}

}  // namespace

Stream resolveEmbed(const std::string& serverName, const std::string& url) {
    std::string n = lower(serverName);
    std::string u = lower(url);
    auto is = [&](const char* s) { return n.find(s) != std::string::npos || u.find(s) != std::string::npos; };

    if (is("mega.nz") || is("mega.co")) return {};  // end-to-end encrypted, unsupported
    // Hosts we have no working recipe for — skip instantly instead of spending a
    // timeout on a page we can't parse (VidGuard is also frequently down/523).
    if (is("vidguard") || is("vgfplay") || is("listeamed") || is("streamtape") || is("stape")) return {};
    if (is("yourupload")) return yourUpload(url);
    if (is("mp4upload")) return mp4Upload(url);
    if (is("pdrain")) return pdrain(url);
    if (n == "hls" || is("/play/") || is("/m3u8/")) return hls(url);
    if (is("voe")) return voe(url);
    // StreamWish (dhcplay), Filemoon (bysesukior), Vidhide (movearnpre), Hqq,
    // Fembed, Okru, Lulustream (luluvdo) and friends.
    return generic(url);
}

}  // namespace anime
