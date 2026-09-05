#ifndef FOREVERTAS_APP_BLOCK_EDITOR_BRIDGE_H
#define FOREVERTAS_APP_BLOCK_EDITOR_BRIDGE_H

#include "blocks/visual_program.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>

namespace forevertas::app {

class SearchController;

// Trust boundary between the embedded editor and the visual interpreter.
// JavaScript submits source graphs, not native search configurations.
class BlockEditorBridge final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString catalogJson READ catalogJson CONSTANT)
  Q_PROPERTY(
      QString workspaceJson READ workspaceJson NOTIFY workspaceJsonChanged)
  Q_PROPERTY(qulonglong workspaceRevision READ workspaceRevision NOTIFY
                 workspaceRevisionChanged)
  Q_PROPERTY(QString diagnosticsJson READ diagnosticsJson NOTIFY
                 diagnosticsJsonChanged)
  Q_PROPERTY(bool editable READ editable NOTIFY editableChanged)
  Q_PROPERTY(bool darkMode READ darkMode NOTIFY darkModeChanged)
  Q_PROPERTY(bool running READ running NOTIFY editableChanged)
  Q_PROPERTY(QString debugJson READ debugJson NOTIFY debugChanged)

public:
  explicit BlockEditorBridge(SearchController *controller,
                             QObject *parent = nullptr);

  QString catalogJson() const { return catalogJson_; }
  QString workspaceJson() const { return workspaceJson_; }
  qulonglong workspaceRevision() const { return lastAcceptedRevision_; }
  QString diagnosticsJson() const { return diagnosticsJson_; }
  bool editable() const;
  bool darkMode() const;
  bool running() const;
  QString debugJson() const { return debugJson_; }

  Q_INVOKABLE bool applyWorkspace(const QString &workspaceJson,
                                  qulonglong revision);
  Q_INVOKABLE void editorReady();
  Q_INVOKABLE void requestNativeWorkspace();
  Q_INVOKABLE bool runWorkspace(const QString &workspaceJson, qulonglong revision, bool debug);
  Q_INVOKABLE void pauseProgram();
  Q_INVOKABLE void resumeProgram(const QString &step);
  Q_INVOKABLE void stopProgram();
  Q_INVOKABLE void inspectProgram(bool enabled);
  Q_INVOKABLE void setBreakpoints(const QStringList &blockIds);
  Q_INVOKABLE QString openProject();
  Q_INVOKABLE bool saveProject(const QString &workspaceJson);
  QString projectFromWorkspace(const QString &workspaceJson);
  QString workspaceFromProject(const QString &projectJson);
  Q_INVOKABLE QString selectedViewerTargetJson(const QString &kind) const;
  Q_INVOKABLE void requestViewerPointPick(const QString &blockId);
  Q_INVOKABLE void completeViewerPointPick(const QString &blockId,
                                           double x,
                                           double y,
                                           double z);

signals:
  void workspaceJsonChanged();
  void workspaceRevisionChanged();
  void diagnosticsJsonChanged();
  void editableChanged();
  void darkModeChanged();
  void editorReadyChanged();
  void debugChanged();
  void viewerPointPickRequested(const QString &blockId);
  void viewerPointPicked(const QString &blockId, double x, double y, double z);

private:
  struct ParsedWorkspace {
    blocks::VisualProgram program;
    QString error;
    std::map<blocks::VisualNodeId, QString> editorIds;
  };

  static QString BuildCatalogJson();
  static ParsedWorkspace ParseBlocklyWorkspace(const QString &json);
  void publishDiagnostics(const QStringList &messages, bool isError = true);

  SearchController *controller_ = nullptr;
  QString catalogJson_;
  QString workspaceJson_;
  QString diagnosticsJson_ = QStringLiteral("[]");
  std::uint64_t lastAcceptedRevision_ = 0;
  bool editorReady_ = false;
  QString pendingViewerPointBlockId_;
  QString debugJson_ = QStringLiteral("{}");
  std::map<blocks::VisualNodeId, QString> editorIds_;
  QStringList breakpointIds_;
};

} // namespace forevertas::app

#endif
