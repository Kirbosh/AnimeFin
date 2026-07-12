/*
    Source dispatch for AnimeFin.

    The browse tabs (recent / directory / search) query whichever source is
    currently active. Detail, server-list and stream resolution are routed by
    the URL's host instead, so a card opened from any grid resolves against the
    right site regardless of which source is active now.
*/

#pragma once

#include <string>
#include <vector>
#include <functional>

#include "api/media.hpp"

namespace provider {

enum class Source { TIO, MONO };

/// Currently active browse source.
Source active();
void setActive(Source s);

/// Display names, indexed to match the Source enum order.
const std::vector<std::string>& names();

// Browse — uses the active source.
void getRecent(std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error);
void getDirectory(int page, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error,
    const std::string& order = "default");

/// Sort options supported by the active source (empty when it can't sort).
const std::vector<std::pair<std::string, std::string>>& sortOptions();
void search(const std::string& query, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error);

// Detail / playback — routed by the item's own URL host.
void getDetail(const anime::AnimeCard& card, std::function<void(anime::AnimeDetail)> then, anime::OnError error);
void getServers(const std::string& episodeUrl, std::function<void(std::vector<anime::Server>)> then, anime::OnError error);
void resolve(const anime::Server& server, std::function<void(anime::Stream)> then, anime::OnError error);

}  // namespace provider
