/*
    AnimeFin browse activity: the JKAnime home screen.

    An AutoTabFrame with three poster grids — recent episodes, the full
    directory (paginated) and search — plus the shared Settings tab. This is
    the app's entry point; picking a poster opens an AnimeDetail.
*/

#pragma once

#include <borealis.hpp>
#include <borealis/core/bind.hpp>

#include "view/recycling_grid.hpp"
#include "api/jkanime.hpp"

class AutoTabFrame;

/// A grid data source over jkanime catalogue cards; selecting one opens detail.
class AnimeCardSource : public RecyclingGridDataSource {
public:
    explicit AnimeCardSource(std::vector<jk::AnimeCard> list);

    size_t getItemCount() override;
    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override;
    void onItemSelected(brls::Box* recycler, size_t index) override;
    void appendData(const std::vector<jk::AnimeCard>& data);
    void clearData() override;

private:
    std::vector<jk::AnimeCard> list;
};

/// Open a source picker (JKAnime / AnimeFLV) and run `onPick` after switching.
void showSourcePicker(brls::View* owner, const std::function<void()>& onPick);

/// Homepage "Animes recientes" grid.
class AnimeRecentTab : public RecyclingGrid {
public:
    AnimeRecentTab();
    static brls::View* create();

private:
    void load();
};

/// Full catalogue, paginated as the user scrolls.
class AnimeDirectoryTab : public RecyclingGrid {
public:
    AnimeDirectoryTab();
    static brls::View* create();

private:
    void reload();
    void loadPage();
    int page = 1;
    bool finished = false;
};

/// Search box + results grid.
class AnimeSearchTab : public brls::Box {
public:
    AnimeSearchTab();
    static brls::View* create();

private:
    void doSearch(const std::string& query);
    RecyclingGrid* grid;
    brls::Label* hint;
    std::string lastQuery;
};

/// Bridge back to the stock Switchfin Jellyfin experience.
class AnimeJellyfinTab : public brls::Box {
public:
    AnimeJellyfinTab();
    static brls::View* create();
};

class AnimeActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("activity/anime.xml");

    AnimeActivity();
    ~AnimeActivity() override;

    void onContentAvailable() override;

private:
    BRLS_BIND(AutoTabFrame, tabFrame, "anime/tabFrame");
};
