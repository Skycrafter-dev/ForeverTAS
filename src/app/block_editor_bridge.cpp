#include "app/block_editor_bridge.h"
#include "blocks/visual_debugger.h"
#include "app/system_file_dialog.h"

#include <QFile>
#include <QDir>
#include <QSaveFile>

#include "app/search_controller.h"
#include "app/visual_program_json.h"
#include "blocks/visual_catalog.h"
#include "blocks/visual_macros.h"
#include "blocks/visual_compiler.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSettings>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace forevertas::app {
namespace {

constexpr auto kStoredWorkspaceKey = "blockEditor/v3Workspace";
constexpr auto kStoredProgramKey = "blockEditor/v3Program";
constexpr auto kDraftWorkspaceKey = "blockEditor/v3DraftWorkspace";
constexpr auto kCorruptProgramBackupKey = "blockEditor/v3ProgramCorruptBackup";
constexpr auto kRetiredWorkspaceBackupKey = "blockEditor/v3WorkspaceRetiredBackup";

QString ToQString(const std::string &value) {
  return QString::fromStdString(value);
}

QString JsonText(const QJsonObject &object) {
  return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

void CopyTargetValue(QJsonObject *object, const QVariantMap &target,
                     const char *key) {
  const QString name = QString::fromLatin1(key);
  const auto value = target.constFind(name);
  if (value != target.constEnd()) object->insert(name, value->toString());
}

QJsonArray ChecksForOutput(blocks::VisualValueType type) {
  QJsonArray checks;
  using T = blocks::VisualValueType;
  const auto add = [&checks](const char *value) {
    checks.push_back(QString::fromLatin1(value));
  };
  switch (type) {
  case T::Any:
    return {};
  case T::Text:
    add("text");
    break;
  case T::List:
    add("list");
    break;
  case T::State:
    add("state");
    break;
  case T::Snapshot:
    add("snapshot");
    break;
  case T::Inputs:
    add("inputs");
    break;
  case T::Procedure:
    add("procedure");
    break;
  case T::None:
    break;
  case T::Scalar:
    add("scalar");
    add("number");
    add("integer");
    add("milliseconds");
    add("meters");
    add("meters_per_second");
    add("degrees");
    add("percent");
    break;
  case T::Number:
    add("number");
    add("scalar");
    add("integer");
    add("milliseconds");
    add("meters");
    add("meters_per_second");
    add("degrees");
    add("percent");
    break;
  case T::Integer:
    add("integer");
    add("number");
    add("scalar");
    add("milliseconds");
    add("meters");
    add("meters_per_second");
    add("degrees");
    add("percent");
    break;
  case T::NumberRange:
    add("number_range");
    break;
  case T::IntegerRange:
    add("integer_range");
    break;
  case T::Milliseconds:
    add("milliseconds");
    add("scalar");
    break;
  case T::Meters:
    add("meters");
    add("scalar");
    break;
  case T::MetersPerSecond:
    add("meters_per_second");
    add("scalar");
    break;
  case T::Degrees:
    add("degrees");
    add("scalar");
    break;
  case T::Percent:
    add("percent");
    add("scalar");
    break;
  case T::Boolean:
    add("boolean");
    break;
  case T::Vector3:
    add("vector3");
    add("position3");
    add("direction3");
    break;
  case T::Position3:
    add("position3");
    add("vector3");
    break;
  case T::Direction3:
    add("direction3");
    add("vector3");
    break;
  case T::Rotation3:
    add("rotation3");
    break;
  case T::Volume:
    add("volume");
    break;
  case T::Polygon2:
    add("polygon2");
    break;
  case T::TimeRange:
    add("time_range");
    break;
  }
  return checks;
}

QJsonArray ChecksForInput(blocks::VisualValueType type) {
  using T = blocks::VisualValueType;
  switch (type) {
  case T::Any:
    return {};
  case T::Text:
    return {QStringLiteral("text")};
  case T::List:
    return {QStringLiteral("list")};
  case T::State:
    return {QStringLiteral("state")};
  case T::Snapshot:
    return {QStringLiteral("snapshot")};
  case T::Inputs:
    return {QStringLiteral("inputs")};
  case T::Procedure:
    return {QStringLiteral("procedure")};
  case T::None:
    return {};
  case T::Scalar:
    return {QStringLiteral("scalar")};
  case T::Number:
    return {QStringLiteral("number"), QStringLiteral("integer")};
  case T::Integer:
    return {QStringLiteral("integer")};
  case T::NumberRange:
    return {QStringLiteral("number_range")};
  case T::IntegerRange:
    return {QStringLiteral("integer_range")};
  case T::Milliseconds:
    return {QStringLiteral("milliseconds")};
  case T::Meters:
    return {QStringLiteral("meters")};
  case T::MetersPerSecond:
    return {QStringLiteral("meters_per_second")};
  case T::Degrees:
    return {QStringLiteral("degrees")};
  case T::Percent:
    return {QStringLiteral("percent")};
  case T::Boolean:
    return {QStringLiteral("boolean")};
  case T::Vector3:
    return {QStringLiteral("vector3")};
  case T::Position3:
    return {QStringLiteral("position3")};
  case T::Direction3:
    return {QStringLiteral("direction3")};
  case T::Rotation3:
    return {QStringLiteral("rotation3")};
  case T::Volume:
    return {QStringLiteral("volume")};
  case T::Polygon2:
    return {QStringLiteral("polygon2")};
  case T::TimeRange:
    return {QStringLiteral("time_range")};
  }
  return {};
}

QString ShapeName(blocks::VisualBlockShape shape) {
  using S = blocks::VisualBlockShape;
  switch (shape) {
  case S::Hat:
    return QStringLiteral("hat");
  case S::Command:
    return QStringLiteral("command");
  case S::Control:
    return QStringLiteral("control");
  case S::Reporter:
    return QStringLiteral("reporter");
  case S::Predicate:
    return QStringLiteral("predicate");
  }
  return {};
}

QString FieldKindName(blocks::VisualFieldDefinition::Kind kind) {
  using K = blocks::VisualFieldDefinition::Kind;
  switch (kind) {
  case K::Number:
    return QStringLiteral("number");
  case K::Integer:
    return QStringLiteral("integer");
  case K::Boolean:
    return QStringLiteral("boolean");
  case K::Enum:
    return QStringLiteral("enum");
  case K::Text:
    return QStringLiteral("text");
  }
  return {};
}

QJsonArray StatementChecks(const std::string &family) {
  return family.empty() ? QJsonArray{} : QJsonArray{ToQString(family)};
}

QJsonObject BlocklyBlock(const std::string &definitionId,
                         const QJsonObject &fields = {},
                         const QJsonObject &inputs = {}, double x = 0.0,
                         double y = 0.0) {
  QJsonObject block{{QStringLiteral("type"),
                     ToQString(blocks::VisualBlocklyType(definitionId))}};
  if (!fields.isEmpty())
    block.insert(QStringLiteral("fields"), fields);
  if (!inputs.isEmpty())
    block.insert(QStringLiteral("inputs"), inputs);
  if (x != 0.0 || y != 0.0) {
    block.insert(QStringLiteral("x"), x);
    block.insert(QStringLiteral("y"), y);
  }
  return block;
}

QJsonObject BlockInput(QJsonObject block, bool shadow = false) {
  return {{shadow ? QStringLiteral("shadow") : QStringLiteral("block"),
           std::move(block)}};
}

QJsonObject Chain(std::vector<QJsonObject> blocks) {
  if (blocks.empty())
    return {};
  for (std::size_t index = blocks.size(); index > 1; --index) {
    QJsonObject next;
    next.insert(QStringLiteral("block"), blocks[index - 1]);
    blocks[index - 2].insert(QStringLiteral("next"), next);
  }
  return blocks.front();
}

QJsonObject BlocklyNodeForProgram(const blocks::VisualProgram &program,
                                  blocks::VisualNodeId id, bool topLevel) {
  const blocks::VisualNode *const node = program.find(id);
  if (node == nullptr)
    return {};
  const blocks::VisualBlockDefinition *const definition =
      blocks::FindVisualBlock(node->definitionId);
  if (definition == nullptr)
    return {};

  QJsonObject fields;
  for (const auto &entry : node->fields) {
    const std::string &key = entry.first;
    const std::string &storedValue = entry.second;
    QString value = ToQString(storedValue);
    const auto field =
        std::find_if(definition->fields.begin(), definition->fields.end(),
                     [&key](const blocks::VisualFieldDefinition &candidate) {
                       return candidate.key == key;
                     });
    if (field != definition->fields.end() &&
        field->kind == blocks::VisualFieldDefinition::Kind::Boolean) {
      value = value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
                  ? QStringLiteral("TRUE")
                  : QStringLiteral("FALSE");
    }
    fields.insert(ToQString(key), value);
  }

  QJsonObject inputs;
  for (const auto &[key, childId] : node->inputs) {
    const QJsonObject child = BlocklyNodeForProgram(program, childId, false);
    if (!child.isEmpty())
      inputs.insert(ToQString(key), BlockInput(child));
  }
  for (const auto &[key, children] : node->statements) {
    std::vector<QJsonObject> sequence;
    sequence.reserve(children.size());
    for (const blocks::VisualNodeId childId : children) {
      const QJsonObject child = BlocklyNodeForProgram(program, childId, false);
      if (!child.isEmpty())
        sequence.push_back(child);
    }
    const QJsonObject body = Chain(std::move(sequence));
    if (!body.isEmpty())
      inputs.insert(ToQString(key), BlockInput(body));
  }

  QJsonObject block = BlocklyBlock(node->definitionId, fields, inputs,
                                   topLevel ? node->x : 0.0,
                                   topLevel ? node->y : 0.0);
  block.insert(QStringLiteral("id"),
               QString::number(static_cast<qulonglong>(node->id)));
  if (!node->enabled) block.insert(QStringLiteral("enabled"), false);
  if (node->definitionId == "procedures/call" || node->definitionId == "procedures/value" || node->definitionId == "procedures/reference") {
    const auto parameters = node->fields.find("parameters");
    const auto name = node->fields.find("name");
    block.insert(QStringLiteral("extraState"), QJsonObject{
        {QStringLiteral("parameters"), parameters == node->fields.end() ? QString() : ToQString(parameters->second)},
        {QStringLiteral("name"), name == node->fields.end() ? QStringLiteral("my block") : ToQString(name->second)}});
  }
  return block;
}

QString WorkspaceForProgram(const blocks::VisualProgram &program) {
  QJsonArray top;
  for (const blocks::VisualNodeId id : program.topLevel) {
    const QJsonObject block = BlocklyNodeForProgram(program, id, true);
    if (!block.isEmpty())
      top.push_back(block);
  }
  QJsonObject blocks{{QStringLiteral("languageVersion"), 0},
                     {QStringLiteral("blocks"), top}};
  return QString::fromUtf8(
      QJsonDocument(QJsonObject{{QStringLiteral("blocks"), blocks}})
          .toJson(QJsonDocument::Compact));
}

QString JsonScalarToString(const QJsonValue &value) {
  if (value.isString())
    return value.toString();
  if (value.isBool())
    return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
  if (value.isDouble())
    return QString::number(value.toDouble(), 'g', 17);
  return {};
}

} // namespace

BlockEditorBridge::BlockEditorBridge(SearchController *controller,
                                     QObject *parent)
    : QObject(parent), controller_(controller),
      catalogJson_(BuildCatalogJson()) {
  Q_ASSERT(controller_ != nullptr);
  connect(controller_, &SearchController::runningChanged, this,
          &BlockEditorBridge::editableChanged);
  connect(controller_, &SearchController::blockDebugChanged, this, [this](const QString &text) {
    auto json = QJsonDocument::fromJson(text.toUtf8()).object();
    const auto id = json.value(QStringLiteral("block")).toString().toULongLong();
    const auto found = editorIds_.find(id);
    json.insert(QStringLiteral("block"), found == editorIds_.end() ? QString() : found->second);
    debugJson_ = JsonText(json);
    emit debugChanged();
  });
  connect(controller_, &SearchController::darkModeChanged, this,
          &BlockEditorBridge::darkModeChanged);
  QSettings settings;
  const QString saved = settings.value(QString::fromLatin1(kStoredProgramKey)).toString();
  const auto parsed = ParseVisualProgramJson(saved);
  QString workspace;
  bool recovered = false;
  if (!saved.isEmpty() && !parsed.program) {
    settings.setValue(QString::fromLatin1(kCorruptProgramBackupKey), saved);
    recovered = true;
  }
  const QString cached = settings.value(QString::fromLatin1(kStoredWorkspaceKey)).toString();
  if (parsed.program) {
    const auto cache = ParseBlocklyWorkspace(cached);
    workspace = cache.error.isEmpty() && PrintVisualProgramJson(cache.program) == saved
        ? cached : WorkspaceForProgram(*parsed.program);
  }
  const QString draft = settings.value(QString::fromLatin1(kDraftWorkspaceKey)).toString();
  if (!draft.isEmpty() && ParseBlocklyWorkspace(draft).error.isEmpty()) workspace = draft;
  else if (!draft.isEmpty()) {
    settings.setValue(QString::fromLatin1(kRetiredWorkspaceBackupKey), draft);
    recovered = true;
  } else if (recovered && !cached.isEmpty()) {
    settings.setValue(QString::fromLatin1(kRetiredWorkspaceBackupKey), cached);
  }
  if (workspace.isEmpty()) {
    workspace = QStringLiteral(R"({"blocks":{"languageVersion":0,"blocks":[{"type":"ft_flow_when_start","id":"start"}]}})");
  }
  const bool applied = applyWorkspace(workspace, lastAcceptedRevision_ + 1);
  if (recovered && applied) publishDiagnostics({QStringLiteral(
      "The previous source uses removed or invalid blocks and was backed up in the application settings. "
      "Macroblocks provides editable replacements for the old input, condition and target options.")}, false);
}

bool BlockEditorBridge::editable() const {
  return controller_ != nullptr && !controller_->running();
}

bool BlockEditorBridge::darkMode() const {
  return controller_ != nullptr && controller_->darkMode();
}

bool BlockEditorBridge::running() const { return controller_ && controller_->running(); }

bool BlockEditorBridge::runWorkspace(const QString &json, qulonglong revision, bool debug) {
  if (!applyWorkspace(json, revision)) return false;
  if (debug && !controller_->executableBlockProgram()) {
    publishDiagnostics({QStringLiteral("Choose block program in the start hat before debugging.")}, false);
    return false;
  }
  if (!controller_->canStart()) {
    publishDiagnostics({controller_->validationMessage()}, false);
    return false;
  }
  controller_->blockDebugger()->enable(debug || controller_->blockDebugger()->enabled() || !breakpointIds_.isEmpty());
  debugJson_ = QStringLiteral("{}");
  emit debugChanged();
  controller_->startBlockProgram(debug);
  return controller_->running();
}

void BlockEditorBridge::pauseProgram() {
  if (running() && controller_->executableBlockProgram()) controller_->blockDebugger()->pause();
}

void BlockEditorBridge::resumeProgram(const QString &step) {
  using Step = blocks::VisualDebugger::Step;
  const std::map<QString, Step> modes{{QStringLiteral("run"),Step::Run}, {QStringLiteral("into"),Step::Into},
      {QStringLiteral("over"),Step::Over}, {QStringLiteral("out"),Step::Out}, {QStringLiteral("tick"),Step::Tick}};
  const auto mode = modes.find(step);
  if (mode == modes.end() || !running()) return;
  controller_->blockDebugger()->resume(mode->second);
  auto json=QJsonDocument::fromJson(debugJson_.toUtf8()).object();
  json.insert(QStringLiteral("paused"),false);
  debugJson_=JsonText(json);
  emit debugChanged();
}

void BlockEditorBridge::stopProgram() { controller_->stopSearch(); }
void BlockEditorBridge::inspectProgram(bool enabled) { controller_->blockDebugger()->enable(enabled); }

void BlockEditorBridge::setBreakpoints(const QStringList &ids) {
  breakpointIds_ = ids;
  std::set<blocks::VisualNodeId> nodes;
  for (const auto &[id, editor] : editorIds_) if (ids.contains(editor)) nodes.insert(id);
  controller_->blockDebugger()->setBreakpoints(std::move(nodes));
}

QString BlockEditorBridge::projectFromWorkspace(const QString &json) {
  const auto parsed=ParseBlocklyWorkspace(json);
  if (!parsed.error.isEmpty()) {
    publishDiagnostics({parsed.error},false);
    return {};
  }
  return PrintVisualProgramJson(parsed.program);
}

QString BlockEditorBridge::workspaceFromProject(const QString &json) {
  const auto parsed=ParseVisualProgramJson(json);
  if (!parsed.program) {
    publishDiagnostics({parsed.error},false);
    return {};
  }
  return WorkspaceForProgram(*parsed.program);
}

QString BlockEditorBridge::openProject() {
  if (!editable()) return {};
  const QString path=OpenSystemFileDialog(QStringLiteral("Open block program or library"),
      QSettings().value(QStringLiteral("blockEditor/projectPath"),QDir::homePath()).toString());
  if (path.isEmpty()) return {};
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size()>4*1024*1024) {
    publishDiagnostics({QStringLiteral("The project must be a readable JSON file under 4 MiB.")},false);
    return {};
  }
  const auto workspace=workspaceFromProject(QString::fromUtf8(file.read(4*1024*1024+1)));
  if (!workspace.isEmpty()) QSettings().setValue(QStringLiteral("blockEditor/projectPath"),path);
  return workspace;
}

