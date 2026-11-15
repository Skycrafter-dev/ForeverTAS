#include "viewer/go_to.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace forevertas::viewer {
namespace {

QString Example() {
    return QStringLiteral("Type a time like 30, 1:02.35, +2, cp 3, next or finish.");
}

// A length of time in milliseconds: 1:02.35, 1m30s, 1500ms, 2.5 (seconds).
std::optional<double> ParseLength(const QString &text) {
    static const QRegularExpression clock(
            QStringLiteral(R"(^(\d+):(\d{1,2})(?::(\d{1,2}))?(?:\.(\d+))?$)"));
    if (const auto match = clock.match(text); match.hasMatch()) {
        const bool hours = match.capturedLength(3) > 0;
        const double first = match.captured(1).toDouble();
        const double second = match.captured(2).toDouble();
        const double third = hours ? match.captured(3).toDouble() : 0.0;
        if (second >= 60 || third >= 60) return std::nullopt;
        const double fraction = match.capturedLength(4) > 0
                ? (QStringLiteral("0.") + match.captured(4)).toDouble() : 0.0;
        const double seconds = hours ? first * 3600 + second * 60 + third : first * 60 + second;
        return (seconds + fraction) * 1000.0;
    }
    static const QRegularExpression part(QStringLiteral(R"((\d+(?:\.\d+)?|\.\d+)\s*(ms|h|min|m|sec|s)?)"));
    double total = 0.0;
    qsizetype position = 0;
    bool any = false;
    bool bare = false;
    auto parts = part.globalMatch(text);
    while (parts.hasNext()) {
        const auto match = parts.next();
        if (match.capturedStart() != position) return std::nullopt;
        // a bare number is seconds, and only alone
        if (bare) return std::nullopt;
        const double value = match.captured(1).toDouble();
        const QString unit = match.captured(2);
        bare = unit.isEmpty();
        total += value * (unit == QLatin1String("ms") ? 1.0
                          : unit == QLatin1String("h") ? 3600000.0
                          : unit == QLatin1String("m") || unit == QLatin1String("min") ? 60000.0
                          : 1000.0);
        position = match.capturedEnd();
        while (position < text.size() && text.at(position).isSpace()) ++position;
        any = true;
    }
    if (!any || position != text.size()) return std::nullopt;
    return total;
}

}  // namespace

GoToResult ResolveGoTo(const QString &typed, const GoToContext &context) {
    QString text = typed.trimmed().toLower();
    // a decimal comma, as some keyboards type it
    text.replace(QRegularExpression(QStringLiteral(R"((\d),(\d))")), QStringLiteral("\\1.\\2"));
    text = text.simplified();
    if (text.isEmpty()) return {std::nullopt, Example()};

    const qint64 tick = std::max<qint64>(1, context.tickMs);
    const auto place = [&](double ms) -> GoToResult {
        if (!std::isfinite(ms)) return {std::nullopt, Example()};
        const qint64 snapped = static_cast<qint64>(std::llround(ms / static_cast<double>(tick))) * tick;
        return {std::clamp<qint64>(snapped, 0, std::max<qint64>(0, context.durationMs)), {}};
    };

    if (text == QLatin1String("start") || text == QLatin1String("beginning")) return place(0);
    if (text == QLatin1String("end")) return place(static_cast<double>(context.durationMs));
    if (text == QLatin1String("finish") || text == QLatin1String("fin")) {
        for (const GoToMark &mark : context.marks)
            if (mark.isFinish) return place(static_cast<double>(mark.timeMs));
        return {std::nullopt, QStringLiteral("This race doesn't reach the finish.")};
    }
    static const QRegularExpression next(QStringLiteral(R"(^(next|next cp|next checkpoint|cp\+)$)"));
    static const QRegularExpression previous(
            QStringLiteral(R"(^(prev|previous|prev cp|previous cp|previous checkpoint|cp-)$)"));
    if (next.match(text).hasMatch()) {
        for (const GoToMark &mark : context.marks)
            if (mark.timeMs > context.currentMs) return place(static_cast<double>(mark.timeMs));
        return {std::nullopt, QStringLiteral("There is no checkpoint after this.")};
    }
    if (previous.match(text).hasMatch()) {
        for (auto mark = context.marks.rbegin(); mark != context.marks.rend(); ++mark)
            if (mark->timeMs < context.currentMs) return place(static_cast<double>(mark->timeMs));
        return {std::nullopt, QStringLiteral("There is no checkpoint before this.")};
    }
    static const QRegularExpression checkpoint(QStringLiteral(R"(^(?:cp|checkpoint)\s*(\d+)$)"));
    if (const auto match = checkpoint.match(text); match.hasMatch()) {
        const int wanted = match.captured(1).toInt();
        int count = 0;
        for (const GoToMark &mark : context.marks) {
            if (mark.isFinish) continue;
            ++count;
            if (mark.index == wanted) return place(static_cast<double>(mark.timeMs));
        }
        return {std::nullopt, count == 0 ? QStringLiteral("This race takes no checkpoint.")
                                         : QStringLiteral("This race takes %1 checkpoint%2.")
                                                   .arg(count)
                                                   .arg(count == 1 ? QString() : QStringLiteral("s"))};
    }
    static const QRegularExpression tickNumber(QStringLiteral(R"(^(?:t|tick)\s*(\d+)$)"));
    if (const auto match = tickNumber.match(text); match.hasMatch())
        return place(match.captured(1).toDouble() * static_cast<double>(tick));
    static const QRegularExpression percent(QStringLiteral(R"(^(\d+(?:\.\d+)?)\s*%$)"));
    if (const auto match = percent.match(text); match.hasMatch())
        return place(match.captured(1).toDouble() / 100.0 * static_cast<double>(context.durationMs));

    double sign = 0.0;
    if (text.startsWith(QLatin1Char('+')) || text.startsWith(QLatin1Char('-'))) {
        sign = text.startsWith(QLatin1Char('+')) ? 1.0 : -1.0;
        text = text.mid(1).trimmed();
    }
    const auto length = ParseLength(text);
    if (!length) return {std::nullopt, Example()};
    return place(sign == 0.0 ? *length : static_cast<double>(context.currentMs) + sign * *length);
}

}  // namespace forevertas::viewer
