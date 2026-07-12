/*
    TioAnime provider for AnimeFin — see api/tioanime.hpp.

    Scrapes tioanime.com. The win over AnimeFLV is that the episode page
    server-renders `var videos = [["Server","embed_url"], ...]`, so the server
    list needs no XHR and can be read straight out of the HTML. The detail page
    carries `var anime_info = ["id","title","slug",...]` and `var episodes = [n,
    ...]`, giving an exact episode list and a stable poster id
    (/uploads/portadas/<id>.jpg, /uploads/thumbs/<id>.jpg). Stream resolution
    reuses jk's shared embed resolver.
*/

#include "api/tioanime.hpp"
#include "api/diag.hpp"

#include <nlohmann/json.hpp>
#include <borealis/core/thread.hpp>
#include <algorithm>
#include <cctype>

namespace tio {

static const std::string USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

namespace {

HTTP::Header headers() {
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
        diag::log("tio GET " + url + " -> " + std::to_string(body.size()) + " bytes" + (blocked ? " [BLOCKED?]" : ""));
        if (blocked) diag::dump("tio_blocked", url, body);
        return body;
    } catch (const std::exception& e) {
        diag::log("tio GET " + url + " -> ERROR: " + e.what());
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

/// Strip HTML tags and decode the handful of entities TioAnime emits.
std::string decode(std::string s) {
    // drop any tags
    std::string out;
    bool intag = false;
    for (char c : s) {
        if (c == '<') intag = true;
        else if (c == '>') intag = false;
        else if (!intag) out += c;
    }
    replaceAll(out, "&quot;", "\"");
    replaceAll(out, "&#039;", "'");
    replaceAll(out, "&apos;", "'");
    replaceAll(out, "&amp;", "&");
    replaceAll(out, "&aacute;", "á");
    replaceAll(out, "&eacute;", "é");
    replaceAll(out, "&iacute;", "í");
    replaceAll(out, "&oacute;", "ó");
    replaceAll(out, "&uacute;", "ú");
    replaceAll(out, "&ntilde;", "ñ");
    replaceAll(out, "&Aacute;", "Á");
    replaceAll(out, "&Eacute;", "É");
    replaceAll(out, "&Iacute;", "Í");
    replaceAll(out, "&Oacute;", "Ó");
    replaceAll(out, "&Uacute;", "Ú");
    replaceAll(out, "&Ntilde;", "Ñ");
    return trim(out);
}

std::string toAbs(const std::string& u) {
    if (u.empty()) return u;
    if (u.rfind("http", 0) == 0) return u;
    if (u.rfind("//", 0) == 0) return "https:" + u;
    if (u.rfind("/", 0) == 0) return "https://tioanime.com" + u;
    return HOST + u;
}

/// Text of the element whose opening tag contains `afterToken` — finds that
/// token, then the next '>' and reads up to the following '<'. Handles both
/// `class="title">Text<` and `class="title" title="X">Text<`.
std::string tagInner(const std::string& piece, const std::string& afterToken) {
    size_t a = piece.find(afterToken);
    if (a == std::string::npos) return "";
    a += afterToken.length();
    size_t gt = piece.find('>', a);
    if (gt == std::string::npos) return "";
    size_t lt = piece.find('<', gt);
    if (lt == std::string::npos) return "";
    return piece.substr(gt + 1, lt - (gt + 1));
}

/// First real image url inside a card fragment: prefers a lazy `data-src`, then
/// `src`, and only accepts an `/uploads/` path or an absolute url.
std::string firstImage(const std::string& piece) {
    for (const char* key : {"data-src=\"", "src=\""}) {
        size_t pos = 0;
        while ((pos = piece.find(key, pos)) != std::string::npos) {
            size_t a = pos + std::string(key).length();
            size_t b = piece.find('"', a);
            pos = a;
            if (b == std::string::npos) break;
            std::string v = piece.substr(a, b - a);
            if (v.find("/uploads/") != std::string::npos || v.rfind("http", 0) == 0) return toAbs(v);
        }
    }
    // fall back to the very first src of any kind
    std::string v = between(piece, "src=\"", "\"");
    return toAbs(v);
}

bool validSlug(const std::string& s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '-'; });
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
    std::vector<std::string> seen;
    for (auto& piece : split(html, "href=\"/ver/")) {
        size_t end = piece.find('"');
        if (end == std::string::npos) continue;
        std::string verPath = piece.substr(0, end);
        std::string slug = slugOf("/ver/" + verPath);
        if (!validSlug(verPath) || slug.empty()) continue;
        if (std::find(seen.begin(), seen.end(), slug) != seen.end()) continue;
        seen.push_back(slug);

        jk::AnimeCard c;
        c.slug = slug;
        c.url = HOST + "anime/" + slug;
        c.poster = firstImage(piece);
        std::string title = decode(tagInner(piece, "class=\"title\""));
        if (title.empty()) title = decode(between(piece, "alt=\"", "\""));
        c.title = title.empty() ? slug : title;
        std::string ep = decode(tagInner(piece, "class=\"year\""));
        if (ep.empty()) ep = decode(tagInner(piece, "class=\"ep\""));
        c.extra = ep;
        out.push_back(std::move(c));
    }
    return out;
}

static std::vector<jk::AnimeCard> parseAnimeGrid(const std::string& html) {
    std::vector<jk::AnimeCard> out;
    std::vector<std::string> seen;
    for (auto& piece : split(html, "href=\"/anime/")) {
        size_t end = piece.find('"');
        if (end == std::string::npos) continue;
        std::string slug = piece.substr(0, end);
        while (!slug.empty() && slug.back() == '/') slug.pop_back();
        if (!validSlug(slug)) continue;
        if (std::find(seen.begin(), seen.end(), slug) != seen.end()) continue;
        seen.push_back(slug);

        jk::AnimeCard c;
        c.slug = slug;
        c.url = HOST + "anime/" + slug;
        c.poster = firstImage(piece);
        std::string title = decode(tagInner(piece, "class=\"title\""));
        if (title.empty()) title = decode(between(piece, "alt=\"", "\""));
        c.title = title.empty() ? slug : title;
        c.extra = decode(tagInner(piece, "class=\"type"));
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<jk::AnimeCard> parseDirectory(const std::string& html) { return parseAnimeGrid(html); }

jk::AnimeDetail parseDetail(const std::string& slug, const std::string& html) {
    jk::AnimeDetail d;
    d.slug = slug;

    // var anime_info = ["id","title","slug","next_air_date?"]
    std::string id;
    std::string infoRaw = between(html, "var anime_info = ", ";");
    if (!infoRaw.empty() && nlohmann::json::accept(infoRaw)) {
        nlohmann::json info = nlohmann::json::parse(infoRaw);
        if (info.is_array()) {
            if (info.size() > 0 && info[0].is_string()) id = info[0].get<std::string>();
            if (info.size() > 1 && info[1].is_string()) d.title = decode(info[1].get<std::string>());
            if (info.size() > 2 && info[2].is_string() && !info[2].get<std::string>().empty())
                d.slug = info[2].get<std::string>();
            if (info.size() > 3 && info[3].is_string()) {
                std::string next = info[3].get<std::string>();
                if (!next.empty() && next != "null") {
                    d.next = next;
                    d.status = "En emisión";
                }
            }
        }
    }
    if (id.empty()) id = between(html, "var anime_info = [\"", "\"");

    if (d.title.empty()) d.title = decode(tagInner(html, "class=\"title\""));
    if (!id.empty()) d.poster = HOST + "uploads/portadas/" + id + ".jpg";

    std::string syn = between(html, "class=\"sinopsis\"", "</p>");
    d.synopsis = decode(syn);

    std::string genresBlock = between(html, "class=\"genres\"", "</p>");
    std::string genres;
    for (auto& g : split(genresBlock, "<a")) {
        std::string name = decode(between(g, ">", "</a>"));
        if (name.empty()) continue;
        if (!genres.empty()) genres += ", ";
        genres += name;
    }
    if (genres.empty()) {
        for (auto& g : split(genresBlock, "<span")) {
            std::string name = decode(between(g, ">", "</span>"));
            if (name.empty()) continue;
            if (!genres.empty()) genres += ", ";
            genres += name;
        }
    }
    d.genres = genres;
    d.type = "Anime";

    if (d.status.empty()) {
        if (html.find("En emisi") != std::string::npos)
            d.status = "En emisión";
        else if (html.find("Finalizado") != std::string::npos)
            d.status = "Finalizado";
    }

    // var episodes = [12, 11, ..., 1];  (descending list of episode numbers)
    std::string epsRaw = between(html, "var episodes = ", ";");
    std::vector<int> numbers;
    if (!epsRaw.empty() && nlohmann::json::accept(epsRaw)) {
        nlohmann::json eps = nlohmann::json::parse(epsRaw);
        if (eps.is_array()) {
            for (auto& e : eps) {
                if (e.is_number()) numbers.push_back(e.get<int>());
                else if (e.is_array() && !e.empty() && e[0].is_number()) numbers.push_back(e[0].get<int>());
                else if (e.is_string()) numbers.push_back(atoi(e.get<std::string>().c_str()));
            }
        }
    }
    std::sort(numbers.begin(), numbers.end());
    std::string epSlug = d.slug.empty() ? slug : d.slug;
    std::string thumb = id.empty() ? d.poster : HOST + "uploads/thumbs/" + id + ".jpg";
    for (int n : numbers) {
        jk::Episode e;
        e.number = n;
        e.url = HOST + "ver/" + epSlug + "-" + std::to_string(n);
        e.title = "Episodio " + std::to_string(n);
        e.thumb = thumb;
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
    // var videos = [["Server","https://embed..."], ...];
    std::string raw = between(html, "var videos = ", ";");
    if (raw.empty()) return out;
    if (!nlohmann::json::accept(raw)) return out;

    nlohmann::json j = nlohmann::json::parse(raw);
    if (!j.is_array()) return out;
    for (auto& v : j) {
        if (!v.is_array() || v.empty()) continue;
        std::string name, url;
        for (auto& field : v) {
            if (!field.is_string()) continue;
            std::string s = field.get<std::string>();
            if (s.rfind("http", 0) == 0 || s.rfind("//", 0) == 0) {
                if (url.empty()) url = toAbs(s);
            } else if (name.empty()) {
                name = s;
            }
        }
        if (url.empty()) continue;
        jk::Server sv;
        sv.name = name.empty() ? "Servidor" : name;
        sv.url = url;
        out.push_back(std::move(sv));
    }
    return out;
}

// ------------------------------------------------------------- async wrappers

void getRecent(std::function<void(std::vector<jk::AnimeCard>)> then, jk::OnError error) {
    brls::async([then, error]() {
        try {
            std::string html = httpGet(HOST);
            auto r = parseRecent(html);
            diag::log("tio recent: " + std::to_string(r.size()) + " cards");
            if (r.empty()) diag::dump("tio_recent_empty", HOST, html);
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
            std::string url = HOST + "directorio?p=" + std::to_string(page);
            if (!order.empty() && order != "default") url += "&sort=" + order;
            std::string html = httpGet(url);
            auto r = parseDirectory(html);
            diag::log("tio directory p" + std::to_string(page) + ": " + std::to_string(r.size()) + " cards");
            if (r.empty() && page == 1) diag::dump("tio_directory_empty", url, html);
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
            std::string url = HOST + "directorio?q=" + HTTP::encode_form({{"q", q}}).substr(2);
            std::string html = httpGet(url);
            auto r = parseDirectory(html);
            diag::log("tio search '" + q + "': " + std::to_string(r.size()) + " results");
            if (r.empty()) diag::dump("tio_search_empty", url, html);
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
            diag::log("tio detail '" + key + "': " + std::to_string(d.episodes.size()) + " eps, status=" + d.status);
            if (d.episodes.empty()) diag::dump("tio_detail_empty", url, html);
            brls::sync([then, d]() { then(d); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

void getServers(const std::string& episodeUrl, std::function<void(std::vector<jk::Server>)> then, jk::OnError error) {
    brls::async([episodeUrl, then, error]() {
        try {
            std::string html = httpGet(episodeUrl);
            auto r = parseServers(html);
            diag::log("tio servers " + episodeUrl + ": " + std::to_string(r.size()) + " found");
            if (r.empty()) diag::dump("tio_servers_empty", episodeUrl, html);
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
            diag::log("tio resolve '" + server.name + "' [" + server.url + "] -> " +
                (out.url.empty() ? "FAILED" : out.url));
            if (out.url.empty()) throw std::runtime_error("No stream found");
            brls::sync([then, out]() { then(out); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            diag::log("tio resolve '" + server.name + "' -> ERROR: " + m);
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

}  // namespace tio
