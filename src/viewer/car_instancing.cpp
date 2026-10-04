#include "viewer/car_instancing.h"

namespace forevertas::viewer {

CarInstancing::CarInstancing(QQuick3DObject *parent)
    : QQuick3DInstancing(parent) {}

void CarInstancing::setInstances(const std::vector<Instance> &instances) {
    QByteArray table;
    table.resize(static_cast<qsizetype>(instances.size() * sizeof(InstanceTableEntry)));
    auto *entry = reinterpret_cast<InstanceTableEntry *>(table.data());
    bool transparent = false;
    for (const Instance &instance : instances) {
        entry->row0 = instance.transform.row(0);
        entry->row1 = instance.transform.row(1);
        entry->row2 = instance.transform.row(2);
        entry->color = QVector4D(static_cast<float>(instance.color.redF()),
                                 static_cast<float>(instance.color.greenF()),
                                 static_cast<float>(instance.color.blueF()),
                                 static_cast<float>(instance.color.alphaF()));
        entry->instanceData = {};
        transparent |= instance.color.alphaF() < 1.0;
        ++entry;
    }
    if (table == table_) return;
    table_ = std::move(table);
    count_ = static_cast<int>(instances.size());
    setHasTransparency(transparent);
    markDirty();
}

QByteArray CarInstancing::getInstanceBuffer(int *instanceCount) {
    if (instanceCount != nullptr) *instanceCount = count_;
    return table_;
}

}  // namespace forevertas::viewer
