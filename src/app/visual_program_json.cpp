#include "app/visual_program_json.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStringList>

#include <charconv>
#include <cmath>

namespace forevertas::app {
namespace {

constexpr qsizetype kMaximumDocumentSize = 4 * 1024 * 1024;
constexpr std::size_t kMaximumNodeCount = 4096;

QString IdString(blocks::VisualNodeId id) {
  return QString::number(static_cast<qulonglong>(id));
}

std::optional<blocks::VisualNodeId> ParseId(const QJsonValue &value) {
  if (!value.isString())
    return std::nullopt;
  const QByteArray bytes = value.toString().toLatin1();
  if (bytes.isEmpty())
    return std::nullopt;
  blocks::VisualNodeId id = 0;
  const char *const begin = bytes.constData();
  const char *const end = begin + bytes.size();
  const auto parsed = std::from_chars(begin, end, id);
  if (parsed.ec != std::errc() || parsed.ptr != end || id == 0)
    return std::nullopt;
  return id;
}

bool HasOnlyKeys(const QJsonObject &object,
                 std::initializer_list<const char *> allowed,
                 QString *unknown) {
  for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
    bool known = false;
    for (const char *const key : allowed) {
      if (it.key() == QString::fromLatin1(key)) {
        known = true;
        break;
      }
    }
    if (!known) {
      if (unknown != nullptr)
        *unknown = it.key();
      return false;
    }
  }
  return true;
}

QJsonObject StringMap(const std::map<std::string, std::string> &values) {
  QJsonObject object;
  for (const auto &[key, value] : values)
    object.insert(QString::fromStdString(key), QString::fromStdString(value));
  return object;
}

} // namespace

QString PrintVisualProgramJson(const blocks::VisualProgram &program) {
  QJsonArray nodes;
  for (const auto &[id, node] : program.nodes) {
    QJsonObject inputs;
    for (const auto &[key, child] : node.inputs)
      inputs.insert(QString::fromStdString(key), IdString(child));

    QJsonObject statements;
    for (const auto &[key, children] : node.statements) {
      QJsonArray sequence;
      for (const blocks::VisualNodeId child : children)
        sequence.push_back(IdString(child));
      statements.insert(QString::fromStdString(key), sequence);
    }

    nodes.push_back(QJsonObject{
        {QStringLiteral("id"), IdString(id)},
        {QStringLiteral("definitionId"), QString::fromStdString(node.definitionId)},
        {QStringLiteral("fields"), StringMap(node.fields)},
        {QStringLiteral("inputs"), inputs},
        {QStringLiteral("statements"), statements},
        {QStringLiteral("x"), node.x},
        {QStringLiteral("y"), node.y},
    });
  }

  QJsonArray topLevel;
  for (const blocks::VisualNodeId id : program.topLevel)
    topLevel.push_back(IdString(id));

  return QString::fromUtf8(
      QJsonDocument(QJsonObject{{QStringLiteral("version"), 3},
                                {QStringLiteral("topLevel"), topLevel},
                                {QStringLiteral("nodes"), nodes}})
          .toJson(QJsonDocument::Compact));
}

