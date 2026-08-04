#include "app/cuboid_target_model.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>

namespace forevertas::app {
namespace {

constexpr char kCuboidTargetsKey[] = "targets/cuboids";
constexpr int kPersistenceVersion = 1;
constexpr qsizetype kMaximumPersistedTargets = 512;
constexpr float kMinimumSize = 0.001F;

double SettingNumber(const QVariantMap &settings,
                     const QString &key,
                     double fallback,
                     bool positive = false) {
    bool okay = false;
    const double value = settings.value(key).toString().toDouble(&okay);
    return okay && std::isfinite(value) &&
            std::abs(value) <= std::numeric_limits<float>::max() &&
            (!positive || value > 0.0)
            ? value
            : fallback;
}

bool IsFinite(const QVector3D &value) {
    return std::isfinite(value.x()) && std::isfinite(value.y()) &&
            std::isfinite(value.z());
}

bool HasPositiveComponents(const QVector3D &value) {
    return IsFinite(value) && value.x() >= kMinimumSize &&
            value.y() >= kMinimumSize && value.z() >= kMinimumSize;
}

QString Number(double value) {
    return QString::number(value, 'g', 15);
}

}  // namespace

CuboidTargetModel::CuboidTargetModel(const QVariantMap &legacySettings,
                                     QObject *parent)
    : QAbstractListModel(parent) {
    persistenceTimer_.setSingleShot(true);
    persistenceTimer_.setInterval(150);
    connect(&persistenceTimer_, &QTimer::timeout,
            this, &CuboidTargetModel::persist);
    load(legacySettings);
}

CuboidTargetModel::~CuboidTargetModel() {
    if (persistenceTimer_.isActive()) persist();
}

QVariantList CuboidTargetModel::targets() const {
    QVariantList values;
    values.reserve(static_cast<qsizetype>(targets_.size()));
    for (int index = 0; index < count(); ++index) {
        values.push_back(ToVariantMap(
                targets_[static_cast<std::size_t>(index)],
                index == selectedIndex_));
    }
    return values;
}

int CuboidTargetModel::count() const {
    return static_cast<int>(targets_.size());
}

int CuboidTargetModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : count();
}

QVariant CuboidTargetModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) {
        return {};
    }
    const Target &target = targets_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case IdRole: return target.id;
    case NameRole: return target.name;
    case CenterRole: return target.center;
    case SizeRole: return target.size;
    case CenterXRole: return Number(target.center.x());
    case CenterYRole: return Number(target.center.y());
    case CenterZRole: return Number(target.center.z());
    case SizeXRole: return Number(target.size.x());
    case SizeYRole: return Number(target.size.y());
    case SizeZRole: return Number(target.size.z());
    case SelectedRole: return index.row() == selectedIndex_;
    default: return {};
    }
}

QHash<int, QByteArray> CuboidTargetModel::roleNames() const {
    return {{IdRole, "targetId"},
            {NameRole, "targetName"},
            {CenterRole, "targetCenter"},
            {SizeRole, "targetSize"},
            {CenterXRole, "targetCenterX"},
            {CenterYRole, "targetCenterY"},
            {CenterZRole, "targetCenterZ"},
            {SizeXRole, "targetSizeX"},
            {SizeYRole, "targetSizeY"},
            {SizeZRole, "targetSizeZ"},
            {SelectedRole, "targetSelected"}};
}

int CuboidTargetModel::maximumCount() const {
    return static_cast<int>(kMaximumPersistedTargets);
}

bool CuboidTargetModel::editingEnabled() const {
    return editingEnabled_;
}

int CuboidTargetModel::selectedIndex() const {
    return selectedIndex_;
}

QVariantMap CuboidTargetModel::selectedTarget() const {
    if (selectedIndex_ < 0 || selectedIndex_ >= count()) return {};
    return ToVariantMap(
            targets_[static_cast<std::size_t>(selectedIndex_)], true);
}

