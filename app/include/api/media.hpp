/*
    Shared media types for AnimeFin.

    Source-agnostic building blocks the UI speaks: a catalogue card, an episode,
    a streaming server, a resolved stream, and full anime detail. The data is
    scraped natively from the source (api/tioanime.*) and resolved by
    api/extractors.*.
*/

#pragma once

#include <string>
#include <vector>
#include <functional>

namespace anime {

/// A desktop-browser UA; some stream CDNs reject the default agent.
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

/// Turn a chosen streaming server (host embed page) into a directly-playable
/// Stream, natively — no external resolver server. Host-routed across the hosts
/// these Spanish sources use (YourUpload / MP4Upload / Voe / StreamWish /
/// Filemoon / Okru / Vidhide / ...). Returns an empty Stream when nothing was
/// found. Must run off the UI thread. See api/extractors.cpp.
Stream resolveEmbed(const std::string& serverName, const std::string& url);

}  // namespace anime
