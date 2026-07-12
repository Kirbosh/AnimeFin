/*
    Source dispatch for AnimeFin.

    The backend is a Stremio addon that aggregates several Spanish sources and
    returns directly-playable stream URLs (its server resolves the video hosts).
    The "source" the user picks is which of the addon's catalogs to browse; an
    item's id names its own source, so detail and playback work for a card
    opened from any catalog.
*/

#pragma once

#include <string>
#include <vector>
#include <functional>

#include "api/media.hpp"

namespace provider {

/// Active browse catalog (index into names()).
size_t active();
void setActive(size_t i);

/// Display names of the selectable catalogs.
const std::vector<std::string>& names();

// Browse — uses the active catalog.
void getRecent(std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error);
void getDirectory(int page, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error,
    const std::string& order = "default");

/// Sort options supported by the active source (empty when it can't sort).
const std::vector<std::pair<std::string, std::string>>& sortOptions();
void search(const std::string& query, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error);

// Detail / playback — the item id names its source.
void getDetail(const anime::AnimeCard& card, std::function<void(anime::AnimeDetail)> then, anime::OnError error);
void getServers(const std::string& episodeId, std::function<void(std::vector<anime::Server>)> then, anime::OnError error);
void resolve(const anime::Server& server, std::function<void(anime::Stream)> then, anime::OnError error);

}  // namespace provider
