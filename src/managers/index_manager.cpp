#include "nong_manager.hpp"
#include "index_manager.hpp"

#include <Geode/utils/web.hpp>
#include <matjson.hpp>

#include "../../include/nong.hpp"
#include "../../include/index_serialize.hpp"
#include "../ui/indexes_setting.hpp"

namespace jukebox {

bool IndexManager::init() {
    if (m_initialized) {
        return true;
    }

    Mod::get()->addCustomSetting<IndexesSettingValue>(
        "indexes", Mod::get()
                       ->getSettingDefinition("indexes")
                       ->get<CustomSetting>()
                       ->json->get<std::vector<IndexSource>>("default"));

    auto path = this->baseIndexesPath();
    if (!std::filesystem::exists(path)) {
        std::filesystem::create_directory(path);
        return true;
    }

    if (auto err = fetchIndexes().error(); !err.empty()) {
        log::error("Failed to fetch indexes: {}", err);
        return false;
    }

    m_initialized = true;
    return true;
}

Result<std::vector<IndexSource>> IndexManager::getIndexes() {
    auto setting = Mod::get()->getSettingValue<IndexesSettingStruct>("indexes");
    log::info("Indexes: {}", setting.m_indexes.size());
    for (const auto index : setting.m_indexes) {
      log::info("Index({}): {}", index.m_enabled, index.m_url);
    }
    return Ok(setting.m_indexes);
}

std::filesystem::path IndexManager::baseIndexesPath() {
    static std::filesystem::path path = Mod::get()->getSaveDir() / "indexes-cache";
    return path;
}

Result<> IndexManager::loadIndex(std::filesystem::path path) {
    if (!std::filesystem::exists(path)) {
        return Err("Index file does not exist");
    }

    std::ifstream input(path);
    if (!input.is_open()) {
        return Err(fmt::format("Couldn't open file: {}", path.filename().string()));
    }

    std::string contents;
    input.seekg(0, std::ios::end);
    contents.resize(input.tellg());
    input.seekg(0, std::ios::beg);
    input.read(&contents[0], contents.size());
    input.close();

    std::string error;
    std::optional<matjson::Value> jsonObj = matjson::parse(contents, error);

    if (!jsonObj.has_value()) {
        return Err(error);
    }

    const auto indexRes = matjson::Serialize<IndexMetadata>::from_json(jsonObj.value());

    if (indexRes.isErr()) {
        return Err(indexRes.error());
    }

    const auto&& index = std::move(indexRes.value());

    cacheIndexName(index.m_id, index.m_name);

    for (const auto& [key, ytNong] : jsonObj.value()["nongs"]["youtube"].as_object()) {
        const auto& gdSongIDs = ytNong["songs"].as_array();
        for (const auto& gdSongIDValue : gdSongIDs) {
            int gdSongID = gdSongIDValue.as_int();
            if (!m_indexNongs.contains(gdSongID)) {
                m_indexNongs.emplace(gdSongID, Nongs(gdSongID));
            }
            if (auto err = m_indexNongs.at(gdSongID).add(
              YTSong(
                  SongMetadata(
                    gdSongID,
                    key,
                    ytNong["name"].as_string(),
                    ytNong["artist"].as_string(),
                    std::nullopt,
                    ytNong["startOffset"].as_int()
                  ),
                  ytNong["ytID"].as_string(),
                  index.m_id,
                  std::nullopt
              )); err.isErr()) {
                log::error("Failed to add YT song from index: {}", err.error());
            }
        }
    }

    for (const auto& [key, hostedNong] : jsonObj.value()["nongs"]["hosted"].as_object()) {
        const auto& gdSongIDs = hostedNong["songs"].as_array();
        for (const auto& gdSongIDValue : gdSongIDs) {
            int gdSongID = gdSongIDValue.as_int();
            if (!m_indexNongs.contains(gdSongID)) {
                m_indexNongs.emplace(gdSongID, Nongs(gdSongID));
            }
            if (auto err = m_indexNongs.at(gdSongID).add(
              HostedSong(
                  SongMetadata(
                    gdSongID,
                    key,
                    hostedNong["name"].as_string(),
                    hostedNong["artist"].as_string(),
                    std::nullopt,
                    hostedNong["startOffset"].as_int()
                  ),
                  hostedNong["url"].as_string(),
                  index.m_id,
                  std::nullopt
              )); err.isErr()) {
                log::error("Failed to add Hosted song from index: {}", err.error());
            }
        }
    }

    m_loadedIndexes.emplace(
        index.m_id,
        std::make_unique<IndexMetadata>(index)
    );

    log::info("Index \"{}\" ({}) Loaded. There are currently {} index Nongs objects.", index.m_name, index.m_id, m_indexNongs.size());

    return Ok();
}

Result<> IndexManager::fetchIndexes() {
    m_indexListeners.clear();
    m_indexNongs.clear();
    m_downloadSongListeners.clear();

    const auto indexesRes = getIndexes();
    if (indexesRes.isErr()) {
        return Err(indexesRes.error());
    }
    const auto indexes = indexesRes.value();

    for (const auto index : indexes) {
        log::info("Fetching index {}", index.m_url);
        if (!index.m_enabled || index.m_url.size() < 3) continue;

        // TODO replace "replace-later" with url to filename function
        auto filepath = baseIndexesPath() / fmt::format("{}.json", "replace-later");

        FetchIndexTask task = web::WebRequest().timeout(std::chrono::seconds(30)).get(index.m_url).map(
            [this, filepath, index](web::WebResponse *response) -> FetchIndexTask::Value {
                if (response->ok() && response->string().isOk()) {
                    std::string error;
                    std::optional<matjson::Value> jsonObj = matjson::parse(response->string().value(), error);

                    if (!jsonObj.has_value()) {
                        return Err(error);
                    }

                    if (!jsonObj.value().is_object()) {
                        return Err("Index supposed to be an object");
                    }
                    jsonObj.value().set("url", index.m_url);
                    const auto indexRes = matjson::Serialize<IndexMetadata>::from_json(jsonObj.value());

                    if (indexRes.isErr()) {
                        return Err(indexRes.error());
                    }

                    std::ofstream output(filepath);
                    if (!output.is_open()) {
                        return Err(fmt::format("Couldn't open file: {}", filepath));
                    }
                    output << jsonObj.value().dump(matjson::NO_INDENTATION);
                    output.close();

                    return Ok();
                }
                return Err("Web request failed");
            },
            [](web::WebProgress *progress) -> FetchIndexTask::Progress {
                return progress->downloadProgress().value_or(0) / 100.f;
            }
        );

        auto listener = EventListener<FetchIndexTask>();
        listener.bind([this, index, filepath](FetchIndexTask::Event* event) {
            if (float *progress = event->getProgress()) {
                return;
            }

            m_indexListeners.erase(index.m_url);

            if (FetchIndexTask::Value *result = event->getValue()) {
                if (result->isErr()) {
                    log::error("Failed to fetch index: {}", result->error());
                } else {
                    log::info("Index fetched and cached: {}", index.m_url);
                }
            } else if (event->isCancelled()) {}

            if (auto err = loadIndex(filepath).error(); !err.empty()) {
                log::error("Failed to load index: {}", err);
            }
        });
        listener.setFilter(task);
        m_indexListeners.emplace(index.m_url, std::move(listener));
    }

    return Ok();
}

std::optional<float> IndexManager::getSongDownloadProgress(const std::string& uniqueID) {
    if (m_downloadSongListeners.contains(uniqueID)) {
        return m_downloadProgress.at(uniqueID);
    }
    return std::nullopt;
}

std::optional<std::string> IndexManager::getIndexName(const std::string& indexID) {
    auto jsonObj = Mod::get()->getSavedValue<matjson::Value>("cached-index-names");
    if (!jsonObj.contains(indexID)) return std::nullopt;
    return jsonObj[indexID].as_string();
}

void IndexManager::cacheIndexName(const std::string& indexId, const std::string& indexName) {
    auto jsonObj = Mod::get()->getSavedValue<matjson::Value>("cached-index-names", {});
    jsonObj.set(indexId, indexName);
    Mod::get()->setSavedValue("cached-index-names", jsonObj);
}

Result<std::vector<Nong>> IndexManager::getNongs(int gdSongID) {
    auto nongs = std::vector<Nong>();
    auto localNongs = NongManager::get()->getNongs(gdSongID);
    if (!localNongs.has_value()) {
        return Err("Failed to get nongs");
    }
    std::optional<Nongs*> indexNongs = m_indexNongs.contains(gdSongID) ? std::optional(&m_indexNongs.at(gdSongID)) : std::nullopt;

    nongs.push_back(Nong(*localNongs.value()->defaultSong()));

    for (std::unique_ptr<LocalSong>& song : localNongs.value()->locals()) {
        nongs.push_back(Nong(*song));
    }

    std::vector<std::string> addedIndexSongs;
    for (std::unique_ptr<YTSong>& song : localNongs.value()->youtube()) {
        // Check if song is from an index
        if (indexNongs.has_value() && song->indexID().has_value()) {
            for (std::unique_ptr<YTSong>& indexSong : indexNongs.value()->youtube()) {
                if (song->metadata()->m_uniqueID == indexSong->metadata()->m_uniqueID) {
                    addedIndexSongs.push_back(song->metadata()->m_uniqueID);
                    // TODO: update song metadata from index
                }
            }
        }
        nongs.push_back(Nong(*song));
    }

    for (std::unique_ptr<HostedSong>& song : localNongs.value()->hosted()) {
        // Check if song is from an index
        if (indexNongs.has_value() && song->indexID().has_value()) {
            for (std::unique_ptr<HostedSong>& indexSong : indexNongs.value()->hosted()) {
                if (song->metadata()->m_uniqueID == indexSong->metadata()->m_uniqueID) {
                    addedIndexSongs.push_back(song->metadata()->m_uniqueID);
                    // TODO: update song metadata from index
                }
            }
        }
        nongs.push_back(Nong(*song));
    }

    if (indexNongs.has_value()) {
        for (std::unique_ptr<YTSong>& song : indexNongs.value()->youtube()) {
            // Check if song is not already added
            if (std::find(addedIndexSongs.begin(), addedIndexSongs.end(), song->metadata()->m_uniqueID) == addedIndexSongs.end()) {
                nongs.push_back(Nong(*song));
            }
        }

        for (std::unique_ptr<HostedSong>& song : indexNongs.value()->hosted()) {
            // Check if song is not already added
            if (std::find(addedIndexSongs.begin(), addedIndexSongs.end(), song->metadata()->m_uniqueID) == addedIndexSongs.end()) {
                nongs.push_back(Nong(*song));
            }
        }
    }

    return Ok(std::move(nongs));
}

Result<> IndexManager::stopDownloadingSong(int gdSongID, const std::string& uniqueID) {
    if (m_downloadSongListeners.contains(uniqueID)) {
        m_downloadSongListeners.erase(uniqueID);
        m_downloadProgress.erase(uniqueID);
    } else {
        return Err("Trying to stop downloading a song that is not being downloaded");
    }
    SongStateChanged(gdSongID).post();
    return Ok();
}

Result<> IndexManager::downloadSong(int gdSongID, const std::string& uniqueID) {
    auto nongs = IndexManager::get()->getNongs(gdSongID);
    if (!nongs.has_value()) {
        return Err("GD song {} not initialized in manifest", gdSongID);
    }
    for (Nong& nong : nongs.value()) {
        if (nong.metadata()->m_uniqueID == uniqueID) {
            return nong.visit<Result<>>([](LocalSong* localSong){
                return Err("Song type not supported for download");
            }, [](YTSong* ytSong){
                return Err("Song type not supported for download");
            }, [](HostedSong* hostedSong){
                return IndexManager::get()->downloadSong(*hostedSong);
            });
        }
    }

    return Err("Song {} not found in manifest", uniqueID);
}

Result<> IndexManager::downloadSong(HostedSong& hosted) {
    const std::string id = hosted.metadata()->m_uniqueID;

    m_downloadProgress.erase(id);
    m_downloadSongListeners.erase(id);

    DownloadSongTask task = web::WebRequest().timeout(std::chrono::seconds(30)).get(hosted.url()).map(
        [this](web::WebResponse *response) -> DownloadSongTask::Value {
            if (response->ok() && response->string().isOk()) {

              auto destination = NongManager::get()->generateSongFilePath("mp3");
              std::ofstream file(destination, std::ios::out | std::ios::binary);
              file.write(reinterpret_cast<const char *>(response->data().data()), response->data().size());
              file.close();

              return Ok(destination);
            }
            return Err("Web request failed");
        },
        [](web::WebProgress *progress) -> DownloadSongTask::Progress {
            return progress->downloadProgress().value_or(0) / 100.f;
        }
    );

    auto listener = EventListener<DownloadSongTask>();
    listener.bind([this, id, hosted](DownloadSongTask::Event* event) {
        if (float *progress = event->getProgress()) {
            m_downloadProgress[id] = *progress;
            SongDownloadProgress(hosted.metadata()->m_gdID, id, *event->getProgress()).post();
            return;
        }
        m_downloadProgress.erase(id);
        m_downloadSongListeners.erase(id);
        if (event->isCancelled())  {
            log::error("Failed to fetch song: cancelled");
            SongStateChanged(hosted.metadata()->m_gdID).post();
            return;
        }
        DownloadSongTask::Value *result = event->getValue();
        if (result->isErr()) {
            log::error("Failed to fetch song: {}", result->error());
            SongStateChanged(hosted.metadata()->m_gdID).post();
            return;
        }

        if (!hosted.indexID().has_value()) {
            auto _ = NongManager::get()->getNongs(hosted.metadata()->m_gdID).value()->deleteSong(id);
        }

        Nong nong = {
            HostedSong {
              SongMetadata(*hosted.metadata()),
              hosted.url(),
              hosted.indexID(),
              result->ok().value(),
            },
        };
        if (auto res = NongManager::get()->addNongs(std::move(nong.toNongs().unwrap())); res.isErr()) {
            log::error("Failed to add song: {}", res.error());
            SongStateChanged(hosted.metadata()->m_gdID).post();
            return;
        }
        if (auto res = NongManager::get()->setActiveSong(hosted.metadata()->m_gdID, hosted.metadata()->m_uniqueID); res.isErr()) {
            log::error("Failed to set song as active: {}", res.error());
            SongStateChanged(hosted.metadata()->m_gdID).post();
            return;
        }

        SongStateChanged(hosted.metadata()->m_gdID).post();
    });
    listener.setFilter(task);
    m_downloadSongListeners.emplace(id, std::move(listener));
    m_downloadProgress[id] = 0.f;
    SongDownloadProgress(hosted.metadata()->m_gdID, id, 0.f).post();
    return Ok();
}

};
