#include "app/block_program_model.h"

#include "app/option_settings_store.h"
#include "blocks/block_catalog.h"
#include "blocks/block_lowering.h"
#include "blocks/block_program_io.h"
#include "searches/algorithm_registry.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include <random>
#include <set>
#include <utility>


namespace forevertas::app {
namespace {

constexpr char kProgramKey[] = "blocks/program";
constexpr char kProgramCorruptBackupKey[] = "blocks/program.corrupt";
constexpr char kSearchAlgorithmKey[] = "selection/searchAlgorithm";
constexpr char kLegacyMutationAlgorithmKey[] = "selection/mutationAlgorithm";
constexpr char kModifierPassesKey[] = "composition/modifiers";
constexpr char kEvaluationTargetKey[] = "selection/evaluationTarget";

// Resolves option aliases ("evaluate/finish-time", registry legacy ids) to
// their canonical atom definition ids.
std::string CanonicalBlockDefinition(const std::string &candidate) {
    if (blocks::FindBlock(candidate) != nullptr) return candidate;
    const auto separator = candidate.find('/');
    if (separator == std::string::npos) return std::string();
    const std::string category = candidate.substr(0, separator);
    const std::string optionId = candidate.substr(separator + 1);
    if (category == "evaluate") {
        return blocks::EvaluationAtomDefinitionForOption(optionId);
    }
    if (category == "search") {
        return blocks::SearchAtomDefinitionForOption(optionId);
    }
    return std::string();
}


QString FromStd(const std::string &text) {
    return QString::fromStdString(text);
}

std::string ShapeName(blocks::BlockShape shape) {
    switch (shape) {
    case blocks::BlockShape::Hat: return "hat";
    case blocks::BlockShape::Container: return "container";
    case blocks::BlockShape::Stack: return "stack";
    case blocks::BlockShape::Reporter: return "reporter";
    }
    return std::string();
}

std::string FieldKindName(OptionField::Kind kind) {
    switch (kind) {
    case OptionField::Kind::Number: return "number";
    case OptionField::Kind::Line: return "line";
    case OptionField::Kind::Enum: return "enum";
    case OptionField::Kind::Boolean: return "boolean";
    case OptionField::Kind::Mirrored: return "mirrored";
    }
    return std::string();
}

QVariantMap FieldData(const blocks::BlockProgram &program,
                      const blocks::BlockNode &node,
                      const OptionField &field) {
    QVariantMap data;
    data.insert(QStringLiteral("key"), FromStd(field.key));
    data.insert(QStringLiteral("label"), FromStd(field.label));
    data.insert(QStringLiteral("kind"),
                QString::fromStdString(FieldKindName(field.kind)));
    data.insert(QStringLiteral("group"), FromStd(field.group));
    data.insert(QStringLiteral("isSeed"), field.isSeed);
    data.insert(QStringLiteral("decimals"), field.decimals);
    data.insert(QStringLiteral("step"), field.step);
    data.insert(QStringLiteral("hasRange"), field.hasRange);
    data.insert(QStringLiteral("minimum"), field.minimum);
    data.insert(QStringLiteral("maximum"), field.maximum);
    QVariantList enumValues;
    for (const auto &[value, label] : field.enumValues) {
        enumValues.push_back(QVariantMap{
                {QStringLiteral("value"), FromStd(value)},
                {QStringLiteral("label"), FromStd(label)}});
    }
    data.insert(QStringLiteral("enumValues"), enumValues);
    const auto literal = node.fields.find(field.key);
    data.insert(QStringLiteral("value"),
                literal == node.fields.end()
                        ? FromStd(field.defaultValue)
                        : FromStd(literal->second));
    const auto reporter = node.reporters.find(field.key);
    if (reporter != node.reporters.end()) {
        const blocks::BlockNode *const reporterNode =
                program.find(reporter->second);
        if (reporterNode != nullptr) {
            const blocks::BlockDefinition *const reporterDefinition =
                    blocks::FindBlock(reporterNode->definitionId);
            if (reporterDefinition != nullptr) {
                QVariantMap chip;
                chip.insert(QStringLiteral("blockId"),
                            static_cast<int>(reporter->second));
                chip.insert(QStringLiteral("definitionId"),
                            FromStd(reporterNode->definitionId));
                chip.insert(QStringLiteral("label"),
                            FromStd(reporterDefinition->label));
                QVariantList chipFields;
                for (const OptionField &chipField :
                     reporterDefinition->fields) {
                    chipFields.push_back(FieldData(
                            program, *reporterNode, chipField));
                }
                chip.insert(QStringLiteral("fields"), chipFields);
                data.insert(QStringLiteral("reporter"), chip);
            }
        }
    }
    return data;
}

void RemoveRetiredSearchBudget() {
    const QString retiredKey = QString::fromLatin1(
            QByteArray::fromHex("617474656d7074436f756e74"));
    QSettings storage;
    storage.remove(QStringLiteral("search/") + retiredKey);
    storage.remove(QStringLiteral("configuration/search/basic-brute-force/") +
                   retiredKey);
    storage.remove(QStringLiteral("configuration/search/serial-brute-force/") +
                   retiredKey);
    storage.remove(QStringLiteral("search/minEvalTimeMs"));
    storage.remove(QStringLiteral("search/maxEvalTimeMs"));
}

bool IsContainerNode(const blocks::BlockProgram &program,
                     blocks::BlockId id) {
    const blocks::BlockNode *const node = program.find(id);
    if (node == nullptr) return false;
    const blocks::BlockDefinition *const definition =
            blocks::FindBlock(node->definitionId);
    return definition != nullptr &&
            definition->shape == blocks::BlockShape::Container;
}

// True when `candidate` lives anywhere inside `root`'s subtree (but is not
// root itself); guards grafts against reference cycles.
bool IsWithinSubtree(const blocks::BlockProgram &program,
                     blocks::BlockId root,
                     blocks::BlockId candidate) {
    std::set<blocks::BlockId> visited;
    std::vector<blocks::BlockId> pending{root};
    while (!pending.empty()) {
        const blocks::BlockId current = pending.back();
        pending.pop_back();
        if (!visited.insert(current).second) continue;
        if (current == candidate && current != root) return true;
        const blocks::BlockNode *const node = program.find(current);
        if (node == nullptr) continue;
        for (const blocks::BlockId child : node->substack) {
            pending.push_back(child);
        }
        for (const auto &[key, child] : node->reporters) {
            pending.push_back(child);
        }
        if (node->evaluator != 0) pending.push_back(node->evaluator);
    }
    return false;
}

}  // namespace

BlockProgramModel::BlockProgramModel(QObject *parent) : QObject(parent) {
    load();
}

void BlockProgramModel::load() {
    RemoveRetiredSearchBudget();
    QSettings storage;
    const QByteArray stored =
            storage.value(QLatin1String(kProgramKey)).toByteArray();
    if (!stored.isEmpty()) {
        const blocks::BlockProgramJson parsed =
                blocks::ParseBlockProgramJson(stored.toStdString());
        if (parsed.program) {
            program_ = std::move(*parsed.program);
            remembered_ = parsed.remembered;
            return;
        }
        // Keep the unreadable program under a backup key instead of
        // silently replacing the user's work with defaults.
        storage.setValue(QLatin1String(kProgramCorruptBackupKey),
                         stored);
        qWarning("ForeverTAS: the stored block program could not be "
                 "parsed; it was backed up and defaults were loaded");
    }
    if (migrateLegacy(storage)) {
        persist();
        return;
    }
    buildDefault();
    persist();
}

bool BlockProgramModel::migrateLegacy(QSettings &storage) {
    const OptionConfiguration defaultSearch =
            DefaultSearchAlgorithmConfiguration();
    const OptionConfiguration defaultEvaluation =
            DefaultEvaluationTargetConfiguration();

    QString searchId = storage.value(QLatin1String(kSearchAlgorithmKey))
                               .toString();
    if (searchId.isEmpty()) searchId = FromStd(defaultSearch.id);
    const SearchAlgorithmRegistration *searchRegistration =
            FindSearchAlgorithm(searchId.toStdString());
    if (searchRegistration == nullptr) {
        searchRegistration = FindSearchAlgorithm(defaultSearch.id);
    }

    QString evaluationId = storage.value(QLatin1String(kEvaluationTargetKey))
                                   .toString();
    if (evaluationId.isEmpty()) {
        evaluationId = FromStd(defaultEvaluation.id);
    }
    const EvaluationTargetRegistration *evaluationRegistration =
            FindEvaluationTarget(evaluationId.toStdString());
    if (evaluationRegistration == nullptr) {
        evaluationRegistration =
                FindEvaluationTarget(defaultEvaluation.id);
    }
    if (searchRegistration == nullptr || evaluationRegistration == nullptr) {
        return false;
    }

    blocks::SearchComponentConfiguration components;
    components.searchAlgorithm = OptionConfiguration{
            searchRegistration->id,
            ToOptionSettings(LoadPersistedOptionSettings(
                    QStringLiteral("search"), *searchRegistration))};
    components.evaluationTarget = OptionConfiguration{
            evaluationRegistration->id,
            ToOptionSettings(LoadPersistedOptionSettings(
                    QStringLiteral("evaluation"), *evaluationRegistration))};

    const QJsonDocument document = QJsonDocument::fromJson(
            storage.value(QLatin1String(kModifierPassesKey)).toByteArray());
    if (document.isArray()) {
        for (const QJsonValue &value : document.array()) {
            if (!value.isObject()) continue;
            const QJsonObject object = value.toObject();
            const ModifierRegistration *const registration = FindModifier(
                    object.value(QStringLiteral("id"))
                            .toString()
                            .toStdString());
            if (registration == nullptr) continue;
            OptionSettings settings = registration->defaultSettings;
            const QJsonObject storedSettings =
                    object.value(QStringLiteral("settings")).toObject();
            for (auto iterator = storedSettings.constBegin();
                 iterator != storedSettings.constEnd();
                 ++iterator) {
                const std::string key = iterator.key().toStdString();
                settings.erase(key);
                settings.emplace(key,
                                 iterator.value().toString().toStdString());
            }
            components.modifiers.push_back(
                    OptionConfiguration{registration->id, settings});
        }
    }
    if (components.modifiers.empty()) {
        const ModifierRegistration &fallback = ModifierRegistry().front();
        const QString legacyId = storage
                .value(QLatin1String(kLegacyMutationAlgorithmKey))
                .toString();
        const ModifierRegistration *registration =
                FindModifier(legacyId.toStdString());
        if (registration == nullptr) registration = &fallback;
        components.modifiers.push_back(OptionConfiguration{
                registration->id,
                ToOptionSettings(LoadPersistedOptionSettings(
                        QStringLiteral("mutation"), *registration))});
    }

    program_ = blocks::BuildProgramFromComponents(components);
    return true;
}

void BlockProgramModel::buildDefault() {
    program_ = blocks::BuildProgramFromComponents(
            blocks::SearchComponentConfiguration{
                    DefaultSearchAlgorithmConfiguration(),
                    DefaultModifierConfigurations(),
                    DefaultEvaluationTargetConfiguration()});
}

void BlockProgramModel::persist() const {
    QSettings().setValue(
            QLatin1String(kProgramKey),
            QByteArray::fromStdString(
                    blocks::PrintBlockProgramJson(program_, remembered_)));
}

blocks::BlockId BlockProgramModel::scriptHat() const {
    if (!program_.script()) return 0;
    const blocks::BlockNode *const hat =
            program_.find(*program_.script());
    return hat == nullptr ? 0 : hat->id;
}

QVariantList BlockProgramModel::palette() const {
    QVariantList categories;
    for (const blocks::BlockCategory &category :
         blocks::BlockCategories()) {
        QVariantList paletteBlocks;
        for (const blocks::BlockDefinition &definition :
             blocks::BlockCatalog()) {
            if (definition.categoryId != category.id) continue;
            paletteBlocks.push_back(QVariantMap{
                    {QStringLiteral("id"), FromStd(definition.id)},
                    {QStringLiteral("label"), FromStd(definition.label)},
                    {QStringLiteral("shape"),
                     QString::fromStdString(
                             ShapeName(definition.shape))},
                    {QStringLiteral("optionKind"),
                     FromStd(definition.optionKind)}});
        }
        categories.push_back(QVariantMap{
                {QStringLiteral("id"), FromStd(category.id)},
                {QStringLiteral("label"), FromStd(category.label)},
                {QStringLiteral("color"), FromStd(category.color)},
                {QStringLiteral("blocks"), paletteBlocks}});
    }
    return categories;
}

QVariantMap BlockProgramModel::scriptSummary() const {
    QVariantMap summary;
    const blocks::BlockId hatId = scriptHat();
    summary.insert(QStringLiteral("hat"), static_cast<int>(hatId));
    QVariantList groups;
    if (hatId != 0) {
        const blocks::BlockNode *const hat = program_.find(hatId);
        summary.insert(QStringLiteral("evaluator"),
                       static_cast<int>(hat->evaluator));
        for (const blocks::BlockId windowId : hat->substack) {
            const blocks::BlockNode *const window =
                    program_.find(windowId);
            if (window == nullptr) continue;
            QVariantList atoms;
            for (const blocks::BlockId atomId : window->substack) {
                atoms.push_back(static_cast<int>(atomId));
            }
            groups.push_back(QVariantMap{
                    {QStringLiteral("blockId"),
                     static_cast<int>(windowId)},
                    {QStringLiteral("atoms"), atoms}});
        }
    }
    summary.insert(QStringLiteral("groups"), groups);
    return summary;
}

QVariantList BlockProgramModel::blockCanvas() const {
    QVariantList entries;
    const blocks::BlockId hatId = scriptHat();
    if (hatId != 0) {
        const blocks::BlockNode *const hat = program_.find(hatId);
        if (hat != nullptr) {
            entries.push_back(QVariantMap{
                    {QStringLiteral("blockId"), static_cast<int>(hatId)},
                    {QStringLiteral("x"), hat->x},
                    {QStringLiteral("y"), hat->y},
                    {QStringLiteral("isScript"), true}});
        }
    }
    for (const blocks::BlockId id : program_.topLevel()) {
        if (id == hatId) continue;
        const blocks::BlockNode *const node = program_.find(id);
        if (node == nullptr) continue;
        entries.push_back(QVariantMap{
                {QStringLiteral("blockId"), static_cast<int>(id)},
                {QStringLiteral("x"), node->x},
                {QStringLiteral("y"), node->y},
                {QStringLiteral("isScript"), false}});
    }
    return entries;
}

QVariantMap BlockProgramModel::blockData(int blockId) const {
    QVariantMap data;
    const blocks::BlockNode *const node =
            program_.find(static_cast<blocks::BlockId>(blockId));
    if (node == nullptr) return data;
    const blocks::BlockDefinition *const definition =
            blocks::FindBlock(node->definitionId);
    if (definition == nullptr) return data;
    const blocks::BlockCategory *const category =
            blocks::FindBlockCategory(definition->categoryId);
    data.insert(QStringLiteral("blockId"), blockId);
    data.insert(QStringLiteral("definitionId"), FromStd(node->definitionId));
    data.insert(QStringLiteral("optionId"), FromStd(definition->optionId));
    data.insert(QStringLiteral("label"), FromStd(definition->label));
    data.insert(
            QStringLiteral("shape"),
            QString::fromStdString(ShapeName(definition->shape)));
    data.insert(QStringLiteral("optionKind"),
                FromStd(definition->optionKind));
    data.insert(QStringLiteral("category"), FromStd(definition->categoryId));
    data.insert(QStringLiteral("color"),
                category == nullptr ? QStringLiteral("#64748b")
                                    : FromStd(category->color));
    data.insert(QStringLiteral("detail"),
                FromStd(definition->settingsComponent));
    QVariantList fields;
    for (const OptionField &field : definition->fields) {
        fields.push_back(FieldData(program_, *node, field));
    }
    data.insert(QStringLiteral("fields"), fields);
    data.insert(QStringLiteral("x"), node->x);
    data.insert(QStringLiteral("y"), node->y);
    QVariantList substack;
    for (const blocks::BlockId childId : node->substack) {
        substack.push_back(static_cast<int>(childId));
    }
    data.insert(QStringLiteral("substack"), substack);
    data.insert(QStringLiteral("evaluator"),
                static_cast<int>(node->evaluator));
    return data;
}

bool BlockProgramModel::addBlock(const QString &definitionId) {
    const std::string id =
            CanonicalBlockDefinition(definitionId.toStdString());
    if (id.empty()) return false;
    const blocks::BlockDefinition *const definition = blocks::FindBlock(id);
    if (definition == nullptr) return false;
    if (definition->shape == blocks::BlockShape::Hat) {
        const blocks::BlockId hatId = scriptHat();
        if (hatId != 0 &&
            program_.find(hatId)->definitionId == id) {
            return false;
        }
        replaceOptionBlock(hatId, id);
        persist();
        emit structureChanged();
        return true;
    }
    if (definition->optionKind == "evaluation") {
        return setEvaluator(definitionId);
    }
    const blocks::BlockId hatId = scriptHat();
    if (hatId == 0) return false;
    if (definition->shape == blocks::BlockShape::Container) {
        const blocks::BlockId created = program_.createBlock(id);
        program_.appendToSubstack(hatId, created);
        persist();
        emit structureChanged();
        return true;
    }
    if (definition->optionKind == "mutation") {
        // Mutation atoms snap into the last window; clicking an atom with
        // no window present creates one to hold it.
        const blocks::BlockNode *const hat = program_.find(hatId);
        blocks::BlockId groupId = 0;
        if (!hat->substack.empty() &&
            IsContainerNode(program_, hat->substack.back())) {
            groupId = hat->substack.back();
        } else {
            groupId = program_.createBlock("mutate/window");
            program_.appendToSubstack(hatId, groupId);
        }
        const blocks::BlockId created = program_.createBlock(id);
        program_.appendToSubstack(groupId, created);
        persist();
        emit structureChanged();
        return true;
    }
    // Value reporters are placed on the canvas for later slot attachment.
    program_.createBlock(id);
    persist();
    emit structureChanged();
    return true;
}

bool BlockProgramModel::removeBlock(int blockId) {
    const blocks::BlockId id = static_cast<blocks::BlockId>(blockId);
    if (id == scriptHat()) return false;
    if (program_.find(id) == nullptr) return false;
    if (!program_.removeBlock(id)) return false;
    persist();
    emit structureChanged();
    return true;
}

void BlockProgramModel::rememberFields(const blocks::BlockNode &node) {
    const blocks::BlockDefinition *const definition =
            blocks::FindBlock(node.definitionId);
    if (definition == nullptr || definition->optionKind.empty()) return;
    remembered_[node.definitionId] = node.fields;
}

std::map<std::string, std::string> BlockProgramModel::rememberedOr(
        const std::string &definitionId) const {
    const blocks::BlockDefinition *const definition =
            blocks::FindBlock(definitionId);
    if (definition == nullptr) return {};
    std::map<std::string, std::string> values;
    for (const OptionField &field : definition->fields) {
        values.emplace(field.key, field.defaultValue);
    }
    const auto remembered = remembered_.find(definitionId);
    if (remembered == remembered_.end()) return values;
    for (const auto &[key, value] : remembered->second) {
        values.erase(key);
        values.emplace(key, value);
    }
    return values;
}

blocks::BlockId BlockProgramModel::replaceOptionBlock(
        blocks::BlockId existingId,
        const std::string &definitionId) {
    const blocks::BlockDefinition *const definition =
            blocks::FindBlock(definitionId);
    if (definition == nullptr) return 0;
    std::string oldSlot;
    const blocks::BlockNode *const existing = program_.find(existingId);
    if (existing != nullptr) {
        oldSlot = existing->definitionId;
        rememberFields(*existing);
    }
    const blocks::BlockId created = program_.createBlock(
            definitionId, rememberedOr(definitionId));
    if (definition->shape == blocks::BlockShape::Hat) {
        if (existing != nullptr) {
            blocks::BlockNode *const hat = program_.find(created);
            hat->substack = existing->substack;
            hat->evaluator = existing->evaluator;
        }
        program_.setScript(created);
        if (existing != nullptr) {
            program_.removeBlock(existingId);
        }
    }
    if (!oldSlot.empty() && oldSlot != definitionId) {
        remembered_.erase(oldSlot);
    }
    return created;
}

bool BlockProgramModel::setBlockField(int blockId,
                                      const QString &key,
                                      const QString &value) {
    blocks::BlockNode *const node =
            program_.find(static_cast<blocks::BlockId>(blockId));
    if (node == nullptr) return false;
    if (!program_.setFieldValue(
                static_cast<blocks::BlockId>(blockId),
                key.toStdString(),
                value.toStdString())) {
        return false;
    }
    if (node->id == scriptHat() ||
        (program_.script() &&
         node->id == program_.find(*program_.script())->evaluator)) {
        rememberFields(*node);
    }
    persist();
    emit blockChanged(blockId);
    return true;
}

int BlockProgramModel::attachReporter(int blockId,
                                      const QString &key,
                                      const QString &reporterDefinitionId) {
    const blocks::BlockId created = program_.attachReporter(
            static_cast<blocks::BlockId>(blockId),
            key.toStdString(),
            reporterDefinitionId.toStdString());
    if (created == 0) return 0;
    persist();
    emit blockChanged(blockId);
    return static_cast<int>(created);
}

bool BlockProgramModel::detachReporter(int blockId, const QString &key) {
    if (!program_.detachReporter(static_cast<blocks::BlockId>(blockId),
                                 key.toStdString())) {
        return false;
    }
    persist();
    emit blockChanged(blockId);
    return true;
}

bool BlockProgramModel::moveBlock(int blockId, int toIndex) {
    if (toIndex < 0) return false;
    if (!program_.moveWithinSubstack(static_cast<blocks::BlockId>(blockId),
                                     static_cast<std::size_t>(toIndex))) {
        return false;
    }
    persist();
    emit structureChanged();
    return true;
}

bool BlockProgramModel::setBlockPosition(int blockId, double x, double y) {
    blocks::BlockNode *const node =
            program_.find(static_cast<blocks::BlockId>(blockId));
    if (node == nullptr) return false;
    node->x = x;
    node->y = y;
    persist();
    // Positions live on the canvas summary, so re-read the structure.
    emit structureChanged();
    return true;
}

int BlockProgramModel::addLooseBlock(const QString &definitionId,
                                     double x,
                                     double y) {
    const std::string id =
            CanonicalBlockDefinition(definitionId.toStdString());
    if (id.empty()) return 0;
    const blocks::BlockDefinition *const definition = blocks::FindBlock(id);
    if (definition == nullptr) return 0;
    if (definition->shape == blocks::BlockShape::Hat) {
        // Exactly one script exists; a dropped hat replaces it like a click.
        return addBlock(definitionId) ? static_cast<int>(scriptHat()) : 0;
    }
    const blocks::BlockId created = program_.createBlock(
            id,
            definition->optionKind.empty() ? std::map<std::string, std::string>{}
                                           : rememberedOr(id));
    if (created == 0) return 0;
    blocks::BlockNode *const node = program_.find(created);
    node->x = x;
    node->y = y;
    persist();
    emit structureChanged();
    return static_cast<int>(created);
}

bool BlockProgramModel::attachBlock(int parentId, int index, int childId) {
    if (index < 0) return false;
    const blocks::BlockNode *const parent =
            program_.find(static_cast<blocks::BlockId>(parentId));
    blocks::BlockNode *const child =
            program_.find(static_cast<blocks::BlockId>(childId));
    if (parent == nullptr || child == nullptr || parent == child) {
        return false;
    }
    const blocks::BlockDefinition *const parentDefinition =
            blocks::FindBlock(parent->definitionId);
    const blocks::BlockDefinition *const childDefinition =
            blocks::FindBlock(child->definitionId);
    if (parentDefinition == nullptr || childDefinition == nullptr) {
        return false;
    }
    if (static_cast<blocks::BlockId>(childId) == scriptHat()) return false;
    // Substacks follow the compile rules: windows under the hat, mutation
    // atoms under windows, nothing else.
    if (parentDefinition->shape == blocks::BlockShape::Hat) {
        if (childDefinition->shape != blocks::BlockShape::Container) {
            return false;
        }
    } else if (parentDefinition->shape == blocks::BlockShape::Container) {
        if (childDefinition->shape != blocks::BlockShape::Stack ||
            childDefinition->optionKind != "mutation") {
            return false;
        }
    } else {
        return false;
    }
    if (IsWithinSubtree(program_,
                        static_cast<blocks::BlockId>(childId),
                        static_cast<blocks::BlockId>(parentId))) {
        return false;
    }
    if (!program_.insertInSubstack(static_cast<blocks::BlockId>(parentId),
                                   static_cast<std::size_t>(index),
                                   static_cast<blocks::BlockId>(childId))) {
        return false;
    }
    child->x = 0.0;
    child->y = 0.0;
    persist();
    emit structureChanged();
    return true;
}

bool BlockProgramModel::detachBlockToCanvas(int blockId, double x, double y) {
    const blocks::BlockId id = static_cast<blocks::BlockId>(blockId);
    if (id == scriptHat()) return false;
    blocks::BlockNode *const node = program_.find(id);
    if (node == nullptr) return false;
    if (!program_.makeTopLevel(id)) return false;
    node->x = x;
    node->y = y;
    persist();
    emit structureChanged();
    return true;
}

bool BlockProgramModel::graftReporterBlock(int blockId,
                                           const QString &key,
                                           int reporterId) {
    const blocks::BlockId ownerId = static_cast<blocks::BlockId>(blockId);
    const blocks::BlockId graftId = static_cast<blocks::BlockId>(reporterId);
    if (ownerId == graftId) return false;
    if (IsWithinSubtree(program_, graftId, ownerId)) return false;
    blocks::BlockNode *const reporter = program_.find(graftId);
    if (reporter == nullptr) return false;
    if (!program_.graftReporter(ownerId, key.toStdString(), graftId)) {
        return false;
    }
    reporter->x = 0.0;
    reporter->y = 0.0;
    persist();
    // The reporter may have moved from the canvas or another slot, so the
    // whole structure is re-read.
    emit structureChanged();
    return true;
}

bool BlockProgramModel::setEvaluatorBlockId(int blockId) {
    const blocks::BlockId hatId = scriptHat();
    if (hatId == 0) return false;
    blocks::BlockNode *const node =
            program_.find(static_cast<blocks::BlockId>(blockId));
    if (node == nullptr) return false;
    const blocks::BlockDefinition *const definition =
            blocks::FindBlock(node->definitionId);
    if (definition == nullptr || definition->optionKind != "evaluation") {
        return false;
    }
    const blocks::BlockNode *const hat = program_.find(hatId);
    if (hat->evaluator == node->id) return false;
    if (hat->evaluator != 0) {
        const blocks::BlockNode *const current =
                program_.find(hat->evaluator);
        if (current != nullptr) rememberFields(*current);
    }
    if (!program_.setEvaluator(hatId, node->id)) return false;
    node->x = 0.0;
    node->y = 0.0;
    persist();
    emit structureChanged();
    return true;
}

bool BlockProgramModel::setEvaluator(const QString &definitionId) {
    const std::string id =
            CanonicalBlockDefinition(definitionId.toStdString());
    const blocks::BlockDefinition *const definition =
            id.empty() ? nullptr : blocks::FindBlock(id);
    if (definition == nullptr || definition->optionKind != "evaluation") {
        return false;
    }
    const blocks::BlockId hatId = scriptHat();
    if (hatId == 0) return false;
    const blocks::BlockNode *const hat = program_.find(hatId);
    if (hat->evaluator != 0) {
        const blocks::BlockNode *const current =
                program_.find(hat->evaluator);
        if (current != nullptr && current->definitionId == id) {
            return false;
        }
    }
    const blocks::BlockId created = replaceOptionBlock(
            hat->evaluator, id);
    if (created == 0) return false;
    program_.setEvaluator(hatId, created);
    persist();
    emit structureChanged();
    return true;
}

void BlockProgramModel::resetToDefault() {
    buildDefault();
    persist();
    emit structureChanged();
}

QString BlockProgramModel::programText() const {
    return QString::fromStdString(blocks::PrintBlockProgramText(program_));
}

bool BlockProgramModel::setProgramText(const QString &text, QString *error) {
    const blocks::BlockProgramText parsed =
            blocks::ParseBlockProgramText(text.toStdString());
    if (!parsed.program) {
        if (error != nullptr) {
            *error = QString::fromStdString(parsed.error);
        }
        return false;
    }
    program_ = std::move(*parsed.program);
    persist();
    emit structureChanged();
    return true;
}

bool BlockProgramModel::randomizeSeeds(std::uint32_t entropy) {
    std::mt19937 random(entropy);
    bool changed = false;
    const blocks::BlockId hatId = scriptHat();
    if (hatId == 0) return false;
    blocks::BlockNode *const hat = program_.find(hatId);
    // Seeds live on the mutation windows, in substack order.
    std::vector<blocks::BlockId> order = hat->substack;
    for (const blocks::BlockId blockId : order) {
        const blocks::BlockNode *const node = program_.find(blockId);
        if (node == nullptr) continue;
        const blocks::BlockDefinition *const definition =
                blocks::FindBlock(node->definitionId);
        if (definition == nullptr) continue;
        for (const OptionField &field : definition->fields) {
            if (!field.isSeed) continue;
            std::uint32_t generated = random();
            const auto current = node->fields.find(field.key);
            if (current != node->fields.end()) {
                // Compare numerically: "42" and "0042" are the same seed.
                bool storedParsed = false;
                const quint64 storedSeed =
                        QString::fromStdString(current->second)
                                .toULongLong(&storedParsed);
                if (storedParsed && storedSeed == generated) {
                    ++generated;
                }
            }
            changed |= program_.setFieldValue(
                    blockId,
                    field.key,
                    std::to_string(generated));
        }
    }
    if (changed) {
        persist();
        for (const blocks::BlockId blockId : hat->substack) {
            emit blockChanged(static_cast<int>(blockId));
        }
    }
    return changed;
}

QString BlockProgramModel::searchAlgorithmId() const {
    const blocks::BlockNode *const hat =
            program_.find(scriptHat());
    if (hat == nullptr) return {};
    const blocks::BlockDefinition *const definition =
            blocks::FindBlock(hat->definitionId);
    return definition == nullptr ? QString()
                                 : FromStd(definition->optionId);
}

QString BlockProgramModel::evaluationTargetId() const {
    const blocks::BlockNode *const hat =
            program_.find(scriptHat());
    if (hat == nullptr || hat->evaluator == 0) return {};
    const blocks::BlockNode *const evaluator =
            program_.find(hat->evaluator);
    if (evaluator == nullptr) return {};
    const blocks::BlockDefinition *const definition =
            blocks::FindBlock(evaluator->definitionId);
    return definition == nullptr ? QString()
                                 : FromStd(definition->optionId);
}

QVariantMap BlockProgramModel::legacyEvaluationSettings(
        const QString &optionId) const {
    const EvaluationTargetRegistration *const registration =
            FindEvaluationTarget(optionId.toStdString());
    if (registration == nullptr) return {};
    return LoadPersistedOptionSettings(
            QStringLiteral("evaluation"), *registration);
}

BlockConfigurationValidation BlockProgramModel::validate(
        std::uint32_t tickDurationMs,
        std::uint32_t simulationHorizonMs) const {
    BlockConfigurationValidation result;
    const blocks::CompileResult compiled =
            blocks::CompileProgram(program_);
    if (!compiled.ok) {
        QString joined;
        for (const std::string &error : compiled.errors) {
            if (!joined.isEmpty()) joined += QLatin1Char('\n');
            joined += QString::fromStdString(error);
        }
        result.error = joined;
        return result;
    }
    if (const auto error = blocks::ValidateSearchComponents(
                compiled.configuration, tickDurationMs,
                simulationHorizonMs)) {
        result.error = QString::fromStdString(*error);
        return result;
    }
    result.configuration = compiled.configuration;
    return result;
}

}  // namespace forevertas::app
