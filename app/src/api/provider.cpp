#include "api/provider.hpp"
#include "api/tioanime.hpp"
#include "api/monoschinos.hpp"

namespace provider {

// TioAnime is the default browse source: it server-renders the whole pipeline
// (episode list and `var videos = [...]` server list live in the page HTML), so
// it scrapes reliably without any XHR. Monoschinos is the dub-heavy alternative
// and is one button away (BUTTON_X).
static Source current = Source::TIO;

Source active() { return current; }
void setActive(Source s) { current = s; }

const std::vector<std::string>& names() {
    // Indexed to match the Source enum order (TIO, MONO).
    static const std::vector<std::string> n = {"TioAnime", "Monoschinos"};
    return n;
}

void getRecent(std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error) {
    if (current == Source::MONO)
        mono::getRecent(then, error);
    else
        tio::getRecent(then, error);
}

void getDirectory(
    int page, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error, const std::string& order) {
    if (current == Source::MONO)
        mono::getDirectory(page, then, error, order);
    else
        tio::getDirectory(page, then, error, order);
}

const std::vector<std::pair<std::string, std::string>>& sortOptions() {
    // pair = { source order value, i18n key }. TioAnime's /directorio?sort= knob.
    static const std::vector<std::pair<std::string, std::string>> tioSort = {
        {"default", "anime/sort_default"},
        {"recent", "anime/sort_recent"},
    };
    static const std::vector<std::pair<std::string, std::string>> none = {};
    return current == Source::TIO ? tioSort : none;
}

void search(const std::string& query, std::function<void(std::vector<anime::AnimeCard>)> then, anime::OnError error) {
    if (current == Source::MONO)
        mono::search(query, then, error);
    else
        tio::search(query, then, error);
}

// --- routed by host so a card resolves against its own site ---------------

static bool isMono(const std::string& url) { return url.find("monoschinos") != std::string::npos; }

void getDetail(const anime::AnimeCard& card, std::function<void(anime::AnimeDetail)> then, anime::OnError error) {
    if (isMono(card.url))
        mono::getDetail(card.slug, then, error);
    else
        tio::getDetail(card.slug, then, error);
}

void getServers(const std::string& episodeUrl, std::function<void(std::vector<anime::Server>)> then, anime::OnError error) {
    if (isMono(episodeUrl))
        mono::getServers(episodeUrl, then, error);
    else
        tio::getServers(episodeUrl, then, error);
}

void resolve(const anime::Server& server, std::function<void(anime::Stream)> then, anime::OnError error) {
    if (isMono(server.url))
        mono::resolve(server, then, error);
    else
        tio::resolve(server, then, error);
}

}  // namespace provider
