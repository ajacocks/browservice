#include "download_manager.hpp"

#include "temp_dir.hpp"

#include "include/cef_download_handler.h"

namespace browservice {

CompletedDownload::CompletedDownload(CKey,
    shared_ptr<TempDir> tempDir,
    string path,
    string name
) {
    tempDir_ = tempDir;
    path_ = move(path);
    name_ = move(name);
}

CompletedDownload::~CompletedDownload() {
    if(unlink(path_.c_str())) {
        WARNING_LOG("Unlinking file ", path_, " failed");
    }
}

string CompletedDownload::path() {
    REQUIRE_UI_THREAD();
    return path_;
}

string CompletedDownload::name() {
    REQUIRE_UI_THREAD();
    return name_;
}

class DownloadManager::DownloadHandler : public CefDownloadHandler {
public:
    DownloadHandler(shared_ptr<DownloadManager> downloadManager) {
        downloadManager_ = downloadManager;
    }

    // CefDownloadHandler:
    virtual void OnBeforeDownload(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefDownloadItem> downloadItem,
        const CefString& suggestedName,
        CefRefPtr<CefBeforeDownloadCallback> callback
    ) override {
        REQUIRE_UI_THREAD();
        REQUIRE(downloadItem->IsValid());

        uint32_t id = downloadItem->GetId();
        REQUIRE(!downloadManager_->infos_.count(id));
        DownloadInfo& info = downloadManager_->infos_[id];

        info.fileIdx = downloadManager_->nextFileIdx_++;
        info.name = suggestedName;
        info.startCallback = callback;
        info.cancelCallback = nullptr;
        info.progress = 0;

        downloadManager_->pending_.push(id);
        downloadManager_->pendingDownloadCountChanged_();
    }

    virtual void OnDownloadUpdated(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefDownloadItem> downloadItem,
        CefRefPtr<CefDownloadItemCallback> callback
    ) override {
        REQUIRE_UI_THREAD();
        REQUIRE(downloadItem->IsValid());

        uint32_t id = downloadItem->GetId();
        if(!downloadManager_->infos_.count(id)) {
            return;
        }
        DownloadInfo& info = downloadManager_->infos_[id];
        if(info.startCallback) {
            return;
        }
        info.cancelCallback = callback;

        if(downloadItem->IsComplete()) {
            shared_ptr<CompletedDownload> file = CompletedDownload::create(
                downloadManager_->tempDir_,
                downloadManager_->getFilePath_(info.fileIdx),
                move(info.name)
            );
            downloadManager_->infos_.erase(id);
            postTask(
                downloadManager_->eventHandler_,
                &DownloadManagerEventHandler::onDownloadCompleted,
                file
            );
        } else if(!downloadItem->IsInProgress()) {
            info.cancelCallback->Cancel();
            downloadManager_->unlinkFile_(info.fileIdx);
            downloadManager_->infos_.erase(id);
        } else {
            info.progress = downloadItem->GetPercentComplete();
            if(info.progress == -1) {
                info.progress = 50;
            }
            info.progress = max(0, min(100, info.progress));
        }
        downloadManager_->downloadProgressChanged_();
    }

private:
    shared_ptr<DownloadManager> downloadManager_;

    IMPLEMENT_REFCOUNTING(DownloadHandler);
};

DownloadManager::DownloadManager(CKey,
    weak_ptr<DownloadManagerEventHandler> eventHandler
) {
    REQUIRE_UI_THREAD();

    eventHandler_ = eventHandler;
    nextFileIdx_ = 1;
}

/*
src/download_manager.cpp:125:45: note: use non-reference type 'const std::pair<unsigned int, browservice::DownloadManager::DownloadInfo>' to make the copy explicit or 'const std::pair<const unsigned int, browservice::DownloadManager::DownloadInfo>&' to prevent copying
src/download_manager.cpp: In member function 'void browservice::DownloadManager::downloadProgressChanged_()':
*/

DownloadManager::~DownloadManager() {
//    for(const pair<uint32_t, DownloadInfo>& p : infos_) {
    for(const std::pair<uint32_t, browservice::DownloadManager::DownloadInfo>& p : infos_) {
        const DownloadInfo& info = p.second;
        if(!info.startCallback) {
            if(info.cancelCallback) {
                info.cancelCallback->Cancel();
            }
            unlinkFile_(info.fileIdx);
        }
    }
}

void DownloadManager::acceptPendingDownload() {
    REQUIRE_UI_THREAD();

    if(!pending_.empty()) {
        uint32_t id = pending_.front();
        pending_.pop();
        pendingDownloadCountChanged_();

        REQUIRE(infos_.count(id));
        DownloadInfo& info = infos_[id];
        
        string path = getFilePath_(info.fileIdx);

        REQUIRE(info.startCallback);
        info.startCallback->Continue(path, false);
        info.startCallback = nullptr;

        downloadProgressChanged_();
    }
}

CefRefPtr<CefDownloadHandler> DownloadManager::createCefDownloadHandler() {
    REQUIRE_UI_THREAD();
    return new DownloadHandler(shared_from_this());
}

string DownloadManager::getFilePath_(int fileIdx) {
    if(!tempDir_) {
        tempDir_ = TempDir::create();
    }

    return tempDir_->path() + "/file_" + toString(fileIdx) + ".bin";
}

void DownloadManager::unlinkFile_(int fileIdx) {
    string path = getFilePath_(fileIdx);
    if(unlink(path.c_str())) {
        WARNING_LOG("Unlinking file ", path, " failed");
    }
}

void DownloadManager::pendingDownloadCountChanged_() {
    postTask(
        eventHandler_,
        &DownloadManagerEventHandler::onPendingDownloadCountChanged,
        (int)pending_.size()
    );
}

/*
src/download_manager.cpp:187:45: error: loop variable 'elem' of type 'const std::pair<unsigned int, browservice::DownloadManager::DownloadInfo>&' binds to a temporary constructed from type 'std::pair<const unsigned int, browservice::DownloadManager::DownloadInfo>' [-Werror=range-loop-construct]
  187 |     for(const pair<uint32_t, DownloadInfo>& elem : infos_) {
      |                                             ^~~~
src/download_manager.cpp:187:45: note: use non-reference type 'const std::pair<unsigned int, browservice::DownloadManager::DownloadInfo>' to make the copy explicit or 'const std::pair<const unsigned int, browservice::DownloadManager::DownloadInfo>&' to prevent copying
*/

void DownloadManager::downloadProgressChanged_() {
    vector<pair<int, int>> pairs;
//    for(const pair<uint32_t, DownloadInfo>& elem : infos_) {
    for(const std::pair<uint32_t, browservice::DownloadManager::DownloadInfo>& elem : infos_) {
        if(!elem.second.startCallback) {
            pairs.emplace_back(elem.second.fileIdx, elem.second.progress);
        }
    }
    sort(pairs.begin(), pairs.end());
    vector<int> progress;
    for(pair<int, int> p : pairs) {
        progress.push_back(p.second);
    }
    postTask(
        eventHandler_,
        &DownloadManagerEventHandler::onDownloadProgressChanged,
        progress
    );
}

}
