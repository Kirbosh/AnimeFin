/*
    Shared media types + stream resolver for AnimeFin.

    Source-agnostic building blocks used by every provider (TioAnime,
    Monoschinos, ...). The catalogue/episode/server structs are what the browse
    and detail UI speak, and resolveEmbed() turns a chosen streaming server into
    a direct, mpv-playable url — host-routed across the common embed players
    (ok.ru / Mixdrop / Fembed / StreamWish / Filemoon / Voe / mp4upload ...).

    All scraping is plain HTML/JSON over the app's HTTP client, wrapped in the
    same async(then)/error style as the rest of the app.
*/

#pragma once

#include <string>
#include <vector>
#include <functional>

#include "api/http.hpp"

namespace anime {

/// A desktop-browser UA; the streaming CDNs reject the default agent.
inline const std::string USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

/// A catalogue entry as shown in a poster grid (recent / directory / search).
struct AnimeCard {
    std::string slug;   ///< catalogue key, e.g. "one-piece"
    std::string url;    ///< full anime page url
    std::string title;  ///< display title
    std::string poster; ///< absolute image url
    std::string extra;  ///< episode label or type ("Episodio 12", "TV", ...)
};

/// A single episode inside an anime detail page.
struct Episode {
    int number = 0;
    std::string title;
    std::string url;    ///< full episode page url
    std::string thumb;  ///< absolute image url (falls back to the anime poster)
};

/// A streaming source offered for one episode, before resolution.
struct Server {
    std::string name;     ///< human label ("Nozomi", "Mixdrop", "Okru" ...)
    std::string url;      ///< embed url or on-page player url (already absolute)
    std::string referer;  ///< optional Referer the stream needs (from proxyHeaders)
};

/// The resolved, directly-playable stream plus the mpv options it needs.
struct Stream {
    std::string url;     ///< direct video url (mp4 / m3u8)
    std::string referer; ///< Referer header the host expects
    /// Extra `key=value` mpv options already comma-prefixed. Passed verbatim to
    /// setUrl().
    std::string mpvExtra() const;
};

/// Full detail for one anime.
struct AnimeDetail {
    std::string slug;
    std::string title;
    std::string poster;
    std::string synopsis;
    std::string type;    ///< "Anime", "Película", "OVA" ...
    std::string status;  ///< "En emisión" / "Finalizado" ...
    std::string genres;
    std::string aired;
    std::string next;    ///< next episode date, when airing
    int minEpisode = 1;
    int maxEpisode = 0;
    std::vector<Episode> episodes;
};

using OnError = std::function<void(const std::string&)>;

/// Synchronously resolve a generic embed url (ok.ru / Mixdrop / Fembed /
/// packed-JS & jwplayer hosts) to a direct stream. Host-agnostic; returns an
/// empty Stream when nothing was found. Must run off the UI thread.
Stream resolveEmbed(const std::string& serverName, const std::string& url);

// --- Stream extraction primitives (pure, unit-tested) ---------------------

/// Unpack a Dean-Edwards `eval(function(p,a,c,k,e,d){...})` payload (used by
/// Mixdrop, StreamWish, Filemoon, Voe, ...). Returns the unpacked source, or
/// "" if the text isn't a packed script.
std::string unpackPacked(const std::string& source);

/// Find the first direct video url (m3u8 / mp4 / `file:"..."`) in arbitrary
/// player markup or script. Returns a scheme-qualified url, or "".
std::string findStreamUrl(const std::string& text);

/// Extract the best-quality mp4 from an ok.ru embed page's `data-options`
/// JSON blob. Returns "" when no playable url is present.
std::string parseOkruOptions(const std::string& html);

}  // namespace anime
