/*
    AnimeFLV provider for AnimeFin — see api/animeflv.hpp.

    Scrapes www3.animeflv.net. The episode list comes from the page's own
    `var episodes = [[n,id],...]` array (exact, not a guessed range), and the
    server list from `var videos = {SUB:[...], LAT:[...]}`, so subtitled and
    Latino-dubbed sources are labelled. Stream resolution reuses jk's shared
    embed resolver.
*/

#include "api/animeflv.hpp"
#include "api/diag.hpp"

#include <nlohmann/json.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/core/thread.hpp>
#include <algorithm>
#include <cctype>

namespace flv {

static const std::string USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

namespace {

HTTP::Header headers() {
    // Look like a real browser *navigation*. Crucially do NOT send
    // X-Requested-With: AnimeFLV withholds the server-side `var videos = {...}`
    // list from requests that look like XHR/scrapers, which is why episodes
    // came back with an empty video list.
    return {
        "User-Agent: " + USER_AGENT,
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8",
        "Accept-Language: es-ES,es;q=0.9,en;q=0.8",
        "Referer: " + HOST,
    };
}

std::string httpGet(const std::string& url) {
    try {
        std::string body = HTTP::get(url, headers(), HTTP::Timeout{8000});
        bool blocked = diag::looksBlocked(body);
        diag::log("flv GET " + url + " -> " + std::to_string(body.size()) + " bytes" + (blocked ? " [BLOCKED?]" : ""));
        if (blocked) diag::dump("flv_blocked", url, body);
        return body;
    } catch (const std::exception& e) {
        diag::log("flv GET " + url + " -> ERROR: " + e.what());
        throw;
    }
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.length(), to);
        p += to.length();
    }
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

/// Substring from `get` up to `delim` (exclusive), keeping the opener; empty
/// when `get` is absent.
std::string between(const std::string& content, const std::string& get, const std::string& delim) {
    size_t a = content.find(get);
    if (a == std::string::npos) return "";
    a += get.length();
    size_t b = content.find(delim, a);
    if (b == std::string::npos) return "";
    return content.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s, const std::string& d) {
    std::vector<std::string> r;
    size_t start = 0, end, len = d.length();
    while ((end = s.find(d, start)) != std::string::npos) {
        if (end > start) r.push_back(s.substr(start, end - start));
        start = end + len;
    }
    if (start < s.size()) r.push_back(s.substr(start));
    return r;
}

std::string decode(std::string s) {
    replaceAll(s, "&quot;", "\"");
    replaceAll(s, "&#039;", "'");
    replaceAll(s, "&amp;", "&");
    replaceAll(s, "&aacute;", "á");
    replaceAll(s, "&eacute;", "é");
    replaceAll(s, "&iacute;", "í");
    replaceAll(s, "&oacute;", "ó");
    replaceAll(s, "&uacute;", "ú");
    replaceAll(s, "&ntilde;", "ñ");
    return trim(s);
}

std::string toAbs(const std::string& u) {
    if (u.rfind("http", 0) == 0) return u;
    if (u.rfind("//", 0) == 0) return "https:" + u;
    if (u.rfind("/", 0) == 0) return "https://www3.animeflv.net" + u;
    return HOST + u;
}

/// Poster/cover url for an anime numeric id.
std::string coverFor(const std::string& id) {
    return "https://www3.animeflv.net/uploads/animes/covers/" + id + ".jpg";
}

/// Recent items carry a wide episode thumbnail; turn it into the portrait cover
/// so the poster grid isn't cropped. Falls back to the thumbnail if unknown.
std::string coverFromThumb(const std::string& thumb) {
    size_t sp = thumb.find("/screenshots/");
    if (sp != std::string::npos) {
        size_t a = sp + 13;
        size_t b = thumb.find('/', a);
        if (b != std::string::npos) return coverFor(thumb.substr(a, b - a));
    }
    if (thumb.find("/thumbs/") != std::string::npos) {
        std::string r = thumb;
        replaceAll(r, "/thumbs/", "/covers/");
        return r;
    }
    return thumb;
}

}  // namespace

std::string slugOf(const std::string& url) {
    std::string s = url;
    size_t q = s.find('?');
    if (q != std::string::npos) s = s.substr(0, q);
    while (!s.empty() && s.back() == '/') s.pop_back();
    size_t last = s.find_last_of('/');
    if (last != std::string::npos) s = s.substr(last + 1);
    // /ver/<slug>-<n> -> strip the trailing episode number
    size_t dash = s.find_last_of('-');
    if (dash != std::string::npos) {
        std::string tail = s.substr(dash + 1);
        if (!tail.empty() && std::all_of(tail.begin(), tail.end(), ::isdigit)) s = s.substr(0, dash);
    }
    return s;
}

// ------------------------------------------------------------------- parsers

