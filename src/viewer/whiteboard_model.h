#ifndef FOREVERTAS_VIEWER_WHITEBOARD_MODEL_H
#define FOREVERTAS_VIEWER_WHITEBOARD_MODEL_H

#include <QByteArray>
#include <QColor>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QVector3D>

#include <map>
#include <vector>

namespace forevertas::viewer {

class WhiteboardModel final : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    Q_PROPERTY(int count READ count NOTIFY itemsChanged)
    Q_PROPERTY(int maximumCount READ maximumCount CONSTANT)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectedItem READ selectedItem NOTIFY
                       selectionChanged)
    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(double size READ size WRITE setSize NOTIFY sizeChanged)
    Q_PROPERTY(bool drawing READ drawing NOTIFY drawingChanged)
    Q_PROPERTY(QVariantList boards READ boards NOTIFY boardsChanged)
    Q_PROPERTY(QVariantList visibleBoards READ visibleBoards NOTIFY
                       boardsChanged)
    Q_PROPERTY(int boardCount READ boardCount NOTIFY boardsChanged)
    Q_PROPERTY(int maximumBoardCount READ maximumBoardCount CONSTANT)
    Q_PROPERTY(int selectedBoardIndex READ selectedBoardIndex NOTIFY
                       boardSelectionChanged)
    Q_PROPERTY(QString mapKey READ mapKey NOTIFY mapKeyChanged)
    Q_PROPERTY(QString mapName READ mapName NOTIFY mapNameChanged)
    Q_PROPERTY(QString operationMessage READ operationMessage NOTIFY
                       operationMessageChanged)

public:
    explicit WhiteboardModel(QObject *parent = nullptr);

    bool active() const;
    QVariantList items() const;
    int count() const;
    int maximumCount() const;
    int selectedIndex() const;
    QVariantMap selectedItem() const;
    QString tool() const;
    QColor color() const;
    double size() const;
    bool drawing() const;
    QVariantList boards() const;
    QVariantList visibleBoards() const;
    int boardCount() const;
    int maximumBoardCount() const;
    int selectedBoardIndex() const;
    QString mapKey() const;
    QString mapName() const;
    QString operationMessage() const;

    void setActive(bool value);
    void setTool(const QString &value);
    void setColor(const QColor &value);
    void setSize(double value);
    void setMapIdentity(const QString &key, const QString &name);

    Q_INVOKABLE bool beginItem(double x, double y);
    Q_INVOKABLE bool updateItem(double x, double y);
    Q_INVOKABLE bool finishItem();
    Q_INVOKABLE void cancelItem();
    Q_INVOKABLE int addText(double x,
                            double y,
                            const QString &text);
    Q_INVOKABLE bool setText(int index, const QString &text);
    Q_INVOKABLE int itemAt(double x, double y) const;
    Q_INVOKABLE bool selectItem(int index);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE bool moveSelected(double deltaX, double deltaY);
    Q_INVOKABLE bool resizeSelected(double deltaWidth,
                                    double deltaHeight);
    Q_INVOKABLE bool eraseSelected(double x,
                                   double y,
                                   double radius);
    Q_INVOKABLE bool removeSelected();
    Q_INVOKABLE int captureCurrentBoard(
            const QString &name,
            const QVariantMap &capture);
    Q_INVOKABLE bool pickUpBoard(int index);
    Q_INVOKABLE bool selectBoard(int index);
    Q_INVOKABLE bool setBoardVisible(int index, bool visible);
    Q_INVOKABLE bool removeBoard(int index);
    Q_INVOKABLE bool exportBoardSet(const QUrl &fileUrl);
    Q_INVOKABLE bool importBoardSet(const QUrl &fileUrl);
    Q_INVOKABLE bool exportBoardContentImage(
            int index,
            const QUrl &fileUrl);
    Q_INVOKABLE QString imageExportPath(const QUrl &fileUrl) const;
    Q_INVOKABLE bool saveBoardBackgroundImage(
            const QVariant &imageValue,
            const QUrl &fileUrl);
    Q_INVOKABLE void finishBoardImageExport(
            bool success,
            bool fullBackground);
    Q_INVOKABLE quint64 captureHistorySnapshot();
    Q_INVOKABLE bool restoreHistorySnapshot(quint64 token);
    Q_INVOKABLE void pruneHistorySnapshots(const QVariantList &tokens);

