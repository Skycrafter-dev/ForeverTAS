#ifndef FOREVERTAS_VIEWER_GO_TO_H
#define FOREVERTAS_VIEWER_GO_TO_H

// "Go to": a place on the race's timeline typed the way it comes to mind.
//   30  30.5  1:02.35  1:02:03  1500ms  1m30s  2.5s   a time (bare = seconds)
//   +2  -0.5  +500ms                                    from where it is now
//   cp 3  checkpoint 3  finish  start  end              the shown race's marks
//   next  prev  (next cp, previous cp)                  the next or previous mark
//   50%  tick 120                                       of the race's length; a tick

#include <QString>

#include <optional>
#include <vector>

namespace forevertas::viewer {

struct GoToMark {
    qint64 timeMs = 0;
    int index = 0;  // the checkpoint number (1 = first)
    bool isFinish = false;
};

struct GoToContext {
    qint64 currentMs = 0;
    qint64 durationMs = 0;
    qint64 tickMs = 10;
    std::vector<GoToMark> marks;  // in time order
};

struct GoToResult {
    std::optional<qint64> timeMs;  // on a tick, within the race
    QString error;                 // plain words, when there is no time
};

GoToResult ResolveGoTo(const QString &text, const GoToContext &context);

}  // namespace forevertas::viewer

#endif