int CuboidTargetModel::addTarget(double centerX,
                                 double centerY,
                                 double centerZ) {
    const QVector3D center(
            static_cast<float>(centerX),
            static_cast<float>(centerY),
            static_cast<float>(centerZ));
    if (!editingEnabled_ || !IsFinite(center) ||
        count() >= maximumCount()) {
        return -1;
    }
    const int insertionIndex = count();
    beginInsertRows({}, insertionIndex, insertionIndex);
    targets_.push_back(Target{
            QUuid::createUuid().toString(QUuid::WithoutBraces),
            nextDefaultName(),
            center,
            QVector3D(10.0F, 10.0F, 10.0F)});
    endInsertRows();
    const int previousSelection = selectedIndex_;
    selectedIndex_ = count() - 1;
    if (previousSelection >= 0 && previousSelection < count()) {
        emit dataChanged(index(previousSelection), index(previousSelection),
                         {SelectedRole});
    }
    emit dataChanged(index(selectedIndex_), index(selectedIndex_),
                     {SelectedRole});
    persist();
    emit targetsChanged();
    emit selectedTargetChanged();
    return selectedIndex_;
}

int CuboidTargetModel::duplicateSelected() {
    if (!editingEnabled_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count() ||
        count() >= maximumCount()) {
        return -1;
    }
    const Target &source =
            targets_[static_cast<std::size_t>(selectedIndex_)];
    Target duplicate = source;
    duplicate.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    duplicate.name = nextDefaultName();
    duplicate.center += QVector3D(1.0F, 0.0F, 1.0F);
    const int insertionIndex = count();
    beginInsertRows({}, insertionIndex, insertionIndex);
    targets_.push_back(std::move(duplicate));
    endInsertRows();
    const int previousSelection = selectedIndex_;
    selectedIndex_ = count() - 1;
    emit dataChanged(index(previousSelection), index(previousSelection),
                     {SelectedRole});
    emit dataChanged(index(selectedIndex_), index(selectedIndex_),
                     {SelectedRole});
    persist();
    emit targetsChanged();
    emit selectedTargetChanged();
    return selectedIndex_;
}

bool CuboidTargetModel::removeTarget(int index) {
    if (!editingEnabled_ || index < 0 || index >= count() ||
        count() <= 1) {
        return false;
    }
    const int previousSelection = selectedIndex_;
    beginRemoveRows({}, index, index);
    targets_.erase(targets_.begin() + index);
    if (selectedIndex_ > index) {
        --selectedIndex_;
    } else if (selectedIndex_ == index) {
        selectedIndex_ = std::min(index, count() - 1);
    }
    endRemoveRows();
    if (selectedIndex_ >= 0) {
        emit dataChanged(this->index(selectedIndex_),
                         this->index(selectedIndex_), {SelectedRole});
    }
    persist();
    emit targetsChanged();
    if (previousSelection != selectedIndex_ || previousSelection == index) {
        emit selectedTargetChanged();
    }
    return true;
}

bool CuboidTargetModel::selectTarget(int index) {
    if (!editingEnabled_ || index < 0 || index >= count() ||
        selectedIndex_ == index) {
        return false;
    }
    const int previousSelection = selectedIndex_;
    selectedIndex_ = index;
    emit dataChanged(this->index(previousSelection),
                     this->index(previousSelection), {SelectedRole});
    emit dataChanged(this->index(selectedIndex_), this->index(selectedIndex_),
                     {SelectedRole});
    persist();
    emit targetsChanged();
    emit selectedTargetChanged();
    return true;
}

bool CuboidTargetModel::setName(int index, const QString &name) {
    if (!editingEnabled_ || index < 0 || index >= count()) return false;
    const QString normalized = name.trimmed().left(80);
    Target &target = targets_[static_cast<std::size_t>(index)];
    if (normalized.isEmpty() || target.name == normalized) return false;
    target.name = normalized;
    persist();
    notifyTargetChanged(index);
    emit targetsChanged();
    return true;
}

bool CuboidTargetModel::setCenterComponent(int index,
                                           const QString &axis,
                                           const QString &value) {
    if (!editingEnabled_ || index < 0 || index >= count()) return false;
    const int component = AxisIndex(axis);
    double parsed = 0.0;
    if (component < 0 || !ParseFinite(value, &parsed)) return false;
    Target &target = targets_[static_cast<std::size_t>(index)];
    if (qFuzzyCompare(
                static_cast<double>(target.center[component]) + 1.0,
                parsed + 1.0)) {
        return false;
    }
    target.center[component] = static_cast<float>(parsed);
    schedulePersist();
    notifyTargetChanged(index);
    return true;
}

bool CuboidTargetModel::setSizeComponent(int index,
                                         const QString &axis,
                                         const QString &value) {
    if (!editingEnabled_ || index < 0 || index >= count()) return false;
    const int component = AxisIndex(axis);
    double parsed = 0.0;
    if (component < 0 || !ParseFinite(value, &parsed) ||
        parsed < kMinimumSize) {
        return false;
    }
    Target &target = targets_[static_cast<std::size_t>(index)];
    if (qFuzzyCompare(
                static_cast<double>(target.size[component]) + 1.0,
                parsed + 1.0)) {
        return false;
    }
    target.size[component] = static_cast<float>(parsed);
    schedulePersist();
    notifyTargetChanged(index);
    return true;
}