signals:
    void activeChanged();
    void itemsChanged();
    void selectionChanged();
    void toolChanged();
    void colorChanged();
    void sizeChanged();
    void drawingChanged();
    void boardsChanged();
    void boardSelectionChanged();
    void mapKeyChanged();
    void mapNameChanged();
    void operationMessageChanged();

private:
    struct Erasure {
        QPointF point;
        double radius = 0.01;
    };

    struct Item {
        QString id;
        QString type;
        QRectF bounds;
        QColor color;
        double strokeWidth = 4.0;
        double fontSize = 0.05;
        QString text;
        std::vector<QPointF> points;
        std::vector<Erasure> erasures;
    };

    struct Board {
        QString id;
        QString name;
        QString mapKey;
        QString mapName;
        bool visible = true;
        int projectionVersion = 0;
        QString projection = QStringLiteral("perspective-vertical");
        double fieldOfView = 55.0;
        double planeDistance = 1.0;
        double viewportWidth = 1024.0;
        double viewportHeight = 576.0;
        double contentX = 0.0;
        double contentY = 0.0;
        double contentWidth = 1.0;
        double contentHeight = 1.0;
        double canvasWidth = 1024.0;
        double canvasHeight = 576.0;
        QVector3D target;
        double yaw = 0.0;
        double pitch = 0.0;
        double distance = 38.0;
        QVector3D planePosition;
        double planeWidth = 10.0;
        double planeHeight = 6.0;
        std::vector<Item> items;
    };

    struct HistorySnapshot {
        std::vector<Item> items;
        std::vector<Board> boards;
        int selectedIndex = -1;
        int selectedBoardIndex = -1;
        bool active = false;
        QString tool;
        QColor color;
        double size = 4.0;
    };

    static bool IsFinite(double value);
    static bool IsPointFinite(const QPointF &point);
    static double ClampUnit(double value);
    static QString NormalizeText(const QString &text);
    static QVariantMap ToVariantMap(const Item &item, bool selected);
    QVariantMap boardToVariantMap(const Board &board, int index) const;
    static void TranslateItem(Item *item, const QPointF &delta);
    static QString LocalPath(const QUrl &fileUrl);
    static QString NormalizeBoardName(const QString &name);
    static QString NormalizeMapName(const QString &name);
    static QByteArray serializeBoards(
            const std::vector<Board> &boards,
            const QString &setName);
    static bool deserializeBoards(
            const QByteArray &data,
            std::vector<Board> *boards,
            QString *error);
    void updateDraftBounds(Item *item);
    void removeDraft();
    void notifyItemsChanged(bool selectionMayHaveChanged = false);
    void loadPersistedBoards();
    bool persistBoards();
    void setOperationMessage(const QString &value);

    std::vector<Item> items_;
    bool active_ = false;
    int selectedIndex_ = -1;
    QString tool_ = QStringLiteral("select");
    QColor color_ = QColor(QStringLiteral("#f8faf9"));
    double size_ = 4.0;
    bool drawing_ = false;
    int draftIndex_ = -1;
    QPointF draftOrigin_;
    std::vector<Board> boards_;
    int selectedBoardIndex_ = -1;
    QString mapKey_;
    QString mapName_;
    QString operationMessage_;
    std::map<quint64, HistorySnapshot> historySnapshots_;
    quint64 nextHistoryToken_ = 1;
    quint64 historyRevision_ = 0;
    quint64 capturedRevision_ = static_cast<quint64>(-1);
    quint64 capturedToken_ = 0;
    quint64 currentHistoryToken_ = 0;
};

}  // namespace forevertas::viewer

#endif