std::vector<jk::AnimeCard> parseRecent(const std::string& html) {
    std::vector<jk::AnimeCard> out;
    std::string list = between(html, "ListEpisodios", "</ul>");
    if (list.empty()) list = html;

    for (auto& piece : split(list, "href=\"/ver/")) {
        size_t end = piece.find('"');
        if (end == std::string::npos) continue;
        std::string verPath = piece.substr(0, end);
        // Only a clean slug-with-number is a real link; this also skips the
        // pre-match chunk that split() yields before the first href.
        if (verPath.empty() ||
            !std::all_of(verPath.begin(), verPath.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '-'; }))
            continue;

        jk::AnimeCard c;
        c.slug = slugOf("/ver/" + verPath);
        c.url = HOST + "anime/" + c.slug;
        c.poster = coverFromThumb(toAbs(between(piece, "src=\"", "\"")));
        std::string title = decode(between(piece, "Title\">", "<"));
        c.title = title.empty() ? c.slug : title;
        c.extra = decode(between(piece, "Capi\">", "<"));
        if (!c.slug.empty()) out.push_back(std::move(c));
    }
    return out;
}

std::vector<jk::AnimeCard> parseBrowse(const std::string& html) {
    std::vector<jk::AnimeCard> out;
    for (auto& art : split(html, "<article class=\"Anime")) {
        std::string slug;
        std::string href = between(art, "href=\"/anime/", "\"");
        if (href.empty()) continue;
        slug = href;

        jk::AnimeCard c;
        c.slug = slug;
        c.url = HOST + "anime/" + slug;
        c.poster = toAbs(between(art, "src=\"", "\""));
        c.title = decode(between(art, "Title\">", "<"));
        if (c.title.empty()) c.title = slug;
        c.extra = decode(between(art, "class=\"Type", "<"));
        replaceAll(c.extra, "\">", "");  // strip the leftover class tail
        c.extra = trim(c.extra);
        out.push_back(std::move(c));
    }
    return out;
}

jk::AnimeDetail parseDetail(const std::string& slug, const std::string& html) {
    jk::AnimeDetail d;
    d.slug = slug;

    std::string id = between(html, "var anime_info = [\"", "\"");
    if (!id.empty()) d.poster = coverFor(id);

    // Title / synopsis / status / genres
    std::string title = between(html, "class=\"Title\">", "<");
    if (title.empty()) title = between(html, "<h1 class=\"Title\" itemprop=\"name\">", "<");
    d.title = decode(title);

    std::string desc = between(html, "class=\"Description\">", "</div>");
    std::string syn = between(desc, "<p>", "</p>");
    d.synopsis = decode(syn.empty() ? desc : syn);

    if (html.find("En emisi") != std::string::npos)
        d.status = "En emisión";
    else if (html.find("Finalizado") != std::string::npos)
        d.status = "Finalizado";

    std::string genres;
    std::string nav = between(html, "Nvgnrs", "</nav>");
    for (auto& g : split(nav, "genre")) {
        std::string name = between(g, "\">", "</a>");
        if (name.empty()) continue;
        if (!genres.empty()) genres += ", ";
        genres += decode(name);
    }
    d.genres = genres;
    d.type = "Anime";

    // Exact episode list: var episodes = [[num,id],[num,id],...];
    std::string eps = between(html, "var episodes = [", "];");
    std::vector<int> numbers;
    for (auto& pair : split(eps, "[")) {
        std::string numStr = pair.substr(0, pair.find(','));
        numStr = trim(numStr);
        if (numStr.empty()) continue;
        int n = atoi(numStr.c_str());
        if (n >= 0) numbers.push_back(n);
    }
    std::sort(numbers.begin(), numbers.end());
    for (int n : numbers) {
        jk::Episode e;
        e.number = n;
        e.url = HOST + "ver/" + slug + "-" + std::to_string(n);
        e.title = "Episodio " + std::to_string(n);
        // Distinct per-episode still (AnimeFLV's own screenshot); fall back to
        // the cover when the anime id is unknown.
        e.thumb = id.empty() ? d.poster
                             : "https://cdn.animeflv.net/screenshots/" + id + "/" + std::to_string(n) + "/3.jpg";
        d.episodes.push_back(std::move(e));
    }
    if (!numbers.empty()) {
        d.minEpisode = numbers.front();
        d.maxEpisode = numbers.back();
    }
    return d;
}

std::vector<jk::Server> parseServers(const std::string& html) {
    std::vector<jk::Server> out;
    std::string block = between(html, "var videos = ", "};");
    if (block.empty()) return out;
    block += "}";
    if (!nlohmann::json::accept(block)) return out;

    nlohmann::json j = nlohmann::json::parse(block);
    // SUB = Spanish subtitled, LAT = Latino dub; label each server accordingly.
    static const std::pair<const char*, const char*> langs[] = {{"SUB", "SUB"}, {"LAT", "LAT"}, {"ESP", "ESP"}};
    for (auto& [key, tag] : langs) {
        if (!j.contains(key) || !j[key].is_array()) continue;
        for (auto& s : j[key]) {
            std::string code = s.value("code", s.value("url", ""));
            if (code.empty()) continue;
            jk::Server sv;
            std::string title = s.value("title", s.value("server", "Servidor"));
            sv.name = title + " (" + tag + ")";
            sv.url = toAbs(code);
            out.push_back(std::move(sv));
        }
    }
    return out;
}

