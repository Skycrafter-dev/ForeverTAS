#ifndef FOREVERTAS_APP_BLOCK_EDITOR_BRIDGE_H
#define FOREVERTAS_APP_BLOCK_EDITOR_BRIDGE_H

#include "blocks/visual_program.h"

#include <QObject>
#include <QString>

#include <cstdint>

namespace forevertas::app {

class SearchController;

// Narrow trust boundary between the embedded Blockly editor and the native
// search engine. JavaScript can only submit serialized workspace structure;
// native C++ parses, type-checks and compiles it before changing any runtime
// configuration.
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

public:
  explicit BlockEditorBridge(SearchController *controller,
                             QObject *parent = nullptr);

  QString catalogJson() const { return catalogJson_; }
  QString workspaceJson() const { return workspaceJson_; }
  qulonglong workspaceRevision() const { return lastAcceptedRevision_; }
  QString diagnosticsJson() const { return diagnosticsJson_; }
  bool editable() const;
  bool darkMode() const;

  Q_INVOKABLE bool applyWorkspace(const QString &workspaceJson,
                                  qulonglong revision);
  Q_INVOKABLE void editorReady();
  Q_INVOKABLE void requestNativeWorkspace();
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
  void viewerPointPickRequested(const QString &blockId);
  void viewerPointPicked(const QString &blockId, double x, double y, double z);

private:
  struct ParsedWorkspace {
    blocks::VisualProgram program;
    QString error;
  };

  static QString BuildCatalogJson();
  static ParsedWorkspace ParseBlocklyWorkspace(const QString &json);
  QString BuildWorkspaceFromController(QString *diagnostic = nullptr) const;
  void publishDiagnostics(const QStringList &messages, bool isError = true);
  void synchronizeFromController();

  SearchController *controller_ = nullptr;
  QString catalogJson_;
  QString workspaceJson_;
  QString diagnosticsJson_ = QStringLiteral("[]");
  std::uint64_t lastAcceptedRevision_ = 0;
  bool applyingWorkspace_ = false;
  bool editorReady_ = false;
  QString pendingViewerPointBlockId_;
};

} // namespace forevertas::app

#endif
