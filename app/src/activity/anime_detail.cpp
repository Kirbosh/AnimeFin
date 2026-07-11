/*
    Anime detail page + episode playback for AnimeFin.
*/

#include "activity/anime_detail.hpp"

#include "api/provider.hpp"
#include "view/recycling_grid.hpp"
#include "view/video_card.hpp"
#include "view/video_view.hpp"
#include "view/mpv_core.hpp"
#include "view/video_profile.hpp"
#include "view/player_setting.hpp"
#include "view/text_box.hpp"
#include "utils/image.hpp"
#include "utils/dialog.hpp"

using namespace brls::literals;

// Browser headers (UA + host-matched referer) so each source's CDN serves the
// poster/thumbnail images.
static HTTP::Header jkImageHeaders(const std::string& url = "") {
    std::string ref;
    size_t s = url.find("://");
    if (s != std::string::npos) {
        size_t e = url.find('/', s + 3);
        ref = (e == std::string::npos ? url : url.substr(0, e)) + "/";
    }
    if (ref.empty()) ref = jk::HOST;
    return {"User-Agent: " + jk::USER_AGENT, "Referer: " + ref};
}

// --------------------------------------------------------------- AnimePlayer

/// Minimal fullscreen player around VideoView + MPVCore, modelled on
/// remote_view.cpp's RemotePlayer but for a single resolved stream.
class AnimePlayer : public brls::Box {
public:
    AnimePlayer(const std::string& title, const std::string& url, const std::string& extra) {
        float width = brls::Application::contentWidth;
        float height = brls::Application::contentHeight;
        view->setDimensions(width, height);
        view->setWidthPercentage(100);
        view->setHeightPercentage(100);
        view->setId("video");
        view->setTitie(title);
        view->hideVideoQuality();
        this->setDimensions(width, height);
        this->addView(view);

        auto& mpv = MPVCore::instance();
        eventSubscribeID = mpv.getEvent()->subscribe([this](MpvEventEnum event) {
            if (event == MpvEventEnum::MPV_LOADED) view->getProfile()->init("JKAnime");
        });
        settingSubscribeID = view->getSettingEvent()->subscribe([]() {
            brls::Application::pushActivity(new brls::Activity(new PlayerSetting()));
        });
        playSubscribeID = view->getPlayEvent()->subscribe([](int index) { return VideoView::close(true); });

        mpv.reset();
        mpv.setUrl(url, extra);
    }

    ~AnimePlayer() override {
        auto& mpv = MPVCore::instance();
        mpv.getEvent()->unsubscribe(eventSubscribeID);
        view->getPlayEvent()->unsubscribe(playSubscribeID);
        view->getSettingEvent()->unsubscribe(settingSubscribeID);
        mpv.stop();
    }

    static void play(const std::string& title, const jk::Stream& stream) {
        std::stringstream ssextra;
        ssextra << fmt::format("network-timeout={}", HTTP::TIMEOUT / 100);
        if (HTTP::PROXY_STATUS) ssextra << ",http-proxy=\"" << HTTP::PROXY << "\"";
        ssextra << stream.mpvExtra();

        AnimePlayer* player = new AnimePlayer(title, stream.url, ssextra.str());
        brls::Application::pushActivity(new brls::Activity(player), brls::TransitionAnimation::NONE);
        brls::Application::giveFocus(player->view);
    }

private:
    VideoView* view = new VideoView();
    MPVEvent::Subscription eventSubscribeID;
    brls::Event<int>::Subscription playSubscribeID;
    brls::VoidEvent::Subscription settingSubscribeID;
};

// --------------------------------------------------------------- EpisodeSource

class EpisodeSource : public RecyclingGridDataSource {
public:
    EpisodeSource(std::vector<jk::Episode> eps, std::string poster)
        : list(std::move(eps)), poster(std::move(poster)) {}

    size_t getItemCount() override { return this->list.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        MediaCardCell* cell = dynamic_cast<MediaCardCell*>(recycler->dequeueReusableCell("Cell"));
        auto& ep = this->list.at(index);
        cell->labelTitle->setText(ep.title);
        cell->labelExt->setVisibility(brls::Visibility::GONE);
        const std::string& img = ep.thumb.empty() ? this->poster : ep.thumb;
        if (!img.empty()) Image::with(cell->picture, img, jkImageHeaders(img));
        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        AnimeDetail::playEpisode(this->list.at(index));
    }

