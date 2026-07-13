/*
    Stremio-addon backend for AnimeFin — see api/stremio.hpp.

    A thin JSON client over the addon's catalog/meta/stream resources. The addon
    resolves the video hosts on its own server, so the streams it returns carry
    plain, directly-playable URLs — resolve() just forwards them to mpv.
*/

#include "api/stremio.hpp"
#include "api/diag.hpp"
#include "api/http.hpp"

#include <nlohmann/json.hpp>
#include <borealis/core/thread.hpp>
#include <algorithm>

namespace strm {

static const std::string USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

const std::vector<Catalog>& catalogs() {
    // Order defines the source-picker order; the addon's paging unit per source.
    static const std::vector<Catalog> c = {
        {"animeflv", "AnimeFLV", 24},
        {"tioanime", "TioAnime", 20},
        {"animeav1", "AnimeAV1", 24},
        {"henaojara", "Henaojara", 24},
        {"animejara", "AnimeJara", 20},
        {"jkanime", "JKAnime", 30},
    };
    return c;
}

static size_t s_active = 0;
size_t activeCatalog() { return s_active; }
void setActiveCatalog(size_t i) {
    if (i < catalogs().size()) s_active = i;
}

namespace {

HTTP::Header headers() {
    return {"User-Agent: " + USER_AGENT, "Accept: application/json"};
}

std::string httpGet(const std::string& url) {
    // The addon runs on free hosting that cold-starts and stalls: a warm request
    // is ~3s, a cold one blows past a short timeout. Use a generous timeout and
    // retry — the first (failed) request tends to wake the server for the next.
    std::string lastErr;
    for (int attempt = 1; attempt <= 3; attempt++) {
        try {
            std::string body = HTTP::get(url, headers(), HTTP::Timeout{20000});
            diag::log("strm GET " + url + " -> " + std::to_string(body.size()) + " bytes" +
                (attempt > 1 ? " (attempt " + std::to_string(attempt) + ")" : ""));
            return body;
        } catch (const std::exception& e) {
            lastErr = e.what();
            diag::log("strm GET " + url + " attempt " + std::to_string(attempt) + " -> ERROR: " + lastErr);
        }
    }
    throw std::runtime_error(lastErr);
}

std::string joinGenres(const nlohmann::json& g) {
    if (!g.is_array()) return "";
    std::string out;
    for (auto& x : g) {
        if (!x.is_string()) continue;
        if (!out.empty()) out += ", ";
        out += x.get<std::string>();
    }
    return out;
}

/// Trailing integer of an episode id like "tioanime:one-piece:1073" -> 1073.
int trailingNumber(const std::string& s) {
    size_t end = s.find_last_of("0123456789");
    if (end == std::string::npos) return -1;
    size_t start = end;
    while (start > 0 && std::isdigit((unsigned char)s[start - 1])) start--;
    return atoi(s.substr(start, end - start + 1).c_str());
}

}  // namespace

// ------------------------------------------------------------------- parsers

std::vector<anime::AnimeCard> parseCatalog(const std::string& json) {
    std::vector<anime::AnimeCard> out;
    if (!nlohmann::json::accept(json)) return out;
    nlohmann::json j = nlohmann::json::parse(json);
    if (!j.contains("metas") || !j["metas"].is_array()) return out;
    for (auto& m : j["metas"]) {
        std::string id = m.value("id", "");
        if (id.empty()) continue;
        anime::AnimeCard c;
        c.slug = id;   // the stremio id, e.g. "tioanime:one-piece"
        c.url = id;    // routed through the addon; keep the id as the "url"
        c.title = m.value("name", id);
        c.poster = m.value("poster", "");
        c.extra = joinGenres(m.value("genres", nlohmann::json::array()));
        out.push_back(std::move(c));
    }
    return out;
}

anime::AnimeDetail parseMeta(const std::string& id, const std::string& json) {
    anime::AnimeDetail d;
    d.slug = id;
    d.type = "Anime";
    if (!nlohmann::json::accept(json)) return d;
    nlohmann::json j = nlohmann::json::parse(json);
    if (!j.contains("meta") || !j["meta"].is_object()) return d;
    nlohmann::json m = j["meta"];

    d.title = m.value("name", id);
    d.poster = m.value("poster", "");
    if (d.poster.empty()) d.poster = m.value("background", "");
    d.synopsis = m.value("description", "");
    d.genres = joinGenres(m.value("genres", nlohmann::json::array()));
    d.status = m.value("status", "");

    std::vector<std::pair<int, anime::Episode>> eps;
    if (m.contains("videos") && m["videos"].is_array()) {
        for (auto& v : m["videos"]) {
            std::string vid = v.value("id", "");
            if (vid.empty()) continue;
            int n = v.contains("episode") && v["episode"].is_number() ? v["episode"].get<int>()
                                                                       : trailingNumber(vid);
            if (n < 0) n = (int)eps.size() + 1;
            anime::Episode e;
            e.number = n;
            e.url = vid;  // "tioanime:one-piece:1073" — used for the stream lookup
            e.title = v.value("title", std::string("Episodio ") + std::to_string(n));
            e.thumb = v.value("thumbnail", d.poster);
            if (e.thumb.empty()) e.thumb = d.poster;
            eps.push_back({n, std::move(e)});
        }
    }
    std::sort(eps.begin(), eps.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& p : eps) d.episodes.push_back(std::move(p.second));
    if (!d.episodes.empty()) {
        d.minEpisode = d.episodes.front().number;
        d.maxEpisode = d.episodes.back().number;
    }
    return d;
}

std::vector<anime::Server> parseStreams(const std::string& json) {
    std::vector<anime::Server> out;
    if (!nlohmann::json::accept(json)) return out;
    nlohmann::json j = nlohmann::json::parse(json);
    if (!j.contains("streams") || !j["streams"].is_array()) return out;
    for (auto& s : j["streams"]) {
        std::string url = s.value("url", "");
        if (url.empty()) continue;  // externalUrl-only streams can't play in mpv
        anime::Server sv;
        sv.url = url;
        std::string name = s.value("name", "");
        std::string title = s.value("title", "");
        // The addon puts the host in `name` and quality/label in `title`.
        sv.name = !name.empty() ? name : (!title.empty() ? title : "Servidor");
        // Replace newlines the addon uses inside title/name for readability.
        std::replace(sv.name.begin(), sv.name.end(), '\n', ' ');

        // Headers the stream needs, if the addon specified them.
        if (s.contains("behaviorHints") && s["behaviorHints"].is_object()) {
            auto& bh = s["behaviorHints"];
            if (bh.contains("proxyHeaders") && bh["proxyHeaders"].is_object()) {
                nlohmann::json req = bh["proxyHeaders"].value("request", nlohmann::json::object());
                if (req.is_object()) sv.referer = req.value("Referer", req.value("referer", ""));
            }
        }
        out.push_back(std::move(sv));
    }
    return out;
}

// ------------------------------------------------------------- async wrappers

static std::string catUrl(const std::string& catId, const std::string& extra) {
    std::string u = BASE + "/catalog/series/" + catId;
    if (!extra.empty()) u += "/" + extra;
    return u + ".json";
}

void getRecent(std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error) {
    std::string cat = catalogs()[s_active].id;
    brls::async([cat, then, error]() {
        try {
            std::string url = catUrl(cat + "-onair", "");
            std::string body = httpGet(url);
            auto r = parseCatalog(body);
            diag::log("strm recent[" + cat + "]: " + std::to_string(r.size()) + " cards");
            if (r.empty()) diag::dump("strm_recent_empty", url, body);
            brls::sync([then, r]() { then(r); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

void getDirectory(
    int page, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error, const std::string&) {
    size_t idx = s_active;
    std::string cat = catalogs()[idx].id;
    int pageSize = catalogs()[idx].pageSize;
    brls::async([cat, page, pageSize, then, error]() {
        try {
            std::string extra = page > 1 ? "skip=" + std::to_string((page - 1) * pageSize) : "";
            std::string url = catUrl(cat, extra);
            std::string body = httpGet(url);
            auto r = parseCatalog(body);
            diag::log("strm directory[" + cat + "] p" + std::to_string(page) + ": " + std::to_string(r.size()) + " cards");
            if (r.empty() && page == 1) diag::dump("strm_directory_empty", url, body);
            brls::sync([then, r]() { then(r); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

void search(const std::string& query, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error) {
    std::string cat = catalogs()[s_active].id;
    std::string q = query;
    brls::async([cat, q, then, error]() {
        try {
            std::string extra = "search=" + HTTP::encode_form({{"q", q}}).substr(2);
            std::string url = catUrl(cat, extra);
            std::string body = httpGet(url);
            auto r = parseCatalog(body);
            diag::log("strm search[" + cat + "] '" + q + "': " + std::to_string(r.size()) + " results");
            if (r.empty()) diag::dump("strm_search_empty", url, body);
            brls::sync([then, r]() { then(r); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

void getDetail(const std::string& id, std::function<void(anime::AnimeDetail)> then, anime::OnError error) {
    std::string key = id;
    brls::async([key, then, error]() {
        try {
            std::string url = BASE + "/meta/series/" + key + ".json";
            std::string body = httpGet(url);
            auto d = parseMeta(key, body);
            diag::log("strm meta '" + key + "': " + std::to_string(d.episodes.size()) + " eps");
            if (d.episodes.empty()) diag::dump("strm_meta_empty", url, body);
            brls::sync([then, d]() { then(d); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

void getStreams(const std::string& episodeId, std::function<void(std::vector<anime::Server>)> then, anime::OnError error) {
    std::string key = episodeId;
    brls::async([key, then, error]() {
        try {
            std::string url = BASE + "/stream/series/" + key + ".json";
            std::string body = httpGet(url);
            auto r = parseStreams(body);
            diag::log("strm streams '" + key + "': " + std::to_string(r.size()) + " playable");
            if (r.empty()) diag::dump("strm_streams_empty", url, body);
            brls::sync([then, r]() { then(r); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

void resolve(const anime::Server& server, std::function<void(anime::Stream)> then, anime::OnError error) {
    brls::async([server, then, error]() {
        // The addon already resolved the host; the url is directly playable.
        anime::Stream out;
        out.url = server.url;
        out.referer = server.referer;
        diag::log("strm resolve '" + server.name + "' -> " + out.url);
        if (out.url.empty()) {
            if (error) brls::sync([error]() { error("No stream found"); });
            return;
        }
        brls::sync([then, out]() { then(out); });
    });
}

}  // namespace strm
