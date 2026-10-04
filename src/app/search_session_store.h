#ifndef FOREVERTAS_APP_SEARCH_SESSION_STORE_H
#define FOREVERTAS_APP_SEARCH_SESSION_STORE_H

#include "searches/search_runner.h"

#include <QString>
#include <QVariantList>

namespace forevertas::app {

struct SearchSessionLocation {
    QString mapKey;
    QString mapName;
    QString directory;
};

class SearchSessionStore {
public:
    static SearchSessionLocation Create(const SearchRequest &request,
                                        const QString &root = {});
    static SearchSessionLocation Identify(const SearchRequest &request);
    static QVariantList SessionsForMap(const QString &mapKey,
                                       const QString &root = {});
    static QVariantList Cycles(const QString &directory);
    static QString Inputs(const QString &directory,
                          const QString &fileName);
    static void SaveCycle(const SearchSessionLocation &session,
                          const SearchRequest &request,
                          std::uint64_t restartNumber,
                          const SearchResult &result);
    static void SaveAbortedCycle(const SearchSessionLocation &session,
                                 const SearchRequest &request,
                                 std::uint64_t restartNumber,
                                 const SearchLiveUpdate &result);
    static QString Root(const QString &overrideRoot = {});
};

}  // namespace forevertas::app

#endif
