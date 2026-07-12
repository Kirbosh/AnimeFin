/*
    TioAnime provider for AnimeFin.

    TioAnime (tioanime.com) is the primary Spanish source. Unlike a JS-gated source it
    server-renders the whole streaming pipeline into the episode page: a plain
    `var videos = [["Server","embed_url"], ...]` JavaScript array that needs no
    XHR, so scraping it is reliable. The anime detail page likewise carries
    `var anime_info = ["id","title","slug",...]` and `var episodes = [n, ...]`,
    giving an exact episode list and a stable poster id.

    It reuses the shared anime:: data types and the anime:: embed resolver
    (ok.ru / Mixdrop / StreamWish / Filemoon / Voe / mp4upload ...).
*/

#pragma once

#include <string>
#include <vector>
#include <functional>

#include "api/media.hpp"  // shared data types + resolveEmbed

namespace tio {

inline const std::string HOST = "https://tioanime.com/";

// Same async shape as the other providers; callbacks fire on the UI thread.
void getRecent(std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error);
void getDirectory(int page, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error,
    const std::string& order = "default");
void search(const std::string& query, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error);
void getDetail(const std::string& slug, std::function<void(anime::AnimeDetail)> then, anime::OnError error);
void getServers(const std::string& episodeUrl, std::function<void(std::vector<anime::Server>)> then, anime::OnError error);
void resolve(const anime::Server& server, std::function<void(anime::Stream)> then, anime::OnError error);

// Pure cores, exposed for the offline test harness.
std::vector<anime::AnimeCard> parseRecent(const std::string& html);
std::vector<anime::AnimeCard> parseDirectory(const std::string& html);
anime::AnimeDetail parseDetail(const std::string& slug, const std::string& html);
std::vector<anime::Server> parseServers(const std::string& html);

/// TioAnime anime slug from an /anime/<slug> or /ver/<slug>-<n> url.
std::string slugOf(const std::string& url);

}  // namespace tio
