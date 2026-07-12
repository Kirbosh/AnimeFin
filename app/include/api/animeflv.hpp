/*
    AnimeFLV provider for AnimeFin.

    A second Spanish source alongside JKAnime. AnimeFLV's episode pages expose a
    `var videos = {SUB:[...], LAT:[...]}` object, so it cleanly distinguishes
    Spanish subtitled from Latino-dubbed servers, and a `var episodes = [...]`
    array giving an exact episode list. It reuses the shared jk:: data types and
    the jk:: embed resolver (ok.ru / Mixdrop / packed-JS / jwplayer hosts).
*/

#pragma once

#include <string>
#include <vector>
#include <functional>

#include "api/jkanime.hpp"  // shared data types + resolveEmbedSync

namespace flv {

// Canonical host. The logs showed episode pages canonicalising to www4; www3
// is an older mirror that returns an empty video list, so target www4 directly.
inline const std::string HOST = "https://www4.animeflv.net/";

// Same async shape as the jk:: fetchers; callbacks fire on the UI thread.
void getRecent(std::function<void(std::vector<jk::AnimeCard>)> then, jk::OnError error);
void getDirectory(int page, std::function<void(std::vector<jk::AnimeCard>)> then, jk::OnError error,
    const std::string& order = "default");
void search(const std::string& query, std::function<void(std::vector<jk::AnimeCard>)> then, jk::OnError error);
void getDetail(const std::string& slug, std::function<void(jk::AnimeDetail)> then, jk::OnError error);
void getServers(const std::string& episodeUrl, std::function<void(std::vector<jk::Server>)> then, jk::OnError error);
void resolve(const jk::Server& server, std::function<void(jk::Stream)> then, jk::OnError error);

// Pure cores, exposed for the offline test harness.
std::vector<jk::AnimeCard> parseRecent(const std::string& html);
std::vector<jk::AnimeCard> parseBrowse(const std::string& html);
jk::AnimeDetail parseDetail(const std::string& slug, const std::string& html);
std::vector<jk::Server> parseServers(const std::string& html);

/// AnimeFLV anime slug from a /anime/<slug> or /ver/<slug>-<n> url.
std::string slugOf(const std::string& url);

}  // namespace flv
