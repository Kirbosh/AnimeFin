/*
    Monoschinos provider for AnimeFin — see api/monoschinos.hpp.

    Monoschinos loads its episode list and its server list over XHR, so this
    provider mirrors the site's own `/ajax_pagination` POSTs. Everything is
    logged and dumped through diag:: because the AJAX shape can't be exercised
    from the build host.
*/

#include "api/monoschinos.hpp"
#include "api/diag.hpp"

#include <borealis/core/thread.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace mono {

static const std::string USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

namespace {

HTTP::Header headers(const std::string& referer = HOST) {
    return {
        "User-Agent: " + USER_AGENT,
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8",
        "Accept-Language: es-ES,es;q=0.9,en;q=0.8",
        "Referer: " + referer,
    };
}

std::string httpGet(const std::string& url) {
    try {
        std::string body = HTTP::get(url, headers(), HTTP::Timeout{8000});
        bool blocked = diag::looksBlocked(body);
        diag::log("mono GET " + url + " -> " + std::to_string(body.size()) + " bytes" + (blocked ? " [BLOCKED?]" : ""));
        if (blocked) diag::dump("mono_blocked", url, body);
        return body;
    } catch (const std::exception& e) {
        diag::log("mono GET " + url + " -> ERROR: " + e.what());
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

std::string decode(std::string s) {
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
    return trim(out);
}

std::string toAbs(const std::string& u) {
    std::string s = u;
    if (s.rfind("./", 0) == 0) s = s.substr(2);  // "./ver/x" -> "ver/x" (avoids /./)
    if (s.empty()) return s;
    if (s.rfind("http", 0) == 0) return s;
    if (s.rfind("//", 0) == 0) return "https:" + s;
    if (s.rfind("/", 0) == 0) return "https://wwv.monoschinos2.net" + s;
    return HOST + s;
}

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

/// First usable image url in a fragment: data-src / data-lazy-src / src, taking
/// the first `/uploads/`-style or absolute url and skipping the placeholder.
std::string firstImage(const std::string& piece) {
    for (const char* key : {"data-src=\"", "data-lazy-src=\"", "src=\""}) {
        size_t pos = 0;
        while ((pos = piece.find(key, pos)) != std::string::npos) {
            size_t a = pos + std::string(key).length();
            size_t b = piece.find('"', a);
            pos = a;
            if (b == std::string::npos) break;
            std::string v = piece.substr(a, b - a);
            if (v.find("anime.png") != std::string::npos) continue;  // placeholder
            if (v.rfind("http", 0) == 0 || v.rfind("//", 0) == 0 || v.find("/uploads/") != std::string::npos ||
                v.find("/img/") != std::string::npos)
                return toAbs(v);
        }
    }
    return toAbs(between(piece, "src=\"", "\""));
}

/// Last run of digits in a string (the episode number in an episode url).
int trailingNumber(const std::string& s) {
    size_t end = s.find_last_of("0123456789");
    if (end == std::string::npos) return -1;
    size_t start = end;
    while (start > 0 && std::isdigit((unsigned char)s[start - 1])) start--;
    return atoi(s.substr(start, end - start + 1).c_str());
}

}  // namespace

std::string base64Decode(const std::string& in) {
    static const std::string T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=' || std::isspace(c)) continue;
        size_t idx = T.find(c);
        if (idx == std::string::npos) continue;
        val = (val << 6) + (int)idx;
        bits += 6;
        if (bits >= 0) {
            out.push_back(char((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

std::string slugOf(const std::string& url) {
    std::string s = url;
    size_t q = s.find('?');
    if (q != std::string::npos) s = s.substr(0, q);
    while (!s.empty() && s.back() == '/') s.pop_back();
    size_t last = s.find_last_of('/');
    if (last != std::string::npos) s = s.substr(last + 1);
    return s;
}

// ------------------------------------------------------------------- parsers

std::vector<anime::AnimeCard> parseGrid(const std::string& html) {
    std::vector<anime::AnimeCard> out;
    std::vector<std::string> seen;
    // Anime cards live inside elements with class "ficha_efecto".
    auto pieces = split(html, "ficha_efecto");
    // If the class is absent (markup drift) fall back to splitting on anime links.
    if (pieces.size() <= 1) pieces = split(html, "href=\"");
    for (auto& piece : pieces) {
        std::string href = between(piece, "href=\"", "\"");
        if (href.empty()) href = piece.substr(0, piece.find('"'));
        if (href.find("/anime/") == std::string::npos) continue;
        std::string slug = slugOf(href);
        if (slug.empty()) continue;
        if (std::find(seen.begin(), seen.end(), slug) != seen.end()) continue;
        seen.push_back(slug);

        anime::AnimeCard c;
        c.slug = slug;
        c.url = HOST + "anime/" + slug;
        c.poster = firstImage(piece);
        std::string title = decode(tagInner(piece, "title_cap"));
        if (title.empty()) title = decode(between(piece, "alt=\"", "\""));
        if (title.empty()) title = slug;
        c.title = title;
        out.push_back(std::move(c));
    }
    return out;
}

/// Parse /ver/ episode links into (number,url,thumb) episodes. When slugFilter is
/// set, only links containing that slug are kept — the detail page mixes in a
/// "Sugerencias" block of *other* animes' episodes that must be excluded.
static std::vector<anime::Episode> parseEpisodesChunk(
    const std::string& html, const std::string& poster, const std::string& slugFilter = "") {
    std::vector<anime::Episode> out;
    for (auto& piece : split(html, "href=\"")) {
        size_t q = piece.find('"');
        if (q == std::string::npos) continue;
        std::string href = piece.substr(0, q);
        if (href.find("/ver/") == std::string::npos) continue;
        if (!slugFilter.empty() && href.find(slugFilter) == std::string::npos) continue;
        int n = trailingNumber(href);
        if (n < 0) continue;
        anime::Episode e;
        e.number = n;
        e.url = toAbs(href);
        e.title = "Episodio " + std::to_string(n);
        std::string thumb = firstImage(piece);
        e.thumb = thumb.empty() ? poster : thumb;
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<anime::Server> parsePlayers(const std::string& ajaxHtml) {
    std::vector<anime::Server> out;
    std::vector<std::string> seen;
    auto add = [&](std::string url, const std::string& name) {
        url = toAbs(url);
        if (url.rfind("http", 0) != 0) return;
        if (std::find(seen.begin(), seen.end(), url) != seen.end()) return;
        seen.push_back(url);
        anime::Server sv;
        sv.url = url;
        // Human label from the host, e.g. "sfastwish.com" -> "Sfastwish".
        std::string label = name;
        if (label.empty()) {
            size_t h = url.find("://");
            size_t s = url.find('/', h + 3);
            label = url.substr(h + 3, (s == std::string::npos ? url.size() : s) - (h + 3));
            if (label.rfind("www.", 0) == 0) label = label.substr(4);
            size_t dot = label.find('.');
            if (dot != std::string::npos) label = label.substr(0, dot);
            if (!label.empty()) label[0] = std::toupper((unsigned char)label[0]);
        }
        sv.name = label.empty() ? "Servidor" : label;
        out.push_back(std::move(sv));
    };

    // data-player="<base64 embed url>" is the primary carrier.
    size_t pos = 0;
    while ((pos = ajaxHtml.find("data-player=\"", pos)) != std::string::npos) {
        size_t a = pos + 13;
        size_t b = ajaxHtml.find('"', a);
        pos = a;
        if (b == std::string::npos) break;
        std::string enc = ajaxHtml.substr(a, b - a);
        std::string url = enc;
        if (url.rfind("http", 0) != 0 && url.rfind("//", 0) != 0) {
            std::string dec = base64Decode(enc);
            if (dec.find("http") != std::string::npos) url = dec.substr(dec.find("http"));
        }
        add(url, "");
    }
    // Fallback: raw iframes in the response.
    pos = 0;
    while ((pos = ajaxHtml.find("<iframe", pos)) != std::string::npos) {
        std::string frag = ajaxHtml.substr(pos, 500);
        pos += 7;
        std::string src = between(frag, "src=\"", "\"");
        if (!src.empty()) add(src, "");
    }
    return out;
}

/// Servers rendered straight into the episode page's "Descargas" block:
///   <a class="btn btn-warning" target="_blank" href="https://voe.sx/xxx"> Voe </a>
/// This is the reliable path — the site's /ajax_pagination endpoint is dead.
std::vector<anime::Server> parseDownloadServers(const std::string& html) {
    std::vector<anime::Server> out;
    std::vector<std::string> seen;
    auto pieces = split(html, "btn-warning");
    for (size_t idx = 1; idx < pieces.size(); idx++) {  // skip the page head before the first match
        const std::string& piece = pieces[idx];
        // The href is inside the same <a> tag, so it's the first one in the piece.
        std::string href = between(piece, "href=\"", "\"");
        if (href.rfind("http", 0) != 0) continue;
        if (href.find("monoschinos") != std::string::npos) continue;  // the "Lista" nav button
        if (std::find(seen.begin(), seen.end(), href) != seen.end()) continue;
        seen.push_back(href);

        std::string name = decode(between(piece, "</svg>", "</a>"));
        if (name.empty()) {
            size_t h = href.find("://");
            size_t s = href.find('/', h + 3);
            name = href.substr(h + 3, (s == std::string::npos ? href.size() : s) - (h + 3));
            if (name.rfind("www.", 0) == 0) name = name.substr(4);
            size_t dot = name.find('.');
            if (dot != std::string::npos) name = name.substr(0, dot);
            if (!name.empty()) name[0] = std::toupper((unsigned char)name[0]);
        }
        anime::Server sv;
        sv.url = href;
        sv.name = name.empty() ? "Servidor" : name;
        out.push_back(std::move(sv));
    }
    return out;
}

// ------------------------------------------------------------- async wrappers

void getRecent(std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error) {
    brls::async([then, error]() {
        try {
            std::string url = HOST + "animes?estado=en+emision&pag=1";
            std::string html = httpGet(url);
            auto r = parseGrid(html);
            diag::log("mono recent: " + std::to_string(r.size()) + " cards");
            if (r.empty()) diag::dump("mono_recent_empty", url, html);
            brls::sync([then, r]() { then(r); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

void getDirectory(
    int page, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error, const std::string& order) {
    brls::async([page, order, then, error]() {
        try {
            std::string url = HOST + "animes?pag=" + std::to_string(page);
            if (!order.empty() && order != "default") url += "&categoria=" + order;
            std::string html = httpGet(url);
            auto r = parseGrid(html);
            diag::log("mono directory p" + std::to_string(page) + ": " + std::to_string(r.size()) + " cards");
            if (r.empty() && page == 1) diag::dump("mono_directory_empty", url, html);
            brls::sync([then, r]() { then(r); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

void search(const std::string& query, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error) {
    std::string q = query;
    brls::async([q, then, error]() {
        try {
            std::string url = HOST + "animes?buscar=" + HTTP::encode_form({{"q", q}}).substr(2) + "&pag=1";
            std::string html = httpGet(url);
            auto r = parseGrid(html);
            diag::log("mono search '" + q + "': " + std::to_string(r.size()) + " results");
            if (r.empty()) diag::dump("mono_search_empty", url, html);
            brls::sync([then, r]() { then(r); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

void getDetail(const std::string& slug, std::function<void(anime::AnimeDetail)> then, anime::OnError error) {
    std::string key = slugOf(slug);
    brls::async([key, then, error]() {
        try {
            std::string url = HOST + "anime/" + key;
            std::string html = httpGet(url);

            anime::AnimeDetail d;
            d.slug = key;
            d.type = "Anime";
            d.title = decode(between(html, "property=\"og:title\" content=\"", "\""));
            if (d.title.empty()) d.title = decode(tagInner(html, "text-capitalize"));
            if (d.title.empty()) d.title = key;
            d.poster = decode(between(html, "property=\"og:image\" content=\"", "\""));
            d.poster = toAbs(d.poster);
            d.synopsis = decode(between(html, "property=\"og:description\" content=\"", "\""));

            if (html.find("Finalizado") != std::string::npos)
                d.status = "Finalizado";
            else if (html.find("En emision") != std::string::npos || html.find("En emisi") != std::string::npos)
                d.status = "En emisión";

            // Episodes are rendered into the detail page as /ver/<slug>-episodio-N
            // links (the /ajax_pagination XHR the site once used now returns empty).
            // Filter to this anime's own slug so the "Sugerencias" block doesn't leak.
            std::vector<anime::Episode> eps = parseEpisodesChunk(html, d.poster, key);

            std::sort(eps.begin(), eps.end(), [](const anime::Episode& a, const anime::Episode& b) { return a.number < b.number; });
            eps.erase(std::unique(eps.begin(), eps.end(),
                          [](const anime::Episode& a, const anime::Episode& b) { return a.number == b.number; }),
                eps.end());
            d.episodes = eps;
            if (!eps.empty()) {
                d.minEpisode = eps.front().number;
                d.maxEpisode = eps.back().number;
            }

            diag::log("mono detail '" + key + "': " + std::to_string(d.episodes.size()) + " eps, status=" + d.status);
            if (d.episodes.empty()) diag::dump("mono_detail_empty", url, html);
            brls::sync([then, d]() { then(d); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

void getServers(const std::string& episodeUrl, std::function<void(std::vector<anime::Server>)> then, anime::OnError error) {
    brls::async([episodeUrl, then, error]() {
        try {
            std::string html = httpGet(episodeUrl);
            // The server list is rendered into the page (the /ajax_pagination
            // XHR the site once used now returns empty). Read it directly.
            std::vector<anime::Server> r = parseDownloadServers(html);
            // Legacy fallbacks in case a page still ships data-player/iframes.
            if (r.empty()) r = parsePlayers(html);
            diag::log("mono servers " + episodeUrl + ": " + std::to_string(r.size()) + " found");
            if (r.empty()) diag::dump("mono_servers_empty", episodeUrl, html);
            brls::sync([then, r]() { then(r); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

void resolve(const anime::Server& server, std::function<void(anime::Stream)> then, anime::OnError error) {
    brls::async([server, then, error]() {
        try {
            anime::Stream out = anime::resolveEmbed(server.name, server.url);
            diag::log("mono resolve '" + server.name + "' [" + server.url + "] -> " +
                (out.url.empty() ? "FAILED" : out.url));
            if (out.url.empty()) throw std::runtime_error("No stream found");
            brls::sync([then, out]() { then(out); });
        } catch (const std::exception& ex) {
            std::string m = ex.what();
            diag::log("mono resolve '" + server.name + "' -> ERROR: " + m);
            if (error) brls::sync([error, m]() { error(m); });
        }
    });
}

}  // namespace mono
