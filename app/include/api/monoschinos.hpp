/*
    Monoschinos provider for AnimeFin.

    Monoschinos (wwv.monoschinos2.net) is the dub-heavy Spanish source. Unlike
    TioAnime it does NOT server-render the episode/server lists into the page:
    both are fetched with XHR POSTs to `/ajax_pagination`.

      - Episodes: the detail page carries an element with data-i / data-u / data-e
        (id / url-key / episode count). POST acc=episodes&i=&u=&p=<page> returns a
        chunk of episode cards (50 per page).
      - Servers: the episode page carries `.opt[data-encrypt]`. POST acc=opt&i=<enc>
        returns `[data-player]` elements whose value is a Base64-encoded embed url.

    Stream resolution reuses jk's shared embed resolver. Because the AJAX shape
    can drift and can't be exercised from the build host, every step logs and
    dumps its raw response through diag::.
*/

#pragma once

#include <string>
#include <vector>
#include <functional>

#include "api/jkanime.hpp"  // shared data types + resolveEmbedSync

namespace mono {

inline const std::string HOST = "https://wwv.monoschinos2.net/";

void getRecent(std::function<void(std::vector<jk::AnimeCard>)> then, jk::OnError error);
void getDirectory(int page, std::function<void(std::vector<jk::AnimeCard>)> then, jk::OnError error,
    const std::string& order = "default");
void search(const std::string& query, std::function<void(std::vector<jk::AnimeCard>)> then, jk::OnError error);
void getDetail(const std::string& slug, std::function<void(jk::AnimeDetail)> then, jk::OnError error);
void getServers(const std::string& episodeUrl, std::function<void(std::vector<jk::Server>)> then, jk::OnError error);
void resolve(const jk::Server& server, std::function<void(jk::Stream)> then, jk::OnError error);

// Pure cores, exposed for the offline test harness.
std::vector<jk::AnimeCard> parseGrid(const std::string& html);
std::vector<jk::Server> parsePlayers(const std::string& ajaxHtml);
std::string base64Decode(const std::string& in);

/// Monoschinos anime slug from an /anime/<slug> or /ver/<slug>-... url.
std::string slugOf(const std::string& url);

}  // namespace mono
