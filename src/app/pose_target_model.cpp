#include "app/pose_target_model.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>

namespace forevertas::app {
namespace {

constexpr char kPoseTargetsKey[] = "targets/poses";
constexpr int kVersion = 1;
constexpr qsizetype kMaximumTargets = 256;
constexpr double kMaximumCoordinate = 10000000.0;

QString Number(double value) {
    return QString::number(value, 'g', 15);
}

bool FinitePosition(const QVector3D &position) {
    return std::isfinite(position.x()) &&
            std::isfinite(position.y()) &&
            std::isfinite(position.z()) &&
            std::abs(position.x()) <= kMaximumCoordinate &&
            std::abs(position.y()) <= kMaximumCoordinate &&
            std::abs(position.z()) <= kMaximumCoordinate;
}

bool FiniteRotation(const QQuaternion &rotation) {
    const float lengthSquared = rotation.lengthSquared();
    return std::isfinite(rotation.scalar()) &&
            std::isfinite(rotation.x()) &&
            std::isfinite(rotation.y()) &&
            std::isfinite(rotation.z()) &&
            std::isfinite(lengthSquared) &&
            lengthSquared > 1e-12F;
}

}  // namespace

PoseTargetModel::PoseTargetModel(const QVariantMap &legacySettings,
                                 QObject *parent)
    : QObject(parent) {
    load(legacySettings);
}

QVariantList PoseTargetModel::targets() const {
    QVariantList result;
    result.reserve(count());
    for (int index = 0; index < count(); ++index) {
        result.push_back(ToVariantMap(
                targets_[static_cast<std::size_t>(index)],
                index == selectedIndex_));
    }
    return result;
}

int PoseTargetModel::count() const {
    return static_cast<int>(targets_.size());
}

int PoseTargetModel::maximumCount() const {
    return static_cast<int>(kMaximumTargets);
}

int PoseTargetModel::selectedIndex() const {
    return selectedIndex_;
}

QVariantMap PoseTargetModel::selectedTarget() const {
    if (selectedIndex_ < 0 || selectedIndex_ >= count()) return {};
    return ToVariantMap(
            targets_[static_cast<std::size_t>(selectedIndex_)], true);
}

bool PoseTargetModel::editingEnabled() const {
    return editingEnabled_;
}

int PoseTargetModel::addTarget(double x,
                               double y,
                               double z,
                               const QQuaternion &rotation) {
    const QVector3D position(
            static_cast<float>(x),
            static_cast<float>(y),
            static_cast<float>(z));
    if (!editingEnabled_ || !FinitePosition(position) ||
        !FiniteRotation(rotation) ||
        targets_.size() >= static_cast<std::size_t>(kMaximumTargets)) {
        return -1;
    }
    const QVector3D euler = rotation.normalized().toEulerAngles();
    targets_.push_back(Target{
            QUuid::createUuid().toString(QUuid::WithoutBraces),
            nextDefaultName(),
            position,
            NormalizeDegrees(euler.z()),
            NormalizeDegrees(euler.y()),
            NormalizeDegrees(euler.x())});
    selectedIndex_ = count() - 1;
    persist();
    emit targetsChanged();
    emit selectedTargetChanged();
    return selectedIndex_;
}

int PoseTargetModel::duplicateSelected() {
    if (!editingEnabled_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count() ||
        targets_.size() >= static_cast<std::size_t>(kMaximumTargets)) {
        return -1;
    }
    Target target =
            targets_[static_cast<std::size_t>(selectedIndex_)];
    target.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    target.name = nextDefaultName();
    target.position.setX(
            target.position.x() + 1.0F <= kMaximumCoordinate
                    ? target.position.x() + 1.0F
                    : target.position.x() - 1.0F);
    target.position.setZ(
            target.position.z() + 1.0F <= kMaximumCoordinate
                    ? target.position.z() + 1.0F
                    : target.position.z() - 1.0F);
    targets_.push_back(std::move(target));
    selectedIndex_ = count() - 1;
    persist();
    emit targetsChanged();
    emit selectedTargetChanged();
    return selectedIndex_;
}

bool PoseTargetModel::removeTarget(int index) {
    if (!editingEnabled_ || index < 0 || index >= count() ||
        count() <= 1) {
        return false;
    }
    const int previous = selectedIndex_;
    targets_.erase(targets_.begin() + index);
    if (selectedIndex_ > index) --selectedIndex_;
    else if (selectedIndex_ == index) {
        selectedIndex_ = std::min(index, count() - 1);
    }
    persist();
    emit targetsChanged();
    if (previous != selectedIndex_ || previous == index) {
        emit selectedTargetChanged();
    }
    return true;
}

bool PoseTargetModel::selectTarget(int index) {
    if (!editingEnabled_ || index < 0 || index >= count() ||
        selectedIndex_ == index) {
        return false;
    }
    selectedIndex_ = index;
    persist();
    emit targetsChanged();
    emit selectedTargetChanged();
    return true;
}

bool PoseTargetModel::setName(int index, const QString &name) {
    if (!editingEnabled_ || index < 0 || index >= count()) return false;
    const QString normalized = name.trimmed().left(80);
    Target &target = targets_[static_cast<std::size_t>(index)];
    if (normalized.isEmpty() || target.name == normalized) return false;
    target.name = normalized;
    persist();
    notifyTargetChanged(index);
    return true;
}

bool PoseTargetModel::setPositionComponent(
        int index,
        const QString &axis,
        const QString &value) {
    const int component = PositionAxisIndex(axis);
    double parsed = 0.0;
    if (!editingEnabled_ || index < 0 || index >= count() ||
        component < 0 || !ParseFinite(value, &parsed)) {
        return false;
    }
    Target &target = targets_[static_cast<std::size_t>(index)];
    if (qFuzzyCompare(
                static_cast<double>(target.position[component]) + 1.0,
                parsed + 1.0)) {
        return false;
    }
    target.position[component] = static_cast<float>(parsed);
    persist();
    notifyTargetChanged(index);
    return true;
}

bool PoseTargetModel::setRotationComponent(
        int index,
        const QString &axis,
        const QString &value) {
    double parsed = 0.0;
    if (!editingEnabled_ || index < 0 || index >= count() ||
        !ParseFinite(value, &parsed)) {
        return false;
    }
    Target &target = targets_[static_cast<std::size_t>(index)];
    float *component = nullptr;
    if (axis == QStringLiteral("yaw")) component = &target.yawDegrees;
    else if (axis == QStringLiteral("pitch")) {
        component = &target.pitchDegrees;
    } else if (axis == QStringLiteral("roll")) {
        component = &target.rollDegrees;
    } else {
        return false;
    }
    const float normalized = NormalizeDegrees(parsed);
    if (qFuzzyCompare(
                static_cast<double>(*component) + 1.0,
                static_cast<double>(normalized) + 1.0)) {
        return false;
    }
    *component = normalized;
    persist();
    notifyTargetChanged(index);
    return true;
}

bool PoseTargetModel::translateSelected(double x, double y, double z) {
    if (!editingEnabled_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count()) {
        return false;
    }
    const QVector3D delta(
            static_cast<float>(x),
            static_cast<float>(y),
            static_cast<float>(z));
    Target &target =
            targets_[static_cast<std::size_t>(selectedIndex_)];
    const QVector3D proposed = target.position + delta;
    if (!FinitePosition(delta) || delta.isNull() ||
        !FinitePosition(proposed)) {
        return false;
    }
    target.position = proposed;
    persist();
    notifyTargetChanged(selectedIndex_);
    return true;
}

bool PoseTargetModel::moveSelectedTo(
        double x,
        double y,
        double z,
        const QQuaternion &rotation) {
    const QVector3D position(
            static_cast<float>(x),
            static_cast<float>(y),
            static_cast<float>(z));
    if (!editingEnabled_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count() || !FinitePosition(position) ||
        !FiniteRotation(rotation)) {
        return false;
    }
    const QVector3D euler = rotation.normalized().toEulerAngles();
    Target &target = targets_[static_cast<std::size_t>(selectedIndex_)];
    const float yaw = NormalizeDegrees(euler.z());
    const float pitch = NormalizeDegrees(euler.y());
    const float roll = NormalizeDegrees(euler.x());
    if (target.position == position &&
        qFuzzyCompare(target.yawDegrees + 1.0F, yaw + 1.0F) &&
        qFuzzyCompare(target.pitchDegrees + 1.0F, pitch + 1.0F) &&
        qFuzzyCompare(target.rollDegrees + 1.0F, roll + 1.0F)) {
        return false;
    }
    target.position = position;
    target.yawDegrees = yaw;
    target.pitchDegrees = pitch;
    target.rollDegrees = roll;
    persist();
    notifyTargetChanged(selectedIndex_);
    return true;
}

bool PoseTargetModel::rotateSelected(const QString &axis,
                                     double degrees) {
    if (!editingEnabled_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count() || !std::isfinite(degrees)) {
        return false;
    }
    Target &target =
            targets_[static_cast<std::size_t>(selectedIndex_)];
    float *component = nullptr;
    if (axis == QStringLiteral("yaw")) component = &target.yawDegrees;
    else if (axis == QStringLiteral("pitch")) {
        component = &target.pitchDegrees;
    } else if (axis == QStringLiteral("roll")) {
        component = &target.rollDegrees;
    } else {
        return false;
    }
    const float proposed = NormalizeDegrees(
            static_cast<double>(*component) + degrees);
    if (qFuzzyCompare(
                static_cast<double>(*component) + 1.0,
                static_cast<double>(proposed) + 1.0)) {
        return false;
    }
    *component = proposed;
    persist();
    notifyTargetChanged(selectedIndex_);
    return true;
}

void PoseTargetModel::setEditingEnabled(bool enabled) {
    if (editingEnabled_ == enabled) return;
    editingEnabled_ = enabled;
    emit editingEnabledChanged();
}

QVariantMap PoseTargetModel::ToVariantMap(
        const Target &target,
        bool selected) {
    return QVariantMap{
            {QStringLiteral("id"), target.id},
            {QStringLiteral("name"), target.name},
            {QStringLiteral("kind"), QStringLiteral("pose")},
            {QStringLiteral("position"), target.position},
            {QStringLiteral("rotation"), Rotation(target)},
            {QStringLiteral("x"), Number(target.position.x())},
            {QStringLiteral("y"), Number(target.position.y())},
            {QStringLiteral("z"), Number(target.position.z())},
            {QStringLiteral("yawDegrees"), Number(target.yawDegrees)},
            {QStringLiteral("pitchDegrees"), Number(target.pitchDegrees)},
            {QStringLiteral("rollDegrees"), Number(target.rollDegrees)},
            {QStringLiteral("selected"), selected}};
}

bool PoseTargetModel::ParseFinite(
        const QString &value,
        double *result) {
    bool okay = false;
    const double parsed = value.toDouble(&okay);
    if (!okay || !std::isfinite(parsed) ||
        std::abs(parsed) > kMaximumCoordinate) {
        return false;
    }
    *result = parsed;
    return true;
}

int PoseTargetModel::PositionAxisIndex(const QString &axis) {
    if (axis == QStringLiteral("x")) return 0;
    if (axis == QStringLiteral("y")) return 1;
    if (axis == QStringLiteral("z")) return 2;
    return -1;
}

float PoseTargetModel::NormalizeDegrees(double degrees) {
    double normalized = std::fmod(degrees + 180.0, 360.0);
    if (normalized < 0.0) normalized += 360.0;
    return static_cast<float>(normalized - 180.0);
}

QQuaternion PoseTargetModel::Rotation(const Target &target) {
    return QQuaternion::fromEulerAngles(
                   target.rollDegrees,
                   target.pitchDegrees,
                   target.yawDegrees)
            .normalized();
}

QString PoseTargetModel::nextDefaultName() const {
    int suffix = 1;
    for (;;) {
        const QString candidate =
                QStringLiteral("Car pose %1").arg(suffix++);
        const bool exists = std::any_of(
                targets_.cbegin(),
                targets_.cend(),
                [&candidate](const Target &target) {
                    return target.name.compare(
                                   candidate, Qt::CaseInsensitive) == 0;
                });
        if (!exists) return candidate;
    }
}

void PoseTargetModel::load(const QVariantMap &legacySettings) {
    const QJsonDocument document = QJsonDocument::fromJson(
            QSettings().value(QLatin1String(kPoseTargetsKey)).toByteArray());
    QString selectedId;
    QSet<QString> loadedIds;
    int skippedTargets = 0;
    if (document.isObject()) {
        const QJsonObject root = document.object();
        if (root.value(QStringLiteral("version")).toInt() == kVersion) {
            selectedId = root.value(QStringLiteral("selectedId")).toString();
            const QJsonArray array =
                    root.value(QStringLiteral("targets")).toArray();
            const qsizetype limit =
                    std::min(array.size(), kMaximumTargets);
            for (qsizetype index = 0; index < limit; ++index) {
                const QJsonObject object = array.at(index).toObject();
                const QJsonArray positionArray =
                        object.value(QStringLiteral("position")).toArray();
                const QJsonArray rotationArray =
                        object.value(QStringLiteral("rotation")).toArray();
                const QString id = object.value(QStringLiteral("id"))
                                           .toString()
                                           .trimmed();
                const QString name = object.value(QStringLiteral("name"))
                                             .toString()
                                             .trimmed()
                                             .left(80);
                if (id.isEmpty() || loadedIds.contains(id) ||
                    name.isEmpty() || positionArray.size() != 3 ||
                    rotationArray.size() != 3) {
                    ++skippedTargets;
                    continue;
                }
                const QVector3D position(
                        static_cast<float>(positionArray[0].toDouble(
                                std::numeric_limits<double>::quiet_NaN())),
                        static_cast<float>(positionArray[1].toDouble(
                                std::numeric_limits<double>::quiet_NaN())),
                        static_cast<float>(positionArray[2].toDouble(
                                std::numeric_limits<double>::quiet_NaN())));
                const double yaw = rotationArray[0].toDouble(
                        std::numeric_limits<double>::quiet_NaN());
                const double pitch = rotationArray[1].toDouble(
                        std::numeric_limits<double>::quiet_NaN());
                const double roll = rotationArray[2].toDouble(
                        std::numeric_limits<double>::quiet_NaN());
                if (!FinitePosition(position) || !std::isfinite(yaw) ||
                    !std::isfinite(pitch) || !std::isfinite(roll) ||
                    std::abs(yaw) > kMaximumCoordinate ||
                    std::abs(pitch) > kMaximumCoordinate ||
                    std::abs(roll) > kMaximumCoordinate) {
                    ++skippedTargets;
                    continue;
                }
                targets_.push_back(Target{
                        id,
                        name,
                        position,
                        NormalizeDegrees(yaw),
                        NormalizeDegrees(pitch),
                        NormalizeDegrees(roll)});
                loadedIds.insert(id);
            }
        }
    }
    if (skippedTargets > 0) {
        qWarning("ForeverTAS: dropped %d invalid persisted pose "
                 "target(s)",
                 skippedTargets);
    }
    if (targets_.empty()) {
        const auto number = [&legacySettings](
                                    const QString &key,
                                    double fallback) {
            double value = 0.0;
            return ParseFinite(
                           legacySettings.value(key).toString(), &value)
                    ? value
                    : fallback;
        };
        targets_.push_back(Target{
                QUuid::createUuid().toString(QUuid::WithoutBraces),
                QStringLiteral("Car pose 1"),
                QVector3D(
                        static_cast<float>(number(
                                QStringLiteral("x"), 0.0)),
                        static_cast<float>(number(
                                QStringLiteral("y"), 0.0)),
                        static_cast<float>(number(
                                QStringLiteral("z"), 0.0))),
                NormalizeDegrees(number(
                        QStringLiteral("yawDegrees"), 0.0)),
                NormalizeDegrees(number(
                        QStringLiteral("pitchDegrees"), 0.0)),
                NormalizeDegrees(number(
                        QStringLiteral("rollDegrees"), 0.0))});
    }
    const auto selected = std::find_if(
            targets_.cbegin(),
            targets_.cend(),
            [&selectedId](const Target &target) {
                return target.id == selectedId;
            });
    selectedIndex_ = selected == targets_.cend()
            ? 0
            : static_cast<int>(selected - targets_.cbegin());
    persist();
}

void PoseTargetModel::persist() const {
    QJsonArray values;
    for (const Target &target : targets_) {
        values.push_back(QJsonObject{
                {QStringLiteral("id"), target.id},
                {QStringLiteral("name"), target.name},
                {QStringLiteral("position"),
                 QJsonArray{
                         target.position.x(),
                         target.position.y(),
                         target.position.z()}},
                {QStringLiteral("rotation"),
                 QJsonArray{
                         target.yawDegrees,
                         target.pitchDegrees,
                         target.rollDegrees}}});
    }
    const QString selectedId =
            selectedIndex_ >= 0
                    && selectedIndex_ < static_cast<int>(targets_.size())
            ? targets_[static_cast<std::size_t>(selectedIndex_)].id
            : QString{};
    QSettings().setValue(
            QLatin1String(kPoseTargetsKey),
            QJsonDocument(QJsonObject{
                    {QStringLiteral("version"), kVersion},
                    {QStringLiteral("selectedId"), selectedId},
                    {QStringLiteral("targets"), values}})
                    .toJson(QJsonDocument::Compact));
}

void PoseTargetModel::notifyTargetChanged(int index) {
    emit targetsChanged();
    if (index == selectedIndex_) emit selectedTargetChanged();
}

}  // namespace forevertas::app