// ------------------------------------------------------------- async wrappers

void getRecent(std::function<void(std::vector<jk::AnimeCard>)> then, jk::OnError error) {
    brls::async([then, error]() {
        try {
            std::string html = httpGet(HOST);
            auto r = parseRecent(html);
            diag::log("flv recent: " + std::to_string(r.size()) + " cards");
            if (r.empty()) diag::dump("flv_recent_empty", HOST, html);
            brls::sync([then, r]() { then(r); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

void getDirectory(
    int page, std::function<void(std::vector<jk::AnimeCard>)> then, jk::OnError error, const std::string& order) {
    brls::async([page, order, then, error]() {
        try {
            std::string url = HOST + "browse?order=" + order + "&page=" + std::to_string(page);
            std::string html = httpGet(url);
            auto r = parseBrowse(html);
            diag::log("flv directory p" + std::to_string(page) + ": " + std::to_string(r.size()) + " cards");
            if (r.empty() && page == 1) diag::dump("flv_directory_empty", url, html);
            brls::sync([then, r]() { then(r); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

void search(const std::string& query, std::function<void(std::vector<jk::AnimeCard>)> then, jk::OnError error) {
    std::string q = query;
    brls::async([q, then, error]() {
        try {
            std::string url = HOST + "browse?q=" + HTTP::encode_form({{"q", q}}).substr(2);
            std::string html = httpGet(url);
            auto r = parseBrowse(html);
            diag::log("flv search '" + q + "': " + std::to_string(r.size()) + " results");
            if (r.empty()) diag::dump("flv_search_empty", url, html);
            brls::sync([then, r]() { then(r); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

void getDetail(const std::string& slug, std::function<void(jk::AnimeDetail)> then, jk::OnError error) {
    std::string key = slugOf(slug);
    brls::async([key, then, error]() {
        try {
            std::string url = HOST + "anime/" + key;
            std::string html = httpGet(url);
            auto d = parseDetail(key, html);
            diag::log("flv detail '" + key + "': " + std::to_string(d.episodes.size()) + " eps, status=" + d.status);
            if (d.episodes.empty()) diag::dump("flv_detail_empty", url, html);
            brls::sync([then, d]() { then(d); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

/// Extract embed iframes from a page (used for the mobile fallback, which tends
/// to server-render the player instead of loading it with JavaScript).
static std::vector<jk::Server> parseIframeServers(const std::string& html) {
    std::vector<jk::Server> out;
    size_t pos = 0;
    while ((pos = html.find("<iframe", pos)) != std::string::npos) {
        std::string frag = between(html.substr(pos, 400), "src=\"", "\"");
        pos += 7;
        if (frag.empty()) continue;
        std::string url = toAbs(frag);
        if (url.rfind("http", 0) != 0) continue;
        jk::Server sv;
        sv.url = url;
        size_t h = url.find("://");
        size_t s = url.find('/', h + 3);
        sv.name = url.substr(h + 3, (s == std::string::npos ? url.size() : s) - (h + 3));
        out.push_back(std::move(sv));
    }
    return out;
}

void getServers(const std::string& episodeUrl, std::function<void(std::vector<jk::Server>)> then, jk::OnError error) {
    brls::async([episodeUrl, then, error]() {
        try {
            std::string html = httpGet(episodeUrl);
            auto r = parseServers(html);
            diag::log("flv servers " + episodeUrl + ": " + std::to_string(r.size()) + " found");

            // The desktop page loads its video list with JavaScript, so it can
            // come back empty. The mobile site server-renders embeds — try it,
            // and always dump its HTML so we can adapt the parser if needed.
            if (r.empty()) {
                diag::dump("flv_servers_empty", episodeUrl, html);
                std::string mobileUrl = episodeUrl;
                replaceAll(mobileUrl, "www4.animeflv.net", "m.animeflv.net");
                replaceAll(mobileUrl, "www3.animeflv.net", "m.animeflv.net");
                std::string mhtml = httpGet(mobileUrl);
                r = parseServers(mhtml);
                if (r.empty()) r = parseIframeServers(mhtml);
                diag::log("flv mobile servers " + mobileUrl + ": " + std::to_string(r.size()) + " found");
                if (r.empty()) diag::dump("flv_mobile_empty", mobileUrl, mhtml);
            }
            brls::sync([then, r]() { then(r); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

void resolve(const jk::Server& server, std::function<void(jk::Stream)> then, jk::OnError error) {
    brls::async([server, then, error]() {
        try {
            jk::Stream out = jk::resolveEmbedSync(server.name, server.url);
            if (out.url.empty()) {
                out.url = server.url;
                out.referer = HOST;
            }
            diag::log("flv resolve '" + server.name + "' [" + server.url + "] -> " +
                (out.url.empty() ? "FAILED" : out.url));
            if (out.url.empty()) throw std::runtime_error("No stream found");
            brls::sync([then, out]() { then(out); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            diag::log("flv resolve '" + server.name + "' -> ERROR: " + m);
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

}  // namespace flv
