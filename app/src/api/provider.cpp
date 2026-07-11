#include "api/provider.hpp"
#include "api/animeflv.hpp"

namespace provider {

// AnimeFLV is the default browse source: larger catalogue, JSON-embedded
// episode/server data (more robust to scrape) and explicit SUB/LAT labelling.
// JKAnime stays one button away (BUTTON_X) as a fallback.
static Source current = Source::FLV;

Source active() { return current; }
void setActive(Source s) { current = s; }

const std::vector<std::string>& names() {
    static const std::vector<std::string> n = {"JKAnime", "AnimeFLV"};
    return n;
}

void getRecent(std::function<void(std::vector<jk::AnimeCard>)> then, jk::OnError error) {
    if (current == Source::FLV)
        flv::getRecent(then, error);
    else
        jk::getRecent(then, error);
}

void getDirectory(int page, std::function<void(std::vector<jk::AnimeCard>)> then, jk::OnError error) {
    if (current == Source::FLV)
        flv::getDirectory(page, then, error);
    else
        jk::getDirectory(page, then, error);
}

void search(const std::string& query, std::function<void(std::vector<jk::AnimeCard>)> then, jk::OnError error) {
    if (current == Source::FLV)
        flv::search(query, then, error);
    else
        jk::search(query, then, error);
}

// --- routed by host so a card resolves against its own site ---------------

static bool isFlv(const std::string& url) { return url.find("animeflv") != std::string::npos; }

void getDetail(const jk::AnimeCard& card, std::function<void(jk::AnimeDetail)> then, jk::OnError error) {
    if (isFlv(card.url))
        flv::getDetail(card.slug, then, error);
    else
        jk::getDetail(card.slug, then, error);
}

void getServers(const std::string& episodeUrl, std::function<void(std::vector<jk::Server>)> then, jk::OnError error) {
    if (isFlv(episodeUrl))
        flv::getServers(episodeUrl, then, error);
    else
        jk::getServers(episodeUrl, then, error);
}

void resolve(const jk::Server& server, std::function<void(jk::Stream)> then, jk::OnError error) {
    if (isFlv(server.url))
        flv::resolve(server, then, error);
    else
        jk::resolve(server, then, error);
}

}  // namespace provider
