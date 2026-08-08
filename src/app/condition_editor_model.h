#ifndef FOREVERTAS_APP_CONDITION_EDITOR_MODEL_H
#define FOREVERTAS_APP_CONDITION_EDITOR_MODEL_H

#include "app/condition_syntax_highlighter.h"

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class QQuickTextDocument;

namespace forevertas::app {

class ConditionEditorModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY analysisChanged)
    Q_PROPERTY(int cursorPosition READ cursorPosition WRITE setCursorPosition
                       NOTIFY assistanceChanged)
    Q_PROPERTY(QString evaluationTargetId READ evaluationTargetId WRITE
                       setEvaluationTargetId NOTIFY analysisChanged)
    Q_PROPERTY(QString gateMode READ gateMode WRITE setGateMode NOTIFY
                       analysisChanged)
    Q_PROPERTY(bool valid READ valid NOTIFY analysisChanged)
    Q_PROPERTY(int gateCount READ gateCount NOTIFY analysisChanged)
    Q_PROPERTY(QVariantList diagnostics READ diagnostics NOTIFY analysisChanged)
    Q_PROPERTY(QVariantList lineStates READ lineStates NOTIFY analysisChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY analysisChanged)
    Q_PROPERTY(QVariantList completions READ completions NOTIFY
                       assistanceChanged)
    Q_PROPERTY(QVariantMap completionContext READ completionContext NOTIFY
                       assistanceChanged)
    Q_PROPERTY(QVariantMap parameterHint READ parameterHint NOTIFY
                       assistanceChanged)
    Q_PROPERTY(QVariantList catalogue READ catalogue NOTIFY analysisChanged)
    Q_PROPERTY(qulonglong documentRevision READ documentRevision NOTIFY
                       assistanceChanged)

public:
    explicit ConditionEditorModel(QObject *parent = nullptr);

    QString source() const;
    int cursorPosition() const noexcept;
    QString evaluationTargetId() const;
    QString gateMode() const;
    bool valid() const noexcept;
    int gateCount() const noexcept;
    QVariantList diagnostics() const;
    QVariantList lineStates() const;
    QString statusText() const;
    QVariantList completions() const;
    QVariantMap completionContext() const;
    QVariantMap parameterHint() const;
    QVariantList catalogue() const;
    qulonglong documentRevision() const noexcept;

    void setSource(const QString &value);
    void setCursorPosition(int value);
    void setEvaluationTargetId(const QString &value);
    void setGateMode(const QString &value);

    Q_INVOKABLE QVariantMap completionEdit(int index) const;
    Q_INVOKABLE QVariantMap functionCallEdit(int cursorPosition) const;
    Q_INVOKABLE void updateDocumentState(const QString &source,
                                         int cursorPosition);
    Q_INVOKABLE QVariantMap acceptCompletion(const QString &completionId,
                                             qulonglong revision);
    Q_INVOKABLE QVariantMap toggleLine(int lineNumber,
                                      int cursorPosition) const;
    Q_INVOKABLE QVariantList searchCatalogue(const QString &query) const;
    Q_INVOKABLE QVariantMap documentationAt(int position) const;
    Q_INVOKABLE void attachDocument(QQuickTextDocument *document);
    Q_INVOKABLE void setHighlightPalette(
            const QColor &symbol,
            const QColor &previousSymbol,
            const QColor &function,
            const QColor &number,
            const QColor &operatorColor,
            const QColor &error);

signals:
    void analysisChanged();
    void assistanceChanged();

private:
    void analyze();
    void updateAssistance();
    QVariantMap symbolMap(std::size_t index) const;
    QVariantMap constantMap(std::size_t index) const;
    QVariantMap functionMap(std::size_t index) const;
    QVariantMap objectMap(const QString &path) const;
    QVariantMap enumMap(const QString &path) const;

    QString source_;
    int cursorPosition_ = 0;
    QString evaluationTargetId_;
    QString gateMode_ = QStringLiteral("and");
    bool valid_ = true;
    int gateCount_ = 0;
    QVariantList diagnostics_;
    QVariantList lineStates_;
    QString statusText_;
    QVariantList completions_;
    QVariantMap completionContext_;
    QVariantMap parameterHint_;
    QVariantList catalogue_;
    qulonglong documentRevision_ = 0u;
    QPointer<QQuickTextDocument> document_;
    ConditionSyntaxHighlighter *highlighter_ = nullptr;
};

}  // namespace forevertas::app

#endif
