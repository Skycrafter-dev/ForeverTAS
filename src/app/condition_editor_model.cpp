#include "app/condition_editor_model.h"

#include "conditions/condition_language_catalog.h"
#include "conditions/condition_program.h"
#include "conditions/condition_syntax.h"
#include "searches/algorithm_registry.h"

#include <QByteArray>
#include <QQuickTextDocument>
#include <QSet>
#include <QStringList>
#include <QTextDocument>

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace forevertas::app {
namespace {

QString FromView(std::string_view value) {
    return QString::fromUtf8(value.data(),
                             static_cast<qsizetype>(value.size()));
}

QString UserFacingType(std::string_view value) {
    QString result = FromView(value);
    return result.replace(QStringLiteral("scalar"),
                          QStringLiteral("number"),
                          Qt::CaseInsensitive);
}

QStringList AliasList(const std::vector<std::string_view> &aliases,
                      std::string_view canonical) {
    QStringList result;
    for (const std::string_view alias : aliases) {
        if (alias == canonical) continue;
        result.push_back(FromView(alias));
    }
    return result;
}

bool IsDisabledLine(const QString &line) {
    int index = 0;
    while (index < line.size() && line[index].isSpace()) ++index;
    return index + 1 < line.size() && line[index] == QLatin1Char('/') &&
            line[index + 1] == QLatin1Char('/');
}

QString ObjectDescription(const QString &path) {
    if (path == QStringLiteral("car")) {
        return ConditionEditorModel::tr(
                "Current and previous car telemetry. Press Ctrl+Space after "
                "car. to browse its direct fields.");
    }
    if (path == QStringLiteral("car.prev")) {
        return ConditionEditorModel::tr(
                "Car telemetry from the previous simulation tick.");
    }
    if (path == QStringLiteral("car.wheels")) {
        return ConditionEditorModel::tr(
                "Per-wheel contact, sliding, and surface telemetry.");
    }
    if (path.startsWith(QStringLiteral("car.wheels."))) {
        return ConditionEditorModel::tr(
                "Telemetry for this wheel at the current simulation tick.");
    }
    if (path == QStringLiteral("last_improvement")) {
        return ConditionEditorModel::tr(
                "Search state captured when the last best result appeared.");
    }
    if (path == QStringLiteral("last_restart")) {
        return ConditionEditorModel::tr(
                "Search state captured when the current search started.");
    }
    return ConditionEditorModel::tr(
            "Condition object. Press Ctrl+Space after the trailing dot to "
            "browse its direct fields.");
}

int MatchScore(const QString &query, const QStringList &values) {
    if (query.isEmpty()) return 0;
    const QString folded = query.toCaseFolded();
    int best = std::numeric_limits<int>::max();
    for (const QString &value : values) {
        const QString candidate = value.toCaseFolded();
        if (candidate == folded) best = std::min(best, 0);
        else if (candidate.startsWith(folded)) best = std::min(best, 10);
        else {
            const qsizetype index = candidate.indexOf(folded);
            if (index >= 0) best = std::min(best, 30 + static_cast<int>(index));
        }
    }
    return best;
}

int MemberMatchScore(const QString &query, const QStringList &identifiers) {
    if (query.isEmpty()) return 0;
    const QString folded = query.toCaseFolded();
    int best = std::numeric_limits<int>::max();
    for (const QString &identifier : identifiers) {
        const QString candidate = identifier.toCaseFolded();
        if (candidate == folded) best = std::min(best, 0);
        else if (candidate.startsWith(folded)) best = std::min(best, 10);
    }
    return best;
}

QString FunctionInsertion(const ConditionLanguageFunction &function) {
    return FromView(function.canonicalName) + QStringLiteral("()");
}

int FunctionCursorOffset(const QString &insertText) {
    return std::max(0, static_cast<int>(insertText.size()) - 1);
}

struct QtRange final {
    int absoluteStart = 0;
    int length = 1;
    int line = 1;
    int column = 1;
};

QtRange ToQtRange(const QString &source,
                  const ConditionDiagnostic &diagnostic) {
    const QStringList lines = source.split(QLatin1Char('\n'),
                                           Qt::KeepEmptyParts);
    const int lineNumber = std::clamp(
            static_cast<int>(diagnostic.line), 1,
            static_cast<int>(lines.size()));
    const QString &line = lines[lineNumber - 1];
    int absoluteStart = 0;
    for (int index = 0; index + 1 < lineNumber; ++index) {
        absoluteStart += lines[index].size() + 1;
    }

    const QByteArray bytes = line.toUtf8();
    const int byteStart = std::clamp(
            static_cast<int>(diagnostic.column) - 1, 0,
            static_cast<int>(bytes.size()));
    const int byteEnd = std::clamp(
            byteStart + static_cast<int>(diagnostic.length),
            byteStart,
            static_cast<int>(bytes.size()));
    const int utf16Column =
            QString::fromUtf8(bytes.constData(), byteStart).size();
    int utf16Length = QString::fromUtf8(bytes.constData() + byteStart,
                                        byteEnd - byteStart)
                                .size();
    if (utf16Length == 0) utf16Length = 1;
    return {absoluteStart + utf16Column,
            utf16Length,
            lineNumber,
            utf16Column + 1};
}

int Utf16PositionForByteOffset(const QString &source,
                               std::size_t byteOffset) {
    const QByteArray bytes = source.toUtf8();
    const qsizetype bounded = std::clamp<qsizetype>(
            static_cast<qsizetype>(byteOffset), 0, bytes.size());
    return QString::fromUtf8(bytes.constData(), bounded).size();
}

std::size_t Utf8OffsetForUtf16Position(const QString &source,
                                       int position) {
    const int bounded = std::clamp(
            position, 0, static_cast<int>(source.size()));
    return static_cast<std::size_t>(source.left(bounded).toUtf8().size());
}

bool ContainsForDocumentation(ConditionSourceRange range,
                              std::size_t position) {
    return range.begin == range.end ? position == range.begin
                                    : position >= range.begin &&
                    position < range.end;
}

const ConditionSyntaxNode *FindDocumentationNode(
        const ConditionSyntaxNode &node,
        std::size_t position) {
    if (!ContainsForDocumentation(node.range, position)) return nullptr;

    if (node.kind == ConditionSyntaxKind::Call &&
        ContainsForDocumentation(node.segmentRange, position)) {
        return &node;
    }
    if (node.kind == ConditionSyntaxKind::Member &&
        ContainsForDocumentation(node.segmentRange, position)) {
        return &node;
    }
    for (const auto &child : node.children) {
        if (const ConditionSyntaxNode *const found =
                    FindDocumentationNode(*child, position)) {
            return found;
        }
    }
    if (node.kind == ConditionSyntaxKind::Name ||
        node.kind == ConditionSyntaxKind::String ||
        node.kind == ConditionSyntaxKind::Member) {
        return &node;
    }
    return nullptr;
}

QString KindLabel(const ConditionLanguageSymbol &symbol) {
    return symbol.kind == ConditionLanguageSymbolKind::Vector
            ? QStringLiteral("vector")
            : QStringLiteral("number");
}

}  // namespace

ConditionEditorModel::ConditionEditorModel(QObject *parent)
    : QObject(parent),
      highlighter_(new ConditionSyntaxHighlighter(this)) {
    analyze();
}

QString ConditionEditorModel::source() const { return source_; }
int ConditionEditorModel::cursorPosition() const noexcept {
    return cursorPosition_;
}
QString ConditionEditorModel::evaluationTargetId() const {
    return evaluationTargetId_;
}
QString ConditionEditorModel::gateMode() const { return gateMode_; }
bool ConditionEditorModel::valid() const noexcept { return valid_; }
int ConditionEditorModel::gateCount() const noexcept { return gateCount_; }
QVariantList ConditionEditorModel::diagnostics() const { return diagnostics_; }
QVariantList ConditionEditorModel::lineStates() const { return lineStates_; }
QString ConditionEditorModel::statusText() const { return statusText_; }
QVariantList ConditionEditorModel::completions() const { return completions_; }
QVariantMap ConditionEditorModel::completionContext() const {
    return completionContext_;
}
QVariantMap ConditionEditorModel::parameterHint() const {
    return parameterHint_;
}
QVariantList ConditionEditorModel::catalogue() const { return catalogue_; }
qulonglong ConditionEditorModel::documentRevision() const noexcept {
    return documentRevision_;
}

void ConditionEditorModel::setSource(const QString &value) {
    updateDocumentState(value, cursorPosition_);
}

void ConditionEditorModel::setCursorPosition(int value) {
    updateDocumentState(source_, value);
}

void ConditionEditorModel::setEvaluationTargetId(const QString &value) {
    if (evaluationTargetId_ == value) return;
    evaluationTargetId_ = value;
    ++documentRevision_;
    analyze();
    emit analysisChanged();
    emit assistanceChanged();
}

void ConditionEditorModel::setGateMode(const QString &value) {
    const QString normalized = value.compare(QStringLiteral("or"),
                                             Qt::CaseInsensitive) == 0
            ? QStringLiteral("or")
            : QStringLiteral("and");
    if (gateMode_ == normalized) return;
    gateMode_ = normalized;
    ++documentRevision_;
    analyze();
    emit analysisChanged();
    emit assistanceChanged();
}

QVariantMap ConditionEditorModel::completionEdit(int index) const {
    if (index < 0 || index >= completions_.size()) return {};
    return completions_[index].toMap();
}

QVariantMap ConditionEditorModel::functionCallEdit(int cursorPosition) const {
    const int bounded = std::clamp(
            cursorPosition, 0, static_cast<int>(source_.size()));
    int identifierStart = bounded;
    while (identifierStart > 0) {
        const QChar character = source_[identifierStart - 1];
        if (!character.isLetterOrNumber() && character != QLatin1Char('_'))
            break;
        --identifierStart;
    }
    if (identifierStart == bounded ||
        (identifierStart > 0 &&
         source_[identifierStart - 1] == QLatin1Char('.'))) {
        return {};
    }

    const QByteArray identifier =
            source_.mid(identifierStart, bounded - identifierStart).toUtf8();
    if (FindConditionFunction(std::string_view(
                identifier.constData(),
                static_cast<std::size_t>(identifier.size()))) == nullptr) {
        return {};
    }

    const bool hasClosingParenthesis =
            bounded < source_.size() &&
            source_[bounded] == QLatin1Char(')');
    const QString insertion = hasClosingParenthesis
            ? QStringLiteral("(") : QStringLiteral("()");
    return {{QStringLiteral("source"),
             source_.left(bounded) + insertion + source_.mid(bounded)},
            {QStringLiteral("cursorPosition"), bounded + 1}};
}

void ConditionEditorModel::updateDocumentState(const QString &source,
                                               int cursorPosition) {
    const int bounded = std::clamp(
            cursorPosition, 0, static_cast<int>(source.size()));
    const bool sourceChanged = source_ != source;
    const bool cursorChanged = cursorPosition_ != bounded;
    if (!sourceChanged && !cursorChanged) return;

    source_ = source;
    cursorPosition_ = bounded;
    ++documentRevision_;
    if (sourceChanged) {
        analyze();
        emit analysisChanged();
    } else {
        updateAssistance();
    }
    emit assistanceChanged();
}

QVariantMap ConditionEditorModel::acceptCompletion(
        const QString &completionId,
        qulonglong revision) {
    const auto rejected = [this]() {
        return QVariantMap{{QStringLiteral("accepted"), false},
                           {QStringLiteral("source"), source_},
                           {QStringLiteral("cursorPosition"),
                            cursorPosition_},
                           {QStringLiteral("revision"),
                            QVariant::fromValue(documentRevision_)}};
    };
    if (completionId.isEmpty() || revision != documentRevision_) {
        return rejected();
    }

    QVariantMap selected;
    for (const QVariant &entry : completions_) {
        const QVariantMap candidate = entry.toMap();
        if (candidate.value(QStringLiteral("completionId")).toString() ==
            completionId) {
            selected = candidate;
            break;
        }
    }
    if (selected.isEmpty() ||
        selected.value(QStringLiteral("revision")).toULongLong() !=
                documentRevision_) {
        return rejected();
    }

    const int start = selected.value(QStringLiteral("replaceStart")).toInt();
    const int length =
            selected.value(QStringLiteral("replaceLength")).toInt();
    const QString insertion =
            selected.value(QStringLiteral("insertText")).toString();
    if (start < 0 || length < 0 || start + length > source_.size()) {
        return rejected();
    }

    const QString updated = source_.left(start) + insertion +
            source_.mid(start + length);
    const int updatedCursor = start +
            selected.value(QStringLiteral("cursorOffset")).toInt();
    const bool reopen =
            selected.value(QStringLiteral("isContainer")).toBool();
    updateDocumentState(updated, updatedCursor);
    return {{QStringLiteral("accepted"), true},
            {QStringLiteral("source"), source_},
            {QStringLiteral("cursorPosition"), cursorPosition_},
            {QStringLiteral("revision"),
             QVariant::fromValue(documentRevision_)},
            {QStringLiteral("reopen"), reopen}};
}

QVariantMap ConditionEditorModel::toggleLine(int lineNumber,
                                             int cursorPosition) const {
    QStringList lines = source_.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    if (lineNumber < 1 || lineNumber > lines.size()) return {};

    QString &line = lines[lineNumber - 1];
    int contentStart = 0;
    while (contentStart < line.size() && line[contentStart].isSpace() &&
           line[contentStart] != QLatin1Char('\r')) {
        ++contentStart;
    }
    if (line.mid(contentStart).trimmed().isEmpty()) return {};

    int absoluteStart = contentStart;
    for (int index = 0; index < lineNumber - 1; ++index) {
        absoluteStart += lines[index].size() + 1;
    }
    int adjustedCursor = std::clamp(
            cursorPosition, 0, static_cast<int>(source_.size()));
    if (IsDisabledLine(line)) {
        int removeLength = 2;
        if (contentStart + removeLength < line.size() &&
            line[contentStart + removeLength] == QLatin1Char(' ')) {
            ++removeLength;
        }
        line.remove(contentStart, removeLength);
        if (adjustedCursor > absoluteStart + removeLength) {
            adjustedCursor -= removeLength;
        } else if (adjustedCursor >= absoluteStart) {
            adjustedCursor = absoluteStart;
        }
    } else {
        constexpr int kPrefixLength = 3;
        line.insert(contentStart, QStringLiteral("// "));
        if (adjustedCursor >= absoluteStart) adjustedCursor += kPrefixLength;
    }

    const QString result = lines.join(QLatin1Char('\n'));
    return {{QStringLiteral("source"), result},
            {QStringLiteral("cursorPosition"),
             std::clamp(adjustedCursor, 0,
                        static_cast<int>(result.size()))}};
}

QVariantList ConditionEditorModel::searchCatalogue(
        const QString &query) const {
    struct Match final {
        int score = 0;
        int order = 0;
        QVariant value;
    };
    std::vector<Match> matches;
    matches.reserve(static_cast<std::size_t>(catalogue_.size()));
    for (int index = 0; index < catalogue_.size(); ++index) {
        const QVariantMap item = catalogue_[index].toMap();
        QStringList values{item.value(QStringLiteral("symbol")).toString(),
                           item.value(QStringLiteral("label")).toString(),
                           item.value(QStringLiteral("category")).toString(),
                           item.value(QStringLiteral("description")).toString()};
        values.append(item.value(QStringLiteral("aliases")).toStringList());
        const int score = MatchScore(query, values);
        if (score == std::numeric_limits<int>::max()) continue;
        matches.push_back({score, index, catalogue_[index]});
    }
    std::stable_sort(matches.begin(), matches.end(),
                     [](const Match &left, const Match &right) {
                         if (left.score != right.score) {
                             return left.score < right.score;
                         }
                         return left.order < right.order;
                     });
    QVariantList result;
    result.reserve(static_cast<qsizetype>(matches.size()));
    for (const Match &match : matches) result.push_back(match.value);
    return result;
}

QVariantMap ConditionEditorModel::documentationAt(int position) const {
    if (source_.isEmpty()) return {};
    const int bounded = std::clamp(
            position, 0, static_cast<int>(source_.size()));
    const int probe = bounded == source_.size() ? bounded - 1 : bounded;
    const QByteArray utf8 = source_.toUtf8();
    const std::size_t bytePosition =
            Utf8OffsetForUtf16Position(source_, probe);
    std::size_t lineBegin = bytePosition;
    while (lineBegin > 0u && utf8.at(static_cast<qsizetype>(lineBegin - 1u)) !=
                                      '\n') {
        --lineBegin;
    }
    std::size_t lineEnd = bytePosition;
    while (lineEnd < static_cast<std::size_t>(utf8.size()) &&
           utf8.at(static_cast<qsizetype>(lineEnd)) != '\n' &&
           utf8.at(static_cast<qsizetype>(lineEnd)) != '\r') {
        ++lineEnd;
    }
    const std::string_view bytes(
            utf8.constData(), static_cast<std::size_t>(utf8.size()));
    const ConditionParsedLine parsed = ParseConditionLine(
            bytes.substr(lineBegin, lineEnd - lineBegin), lineBegin);
    if (!parsed.root) return {};
    const ConditionSyntaxNode *const syntax =
            FindDocumentationNode(*parsed.root, bytePosition);
    if (syntax == nullptr) return {};
    const ConditionSourceRange documentationRange =
            syntax->kind == ConditionSyntaxKind::Call ||
                    syntax->kind == ConditionSyntaxKind::Member
            ? syntax->segmentRange : syntax->range;
    const int documentationStart =
            Utf16PositionForByteOffset(source_, documentationRange.begin);
    const int documentationEnd =
            Utf16PositionForByteOffset(source_, documentationRange.end);
    const auto located = [documentationStart,
                          documentationEnd](QVariantMap value) {
        value.insert(QStringLiteral("start"), documentationStart);
        value.insert(QStringLiteral("length"),
                     std::max(0, documentationEnd - documentationStart));
        return value;
    };
    const std::string syntaxName = syntax->kind == ConditionSyntaxKind::Call
            ? syntax->text
            : ConditionSyntaxName(*syntax);
    if (syntaxName.empty()) return {};
    const std::string_view name(syntaxName);
    const std::vector<ConditionLanguageSymbol> &symbols =
            GetConditionSymbols();
    if (const ConditionLanguageSymbol *const symbol =
                FindConditionSymbol(name)) {
        const auto found = std::find_if(
                symbols.begin(), symbols.end(),
                [symbol](const ConditionLanguageSymbol &candidate) {
                    return &candidate == symbol;
                });
        if (found != symbols.end()) {
            return located(symbolMap(static_cast<std::size_t>(
                    std::distance(symbols.begin(), found))));
        }
    }
    const std::vector<ConditionLanguageFunction> &functions =
            GetConditionFunctions();
    if (const ConditionLanguageFunction *const function =
                FindConditionFunction(name)) {
        const auto found = std::find_if(
                functions.begin(), functions.end(),
                [function](const ConditionLanguageFunction &candidate) {
                    return &candidate == function;
                });
        if (found != functions.end()) {
            return located(functionMap(static_cast<std::size_t>(
                    std::distance(functions.begin(), found))));
        }
    }
    QString objectPath = QString::fromUtf8(
            syntaxName.data(), static_cast<qsizetype>(syntaxName.size()));
    while (objectPath.endsWith(QLatin1Char('.'))) objectPath.chop(1);
    const QString objectPrefix = objectPath + QLatin1Char('.');
    const bool hasChildren = std::any_of(
            symbols.begin(), symbols.end(),
            [&objectPrefix](const ConditionLanguageSymbol &symbol) {
                return FromView(symbol.canonicalName).startsWith(objectPrefix);
            });
    if (hasChildren) return located(objectMap(objectPath));
    return {};
}

void ConditionEditorModel::attachDocument(QQuickTextDocument *document) {
    if (document_ == document) return;
    if (document_ != nullptr) disconnect(document_, nullptr, this, nullptr);
    document_ = document;
    highlighter_->setDocument(document == nullptr
                                      ? nullptr
                                      : document->textDocument());
    if (document_ != nullptr) {
        connect(document_, &QObject::destroyed, this, [this]() {
            document_ = nullptr;
            highlighter_->setDocument(nullptr);
        });
    }
}

void ConditionEditorModel::setHighlightPalette(
        const QColor &symbol,
        const QColor &previousSymbol,
        const QColor &function,
        const QColor &number,
        const QColor &operatorColor,
        const QColor &error) {
    highlighter_->setPalette(symbol,
                             previousSymbol,
                             function,
                             number,
                             operatorColor,
                             error);
}

QVariantMap ConditionEditorModel::symbolMap(std::size_t index) const {
    const ConditionLanguageSymbol &symbol = GetConditionSymbols().at(index);
    const bool available = !symbol.pointTargetOnly ||
            evaluationTargetId_ == QLatin1String(kPointTargetEvaluationId);
    const QString unit = FromView(symbol.unit);
    const QString detail = unit.isEmpty()
            ? FromView(symbol.detail)
            : FromView(symbol.detail) + QStringLiteral(" · ") + unit;
    return {{QStringLiteral("category"), FromView(symbol.category)},
            {QStringLiteral("label"), FromView(symbol.friendlyName)},
            {QStringLiteral("symbol"), FromView(symbol.canonicalName)},
            {QStringLiteral("kind"), KindLabel(symbol)},
            {QStringLiteral("type"), KindLabel(symbol)},
            {QStringLiteral("unit"), unit},
            {QStringLiteral("detail"), detail},
            {QStringLiteral("description"), FromView(symbol.documentation)},
            {QStringLiteral("aliases"),
             AliasList(symbol.aliases, symbol.canonicalName)},
            {QStringLiteral("example"), FromView(symbol.example)},
            {QStringLiteral("insertText"),
             FromView(symbol.insertionTemplate)},
            {QStringLiteral("available"), available},
            {QStringLiteral("unavailableReason"),
             available
                     ? QString{}
                     : tr("Requires Evaluation → Target → Point target.")}};
}

QVariantMap ConditionEditorModel::functionMap(std::size_t index) const {
    const ConditionLanguageFunction &function =
            GetConditionFunctions().at(index);
    const bool available = !function.pointTargetOnly ||
            evaluationTargetId_ == QLatin1String(kPointTargetEvaluationId);
    return {{QStringLiteral("category"), FromView(function.category)},
            {QStringLiteral("label"), FromView(function.friendlyName)},
            {QStringLiteral("symbol"), FromView(function.canonicalName)},
            {QStringLiteral("kind"), QStringLiteral("function")},
            {QStringLiteral("type"), UserFacingType(function.returnType)},
            {QStringLiteral("unit"), QString{}},
            {QStringLiteral("detail"), UserFacingType(function.signature)},
            {QStringLiteral("signature"),
             UserFacingType(function.signature)},
            {QStringLiteral("description"), FromView(function.documentation)},
            {QStringLiteral("aliases"),
             AliasList(function.aliases, function.canonicalName)},
            {QStringLiteral("example"), FromView(function.example)},
            {QStringLiteral("insertText"),
             FromView(function.insertionTemplate)},
            {QStringLiteral("available"), available},
            {QStringLiteral("unavailableReason"),
             available
                     ? QString{}
                     : tr("Requires Evaluation → Target → Point target.")}};
}

QVariantMap ConditionEditorModel::objectMap(const QString &path) const {
    const qsizetype separator = path.lastIndexOf(QLatin1Char('.'));
    const QString label = separator < 0 ? path : path.mid(separator + 1);
    return {{QStringLiteral("category"), QStringLiteral("object")},
            {QStringLiteral("label"), label},
            {QStringLiteral("symbol"), path},
            {QStringLiteral("kind"), QStringLiteral("object")},
            {QStringLiteral("type"), QStringLiteral("namespace")},
            {QStringLiteral("unit"), QString{}},
            {QStringLiteral("detail"), tr("object · direct members")},
            {QStringLiteral("description"), ObjectDescription(path)},
            {QStringLiteral("aliases"), QStringList{}},
            {QStringLiteral("example"), QString{}},
            {QStringLiteral("available"), true},
            {QStringLiteral("unavailableReason"), QString{}},
            {QStringLiteral("isContainer"), true}};
}

void ConditionEditorModel::analyze() {
    ConditionVariables variables;
    if (evaluationTargetId_ == QLatin1String(kPointTargetEvaluationId)) {
        variables.emplace("bf_target_point",
                          ConditionVariable{0.0, 0.0, 0.0, true});
    }
    const QByteArray utf8 = source_.toUtf8();
    const ConditionCompileResult result = CompileConditionScript(
            std::string(utf8.constData(),
                        static_cast<std::size_t>(utf8.size())),
            variables,
            gateMode_ == QStringLiteral("or") ? ConditionGateMode::Any
                                               : ConditionGateMode::All);

    diagnostics_.clear();
    QVector<ConditionHighlightDiagnostic> highlightDiagnostics;
    for (const ConditionDiagnostic &diagnostic : result.diagnostics) {
        const QtRange range = ToQtRange(source_, diagnostic);
        diagnostics_.push_back(
                QVariantMap{{QStringLiteral("line"), range.line},
                            {QStringLiteral("column"), range.column},
                            {QStringLiteral("start"), range.absoluteStart},
                            {QStringLiteral("length"), range.length},
                            {QStringLiteral("severity"),
                             QStringLiteral("error")},
                            {QStringLiteral("message"),
                             QString::fromStdString(diagnostic.message)}});
        highlightDiagnostics.push_back(
                {range.line - 1, range.column - 1, range.length});
    }
    highlighter_->setDiagnostics(std::move(highlightDiagnostics));

    const QStringList lines = source_.split(QLatin1Char('\n'),
                                            Qt::KeepEmptyParts);
    lineStates_.clear();
    gateCount_ = 0;
    int lineStart = 0;
    for (int index = 0; index < lines.size(); ++index) {
        const bool blank = lines[index].trimmed().isEmpty();
        const bool disabled = !blank && IsDisabledLine(lines[index]);
        if (!blank && !disabled) ++gateCount_;
        QString state = blank ? QStringLiteral("blank")
                              : disabled ? QStringLiteral("disabled")
                                         : QStringLiteral("valid");
        QString message;
        for (const QVariant &entry : diagnostics_) {
            const QVariantMap diagnostic = entry.toMap();
            if (diagnostic.value(QStringLiteral("line")).toInt() !=
                index + 1) {
                continue;
            }
            state = QStringLiteral("error");
            message = diagnostic.value(QStringLiteral("message")).toString();
            break;
        }
        lineStates_.push_back(
                QVariantMap{{QStringLiteral("line"), index + 1},
                            {QStringLiteral("start"), lineStart},
                            {QStringLiteral("state"), state},
                            {QStringLiteral("enabled"),
                             !blank && !disabled},
                            {QStringLiteral("message"), message}});
        lineStart += lines[index].size() + 1;
    }

    valid_ = diagnostics_.isEmpty();
    if (gateCount_ == 0) {
        statusText_ = tr("No gates · every tick is eligible");
    } else if (valid_) {
        const QString mode = gateMode_.toUpper();
        statusText_ = gateCount_ == 1
                ? tr("%1 · 1 gate").arg(mode)
                : tr("%1 · %2 gates").arg(mode).arg(gateCount_);
    } else {
        const QVariantMap first = diagnostics_.first().toMap();
        statusText_ = tr("Line %1, column %2 · %3")
                              .arg(first.value(QStringLiteral("line")).toInt())
                              .arg(first.value(QStringLiteral("column")).toInt())
                              .arg(first.value(QStringLiteral("message")).toString());
    }

    catalogue_.clear();
    const std::vector<ConditionLanguageSymbol> &symbols =
            GetConditionSymbols();
    for (std::size_t index = 0; index < symbols.size(); ++index) {
        catalogue_.push_back(symbolMap(index));
    }
    const std::vector<ConditionLanguageFunction> &functions =
            GetConditionFunctions();
    for (std::size_t index = 0; index < functions.size(); ++index) {
        catalogue_.push_back(functionMap(index));
    }
    updateAssistance();
}

void ConditionEditorModel::updateAssistance() {
    const QByteArray utf8 = source_.toUtf8();
    const std::size_t cursorByte =
            Utf8OffsetForUtf16Position(source_, cursorPosition_);
    const std::optional<ConditionCursorContext> parsedContext =
            AnalyzeConditionCursor(
                    std::string_view(utf8.constData(),
                                     static_cast<std::size_t>(utf8.size())),
                    cursorByte);

    completions_.clear();
    completionContext_.clear();
    parameterHint_.clear();
    if (!parsedContext) return;

    const ConditionCursorContext &context = *parsedContext;
    const QString parentPath = QString::fromUtf8(
            context.receiver.data(),
            static_cast<qsizetype>(context.receiver.size()));
    const QString fragment = QString::fromUtf8(
            context.fragment.data(),
            static_cast<qsizetype>(context.fragment.size()));
    const int replaceStart =
            Utf16PositionForByteOffset(source_, context.replacement.begin);
    const int replaceEnd =
            Utf16PositionForByteOffset(source_, context.replacement.end);
    const bool externalName = context.expected ==
            ConditionLanguageValueType::ExternalName;
    const bool vectorArgument = context.expected ==
            ConditionLanguageValueType::Vector;
    const bool memberSite = context.site == ConditionCursorSite::Member;
    const bool pointTarget = evaluationTargetId_ ==
            QLatin1String(kPointTargetEvaluationId);

    completionContext_ = parentPath.isEmpty() ? QVariantMap{}
                                               : objectMap(parentPath);
    completionContext_.insert(
            QStringLiteral("site"),
            context.site == ConditionCursorSite::Member
                    ? QStringLiteral("member")
            : context.site == ConditionCursorSite::FunctionArgument
                    ? QStringLiteral("argument")
            : context.site == ConditionCursorSite::ExternalName
                    ? QStringLiteral("external-name")
                    : QStringLiteral("expression"));
    completionContext_.insert(QStringLiteral("fragment"), fragment);
    completionContext_.insert(QStringLiteral("automaticTrigger"),
                              context.automaticTrigger);
    completionContext_.insert(
            QStringLiteral("revision"),
            QVariant::fromValue(documentRevision_));

    if (!context.functionName.empty()) {
        const ConditionLanguageFunction *const function =
                FindConditionFunction(context.functionName);
        if (function != nullptr) {
            parameterHint_ =
                    {{QStringLiteral("symbol"),
                      FromView(function->canonicalName)},
                     {QStringLiteral("signature"),
                      UserFacingType(function->signature)},
                     {QStringLiteral("description"),
                      FromView(function->documentation)},
                     {QStringLiteral("activeParameter"),
                      static_cast<int>(context.argumentIndex)}};
        }
    }

    struct Completion final {
        int score = 0;
        int order = 0;
        QVariantMap value;
    };
    std::vector<Completion> candidates;
    int order = 0;
    const auto addCandidate = [&](QVariantMap value,
                                  const QString &completionId,
                                  const QString &label,
                                  const QStringList &searchValues,
                                  const QString &insertText,
                                  int cursorOffset) {
        const int score = memberSite
                ? MemberMatchScore(fragment, searchValues)
                : MatchScore(fragment, searchValues);
        if (score == std::numeric_limits<int>::max()) return;
        value.insert(QStringLiteral("completionId"), completionId);
        value.insert(QStringLiteral("revision"),
                     QVariant::fromValue(documentRevision_));
        value.insert(QStringLiteral("label"), label);
        value.insert(QStringLiteral("insertText"), insertText);
        value.insert(QStringLiteral("replaceStart"), replaceStart);
        value.insert(QStringLiteral("replaceLength"),
                     std::max(0, replaceEnd - replaceStart));
        value.insert(QStringLiteral("cursorOffset"), cursorOffset);
        candidates.push_back({score, order++, std::move(value)});
    };

    const std::vector<ConditionLanguageSymbol> &symbols =
            GetConditionSymbols();
    if (externalName) {
        if (pointTarget) {
            const ConditionLanguageSymbol *const target =
                    FindConditionSymbol("bf_target_point");
            if (target != nullptr) {
                const auto found = std::find_if(
                        symbols.begin(), symbols.end(),
                        [target](const ConditionLanguageSymbol &candidate) {
                            return &candidate == target;
                        });
                if (found != symbols.end()) {
                    QVariantMap value = symbolMap(static_cast<std::size_t>(
                            std::distance(symbols.begin(), found)));
                    const QString name = QStringLiteral("bf_target_point");
                    addCandidate(std::move(value),
                                 QStringLiteral("external:bf_target_point"),
                                 name,
                                 {name, tr("Point target")},
                                 name,
                                 name.size());
                }
            }
        }
    } else {
        QSet<QString> addedObjects;
        const QString parentPrefix = parentPath.isEmpty()
                ? QString{}
                : parentPath + QLatin1Char('.');
        for (std::size_t index = 0; index < symbols.size(); ++index) {
            const ConditionLanguageSymbol &symbol = symbols[index];
            if (symbol.external) continue;
            if (symbol.pointTargetOnly && !pointTarget) continue;
            if (vectorArgument &&
                symbol.kind != ConditionLanguageSymbolKind::Vector) {
                continue;
            }
            if (!vectorArgument &&
                symbol.kind != ConditionLanguageSymbolKind::Scalar) {
                continue;
            }

            const QString canonical = FromView(symbol.canonicalName);
            if (!canonical.startsWith(parentPrefix)) continue;
            const QString remaining = canonical.mid(parentPrefix.size());
            if (remaining.isEmpty()) continue;
            const qsizetype separator =
                    remaining.indexOf(QLatin1Char('.'));
            const QString child = separator < 0
                    ? remaining
                    : remaining.left(separator);
            const QString childPath = parentPrefix + child;
            if (separator >= 0) {
                if (addedObjects.contains(childPath)) continue;
                addedObjects.insert(childPath);
                const QString insertion = child + QLatin1Char('.');
                QStringList names{child};
                if (!memberSite) {
                    names.push_back(childPath);
                    names.push_back(ObjectDescription(childPath));
                }
                addCandidate(objectMap(childPath),
                             QStringLiteral("object:") + childPath,
                             child,
                             names,
                             insertion,
                             insertion.size());
                continue;
            }

            QVariantMap value = symbolMap(index);
            QStringList names{child};
            if (!memberSite) {
                names.push_back(canonical);
                names.push_back(FromView(symbol.friendlyName));
                names.push_back(FromView(symbol.detail));
            }
            for (const std::string_view alias : symbol.aliases) {
                const QString aliasName = FromView(alias);
                if (!memberSite) {
                    names.push_back(aliasName);
                    continue;
                }
                if (!aliasName.startsWith(parentPrefix)) continue;
                const QString localAlias =
                        aliasName.mid(parentPrefix.size());
                if (!localAlias.contains(QLatin1Char('.'))) {
                    names.push_back(localAlias);
                }
            }
            addCandidate(std::move(value),
                         QStringLiteral("symbol:") + canonical,
                         child,
                         names,
                         child,
                         child.size());
        }

        if (parentPath.isEmpty() && vectorArgument && pointTarget) {
            const QString insertion =
                    QStringLiteral("variable(bf_target_point)");
            const ConditionLanguageSymbol *const target =
                    FindConditionSymbol("bf_target_point");
            QVariantMap value;
            if (target != nullptr) {
                const auto found = std::find_if(
                        symbols.begin(), symbols.end(),
                        [target](const ConditionLanguageSymbol &candidate) {
                            return &candidate == target;
                        });
                if (found != symbols.end()) {
                    value = symbolMap(static_cast<std::size_t>(
                            std::distance(symbols.begin(), found)));
                }
            }
            addCandidate(std::move(value),
                         QStringLiteral("external-expression:bf_target_point"),
                         QStringLiteral("bf_target_point"),
                         {insertion,
                          QStringLiteral("bf_target_point"),
                          tr("Point target")},
                         insertion,
                         insertion.size());
        }

        if (parentPath.isEmpty() && !vectorArgument) {
            const std::vector<ConditionLanguageFunction> &functions =
                    GetConditionFunctions();
            for (std::size_t index = 0; index < functions.size(); ++index) {
                const ConditionLanguageFunction &function = functions[index];
                if (function.pointTargetOnly && !pointTarget) continue;
                QStringList names{FromView(function.canonicalName),
                                  FromView(function.friendlyName),
                                  FromView(function.documentation)};
                for (const std::string_view alias : function.aliases) {
                    names.push_back(FromView(alias));
                }
                const QString insertion = FunctionInsertion(function);
                addCandidate(functionMap(index),
                             QStringLiteral("function:") +
                                     FromView(function.canonicalName),
                             FromView(function.canonicalName),
                             names,
                             insertion,
                             FunctionCursorOffset(insertion));
            }
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Completion &left, const Completion &right) {
                         if (left.score != right.score) {
                             return left.score < right.score;
                         }
                         return left.order < right.order;
                     });
    completions_.reserve(static_cast<qsizetype>(candidates.size()));
    for (Completion &candidate : candidates) {
        completions_.push_back(std::move(candidate.value));
    }
}

}  // namespace forevertas::app
