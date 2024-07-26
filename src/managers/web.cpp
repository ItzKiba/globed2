#include "web.hpp"

#include <managers/account.hpp>
#include <managers/central_server.hpp>
#include <util/net.hpp>
#include <util/time.hpp>
#include <net/manager.hpp>

using namespace geode::prelude;

using RequestTask = WebRequestManager::Task;

static std::string makeUrl(std::string_view baseUrl, std::string_view suffix) {
    std::string base(baseUrl);

    if (suffix.starts_with('/') && base.ends_with('/')) {
        base.pop_back();
    } else if (!suffix.starts_with('/') && !base.ends_with('/')) {
        base.push_back('/');
    }

    base += suffix;

    return base;
}

static std::string makeCentralUrl(std::string_view suffix) {
    auto& csm = CentralServerManager::get();
    auto active = csm.getActive();

    GLOBED_REQUIRE(active, "trying to make a web request to a central server when no active central server is selected");

    return makeUrl(active->url, suffix);
}

static RequestTask mapTask(CurlManager::Task&& param) {
    return param;
}

RequestTask WebRequestManager::requestAuthToken() {
    auto& gam = GlobedAccountManager::get();

    auto authkey = gam.getAuthKey();
    auto gdData = gam.gdData.lock();

    return this->post(makeCentralUrl("v2/totplogin"), 5, [&](CurlRequest& req) {
        matjson::Object accdata;
        accdata["account_id"] = gdData->accountId;
        accdata["user_id"] = gdData->userId;
        accdata["username"] = gdData->accountName;

        matjson::Object obj;
        obj["account_data"] = accdata;

        // recode as urlsafe
        // honestly i dont remember why this is needed anymore but its almost midnight and im so tired and i just wanna go to sleep but it  didnt work without this
        auto key = util::crypto::base64Encode(util::crypto::base64Decode(authkey), util::crypto::Base64Variant::URLSAFE);
        obj["authkey"] = key;

        req.bodyJSON(obj);
        req.encrypted(true);
        req.param("protocol", NetworkManager::get().getUsedProtocol());
    });
}

RequestTask WebRequestManager::challengeStart() {
    auto& gam = GlobedAccountManager::get();

    auto gdData = gam.gdData.lock();

    return this->post(makeCentralUrl("v2/challenge/new"), 5, [&](CurlRequest& req) {
        matjson::Object accdata;
        accdata["account_id"] = gdData->accountId;
        accdata["user_id"] = gdData->userId;
        accdata["username"] = gdData->accountName;

        req.bodyJSON(accdata);
        req.encrypted(true);
        req.param("protocol", NetworkManager::get().getUsedProtocol());
    });
}

RequestTask WebRequestManager::challengeFinish(std::string_view authcode) {
    auto& gam = GlobedAccountManager::get();

    auto gdData = gam.gdData.lock();

    return this->post(makeCentralUrl("v2/challenge/verify"), 30, [&](CurlRequest& req) {
        matjson::Object accdata;
        accdata["account_id"] = gdData->accountId;
        accdata["user_id"] = gdData->userId;
        accdata["username"] = gdData->accountName;

        matjson::Object obj;
        obj["account_data"] = accdata;
        obj["answer"] = std::string(authcode);

        req.bodyJSON(obj);
        req.encrypted(true);
    });
}

RequestTask WebRequestManager::testServer(std::string_view url) {
    return this->get(makeUrl(url, "version"));
}

RequestTask WebRequestManager::fetchCredits() {
    return this->get("https://credits.globed.dev/credits");
}

RequestTask WebRequestManager::fetchServers() {
    return this->get(makeCentralUrl("servers"), 3, [&](CurlRequest& req) {
        req.param("protocol", NetworkManager::get().getUsedProtocol());
    });
}

RequestTask WebRequestManager::fetchFeaturedLevel() {
    return this->get(makeCentralUrl("flevel/current"));
}

RequestTask WebRequestManager::fetchFeaturedLevelHistory(int page) {
    return this->get(makeCentralUrl("flevel/historyv2"), 5, [&](CurlRequest& req) {
        req.param("page", page);
    });
}

RequestTask WebRequestManager::setFeaturedLevel(int levelId, int rateTier, std::string_view levelName, std::string_view levelAuthor, int difficulty) {
    return this->post(makeCentralUrl("flevel/replace"), 5, [&](CurlRequest& req) {
        req.param("newlevel", levelId);
        req.param("rate_tier", rateTier);
        req.param("aid", GlobedAccountManager::get().gdData.lock()->accountId);
        req.param("adminpwd", GlobedAccountManager::get().getTempAdminPassword());
        req.param("levelname", levelName);
        req.param("levelauthor", levelAuthor);
        req.param("difficulty", difficulty);
    });
}

RequestTask WebRequestManager::get(std::string_view url) {
    return get(url, 5);
}

RequestTask WebRequestManager::get(std::string_view url, int timeoutS) {
    return get(url, timeoutS, [](auto&) {});
}

RequestTask WebRequestManager::get(std::string_view url, int timeoutS, std::function<void(CurlRequest&)> additional) {
#ifdef GLOBED_DEBUG
    log::debug("GET request: {}", url);
#endif

    auto request = CurlRequest()
        .timeout(util::time::seconds(timeoutS));

    additional(request);

    return mapTask(request.get(url).send());
}


RequestTask WebRequestManager::post(std::string_view url) {
    return post(url, 5);
}

RequestTask WebRequestManager::post(std::string_view url, int timeoutS) {
    return post(url, timeoutS, [](auto&) {});
}

RequestTask WebRequestManager::post(std::string_view url, int timeoutS, std::function<void(CurlRequest&)> additional) {
#ifdef GLOBED_DEBUG
    log::debug("POST request: {}", url);
#endif

    auto request = CurlRequest()
        .timeout(util::time::seconds(timeoutS));

    additional(request);

    return mapTask(request.post(url).send());
}