bool BlockEditorBridge::saveProject(const QString &json) {
  if (!editable()) return false;
  const auto project=projectFromWorkspace(json);
  if (project.isEmpty()) return false;
  const QString path=SaveSystemFileDialog(QStringLiteral("Save block program"),
      QSettings().value(QStringLiteral("blockEditor/projectPath"),QDir::homePath()+QStringLiteral("/program.forevertas.json")).toString());
  if (path.isEmpty()) return false;
  QSaveFile file(path);
  const auto bytes=project.toUtf8();
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()) {
    publishDiagnostics({QStringLiteral("The block program could not be saved: ")+file.errorString()},false);
    return false;
  }
  QSettings().setValue(QStringLiteral("blockEditor/projectPath"),path);
  return true;
}

QString BlockEditorBridge::BuildCatalogJson() {
  QJsonArray categories;
  for (const blocks::VisualCategory &category : blocks::VisualCategories()) {
    categories.push_back(
        QJsonObject{{QStringLiteral("id"), ToQString(category.id)},
                    {QStringLiteral("label"), ToQString(category.label)},
                    {QStringLiteral("color"), ToQString(category.color)}});
  }

  QJsonArray definitions;
  for (const blocks::VisualBlockDefinition &definition :
       blocks::VisualBlockCatalog()) {
    QJsonArray inputs;
    for (const blocks::VisualInputDefinition &input : definition.inputs) {
      inputs.push_back(QJsonObject{
          {QStringLiteral("key"), ToQString(input.key)},
          {QStringLiteral("label"), ToQString(input.label)},
          {QStringLiteral("checks"), ChecksForInput(input.type)},
          {QStringLiteral("defaultBlock"), ToQString(input.defaultBlockId)},
          {QStringLiteral("defaultValue"), ToQString(input.defaultValue)}});
    }
    QJsonArray fields;
    for (const blocks::VisualFieldDefinition &field : definition.fields) {
      QJsonArray choices;
      for (const auto &[value, label] : field.enumValues) {
        choices.push_back(QJsonArray{ToQString(label), ToQString(value)});
      }
      fields.push_back(QJsonObject{
          {QStringLiteral("key"), ToQString(field.key)},
          {QStringLiteral("label"), ToQString(field.label)},
          {QStringLiteral("kind"), FieldKindName(field.kind)},
          {QStringLiteral("defaultValue"), ToQString(field.defaultValue)},
          {QStringLiteral("choices"), choices}});
    }
    QJsonArray statements;
    for (const blocks::VisualStatementDefinition &statement :
         definition.statements) {
      statements.push_back(
          QJsonObject{{QStringLiteral("key"), ToQString(statement.key)},
                      {QStringLiteral("label"), ToQString(statement.label)},
                      {QStringLiteral("checks"),
                       StatementChecks(statement.family)}});
    }
    definitions.push_back(QJsonObject{
        {QStringLiteral("id"), ToQString(definition.id)},
        {QStringLiteral("type"), ToQString(definition.blocklyType)},
        {QStringLiteral("category"), ToQString(definition.categoryId)},
        {QStringLiteral("label"), ToQString(definition.label)},
        {QStringLiteral("shape"), ShapeName(definition.shape)},
        {QStringLiteral("toolboxVisible"), definition.toolboxVisible},
        {QStringLiteral("inputsInline"), definition.inputsInline},
        {QStringLiteral("outputChecks"),
         ChecksForOutput(definition.outputType)},
        {QStringLiteral("statementChecks"),
         StatementChecks(definition.statementFamily)},
        {QStringLiteral("viewerPicker"), ToQString(definition.viewerPicker)},
        {QStringLiteral("inputs"), inputs},
        {QStringLiteral("fields"), fields},
        {QStringLiteral("statements"), statements}});
  }
  QJsonArray macros;
  for (const auto &macro : blocks::VisualMacroCatalog()) {
    const auto &body = macro.program.find(macro.program.topLevel.front())->statements.at("body");
    std::vector<QJsonObject> sequence;
    for (auto id : body) sequence.push_back(BlocklyNodeForProgram(macro.program, id, false));
    macros.push_back(QJsonObject{
        {QStringLiteral("id"), ToQString(macro.id)},
        {QStringLiteral("category"), ToQString(macro.category)},
        {QStringLiteral("label"), ToQString(macro.label)},
        {QStringLiteral("description"), ToQString(macro.description)},
        {QStringLiteral("stack"), Chain(std::move(sequence))}});
  }
  return QString::fromUtf8(
      QJsonDocument(QJsonObject{{QStringLiteral("version"), 3},
                                {QStringLiteral("categories"), categories},
                                {QStringLiteral("blocks"), definitions},
                                {QStringLiteral("macros"), macros}})
          .toJson(QJsonDocument::Compact));
}

