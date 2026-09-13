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
  Q_PROPERTY(QString sessionJson READ sessionJson CONSTANT)
  Q_PROPERTY(QString sessionError READ sessionError CONSTANT)
  Q_PROPERTY(bool editorLoaded READ editorLoaded NOTIFY editorReadyChanged)

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
  QString sessionJson() const { return sessionJson_; }
  QString sessionError() const { return sessionError_; }
  bool editorLoaded() const { return editorReady_; }

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
  Q_INVOKABLE QString openProgramFile();
  Q_INVOKABLE QString saveProgramFile(const QString &workspaceJson,
                                      const QString &path,
                                      const QString &programName);
  Q_INVOKABLE QString storeSession(const QString &json);
  Q_INVOKABLE void requestSessionFlush() { emit sessionFlushRequested(); }
  Q_INVOKABLE void finishSessionFlush(const QString &error) { emit sessionFlushFinished(error); }
  Q_INVOKABLE QString selectedViewerTargetJson(const QString &kind) const;
  Q_INVOKABLE void requestViewerPointPick(const QString &blockId);
  Q_INVOKABLE void cancelViewerPointPick();
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
  void sessionFlushRequested();
  void sessionFlushFinished(const QString &error);
  void viewerPointPickRequested(const QString &blockId);
  void viewerPointPickCanceled();
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
  QString sessionJson_;
  QString sessionError_;
};

} // namespace forevertas::app

#endif
