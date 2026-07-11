/*
    JKAnime data layer for AnimeFin.

    A native port of the scraping logic from Darkxex's RipJKAnimeNX into
    Switchfin's architecture. It exposes the jkanime.net catalogue (recent
    episodes, full directory, search and per-anime detail) plus a resolver
    that turns a chosen streaming server into a direct URL mpv can play.

    Everything here is plain HTML/JSON scraping over Switchfin's HTTP client,
    wrapped in the same async(then)/error style as api/jellyfin.hpp so the UI
    code reads the same regardless of backend.
*/

#pragma once

#include <string>
#include <vector>
#include <functional>

#include <borealis/core/thread.hpp>

#include "api/http.hpp"

namespace jk {

/// jkanime.net is fronted by Cloudflare and its image CDN checks the
/// referrer, so every request must look like a real desktop browser.
inline const std::string HOST = "https://jkanime.net/";
inline const std::string USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

/// A catalogue entry as shown in a poster grid (recent / directory / search).
struct AnimeCard {
    std::string slug;   ///< url segment, e.g. "one-piece"
    std::string url;    ///< full page url, e.g. https://jkanime.net/one-piece/
    std::string title;  ///< display title
    std::string poster; ///< absolute image url
    std::string extra;  ///< episode label or type ("Episodio 12", "TV", ...)
};

/// A single episode inside an anime detail page.
struct Episode {
    int number = 0;
    std::string title;
    std::string url;    ///< full page url, e.g. https://jkanime.net/one-piece/12/
    std::string thumb;  ///< absolute image url (falls back to the anime poster)
};

/// A streaming source offered for one episode, before resolution.
struct Server {
    std::string name;  ///< human label ("Nozomi", "Mixdrop", "Okru" ...)
    std::string url;   ///< embed url or on-page player url (already absolute)
};

/// The resolved, directly-playable stream plus the mpv options it needs.
struct Stream {
    std::string url;     ///< direct video url (mp4 / m3u8)
    std::string referer; ///< Referer header the host expects
    /// Extra `key=value` mpv options already comma-prefixed, e.g.
    /// ",http-header-fields='User-Agent: ...'". Passed verbatim to setUrl().
    std::string mpvExtra() const;
};

/// Full detail for one anime.
struct AnimeDetail {
    std::string slug;
    std::string title;
    std::string poster;
    std::string synopsis;
    std::string type;    ///< "Anime", "Película", "OVA" ...
    std::string status;  ///< "En emisión" / "Concluido" ...
    std::string genres;
    std::string aired;
    std::string next;    ///< next episode date, when airing
    int minEpisode = 1;
    int maxEpisode = 0;
    std::vector<Episode> episodes;
};

using OnError = std::function<void(const std::string&)>;

// --- Asynchronous fetchers (callbacks fire on the main/UI thread) ---------

/// Homepage "Animes recientes" strip: the latest released episodes.
void getRecent(std::function<void(std::vector<AnimeCard>)> then, OnError error);

/// Paginated full catalogue via /directorio?p=N. `page` is 1-based.
void getDirectory(int page, std::function<void(std::vector<AnimeCard>)> then, OnError error);

/// Free-text search via /buscar/<query>.
void search(const std::string& query, std::function<void(std::vector<AnimeCard>)> then, OnError error);

/// Anime detail page: metadata + the full episode list.
void getDetail(const std::string& slug, std::function<void(AnimeDetail)> then, OnError error);

/// Streaming servers offered for a single episode page url.
void getServers(const std::string& episodeUrl, std::function<void(std::vector<Server>)> then, OnError error);

/// Resolve a chosen server to a direct, mpv-playable stream.
void resolve(const Server& server, std::function<void(Stream)> then, OnError error);

// --- Synchronous cores (run these off the UI thread) ----------------------
// Exposed for reuse/testing; the async wrappers above call into them.

std::vector<AnimeCard> parseRecent(const std::string& html);
std::vector<AnimeCard> parseDirectory(const std::string& json);
std::vector<AnimeCard> parseSearch(const std::string& html);
AnimeDetail parseDetail(const std::string& slug, const std::string& html);
std::vector<Server> parseServers(const std::string& html);

/// Turn a jkanime page url or slug into its catalogue key ("one-piece").
std::string slugOf(const std::string& url);

}  // namespace jk
