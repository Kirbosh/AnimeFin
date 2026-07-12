/*
    Stremio-addon backend for AnimeFin.

    Instead of scraping each streaming site (and losing at the last step, where
    modern hosts hide their video behind in-browser JavaScript/crypto), AnimeFin
    talks to a Stremio addon whose *server* has already done that work and hands
    back plain, directly-playable URLs as JSON.

    The addon (Pigamer37/animeflv-stremio-addon) aggregates several Spanish
    sources — AnimeFLV, TioAnime, AnimeAV1, Henaojara, AnimeJara, JKAnime — and
    exposes the standard Stremio resources:

      catalog : GET /catalog/series/<cat>[/<extra>].json  -> { metas:[...] }
      meta    : GET /meta/series/<id>.json                -> { meta:{ videos:[...] } }
      stream  : GET /stream/series/<id>.json              -> { streams:[...] }

    Item ids are native, e.g. "tioanime:one-piece" (anime) and
    "tioanime:one-piece:1073" (episode). A stream carries `url` (directly
    playable — what we use) or `externalUrl` (browser-only — skipped).
*/

#pragma once

#include <string>
#include <vector>
#include <functional>

#include "api/media.hpp"

namespace strm {

inline const std::string BASE = "https://pigamer37.alwaysdata.net";

/// A selectable addon catalog (one underlying Spanish source).
struct Catalog {
    std::string id;    ///< addon catalog id, e.g. "animeflv"
    std::string name;  ///< display label, e.g. "AnimeFLV"
    int pageSize;      ///< items per page (the addon's paging unit)
};

/// The catalogs offered, indexed to match the source picker.
const std::vector<Catalog>& catalogs();
size_t activeCatalog();
void setActiveCatalog(size_t i);

// Browse the active catalog.
void getRecent(std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error);
void getDirectory(int page, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error,
    const std::string& order = "default");
void search(const std::string& query, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error);

// Detail / playback — the item id already names its source, so these work for a
// card opened from any catalog.
void getDetail(const std::string& id, std::function<void(anime::AnimeDetail)> then, anime::OnError error);
void getStreams(const std::string& episodeId, std::function<void(std::vector<anime::Server>)> then, anime::OnError error);
void resolve(const anime::Server& server, std::function<void(anime::Stream)> then, anime::OnError error);

// Pure cores, exposed for the offline test harness.
std::vector<anime::AnimeCard> parseCatalog(const std::string& json);
anime::AnimeDetail parseMeta(const std::string& id, const std::string& json);
std::vector<anime::Server> parseStreams(const std::string& json);

}  // namespace strm
