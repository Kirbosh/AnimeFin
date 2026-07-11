/*
    Anime detail page for AnimeFin: poster, metadata, and the episode grid.
    Picking an episode fetches its servers, lets the user choose one, resolves
    it to a direct stream and hands it to mpv.
*/

#pragma once

#include <memory>
#include <atomic>

#include <borealis.hpp>
#include <borealis/core/bind.hpp>

#include "api/jkanime.hpp"

class RecyclingGrid;
class TextBox;

class AnimeDetail : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("activity/anime_detail.xml");

    explicit AnimeDetail(const jk::AnimeCard& card);
    ~AnimeDetail() override;

    void onContentAvailable() override;

    /// Fetch the servers for an episode, prompt for one, resolve and play.
    static void playEpisode(const jk::Episode& ep);

private:
    void load();

    jk::AnimeCard card;
    jk::AnimeDetail detail;
    bool loaded = false;
    // ASYNC_RETAIN is View-only; this Activity guards its async callbacks with
    // a liveness flag instead. Callbacks fire on the UI thread (via brls::sync),
    // same thread as the destructor, so a plain check is race-free.
    std::shared_ptr<std::atomic_bool> alive = std::make_shared<std::atomic_bool>(true);

    BRLS_BIND(brls::Image, imagePoster, "anime/image/poster");
    BRLS_BIND(brls::Label, labelTitle, "anime/label/title");
    BRLS_BIND(brls::Label, labelStatus, "anime/label/status");
    BRLS_BIND(brls::Label, labelType, "anime/label/type");
    BRLS_BIND(brls::Label, labelGenres, "anime/label/genres");
    BRLS_BIND(brls::Label, labelNext, "anime/label/next");
    BRLS_BIND(TextBox, labelOverview, "anime/label/overview");
    BRLS_BIND(brls::Box, btnPlay, "anime/play");
    BRLS_BIND(RecyclingGrid, episodes, "anime/episodes");
};
