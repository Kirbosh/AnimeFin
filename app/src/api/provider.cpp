#include "api/provider.hpp"
#include "api/stremio.hpp"

namespace provider {

size_t active() { return strm::activeCatalog(); }
void setActive(size_t i) { strm::setActiveCatalog(i); }

const std::vector<std::string>& names() {
    static std::vector<std::string> n = [] {
        std::vector<std::string> v;
        for (auto& c : strm::catalogs()) v.push_back(c.name);
        return v;
    }();
    return n;
}

void getRecent(std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error) {
    strm::getRecent(then, error);
}

void getDirectory(
    int page, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error, const std::string& order) {
    strm::getDirectory(page, then, error, order);
}

void search(const std::string& query, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error) {
    strm::search(query, then, error);
}

void getDetail(const anime::AnimeCard& card, std::function<void(anime::AnimeDetail)> then, anime::OnError error) {
    strm::getDetail(card.slug, then, error);
}

void getServers(const std::string& episodeId, std::function<void(std::vector<anime::Server>)> then, anime::OnError error) {
    strm::getStreams(episodeId, then, error);
}

void resolve(const anime::Server& server, std::function<void(anime::Stream)> then, anime::OnError error) {
    strm::resolve(server, then, error);
}

}  // namespace provider
