#ifndef FOREVERTAS_APP_CONDITION_SYNTAX_HIGHLIGHTER_H
#define FOREVERTAS_APP_CONDITION_SYNTAX_HIGHLIGHTER_H

#include <QColor>
#include <QSyntaxHighlighter>
#include <QVector>

namespace forevertas::app {

struct ConditionHighlightDiagnostic final {
    int block = 0;
    int column = 0;
    int length = 1;
};

class ConditionSyntaxHighlighter final : public QSyntaxHighlighter {
public:
    explicit ConditionSyntaxHighlighter(QObject *parent = nullptr);

    void setPalette(const QColor &symbol,
                    const QColor &previousSymbol,
                    const QColor &function,
                    const QColor &number,
                    const QColor &operatorColor,
                    const QColor &error);
    void setDiagnostics(QVector<ConditionHighlightDiagnostic> diagnostics);

protected:
    void highlightBlock(const QString &text) override;

private:
    QColor symbolColor_;
    QColor previousSymbolColor_;
    QColor functionColor_;
    QColor numberColor_;
    QColor operatorColor_;
    QColor errorColor_;
    QVector<ConditionHighlightDiagnostic> diagnostics_;
};

}  // namespace forevertas::app

#endif
