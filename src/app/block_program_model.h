#ifndef FOREVERTAS_APP_BLOCK_PROGRAM_MODEL_H
#define FOREVERTAS_APP_BLOCK_PROGRAM_MODEL_H

#include "blocks/block_compiler.h"
#include "blocks/block_program.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <map>
#include <optional>
#include <string>

class QSettings;

namespace forevertas::app {

struct BlockConfigurationValidation {
    std::optional<blocks::SearchComponentConfiguration> configuration;
    QString error;
};

// QML-facing owner of the placed-block program. Applies the workspace
// policy (one script: one search block, one evaluation slot, an ordered
// stack of mutation windows each holding ordered mutation atoms), persists
// the program as JSON, migrates legacy per-option configuration, and
// compiles for validation and search.
class BlockProgramModel final : public QObject {
    Q_OBJECT

public:
    explicit BlockProgramModel(QObject *parent = nullptr);

    // Rendering views.
    QVariantList palette() const;
    QVariantMap scriptSummary() const;
    QVariantList blockCanvas() const;
    Q_INVOKABLE QVariantMap blockData(int blockId) const;

    // Editing. Structural operations keep the single-script policy.
    Q_INVOKABLE bool addBlock(const QString &definitionId);
    Q_INVOKABLE bool removeBlock(int blockId);
    Q_INVOKABLE bool setBlockField(int blockId,
                                   const QString &key,
                                   const QString &value);
    Q_INVOKABLE int attachReporter(int blockId,
                                   const QString &key,
                                   const QString &reporterDefinitionId);
    Q_INVOKABLE bool detachReporter(int blockId, const QString &key);
    Q_INVOKABLE bool moveBlock(int blockId, int toIndex);
    Q_INVOKABLE bool setBlockPosition(int blockId, double x, double y);
    Q_INVOKABLE bool setEvaluator(const QString &definitionId);
    Q_INVOKABLE void resetToDefault();

    // Canvas editing. Drag-and-drop builds on these: every block can live
    // loose on the canvas at any position and snaps into the script's
    // stacks or number slots. `attachBlock`'s index refers to the parent's
    // substack after the child is detached from its current parent.
    Q_INVOKABLE int addLooseBlock(const QString &definitionId,
                                  double x,
                                  double y);
    Q_INVOKABLE bool attachBlock(int parentId, int index, int childId);
    Q_INVOKABLE bool detachBlockToCanvas(int blockId, double x, double y);
    Q_INVOKABLE bool graftReporterBlock(int blockId,
                                        const QString &key,
                                        int reporterId,
                                        double x,
                                        double y);
    Q_INVOKABLE bool detachReporterToCanvas(int blockId,
                                            const QString &key,
                                            double x,
                                            double y);
    Q_INVOKABLE bool setEvaluatorBlockId(int blockId);

    // Text interchange for tests and interchange with other tools.
    QString programText() const;
    bool setProgramText(const QString &text, QString *error = nullptr);

    // Replaces every seed field in the script with a fresh deterministic
    // value, mirroring the legacy seed randomization order.
    bool randomizeSeeds(std::uint32_t entropy);

    // Derived option ids used by viewer interactions.
    QString searchAlgorithmId() const;
    QString evaluationTargetId() const;

    // Legacy settings access used to seed the target collections.
    QVariantMap legacyEvaluationSettings(const QString &optionId) const;

    BlockConfigurationValidation validate(std::uint32_t tickDurationMs,
                                          std::uint32_t simulationHorizonMs)
            const;

    // Bridge between the compositional v3 editor and the existing optimized
    // runtime program. The editor compiles to these category-neutral
    // components, then this model rebuilds its compatibility program so every
    // existing search/target/viewer path observes exactly the same settings.
    std::optional<blocks::SearchComponentConfiguration> compiledComponents()
            const;
    bool replaceWithComponents(
            const blocks::SearchComponentConfiguration &components);

    const blocks::BlockProgram &program() const { return program_; }

signals:
    void structureChanged();
    void blockChanged(int blockId);

private:
    void load();
    void persist() const;
    void buildDefault();
    bool migrateLegacy(QSettings &storage);
    blocks::BlockId scriptHat() const;
    void rememberFields(const blocks::BlockNode &node);
    std::map<std::string, std::string> rememberedOr(
            const std::string &definitionId) const;
    blocks::BlockId replaceOptionBlock(blocks::BlockId existingId,
                                      const std::string &definitionId);

    blocks::BlockProgram program_;
    std::map<std::string, std::map<std::string, std::string>> remembered_;
};

}  // namespace forevertas::app

#endif
