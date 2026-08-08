#include "app/condition_syntax_highlighter.h"

#include "conditions/condition_language_catalog.h"

#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCharFormat>

#include <algorithm>
#include <string>

namespace forevertas::app {
namespace {

QTextCharFormat ForegroundFormat(const QColor &color) {
    QTextCharFormat format;
    if (color.isValid()) format.setForeground(color);
    return format;
}

}  // namespace

ConditionSyntaxHighlighter::ConditionSyntaxHighlighter(QObject *parent)
    : QSyntaxHighlighter(parent) {}

void ConditionSyntaxHighlighter::setPalette(
        const QColor &symbol,
        const QColor &previousSymbol,
        const QColor &function,
        const QColor &number,
        const QColor &operatorColor,
        const QColor &error) {
    if (symbolColor_ == symbol && previousSymbolColor_ == previousSymbol &&
        functionColor_ == function && numberColor_ == number &&
        operatorColor_ == operatorColor && errorColor_ == error) {
        return;
    }
    symbolColor_ = symbol;
    previousSymbolColor_ = previousSymbol;
    functionColor_ = function;
    numberColor_ = number;
    operatorColor_ = operatorColor;
    errorColor_ = error;
    rehighlight();
}

void ConditionSyntaxHighlighter::setDiagnostics(
        QVector<ConditionHighlightDiagnostic> diagnostics) {
    diagnostics_ = std::move(diagnostics);
    rehighlight();
}

void ConditionSyntaxHighlighter::highlightBlock(const QString &text) {
    static const QRegularExpression identifierExpression(
            QStringLiteral("[A-Za-z_][A-Za-z0-9_.]*"));
    static const QRegularExpression numberExpression(
            QStringLiteral(
                    "(?:^|(?<=[(,=<>+\\-*/\\s]))[+-]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][+-]?\\d+)?"));
    static const QRegularExpression operatorExpression(
            QStringLiteral(">=|<=|[=<>+\\-*/(),]"));

    if (text.trimmed().startsWith(QStringLiteral("//"))) {
        setFormat(0, text.size(), ForegroundFormat(previousSymbolColor_));
        return;
    }

    QRegularExpressionMatchIterator identifiers =
            identifierExpression.globalMatch(text);
    while (identifiers.hasNext()) {
        const QRegularExpressionMatch match = identifiers.next();
        const std::string name = match.captured().toUtf8().toStdString();
        if (FindConditionFunction(name) != nullptr) {
            setFormat(match.capturedStart(),
                      match.capturedLength(),
                      ForegroundFormat(functionColor_));
            continue;
        }
        if (FindConditionConstant(name) != nullptr) {
            setFormat(match.capturedStart(),
                      match.capturedLength(),
                      ForegroundFormat(numberColor_));
            continue;
        }
        const ConditionLanguageSymbol *const symbol =
                FindConditionSymbol(name);
        if (symbol == nullptr) continue;
        const bool previous =
                QString::fromUtf8(symbol->canonicalName.data(),
                                  static_cast<qsizetype>(
                                          symbol->canonicalName.size()))
                        .contains(QStringLiteral(".prev"));
        setFormat(match.capturedStart(),
                  match.capturedLength(),
                  ForegroundFormat(previous ? previousSymbolColor_
                                            : symbolColor_));
    }

    QRegularExpressionMatchIterator numbers = numberExpression.globalMatch(text);
    while (numbers.hasNext()) {
        const QRegularExpressionMatch match = numbers.next();
        setFormat(match.capturedStart(),
                  match.capturedLength(),
                  ForegroundFormat(numberColor_));
    }

    QRegularExpressionMatchIterator operators =
            operatorExpression.globalMatch(text);
    while (operators.hasNext()) {
        const QRegularExpressionMatch match = operators.next();
        setFormat(match.capturedStart(),
                  match.capturedLength(),
                  ForegroundFormat(operatorColor_));
    }

    const int block = currentBlock().blockNumber();
    for (const ConditionHighlightDiagnostic &diagnostic : diagnostics_) {
        if (diagnostic.block != block || text.isEmpty()) continue;
        const int textSize = static_cast<int>(text.size());
        const int start = std::clamp(diagnostic.column, 0, textSize - 1);
        const int length = std::clamp(diagnostic.length, 1,
                                      textSize - start);
        for (int offset = 0; offset < length; ++offset) {
            QTextCharFormat merged = format(start + offset);
            merged.setUnderlineStyle(QTextCharFormat::WaveUnderline);
            merged.setUnderlineColor(errorColor_);
            setFormat(start + offset, 1, merged);
        }
    }
}

}  // namespace forevertas::app
