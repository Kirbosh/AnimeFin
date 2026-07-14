#include "api/provider.hpp"
#include "api/tioanime.hpp"

// Native, self-contained backend: AnimeFin scrapes the source directly and
// resolves the streaming hosts itself (see api/extractors.cpp), so there is no
// external server to depend on. More sources can be added to names()/the
// dispatch as they are ported.

namespace provider {

static size_t s_active = 0;

size_t active() { return s_active; }
void setActive(size_t i) {
    if (i < names().size()) s_active = i;
}

const std::vector<std::string>& names() {
    static const std::vector<std::string> n = {"TioAnime"};
    return n;
}

void getRecent(std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error) {
    tio::getRecent(then, error);
}

void getDirectory(
    int page, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error, const std::string& order) {
    tio::getDirectory(page, then, error, order);
}

void search(const std::string& query, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error) {
    tio::search(query, then, error);
}

void getDetail(const anime::AnimeCard& card, std::function<void(anime::AnimeDetail)> then, anime::OnError error) {
    tio::getDetail(card.slug, then, error);
}

void getServers(const std::string& episodeId, std::function<void(std::vector<anime::Server>)> then, anime::OnError error) {
    tio::getServers(episodeId, then, error);
}

void resolve(const anime::Server& server, std::function<void(anime::Stream)> then, anime::OnError error) {
    tio::resolve(server, then, error);
}

}  // namespace provider