bool CuboidTargetModel::translateSelected(double x, double y, double z) {
    if (!editingEnabled_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count()) {
        return false;
    }
    const QVector3D delta(
            static_cast<float>(x),
            static_cast<float>(y),
            static_cast<float>(z));
    if (!IsFinite(delta) || delta.isNull()) return false;
    Target &target =
            targets_[static_cast<std::size_t>(selectedIndex_)];
    const QVector3D proposed = target.center + delta;
    if (!IsFinite(proposed)) return false;
    target.center = proposed;
    schedulePersist();
    notifyTargetChanged(selectedIndex_);
    return true;
}

bool CuboidTargetModel::moveSelectedTo(double x, double y, double z) {
    if (!editingEnabled_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count()) {
        return false;
    }
    const QVector3D center(
            static_cast<float>(x),
            static_cast<float>(y),
            static_cast<float>(z));
    Target &target = targets_[static_cast<std::size_t>(selectedIndex_)];
    if (!IsFinite(center) || target.center == center) return false;
    target.center = center;
    schedulePersist();
    notifyTargetChanged(selectedIndex_);
    return true;
}

bool CuboidTargetModel::resizeSelected(const QString &axis, double delta) {
    if (!editingEnabled_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count() ||
        !std::isfinite(delta)) {
        return false;
    }
    const int component = AxisIndex(axis);
    if (component < 0) return false;
    Target &target =
            targets_[static_cast<std::size_t>(selectedIndex_)];
    const double proposed = static_cast<double>(target.size[component]) +
            delta;
    if (proposed < kMinimumSize || !std::isfinite(proposed) ||
        proposed > std::numeric_limits<float>::max()) {
        return false;
    }
    target.size[component] = static_cast<float>(proposed);
    schedulePersist();
    notifyTargetChanged(selectedIndex_);
    return true;
}

void CuboidTargetModel::setEditingEnabled(bool enabled) {
    if (editingEnabled_ == enabled) return;
    editingEnabled_ = enabled;
    emit editingEnabledChanged();
}

QVariantMap CuboidTargetModel::ToVariantMap(const Target &target,
                                            bool selected) {
    return QVariantMap{
            {QStringLiteral("id"), target.id},
            {QStringLiteral("name"), target.name},
            {QStringLiteral("center"), target.center},
            {QStringLiteral("size"), target.size},
            {QStringLiteral("centerX"), Number(target.center.x())},
            {QStringLiteral("centerY"), Number(target.center.y())},
            {QStringLiteral("centerZ"), Number(target.center.z())},
            {QStringLiteral("sizeX"), Number(target.size.x())},
            {QStringLiteral("sizeY"), Number(target.size.y())},
            {QStringLiteral("sizeZ"), Number(target.size.z())},
            {QStringLiteral("selected"), selected}};
}

bool CuboidTargetModel::ParseFinite(const QString &value, double *result) {
    bool okay = false;
    const double parsed = value.toDouble(&okay);
    if (!okay || !std::isfinite(parsed) ||
        std::abs(parsed) > std::numeric_limits<float>::max()) {
        return false;
    }
    *result = parsed;
    return true;
}

int CuboidTargetModel::AxisIndex(const QString &axis) {
    if (axis.compare(QStringLiteral("x"), Qt::CaseInsensitive) == 0) return 0;
    if (axis.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0) return 1;
    if (axis.compare(QStringLiteral("z"), Qt::CaseInsensitive) == 0) return 2;
    return -1;
}