VisualProgramJson ParseVisualProgramJson(const QString &json) {
  VisualProgramJson result;
  if (json.size() > kMaximumDocumentSize) {
    result.error = QStringLiteral("Visual program exceeds the 4 MiB limit.");
    return result;
  }

  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    result.error = QStringLiteral("Visual program JSON is invalid: %1")
                       .arg(parseError.errorString());
    return result;
  }

  const QJsonObject root = document.object();
  QString unknown;
  if (!HasOnlyKeys(root, {"version", "topLevel", "nodes"}, &unknown)) {
    result.error = QStringLiteral("Visual program contains unknown key '%1'.")
                       .arg(unknown);
    return result;
  }
  const QJsonValue version = root.value(QStringLiteral("version"));
  if (!version.isDouble() || version.toInt(-1) != 3 ||
      version.toDouble() != 3.0) {
    result.error = QStringLiteral("Unsupported visual program version.");
    return result;
  }
  const QJsonValue nodesValue = root.value(QStringLiteral("nodes"));
  const QJsonValue topValue = root.value(QStringLiteral("topLevel"));
  if (!nodesValue.isArray() || !topValue.isArray()) {
    result.error = QStringLiteral(
        "Visual program must contain nodes and topLevel arrays.");
    return result;
  }
  const QJsonArray nodes = nodesValue.toArray();
  if (nodes.size() > static_cast<qsizetype>(kMaximumNodeCount)) {
    result.error = QStringLiteral("Visual program exceeds the 4096-node limit.");
    return result;
  }

  blocks::VisualProgram program;
  for (const QJsonValue &entry : nodes) {
    if (!entry.isObject()) {
      result.error = QStringLiteral("Visual program node entries must be objects.");
      return result;
    }
    const QJsonObject object = entry.toObject();
    if (!HasOnlyKeys(object,
                     {"id", "definitionId", "fields", "inputs", "statements",
                      "x", "y"},
                     &unknown)) {
      result.error = QStringLiteral("Visual program node contains unknown key '%1'.")
                         .arg(unknown);
      return result;
    }
    const auto id = ParseId(object.value(QStringLiteral("id")));
    const QJsonValue definition = object.value(QStringLiteral("definitionId"));
    if (!id || !definition.isString() || definition.toString().isEmpty()) {
      result.error = QStringLiteral(
          "Visual program node is missing a valid id or definitionId.");
      return result;
    }
    if (program.nodes.count(*id) != 0) {
      result.error = QStringLiteral("Visual program contains duplicate node id %1.")
                         .arg(IdString(*id));
      return result;
    }

    blocks::VisualNode node;
    node.id = *id;
    node.definitionId = definition.toString().toStdString();

    const QJsonValue fieldsValue = object.value(QStringLiteral("fields"));
    const QJsonValue inputsValue = object.value(QStringLiteral("inputs"));
    const QJsonValue statementsValue = object.value(QStringLiteral("statements"));
    if (!fieldsValue.isObject() || !inputsValue.isObject() ||
        !statementsValue.isObject()) {
      result.error = QStringLiteral(
          "Visual program node fields, inputs and statements must be objects.");
      return result;
    }
    const QJsonObject fields = fieldsValue.toObject();
    const QJsonObject inputs = inputsValue.toObject();
    const QJsonObject statements = statementsValue.toObject();
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
      if (!it.value().isString()) {
        result.error = QStringLiteral("Visual program field '%1' must be a string.")
                           .arg(it.key());
        return result;
      }
      node.fields.emplace(it.key().toStdString(), it.value().toString().toStdString());
    }
    for (auto it = inputs.constBegin(); it != inputs.constEnd(); ++it) {
      const auto child = ParseId(it.value());
      if (!child) {
        result.error = QStringLiteral("Visual program input '%1' has an invalid id.")
                           .arg(it.key());
        return result;
      }
      node.inputs.emplace(it.key().toStdString(), *child);
    }
    for (auto it = statements.constBegin(); it != statements.constEnd(); ++it) {
      if (!it.value().isArray()) {
        result.error = QStringLiteral(
            "Visual program statement '%1' must be an array.")
                           .arg(it.key());
        return result;
      }
      std::vector<blocks::VisualNodeId> sequence;
      for (const QJsonValue &childValue : it.value().toArray()) {
        const auto child = ParseId(childValue);
        if (!child) {
          result.error = QStringLiteral(
              "Visual program statement '%1' contains an invalid id.")
                             .arg(it.key());
          return result;
        }
        sequence.push_back(*child);
      }
      node.statements.emplace(it.key().toStdString(), std::move(sequence));
    }

    const QJsonValue x = object.value(QStringLiteral("x"));
    const QJsonValue y = object.value(QStringLiteral("y"));
    if (!x.isDouble() || !y.isDouble() || !std::isfinite(x.toDouble()) ||
        !std::isfinite(y.toDouble())) {
      result.error = QStringLiteral("Visual program node has invalid coordinates.");
      return result;
    }
    node.x = x.toDouble();
    node.y = y.toDouble();
    program.nodes.emplace(*id, std::move(node));
  }

  for (const QJsonValue &value : topValue.toArray()) {
    const auto id = ParseId(value);
    if (!id) {
      result.error = QStringLiteral("Visual program topLevel contains an invalid id.");
      return result;
    }
    program.topLevel.push_back(*id);
  }

  const blocks::VisualProgramValidation validation =
      blocks::ValidateVisualProgram(program, kMaximumNodeCount, 128);
  if (!validation.ok) {
    QStringList errors;
    for (const std::string &error : validation.errors)
      errors.push_back(QString::fromStdString(error));
    result.error = errors.join(QLatin1Char('\n'));
    return result;
  }

  result.program = std::move(program);
  return result;
}

} // namespace forevertas::app
