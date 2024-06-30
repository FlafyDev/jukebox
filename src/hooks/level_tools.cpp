#include <Geode/binding/LevelSelectLayer.hpp>
#include <Geode/binding/SongInfoObject.hpp>
#include <Geode/binding/LevelTools.hpp>
#include <Geode/modify/LevelTools.hpp>
#include <Geode/modify/LevelSelectLayer.hpp>
#include <Geode/modify/Modify.hpp>
#include <Geode/cocos/CCDirector.h>
#include <Geode/utils/cocos.hpp>

#include "../managers/nong_manager.hpp"

using namespace geode::prelude;

bool g_disableTitleOverride = false;

class $modify(LevelTools) {
    // Inlined in 2.206, commented for now
    // static SongInfoObject* getSongObject(int id) {
    //     SongInfoObject* og = LevelTools::getSongObject(id);
    //     if (og == nullptr) {
    //         return og;
    //     }
    //     int searchId = -id - 1;
    //     auto active = jukebox::NongManager::get()->getActiveNong(searchId);
    //     if (active.has_value()) {
    //         auto value = active.value();
    //         og->m_songName = value.songName;
    //         og->m_artistName = value.authorName;
    //     }
    //     return og;
    // }

    static gd::string getAudioTitle(int id) {
        if (
            g_disableTitleOverride ||
            !jukebox::NongManager::get()->initialized()
        ) {
            return LevelTools::getAudioTitle(id);
        }
        int searchID = -id - 1;
        auto res = jukebox::NongManager::get()->getNongs(searchID);
        if (res.has_value()) {
            return res.value()->active()->metadata->m_name;
        }
        return LevelTools::getAudioTitle(id);
    }
};

class $modify(LevelSelectLayer) {
    bool init(int p0) {
        g_disableTitleOverride = true;
        bool ret = LevelSelectLayer::init(p0);
        g_disableTitleOverride = false;
        return ret;
    }
};