BlockEditorBridge::ParsedWorkspace
BlockEditorBridge::ParseBlocklyWorkspace(const QString &json) {
  ParsedWorkspace result;
  if (json.size() > 4 * 1024 * 1024) {
    result.error = QStringLiteral("Block workspace exceeds the 4 MiB limit.");
    return result;
  }
  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(json.toUtf8(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    result.error = QStringLiteral("Block workspace JSON is invalid: %1")
                       .arg(parseError.errorString());
    return result;
  }
  const QJsonArray top = document.object()
                             .value(QStringLiteral("blocks"))
                             .toObject()
                             .value(QStringLiteral("blocks"))
                             .toArray();
  if (top.size() > 4096) {
    result.error =
        QStringLiteral("Block workspace contains too many top-level blocks.");
    return result;
  }

  std::map<QString, blocks::VisualNodeId> ids;
  std::set<blocks::VisualNodeId> usedIds;
  blocks::VisualNodeId nextId = 1;
  auto allocateId = [&usedIds, &nextId]() {
    while (usedIds.count(nextId) != 0)
      ++nextId;
    const blocks::VisualNodeId value = nextId++;
    usedIds.insert(value);
    return value;
  };
  auto idFor = [&ids, &usedIds, &allocateId](const QString &blocklyId) {
    if (!blocklyId.isEmpty()) {
      const auto found = ids.find(blocklyId);
      if (found != ids.end())
        return found->second;
      bool numeric = false;
      const qulonglong parsed = blocklyId.toULongLong(&numeric, 10);
      const blocks::VisualNodeId value =
          numeric && parsed != 0 &&
                  usedIds.count(static_cast<blocks::VisualNodeId>(parsed)) == 0
              ? static_cast<blocks::VisualNodeId>(parsed)
              : allocateId();
      usedIds.insert(value);
      ids.emplace(blocklyId, value);
      return value;
    }
    return allocateId();
  };

  std::function<std::optional<blocks::VisualNodeId>(const QJsonObject &,
                                                    std::size_t, bool)>
      parseBlock;
  parseBlock = [&](const QJsonObject &object, std::size_t depth,
                   bool statementChild) -> std::optional<blocks::VisualNodeId> {
    if (depth > 128) {
      result.error = QStringLiteral("Block workspace is nested too deeply.");
      return std::nullopt;
    }
    const QString type = object.value(QStringLiteral("type")).toString();
    const blocks::VisualBlockDefinition *const definition =
        blocks::FindVisualBlockByBlocklyType(type.toStdString());
    if (definition == nullptr) {
      result.error = QStringLiteral("Unknown block type '%1'.").arg(type);
      return std::nullopt;
    }
    const blocks::VisualNodeId id =
        idFor(object.value(QStringLiteral("id")).toString());
    if (result.program.nodes.count(id) != 0)
      return id;
    if (result.program.nodes.size() >= 4096) {
      result.error =
          QStringLiteral("Block workspace exceeds the 4096-node limit.");
      return std::nullopt;
    }
    blocks::VisualNode node;
    node.id = id;
    node.definitionId = definition->id;
    node.enabled = object.value(QStringLiteral("enabled")).toBool(true) &&
        object.value(QStringLiteral("disabledReasons")).toArray().isEmpty();
    node.x = object.value(QStringLiteral("x")).toDouble();
    node.y = object.value(QStringLiteral("y")).toDouble();
    const QJsonObject fieldValues =
        object.value(QStringLiteral("fields")).toObject();
    for (auto iterator = fieldValues.constBegin();
         iterator != fieldValues.constEnd(); ++iterator) {
      const bool known =
          std::any_of(definition->fields.begin(), definition->fields.end(),
                      [&iterator](const blocks::VisualFieldDefinition &field) {
                        return ToQString(field.key) == iterator.key();
                      });
      if (!known) {
        result.error = QStringLiteral("Block '%1' contains unknown field '%2'.")
                           .arg(type, iterator.key());
        return std::nullopt;
      }
    }
    for (const blocks::VisualFieldDefinition &field : definition->fields) {
      QString value =
          JsonScalarToString(fieldValues.value(ToQString(field.key)));
      if (value.isEmpty())
        value = ToQString(field.defaultValue);
      if (field.kind == blocks::VisualFieldDefinition::Kind::Boolean) {
        if (value.compare(QStringLiteral("TRUE"), Qt::CaseInsensitive) == 0)
          value = QStringLiteral("true");
        else if (value.compare(QStringLiteral("FALSE"), Qt::CaseInsensitive) ==
                 0)
          value = QStringLiteral("false");
      }
      node.fields.emplace(field.key, value.toStdString());
    }
    result.program.nodes.emplace(id, std::move(node));
    blocks::VisualNode *const stored = result.program.find(id);
    const auto nodeInputs = blocks::VisualInputsForNode(*stored);

    const QJsonObject serializedInputs =
        object.value(QStringLiteral("inputs")).toObject();
    for (auto iterator = serializedInputs.constBegin();
         iterator != serializedInputs.constEnd(); ++iterator) {
      const bool valueInput =
          std::any_of(nodeInputs.begin(), nodeInputs.end(),
                      [&iterator](const blocks::VisualInputDefinition &input) {
                        return ToQString(input.key) == iterator.key();
                      });
      const bool statementInput = std::any_of(
          definition->statements.begin(), definition->statements.end(),
          [&iterator](const blocks::VisualStatementDefinition &statement) {
            return ToQString(statement.key) == iterator.key();
          });
      if (!valueInput && !statementInput) {
        result.error = QStringLiteral("Block '%1' contains unknown input '%2'.")
                           .arg(type, iterator.key());
        return std::nullopt;
      }
    }
    for (const blocks::VisualInputDefinition &input : nodeInputs) {
      const QJsonObject socket =
          serializedInputs.value(ToQString(input.key)).toObject();
      QJsonObject child = socket.value(QStringLiteral("block")).toObject();
      if (child.isEmpty()) {
        child = socket.value(QStringLiteral("shadow")).toObject();
      }
      if (child.isEmpty())
        continue;
      const auto childId = parseBlock(child, depth + 1, false);
      if (!childId)
        return std::nullopt;
      stored->inputs.emplace(input.key, *childId);
    }
    for (const blocks::VisualStatementDefinition &statement :
         definition->statements) {
      QJsonObject child = serializedInputs.value(ToQString(statement.key))
                              .toObject()
                              .value(QStringLiteral("block"))
                              .toObject();
      auto &sequence = stored->statements[statement.key];
      while (!child.isEmpty()) {
        const auto childId = parseBlock(child, depth + 1, true);
        if (!childId)
          return std::nullopt;
        sequence.push_back(*childId);
        child = child.value(QStringLiteral("next"))
                    .toObject()
                    .value(QStringLiteral("block"))
                    .toObject();
      }
    }
    static_cast<void>(statementChild);
    return id;
  };

  for (const QJsonValue &value : top) {
    if (!value.isObject())
      continue;
    QJsonObject current = value.toObject();
    while (!current.isEmpty()) {
      const auto id = parseBlock(current, 1, false);
      if (!id)
        return result;
      result.program.topLevel.push_back(*id);
      current = current.value(QStringLiteral("next"))
                    .toObject()
                    .value(QStringLiteral("block"))
                    .toObject();
    }
  }
  const blocks::VisualProgramValidation validation =
      blocks::ValidateVisualProgram(result.program);
  if (!validation.ok) {
    QStringList messages;
    for (const std::string &error : validation.errors) {
      messages.push_back(ToQString(error));
    }
    result.error = messages.join(QLatin1Char('\n'));
  }
  for (const auto &[editor, id] : ids) result.editorIds.emplace(id, editor);
  return result;
}

bool BlockEditorBridge::applyWorkspace(const QString &workspaceJson,
                                       qulonglong revision) {
  if (!editable()) {
    publishDiagnostics(
        {QStringLiteral("Stop the search before editing blocks.")});
    return false;
  }
  if (revision <= lastAcceptedRevision_)
    return false;
  const ParsedWorkspace parsed = ParseBlocklyWorkspace(workspaceJson);
  if (!parsed.error.isEmpty()) {
    publishDiagnostics(parsed.error.split(QLatin1Char('\n')));
    return false;
  }
  workspaceJson_ = workspaceJson;
  QSettings().setValue(QString::fromLatin1(kDraftWorkspaceKey), workspaceJson);
  const auto compiled = blocks::CompileVisualProgram(parsed.program);
  if (!compiled.ok) {
    QStringList messages;
    for (const std::string &error : compiled.errors) {
      messages.push_back(ToQString(error));
    }
    publishDiagnostics(messages);
    return false;
  }
  const bool applied = controller_->applyBlockProgram(compiled.executable);
  if (!applied) {
    publishDiagnostics({QStringLiteral(
        "The block runtime rejected the program.")});
    return false;
  }

  lastAcceptedRevision_ = revision;
  editorIds_ = parsed.editorIds;
  setBreakpoints(breakpointIds_);
  workspaceJson_ = workspaceJson;
  QSettings settings;
  settings.remove(QString::fromLatin1(kDraftWorkspaceKey));
  settings.setValue(QString::fromLatin1(kStoredProgramKey),
                    PrintVisualProgramJson(parsed.program));
  settings.setValue(QString::fromLatin1(kStoredWorkspaceKey), workspaceJson_);
  publishDiagnostics({}, false);
  // Blockly already owns this exact state. Echoing it back through the
  // property would clear selection, viewport and undo history on every
  // accepted edit.
  return true;
}

void BlockEditorBridge::editorReady() {
  if (editorReady_)
    return;
  editorReady_ = true;
  emit editorReadyChanged();
}

void BlockEditorBridge::requestNativeWorkspace() {
  emit workspaceJsonChanged();
}

QString BlockEditorBridge::selectedViewerTargetJson(const QString &kind) const {
  if (controller_ == nullptr) return QStringLiteral("{}");
  QJsonObject result;
  QVariantMap target;
  if (kind == QStringLiteral("box")) {
    target = controller_->cuboidTargets()->selectedTarget();
    for (const char *key : {"id", "name", "centerX", "centerY", "centerZ",
                            "sizeX", "sizeY", "sizeZ"}) {
      CopyTargetValue(&result, target, key);
    }
  } else if (kind == QStringLiteral("prism")) {
    target = controller_->customVolumeTargets()->selectedTarget();
    for (const char *key : {"id", "name", "plane", "originX", "originY",
                            "originZ", "depth", "polygon"}) {
      CopyTargetValue(&result, target, key);
    }
  } else if (kind == QStringLiteral("rotation")) {
    target = controller_->poseTargets()->selectedTarget();
    for (const char *key : {"id", "name", "yawDegrees", "pitchDegrees",
                            "rollDegrees"}) {
      CopyTargetValue(&result, target, key);
    }
  } else {
    return QStringLiteral("{}");
  }
  if (target.isEmpty()) return QStringLiteral("{}");
  result.insert(QStringLiteral("kind"), kind);
  return JsonText(result);
}

void BlockEditorBridge::requestViewerPointPick(const QString &blockId) {
  if (!editable() || blockId.isEmpty()) return;
  pendingViewerPointBlockId_ = blockId;
  emit viewerPointPickRequested(blockId);
}

void BlockEditorBridge::completeViewerPointPick(const QString &blockId,
                                                double x,
                                                double y,
                                                double z) {
  if (blockId.isEmpty() || blockId != pendingViewerPointBlockId_ ||
      !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
    return;
  }
  pendingViewerPointBlockId_.clear();
  emit viewerPointPicked(blockId, x, y, z);
}

void BlockEditorBridge::publishDiagnostics(const QStringList &messages,
                                           bool isError) {
  if (isError || messages.isEmpty())
    controller_->setBlockProgramError(isError ? messages.join(QLatin1Char('\n')) : QString());
  QJsonArray array;
  for (const QString &message : messages) {
    array.push_back(QJsonObject{
        {QStringLiteral("message"), message},
        {QStringLiteral("severity"),
         isError ? QStringLiteral("error") : QStringLiteral("info")}});
  }
  const QString next =
      QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
  if (next == diagnosticsJson_)
    return;
  diagnosticsJson_ = next;
  emit diagnosticsJsonChanged();
}

} // namespace forevertas::app