QString CuboidTargetModel::nextDefaultName() const {
    int suffix = 1;
    for (;;) {
        const QString candidate = QStringLiteral("Cuboid %1").arg(suffix++);
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

void CuboidTargetModel::load(const QVariantMap &legacySettings) {
    const QByteArray encoded = QSettings()
            .value(QLatin1String(kCuboidTargetsKey))
            .toByteArray();
    const QJsonDocument document = QJsonDocument::fromJson(encoded);
    QString selectedId;
    if (document.isObject()) {
        const QJsonObject root = document.object();
        if (root.value(QStringLiteral("version")).toInt() ==
            kPersistenceVersion) {
            selectedId = root.value(QStringLiteral("selectedId")).toString();
            const QJsonArray array =
                    root.value(QStringLiteral("targets")).toArray();
            const qsizetype limit =
                    std::min(array.size(), kMaximumPersistedTargets);
            for (qsizetype index = 0; index < limit; ++index) {
                const QJsonObject object = array.at(index).toObject();
                const QString id = object.value(QStringLiteral("id"))
                                           .toString()
                                           .trimmed();
                const QString name = object.value(QStringLiteral("name"))
                                             .toString()
                                             .trimmed()
                                             .left(80);
                const QJsonArray centerArray =
                        object.value(QStringLiteral("center")).toArray();
                const QJsonArray sizeArray =
                        object.value(QStringLiteral("size")).toArray();
                if (id.isEmpty() || name.isEmpty() ||
                    centerArray.size() != 3 || sizeArray.size() != 3) {
                    continue;
                }
                const QVector3D center(
                        static_cast<float>(centerArray.at(0).toDouble(
                                std::numeric_limits<double>::quiet_NaN())),
                        static_cast<float>(centerArray.at(1).toDouble(
                                std::numeric_limits<double>::quiet_NaN())),
                        static_cast<float>(centerArray.at(2).toDouble(
                                std::numeric_limits<double>::quiet_NaN())));
                const QVector3D size(
                        static_cast<float>(sizeArray.at(0).toDouble(
                                std::numeric_limits<double>::quiet_NaN())),
                        static_cast<float>(sizeArray.at(1).toDouble(
                                std::numeric_limits<double>::quiet_NaN())),
                        static_cast<float>(sizeArray.at(2).toDouble(
                                std::numeric_limits<double>::quiet_NaN())));
                const bool duplicateId = std::any_of(
                        targets_.cbegin(),
                        targets_.cend(),
                        [&id](const Target &target) {
                            return target.id == id;
                        });
                if (!duplicateId && IsFinite(center) &&
                    HasPositiveComponents(size)) {
                    targets_.push_back(Target{id, name, center, size});
                }
            }
        }
    }

    if (targets_.empty()) {
        targets_.push_back(Target{
                QUuid::createUuid().toString(QUuid::WithoutBraces),
                QStringLiteral("Cuboid 1"),
                QVector3D(
                        static_cast<float>(SettingNumber(
                                legacySettings,
                                QStringLiteral("centerX"),
                                0.0)),
                        static_cast<float>(SettingNumber(
                                legacySettings,
                                QStringLiteral("centerY"),
                                0.0)),
                        static_cast<float>(SettingNumber(
                                legacySettings,
                                QStringLiteral("centerZ"),
                                0.0))),
                QVector3D(
                        static_cast<float>(SettingNumber(
                                legacySettings,
                                QStringLiteral("sizeX"),
                                10.0,
                                true)),
                        static_cast<float>(SettingNumber(
                                legacySettings,
                                QStringLiteral("sizeY"),
                                10.0,
                                true)),
                        static_cast<float>(SettingNumber(
                                legacySettings,
                                QStringLiteral("sizeZ"),
                                10.0,
                                true)))});
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

void CuboidTargetModel::persist() {
    persistenceTimer_.stop();
    QJsonArray values;
    for (const Target &target : targets_) {
        values.push_back(QJsonObject{
                {QStringLiteral("id"), target.id},
                {QStringLiteral("name"), target.name},
                {QStringLiteral("center"),
                 QJsonArray{
                         target.center.x(),
                         target.center.y(),
                         target.center.z()}},
                {QStringLiteral("size"),
                 QJsonArray{
                         target.size.x(),
                         target.size.y(),
                         target.size.z()}}});
    }
    const QString selectedId =
            selectedIndex_ >= 0 && selectedIndex_ < count()
            ? targets_[static_cast<std::size_t>(selectedIndex_)].id
            : QString{};
    QSettings().setValue(
            QLatin1String(kCuboidTargetsKey),
            QJsonDocument(QJsonObject{
                    {QStringLiteral("version"), kPersistenceVersion},
                    {QStringLiteral("selectedId"), selectedId},
                    {QStringLiteral("targets"), values}})
                    .toJson(QJsonDocument::Compact));
}

void CuboidTargetModel::schedulePersist() {
    persistenceTimer_.start();
}

void CuboidTargetModel::notifyTargetChanged(int index) {
    emit dataChanged(this->index(index), this->index(index));
    if (index == selectedIndex_) emit selectedTargetChanged();
}

}  // namespace forevertas::app