    void clearData() override { this->list.clear(); }

private:
    std::vector<jk::Episode> list;
    std::string poster;
};

// --------------------------------------------------------------- AnimeDetail

AnimeDetail::AnimeDetail(const jk::AnimeCard& card) : card(card) {
    brls::Logger::debug("AnimeDetail: create {}", card.slug);
}

AnimeDetail::~AnimeDetail() {
    this->alive->store(false);
    Image::cancel(this->imagePoster);
}

void AnimeDetail::onContentAvailable() {
    this->labelTitle->setText(this->card.title);
    this->labelStatus->getParent()->setVisibility(brls::Visibility::GONE);
    this->labelType->getParent()->setVisibility(brls::Visibility::GONE);
    this->labelNext->setVisibility(brls::Visibility::GONE);

    if (!this->card.poster.empty())
        Image::with(this->imagePoster, this->card.poster, jkImageHeaders(this->card.poster));

    this->episodes->registerCell("Cell", []() { return new MediaCardCell(); });
    this->episodes->spanCount = 4;
    this->episodes->estimatedRowHeight = 130;
    this->episodes->showSkeleton();

    this->btnPlay->registerClickAction([this](...) {
        if (this->detail.episodes.empty()) return true;
        AnimeDetail::playEpisode(this->detail.episodes.front());
        return true;
    });
    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });

    this->load();
}

void AnimeDetail::load() {
    auto alive = this->alive;
    provider::getDetail(
        this->card,
        [this, alive](jk::AnimeDetail d) {
            if (!alive->load()) return;
            this->detail = d;
            this->loaded = true;

            // Keep the card's title (set in onContentAvailable) — it is the
            // reliable one; only the richer detail fields are filled in here.
            if (!d.poster.empty()) Image::with(this->imagePoster, d.poster, jkImageHeaders(d.poster));

            if (!d.status.empty()) {
                this->labelStatus->setText(d.status);
                this->labelStatus->getParent()->setVisibility(brls::Visibility::VISIBLE);
            }
            if (!d.type.empty()) {
                this->labelType->setText(d.type);
                this->labelType->getParent()->setVisibility(brls::Visibility::VISIBLE);
            }
            if (!d.genres.empty()) this->labelGenres->setText(d.genres);
            if (!d.next.empty()) {
                this->labelNext->setText(fmt::format("{} {}", "anime/next"_i18n, d.next));
                this->labelNext->setVisibility(brls::Visibility::VISIBLE);
            }
            this->labelOverview->setText(
                d.synopsis.empty() ? "anime/no_synopsis"_i18n : d.synopsis);

            if (d.episodes.empty()) {
                this->episodes->setEmpty();
            } else {
                this->episodes->setDataSource(new EpisodeSource(d.episodes, d.poster));
            }
        },
        [this, alive](const std::string& ex) {
            if (!alive->load()) return;
            this->episodes->setError(ex);
        });
}

void AnimeDetail::playEpisode(const jk::Episode& ep) {
    std::string title = ep.title;
    brls::Application::blockInputs();

    provider::getServers(
        ep.url,
        [title](std::vector<jk::Server> servers) {
            brls::Application::unblockInputs();
            if (servers.empty()) {
                Dialog::show("anime/no_servers"_i18n);
                return;
            }

            std::vector<std::string> names;
            for (auto& s : servers) names.push_back(s.name);

            // Let the user choose which host to stream from.
            brls::Dropdown* dropdown = new brls::Dropdown(
                "anime/choose_server"_i18n, names, [servers, title](int selected) {
                    if (selected < 0 || selected >= (int)servers.size()) return;
                    brls::Application::blockInputs();
                    provider::resolve(
                        servers.at(selected),
                        [title](jk::Stream stream) {
                            brls::Application::unblockInputs();
                            if (stream.url.empty()) {
                                Dialog::show("anime/resolve_failed"_i18n);
                                return;
                            }
                            AnimePlayer::play(title, stream);
                        },
                        [](const std::string& ex) {
                            brls::Application::unblockInputs();
                            Dialog::show(ex);
                        });
                });
            brls::Application::pushActivity(new brls::Activity(dropdown));
        },
        [](const std::string& ex) {
            brls::Application::unblockInputs();
            Dialog::show(ex);
        });
}
