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

#include "api/jkanime.hpp"

namespace provider {

enum class Source { JK, FLV };

/// Currently active browse source.
Source active();
void setActive(Source s);

/// Display names, indexed to match the Source enum order.
const std::vector<std::string>& names();

// Browse — uses the active source.
void getRecent(std::function<void(std::vector<jk::AnimeCard>)> then, jk::OnError error);
void getDirectory(int page, std::function<void(std::vector<jk::AnimeCard>)> then, jk::OnError error,
    const std::string& order = "default");

/// Sort options supported by the active source (empty when it can't sort).
const std::vector<std::pair<std::string, std::string>>& sortOptions();
void search(const std::string& query, std::function<void(std::vector<jk::AnimeCard>)> then, jk::OnError error);

// Detail / playback — routed by the item's own URL host.
void getDetail(const jk::AnimeCard& card, std::function<void(jk::AnimeDetail)> then, jk::OnError error);
void getServers(const std::string& episodeUrl, std::function<void(std::vector<jk::Server>)> then, jk::OnError error);
void resolve(const jk::Server& server, std::function<void(jk::Stream)> then, jk::OnError error);

}  // namespace provider
