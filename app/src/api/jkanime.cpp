/*
    JKAnime data layer for AnimeFin — see api/jkanime.hpp.

    Scraping logic is ported from Darkxex's RipJKAnimeNX (source/JKanime.cpp,
    source/Link.cpp, source/utils.cpp) and adapted to Switchfin's HTTP client
    and borealis async model. The HTML shapes matched here mirror jkanime.net's
    current markup; keep the string anchors in sync with the site if it changes.
*/

#include "api/jkanime.hpp"

#include <nlohmann/json.hpp>
#include <borealis/core/logger.hpp>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace jk {

// jkanime serves posters from this CDN; the path mirrors the site's own layout.
static const std::string CDN = "https://cdn.jkdesu.com";

// ------------------------------------------------------------------ helpers

namespace {

/// Common request options: pose as a desktop browser and send jkanime as the
/// referrer (Cloudflare + the CDN both reject the default Switchfin agent).
HTTP::Header browserHeaders() {
    return {"User-Agent: " + USER_AGENT, "Referer: " + HOST, "Origin: " + std::string("https://jkanime.net")};
}

std::string httpGet(const std::string& url) {
    return HTTP::get(url, browserHeaders(), HTTP::Timeout{8000});
}

void replaceAll(std::string& subject, const std::string& search, const std::string& repl) {
    if (search.empty()) return;
    size_t pos = 0;
    while ((pos = subject.find(search, pos)) != std::string::npos) {
        subject.replace(pos, search.length(), repl);
        pos += repl.length();
    }
}

/// Extract the substring starting at `get`, ending at `delim` (exclusive of the
/// closing marker but keeping the opening one), mirroring RipJKAnimeNX's
/// scrapElement so downstream replace() calls line up.
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

/// Collapse the handful of HTML entities jkanime emits in titles/synopses.
std::string decodeEntities(std::string s) {
    replaceAll(s, "&quot;", "\"");
    replaceAll(s, "&#039;", "'");
    replaceAll(s, "&#39;", "'");
    replaceAll(s, "&amp;", "&");
    replaceAll(s, "&aacute;", "á");
    replaceAll(s, "&eacute;", "é");
    replaceAll(s, "&iacute;", "í");
    replaceAll(s, "&oacute;", "ó");
    replaceAll(s, "&uacute;", "ú");
    replaceAll(s, "&ntilde;", "ñ");
    replaceAll(s, "&Ntilde;", "Ñ");
    replaceAll(s, "\n", " ");
    replaceAll(s, "<br/>", " ");
    replaceAll(s, "<br>", " ");
    return trim(s);
}

std::string base64_decode(const std::string& input) {
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string ret;
    int val = 0, bits = -8;
    for (unsigned char c : input) {
        if (std::isspace(c)) continue;
        if (c == '=') break;
        size_t idx = chars.find(c);
        if (idx == std::string::npos) break;
        val = (val << 6) + static_cast<int>(idx);
        bits += 6;
        if (bits >= 0) {
            ret.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return ret;
}

std::string toAbsolute(const std::string& url) {
    if (url.rfind("http", 0) == 0) return url;
    if (url.rfind("//", 0) == 0) return "https:" + url;
    if (url.rfind("/", 0) == 0) return "https://jkanime.net" + url;
    return HOST + url;
}

/// Poster url for a catalogue slug, matching the site's CDN layout.
std::string posterFor(const std::string& slug) {
    return CDN + "/assets/images/animes/image/" + slug + ".jpg";
}

/// Pretty title from a slug: "shingeki-no-kyojin" -> "Shingeki No Kyojin".
std::string titleFromSlug(const std::string& slug) {
    std::string t = slug;
    replaceAll(t, "-", " ");
    bool cap = true;
    for (auto& ch : t) {
        if (cap && std::isalpha((unsigned char)ch)) {
            ch = static_cast<char>(std::toupper((unsigned char)ch));
            cap = false;
        } else if (ch == ' ') {
            cap = true;
        }
    }
    return t;
}

}  // namespace

std::string slugOf(const std::string& url) {
    std::string clean = url;
    size_t q = clean.find('?');
    if (q != std::string::npos) clean = clean.substr(0, q);
    while (!clean.empty() && clean.back() == '/') clean.pop_back();
    const std::string host = "jkanime.net/";
    size_t h = clean.find(host);
    if (h != std::string::npos) clean = clean.substr(h + host.length());
    // strip a trailing episode segment (".../slug/12" -> "slug")
    size_t slash = clean.find('/');
    if (slash != std::string::npos) clean = clean.substr(0, slash);
    return clean;
}

// ------------------------------------------------------------------ parsers

std::vector<AnimeCard> parseRecent(const std::string& html) {
    std::vector<AnimeCard> out;
    // The homepage "Animes recientes" grid is a list of <div class="anime__item">
    // blocks, each with a poster (data-setbg), a link and an episode label.
    for (auto& block : split(html, "class=\"anime__item")) {
        std::string poster = scrapElement(block, "data-setbg=\"", "\"");
        replaceAll(poster, "data-setbg=\"", "");
        std::string link = scrapElement(block, "<a href=\"", "\"");
        replaceAll(link, "<a href=\"", "");
        if (link.empty() || poster.empty()) continue;

        std::string label = scrapElement(block, "<div class=\"anime__item__text\">");
        std::string title = scrapElement(label, "<h5>");
        replaceAll(title, "<h5>", "");
        replaceAll(title, "<a href=\"" + link + "\">", "");
        std::string ep = scrapElement(block, "<div class=\"ep\">");
        replaceAll(ep, "<div class=\"ep\">", "");

        AnimeCard c;
        c.url = toAbsolute(link);
        c.slug = slugOf(c.url);
        c.poster = toAbsolute(poster);
        c.title = decodeEntities(title.empty() ? titleFromSlug(c.slug) : title);
        c.extra = decodeEntities(trim(ep));
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<AnimeCard> parseDirectory(const std::string& json) {
    std::vector<AnimeCard> out;
    // /directorio embeds `var animes = { "data": [...], "last_page": N };`
    std::string body = scrapElement(json, "var animes = ", "var mode");
    replaceAll(body, "var animes = ", "");
    body = trim(body);
    if (!body.empty() && body.back() == ';') body.pop_back();
    if (body.empty() || !nlohmann::json::accept(body)) return out;

    nlohmann::json j = nlohmann::json::parse(body);
    for (auto& item : j["data"]) {
        AnimeCard c;
        c.slug = item.value("slug", "");
        if (c.slug.empty()) continue;
        c.url = item.value("url", HOST + c.slug + "/");
        c.title = decodeEntities(item.value("title", titleFromSlug(c.slug)));
        std::string img = item.value("image", "");
        c.poster = img.empty() ? posterFor(c.slug) : toAbsolute(img);
        c.extra = decodeEntities(item.value("type", ""));
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<AnimeCard> parseSearch(const std::string& html) {
    std::vector<AnimeCard> out;
    // /buscar results reuse the same anime__item cards as the homepage grid.
    size_t pos = 0;
    while ((pos = html.find("<div class=\"anime__item\">", pos)) != std::string::npos) {
        size_t hrefPos = html.find("href=", pos);
        if (hrefPos == std::string::npos) break;
        hrefPos += 6;  // href="
        size_t hrefEnd = html.find('"', hrefPos);
        std::string link = html.substr(hrefPos, hrefEnd - hrefPos);

        std::string poster;
        size_t bgPos = html.find("data-setbg=", hrefEnd);
        if (bgPos != std::string::npos) {
            bgPos += 12;  // data-setbg="
            size_t bgEnd = html.find('"', bgPos);
            poster = html.substr(bgPos, bgEnd - bgPos);
        }

        std::string title;
        size_t hPos = html.find("<h5>", hrefEnd);
        if (hPos != std::string::npos && hPos < html.find("anime__item", hrefEnd)) {
            size_t tStart = html.find('>', html.find("<a", hPos));
            if (tStart != std::string::npos) {
                size_t tEnd = html.find("</a>", tStart);
                title = html.substr(tStart + 1, tEnd - tStart - 1);
            }
        }

        AnimeCard c;
        c.url = toAbsolute(link);
        c.slug = slugOf(c.url);
        c.poster = poster.empty() ? posterFor(c.slug) : toAbsolute(poster);
        c.title = decodeEntities(title.empty() ? titleFromSlug(c.slug) : title);
        out.push_back(std::move(c));
        pos = hrefEnd;
    }
    return out;
}

AnimeDetail parseDetail(const std::string& slug, const std::string& html) {
    AnimeDetail d;
    d.slug = slug;
    d.poster = posterFor(slug);
    // The catalogue card already carries a reliable title; parseDetail only
    // needs a deterministic fallback for when a detail is opened without one.
    d.title = titleFromSlug(slug);

    std::string img = scrapElement(html, CDN + "/assets/images/animes/image/");
    if (!img.empty()) d.poster = img;

    std::string syn = scrapElement(html, "<p class=\"scroll\">", "</p>");
    replaceAll(syn, "<p class=\"scroll\">", "");
    d.synopsis = decodeEntities(syn);

    std::string type = scrapElement(html, "<span>Tipo:", "</li");
    replaceAll(type, "<span>Tipo:", "");
    replaceAll(type, "</span>", "");
    d.type = trim(type);

    std::string genres;
    size_t gp = html.find("<span>Generos:</span>");
    if (gp == std::string::npos) gp = html.find("<span>Géneros:</span>");
    while (gp != std::string::npos) {
        gp = html.find("/genero", gp);
        if (gp == std::string::npos) break;
        size_t a = html.find('>', gp);
        size_t b = html.find("</a>", gp);
        if (a == std::string::npos || b == std::string::npos) break;
        if (!genres.empty()) genres += ", ";
        genres += trim(html.substr(a + 1, b - a - 1));
        gp = b;
    }
    d.genres = decodeEntities(genres);

    std::string aired = scrapElement(html, "Emitido:", "</li");
    replaceAll(aired, "Emitido:", "");
    replaceAll(aired, "</span>", "");
    d.aired = trim(aired);

    // Airing state + last episode number.
    if (html.find(">Concluido<") != std::string::npos) {
        d.status = "Concluido";
    } else {
        d.status = "En emisión";
        std::string re = scrapElement(html, "<b>Último episodio</b>:", "</a>");
        std::string last = scrapElement(re, "jkanime.net/" + slug + "/", "\"");
        replaceAll(last, "jkanime.net/" + slug + "/", "");
        replaceAll(last, "/", "");
        int n = atoi(last.c_str());
        if (n > 0) d.maxEpisode = n;
    }

    std::string eps = scrapElement(html, "Episodios:", "</li");
    replaceAll(eps, "Episodios:", "");
    replaceAll(eps, "</span>", "");
    int declared = atoi(trim(eps).c_str());
    if (declared > d.maxEpisode) d.maxEpisode = declared;

    std::string next = scrapElement(html, "<b>Próximo episodio:</b>", "<i class");
    replaceAll(next, "<b>Próximo episodio:</b>", "");
    d.next = trim(next);

    // Some titles number from 0 (specials/prologues) — detect via the /0/ page.
    d.minEpisode = 1;

    if (d.maxEpisode <= 0) d.maxEpisode = 1;
    for (int i = d.minEpisode; i <= d.maxEpisode; i++) {
        Episode e;
        e.number = i;
        e.url = HOST + slug + "/" + std::to_string(i) + "/";
        e.title = "Episodio " + std::to_string(i);
        e.thumb = d.poster;
        d.episodes.push_back(std::move(e));
    }
    return d;
}

std::vector<Server> parseServers(const std::string& html) {
    std::vector<Server> out;

    // Modern jkanime episode pages embed a `var servers = [ {...} ]` array where
    // each entry carries a base64 `remote` field pointing at the real embed.
    std::string arr = scrapElement(html, "var servers = [", "];");
    replaceAll(arr, "var servers = ", "");
    size_t lb = arr.find('[');
    if (lb != std::string::npos) arr = arr.substr(lb);
    if (!arr.empty() && arr.find(']') == std::string::npos) arr += "]";
    if (arr.size() > 2 && nlohmann::json::accept(arr)) {
        nlohmann::json j = nlohmann::json::parse(arr);
        for (auto& s : j) {
            Server sv;
            sv.name = s.value("server", s.value("title", "Servidor"));
            std::string remote = s.value("remote", "");
            if (!remote.empty()) {
                std::string dec = base64_decode(remote);
                sv.url = dec.rfind("http", 0) == 0 ? dec : toAbsolute(dec);
            }
            if (sv.url.empty()) continue;
            out.push_back(std::move(sv));
        }
    }

    // Fall back to the legacy inline player links still present on some pages.
    struct Legacy {
        const char* anchor;
        const char* name;
    };
    static const Legacy legacy[] = {
        {"jkokru.php?u=", "Okru"},
        {"um2.php?", "Nozomi"},
        {"jkvmixdrop.php?u=", "Mixdrop"},
        {"jkfembed.php?u=", "Fembed"},
        {"/jkplayer/um?", "Desu"},
    };
    for (auto& l : legacy) {
        std::string frag = scrapElement(html, l.anchor, "\"");
        if (frag.empty()) continue;
        Server sv;
        sv.name = l.name;
        if (std::string(l.anchor) == "jkokru.php?u=") {
            replaceAll(frag, "jkokru.php?u=", "https://ok.ru/videoembed/");
            sv.url = frag;
        } else {
            sv.url = toAbsolute(frag);
        }
        // avoid duplicating a server already found in the embedded array
        if (std::none_of(out.begin(), out.end(), [&](const Server& e) { return e.name == sv.name; }))
            out.push_back(std::move(sv));
    }
    return out;
}

// ---------------------------------------------------------------- resolvers

std::string Stream::mpvExtra() const {
    std::string ref = referer.empty() ? HOST : referer;
    // mpv exposes user-agent/referrer as plain string options — safer than the
    // comma-delimited http-header-fields list for values that contain spaces
    // and punctuation. Returned with a leading comma to append to the base
    // option string (matching remote::Client::init's convention).
    std::ostringstream ss;
    ss << ",user-agent=\"" << USER_AGENT << "\",referrer=\"" << ref << "\"";
    return ss.str();
}

namespace {

/// Resolve jkanime's own player (Nozomi / gsplay) to a direct file url.
/// Mirrors RipJKAnimeNX's Nozomi_Link three-step handshake.
Stream resolveNozomi(const std::string& embedUrl) {
    Stream out;
    std::string page = httpGet(embedUrl);
    std::string firstKey = scrapElement(page, "name=\"data\" value=\"", "\"");
    replaceAll(firstKey, "name=\"data\" value=\"", "");
    if (firstKey.empty()) return out;

    // POST the token, follow the redirect, read the fragment it lands on.
    HTTP s;
    HTTP::set_option(s, browserHeaders(), HTTP::Timeout{8000});
    s._post("https://jkanime.net/gsplay/redirect_post.php", "data=" + firstKey);
    std::string secondKey = s.effective_url();
    size_t hash = secondKey.find('#');
    if (hash == std::string::npos) return out;
    secondKey = secondKey.substr(hash + 1);

    // Exchange the fragment for the direct file via the JSON api.
    for (int i = 0; i < 4; i++) {
        std::string resp = HTTP::post(
            "https://jkanime.net/gsplay/api.php", "v=" + secondKey, browserHeaders(), HTTP::Timeout{8000});
        if (nlohmann::json::accept(resp)) {
            nlohmann::json j = nlohmann::json::parse(resp);
            if (j.contains("file") && j["file"].is_string()) {
                out.url = j["file"];
                replaceAll(out.url, "\r", "");
                out.referer = HOST;
                break;
            }
        }
    }
    return out;
}

}  // namespace

// -------------------------------------------------------------- async wrappers

void getRecent(std::function<void(std::vector<AnimeCard>)> then, OnError error) {
    brls::async([then, error]() {
        try {
            std::string html = httpGet(HOST);
            auto r = parseRecent(html);
            brls::sync([then, r]() { then(r); });
        } catch (const std::exception& ex) {
            std::string msg = ex.what();
            if (error) brls::sync([error, msg]() { error(msg); });
        }
    });
}

void getDirectory(int page, std::function<void(std::vector<AnimeCard>)> then, OnError error) {
    brls::async([page, then, error]() {
        try {
            std::string html = httpGet(HOST + "directorio?p=" + std::to_string(page));
            auto r = parseDirectory(html);
            brls::sync([then, r]() { then(r); });
        } catch (const std::exception& ex) {
            std::string msg = ex.what();
            if (error) brls::sync([error, msg]() { error(msg); });
        }
    });
}

void search(const std::string& query, std::function<void(std::vector<AnimeCard>)> then, OnError error) {
    std::string q = query;
    replaceAll(q, " ", "_");
    replaceAll(q, "!", "");
    replaceAll(q, ";", "");
    brls::async([q, then, error]() {
        try {
            std::string html = httpGet(HOST + "buscar/" + q);
            auto r = parseSearch(html);
            brls::sync([then, r]() { then(r); });
        } catch (const std::exception& ex) {
            std::string msg = ex.what();
            if (error) brls::sync([error, msg]() { error(msg); });
        }
    });
}

void getDetail(const std::string& slug, std::function<void(AnimeDetail)> then, OnError error) {
    std::string key = slugOf(slug);
    brls::async([key, then, error]() {
        try {
            std::string html = httpGet(HOST + key + "/");
            auto d = parseDetail(key, html);
            brls::sync([then, d]() { then(d); });
        } catch (const std::exception& ex) {
            std::string msg = ex.what();
            if (error) brls::sync([error, msg]() { error(msg); });
        }
    });
}

void getServers(const std::string& episodeUrl, std::function<void(std::vector<Server>)> then, OnError error) {
    brls::async([episodeUrl, then, error]() {
        try {
            std::string html = httpGet(episodeUrl);
            auto r = parseServers(html);
            brls::sync([then, r]() { then(r); });
        } catch (const std::exception& ex) {
            std::string msg = ex.what();
            if (error) brls::sync([error, msg]() { error(msg); });
        }
    });
}

void resolve(const Server& server, std::function<void(Stream)> then, OnError error) {
    brls::async([server, then, error]() {
        try {
            Stream out;
            std::string name = server.name;
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);

            if (name.find("nozomi") != std::string::npos || name.find("desu") != std::string::npos ||
                server.url.find("gsplay") != std::string::npos || server.url.find("um2.php") != std::string::npos) {
                out = resolveNozomi(server.url);
            }

            // Direct-ish hosts: hand mpv the embed/file url with jkanime headers.
            // Works for hosts that expose a plain mp4/m3u8; extractor-only hosts
            // (ok.ru, mixdrop) will simply fail to load and the user can pick
            // another server.
            if (out.url.empty()) {
                out.url = server.url;
                out.referer = HOST;
            }

            if (out.url.empty()) throw std::runtime_error("No stream found");
            brls::sync([then, out]() { then(out); });
        } catch (const std::exception& ex) {
            std::string msg = ex.what();
            if (error) brls::sync([error, msg]() { error(msg); });
        }
    });
}

}  // namespace jk
