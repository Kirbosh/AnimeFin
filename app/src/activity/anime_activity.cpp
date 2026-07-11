/*
    AnimeFin browse activity — recent / directory / search tabs over jkanime.
*/

#include "activity/anime_activity.hpp"
#include "activity/anime_detail.hpp"
#include "activity/server_list.hpp"

#include "view/auto_tab_frame.hpp"
#include "view/video_card.hpp"
#include "view/svg_image.hpp"
#include "tab/setting_tab.hpp"
#include "utils/image.hpp"
#include "utils/keybind.hpp"

using namespace brls::literals;

static HTTP::Header jkImageHeaders() {
    return {"User-Agent: " + jk::USER_AGENT, "Referer: " + jk::HOST};
}

// ------------------------------------------------------------ AnimeCardSource

AnimeCardSource::AnimeCardSource(std::vector<jk::AnimeCard> list) : list(std::move(list)) {}

size_t AnimeCardSource::getItemCount() { return this->list.size(); }

RecyclingGridItem* AnimeCardSource::cellForRow(RecyclingView* recycler, size_t index) {
    MediaCardCell* cell = dynamic_cast<MediaCardCell*>(recycler->dequeueReusableCell("Cell"));
    auto& item = this->list.at(index);
    cell->labelTitle->setText(item.title);
    if (item.extra.empty()) {
        cell->labelExt->setVisibility(brls::Visibility::GONE);
    } else {
        cell->labelExt->setVisibility(brls::Visibility::VISIBLE);
        cell->labelExt->setText(item.extra);
    }
    if (!item.poster.empty()) Image::with(cell->picture, item.poster, jkImageHeaders());
    return cell;
}

void AnimeCardSource::onItemSelected(brls::Box* recycler, size_t index) {
    brls::Application::pushActivity(new AnimeDetail(this->list.at(index)));
}

void AnimeCardSource::appendData(const std::vector<jk::AnimeCard>& data) {
    this->list.insert(this->list.end(), data.begin(), data.end());
}

void AnimeCardSource::clearData() { this->list.clear(); }

// ------------------------------------------------------------ AnimeRecentTab

AnimeRecentTab::AnimeRecentTab() {
    this->setGrow(1.f);
    this->setPadding(20, 40, 20, 40);
    this->registerCell("Cell", []() { return new MediaCardCell(); });
    this->estimatedRowHeight = 300;
    this->spanCount = 6;
    this->showSkeleton();

    ASYNC_RETAIN
    jk::getRecent(
        [ASYNC_TOKEN](std::vector<jk::AnimeCard> r) {
            ASYNC_RELEASE
            if (r.empty()) {
                this->setEmpty();
            } else {
                this->setDataSource(new AnimeCardSource(std::move(r)));
            }
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            this->setError(ex);
        });
}

brls::View* AnimeRecentTab::create() { return new AnimeRecentTab(); }

// --------------------------------------------------------- AnimeDirectoryTab

AnimeDirectoryTab::AnimeDirectoryTab() {
    this->setGrow(1.f);
    this->setPadding(20, 40, 20, 40);
    this->registerCell("Cell", []() { return new MediaCardCell(); });
    this->estimatedRowHeight = 300;
    this->spanCount = 6;
    this->showSkeleton();

    this->onNextPage([this]() { this->loadPage(); });
    this->loadPage();
}

void AnimeDirectoryTab::loadPage() {
    if (this->finished) return;

    ASYNC_RETAIN
    jk::getDirectory(
        this->page,
        [ASYNC_TOKEN](std::vector<jk::AnimeCard> r) {
            ASYNC_RELEASE
            if (r.empty()) {
                this->finished = true;
                if (this->page == 1) this->setEmpty();
                return;
            }
            if (this->page == 1) {
                this->setDataSource(new AnimeCardSource(std::move(r)));
            } else {
                auto* src = dynamic_cast<AnimeCardSource*>(this->getDataSource());
                if (src) {
                    src->appendData(r);
                    this->notifyDataChanged();
                }
            }
            this->page++;
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            if (this->page == 1) this->setError(ex);
        });
}

brls::View* AnimeDirectoryTab::create() { return new AnimeDirectoryTab(); }

// ------------------------------------------------------------ AnimeSearchTab

AnimeSearchTab::AnimeSearchTab() {
    this->setGrow(1.f);
    this->setAxis(brls::Axis::COLUMN);

    this->hint = new brls::Label();
    this->hint->setText("anime/search_hint"_i18n);
    this->hint->setFontSize(16);
    this->hint->setMargins(20, 40, 10, 40);
    this->addView(this->hint);

    this->grid = new RecyclingGrid();
    this->grid->setGrow(1.f);
    this->grid->setPadding(10, 40, 20, 40);
    this->grid->registerCell("Cell", []() { return new MediaCardCell(); });
    this->grid->estimatedRowHeight = 300;
    this->grid->spanCount = 6;
    this->grid->setEmpty("anime/search_empty"_i18n);
    this->addView(this->grid);

    auto searchAction = [this](brls::View*) {
        return brls::Application::getImeManager()->openForText(
            [this](const std::string& text) {
                if (!text.empty()) this->doSearch(text);
            },
            "anime/search"_i18n, "", 64, "");
    };
    this->registerAction("anime/search"_i18n, brls::BUTTON_Y, searchAction);
    this->hint->registerClickAction(searchAction);
    this->grid->registerAction("anime/search"_i18n, brls::BUTTON_Y, searchAction);
}

void AnimeSearchTab::doSearch(const std::string& query) {
    this->hint->setText(fmt::format("{}: {}", "anime/search"_i18n, query));
    this->grid->showSkeleton();

    jk::search(
        query,
        [this](std::vector<jk::AnimeCard> r) {
            if (r.empty()) {
                this->grid->setEmpty("anime/search_empty"_i18n);
            } else {
                this->grid->setDataSource(new AnimeCardSource(std::move(r)));
            }
        },
        [this](const std::string& ex) { this->grid->setError(ex); });
}

brls::View* AnimeSearchTab::create() { return new AnimeSearchTab(); }

// ---------------------------------------------------------- AnimeJellyfinTab

AnimeJellyfinTab::AnimeJellyfinTab() {
    this->setGrow(1.f);
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::CENTER);
    this->setAlignItems(brls::AlignItems::CENTER);

    auto* label = new brls::Label();
    label->setText("anime/jellyfin_hint"_i18n);
    label->setFontSize(18);
    label->setMargins(0, 0, 20, 0);
    this->addView(label);

    auto* btn = new brls::Button();
    btn->setText("anime/jellyfin_open"_i18n);
    btn->registerClickAction([](brls::View*) {
        brls::Application::pushActivity(new ServerList());
        return true;
    });
    this->addView(btn);
}

brls::View* AnimeJellyfinTab::create() { return new AnimeJellyfinTab(); }

// ------------------------------------------------------------- AnimeActivity

AnimeActivity::AnimeActivity() { brls::Logger::debug("AnimeActivity: create"); }

AnimeActivity::~AnimeActivity() { brls::Logger::debug("AnimeActivity: delete"); }

void AnimeActivity::onContentAvailable() {}
