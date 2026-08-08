#include "app/condition_editor_model.h"
#include "app/script_file_store.h"

#include <QCoreApplication>
#include <QString>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QVariant>

#include <iostream>

namespace {

bool Check(bool condition, const char *message) {
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

QVariantMap FindBySymbol(const QVariantList &items, const QString &symbol) {
    for (const QVariant &item : items) {
        const QVariantMap map = item.toMap();
        if (map.value(QStringLiteral("symbol")).toString() == symbol) {
            return map;
        }
    }
    return {};
}

bool TestAnalysis() {
    forevertas::app::ConditionEditorModel model;
    bool okay = Check(model.valid() && model.gateCount() == 0,
                      "blank editor model is not an eligible-tick state");

    model.setSource(
            QStringLiteral("unknown = 1\r\niterations >\r\ncar.speed = 0"));
    okay &= Check(!model.valid() && model.gateCount() == 3 &&
                          model.diagnostics().size() == 2 &&
                          model.lineStates().size() == 3,
                  "editor analysis did not retain all line states");
    if (model.lineStates().size() == 3) {
        okay &= Check(
                model.lineStates()[0]
                                .toMap()
                                .value(QStringLiteral("start"))
                                .toInt() == 0 &&
                        model.lineStates()[1]
                                .toMap()
                                .value(QStringLiteral("start"))
                                .toInt() ==
                                model.source().indexOf(
                                        QStringLiteral("iterations")) &&
                        model.lineStates()[2]
                                .toMap()
                                .value(QStringLiteral("start"))
                                .toInt() ==
                                model.source().indexOf(
                                        QStringLiteral("car.speed")),
                "line-state source positions do not track CRLF text");
    }
    if (model.diagnostics().size() == 2) {
        const QVariantMap first = model.diagnostics()[0].toMap();
        const QVariantMap second = model.diagnostics()[1].toMap();
        okay &= Check(
                first.value(QStringLiteral("line")).toInt() == 1 &&
                        first.value(QStringLiteral("column")).toInt() == 1 &&
                        model.source().mid(
                                first.value(QStringLiteral("start")).toInt(),
                                first.value(QStringLiteral("length"))
                                        .toInt()) == QStringLiteral("unknown"),
                "first inline diagnostic does not select its token");
        okay &= Check(second.value(QStringLiteral("line")).toInt() == 2,
                      "CRLF diagnostic line mapping is wrong");
    }

    model.setSource(QStringLiteral("variable(é) = 0"));
    okay &= Check(
            model.diagnostics().size() == 1 &&
                    model.source().mid(model.diagnostics()[0]
                                               .toMap()
                                               .value(QStringLiteral("start"))
                                               .toInt(),
                                       model.diagnostics()[0]
                                               .toMap()
                                               .value(QStringLiteral("length"))
                                               .toInt()) == QStringLiteral("é"),
            "UTF-8 diagnostic range was not converted to Qt UTF-16");

    model.setSource(QStringLiteral("car.speed >= 0"));
    const QVariantMap documentation = model.documentationAt(4);
    okay &= Check(documentation.value(QStringLiteral("symbol")).toString() ==
                                  QStringLiteral("car.speed") &&
                          documentation.value(QStringLiteral("type"))
                                          .toString() ==
                                  QStringLiteral("number") &&
                          documentation.value(QStringLiteral("unit"))
                                          .toString() ==
                                  QStringLiteral("m/s") &&
                          documentation.value(QStringLiteral("start"))
                                          .toInt() == 4 &&
                          documentation.value(QStringLiteral("length"))
                                          .toInt() == 5,
                  "hover-style symbol documentation lookup failed");
    model.setSource(QStringLiteral("kmh(car.speed) > 0"));
    const QVariantMap functionDocumentation = model.documentationAt(1);
    okay &= Check(
            functionDocumentation.value(QStringLiteral("symbol")).toString() ==
                            QStringLiteral("kmh") &&
                    functionDocumentation.value(QStringLiteral("signature"))
                                    .toString() ==
                            QStringLiteral("kmh(number)") &&
                    functionDocumentation.value(QStringLiteral("type"))
                                    .toString() ==
                            QStringLiteral("number") &&
                    functionDocumentation.value(QStringLiteral("start"))
                                    .toInt() == 0 &&
                    functionDocumentation.value(QStringLiteral("length"))
                                    .toInt() == 3,
            "function hover documentation lost its signature, type, or range");
    model.setSource(QStringLiteral(
            "car.wheels.frontleft.surface = surface.ice"));
    const int icePosition = model.source().indexOf(QStringLiteral("ice"));
    const QVariantMap constantDocumentation =
            model.documentationAt(icePosition + 1);
    okay &= Check(
            constantDocumentation.value(QStringLiteral("symbol")).toString() ==
                            QStringLiteral("surface.ice") &&
                    constantDocumentation.value(QStringLiteral("kind"))
                                    .toString() == QStringLiteral("enum") &&
                    constantDocumentation.value(QStringLiteral("type"))
                                    .toString() == QStringLiteral("surface") &&
                    constantDocumentation.value(QStringLiteral("value"))
                                    .toDouble() == 3.0,
            "enum hover lost its definition, type, or numeric value");
    return okay;
}

bool TestCompletionAndHints() {
    forevertas::app::ConditionEditorModel model;
    model.setSource(QString{});
    model.setCursorPosition(0);
    const QVariantMap carObject =
            FindBySymbol(model.completions(), QStringLiteral("car"));
    bool okay = Check(
            !carObject.isEmpty() &&
                    carObject.value(QStringLiteral("isContainer")).toBool() &&
                    carObject.value(QStringLiteral("insertText")).toString() ==
                            QStringLiteral("car.") &&
                    FindBySymbol(model.completions(),
                                 QStringLiteral("car.speed"))
                            .isEmpty(),
            "root completion did not collapse car fields into car");

    const QVariantMap expanded = model.acceptCompletion(
            carObject.value(QStringLiteral("completionId")).toString(),
            carObject.value(QStringLiteral("revision")).toULongLong());
    okay &= Check(expanded.value(QStringLiteral("accepted")).toBool() &&
                          expanded.value(QStringLiteral("source")).toString() ==
                                  QStringLiteral("car.") &&
                          expanded.value(QStringLiteral("cursorPosition"))
                                          .toInt() == 4 &&
                          expanded.value(QStringLiteral("reopen")).toBool() &&
                          model.completionContext()
                                          .value(QStringLiteral("symbol"))
                                          .toString() == QStringLiteral("car"),
                  "accepting car was not one atomic member expansion");
    const QVariantMap stale = model.acceptCompletion(
            carObject.value(QStringLiteral("completionId")).toString(),
            carObject.value(QStringLiteral("revision")).toULongLong());
    okay &= Check(!stale.value(QStringLiteral("accepted")).toBool() &&
                          model.source() == QStringLiteral("car."),
                  "stale completion revision modified the document");

    okay &= Check(
            !FindBySymbol(model.completions(), QStringLiteral("car.speed"))
                            .isEmpty() &&
                    !FindBySymbol(model.completions(),
                                  QStringLiteral("car.prev"))
                             .isEmpty() &&
                    FindBySymbol(model.completions(),
                                 QStringLiteral("car.prev.speed"))
                            .isEmpty() &&
                    model.completionContext()
                                    .value(QStringLiteral("symbol"))
                                    .toString() == QStringLiteral("car"),
            "car completion did not show only direct children/context");

    const QVariantMap previousObject =
            FindBySymbol(model.completions(), QStringLiteral("car.prev"));
    const QVariantMap previousExpanded = model.acceptCompletion(
            previousObject.value(QStringLiteral("completionId")).toString(),
            previousObject.value(QStringLiteral("revision")).toULongLong());
    okay &= Check(
            previousExpanded.value(QStringLiteral("accepted")).toBool() &&
                    model.source() == QStringLiteral("car.prev.") &&
                    model.cursorPosition() == 9 &&
                    model.completionContext()
                                    .value(QStringLiteral("symbol"))
                                    .toString() == QStringLiteral("car.prev"),
            "nested object completion did not insert only its local segment");

    model.updateDocumentState(QStringLiteral("car.speedX"), 6);
    const QVariantMap speed =
            FindBySymbol(model.completions(), QStringLiteral("car.speed"));
    okay &= Check(speed.value(QStringLiteral("replaceStart")).toInt() == 4 &&
                          speed.value(QStringLiteral("replaceLength")).toInt() ==
                                  6 &&
                          speed.value(QStringLiteral("insertText")).toString() ==
                                  QStringLiteral("speed"),
                  "parser did not select the complete member-token range");
    const QVariantMap repaired = model.acceptCompletion(
            speed.value(QStringLiteral("completionId")).toString(),
            speed.value(QStringLiteral("revision")).toULongLong());
    okay &= Check(repaired.value(QStringLiteral("accepted")).toBool() &&
                          model.source() == QStringLiteral("car.speed"),
                  "member completion left a random suffix in the document");

    const QString deepMember = QStringLiteral("car.wheels.frontleft.g");
    model.updateDocumentState(deepMember, deepMember.size());
    const QVariantMap groundContact = FindBySymbol(
            model.completions(),
            QStringLiteral("car.wheels.frontleft.groundcontact"));
    okay &= Check(
            model.completions().size() == 1 && !groundContact.isEmpty() &&
                    model.completionContext()
                                    .value(QStringLiteral("symbol"))
                                    .toString() ==
                            QStringLiteral("car.wheels.frontleft") &&
                    model.completionContext()
                                    .value(QStringLiteral("fragment"))
                                    .toString() == QStringLiteral("g"),
            "deep member completion stopped filtering the active segment");

    const QString surfaceComparison =
            QStringLiteral("car.wheels.frontleft.surface = ");
    model.updateDocumentState(surfaceComparison, surfaceComparison.size());
    const QVariantMap surfaceObject =
            FindBySymbol(model.completions(), QStringLiteral("surface"));
    okay &= Check(
            model.completions().size() == 1 &&
                    surfaceObject.value(QStringLiteral("isContainer"))
                            .toBool() &&
                    model.completionContext()
                                    .value(QStringLiteral("enumNamespace"))
                                    .toString() == QStringLiteral("surface"),
            "surface comparison did not offer its typed enum namespace");
    const QVariantMap expandedSurface = model.acceptCompletion(
            surfaceObject.value(QStringLiteral("completionId")).toString(),
            surfaceObject.value(QStringLiteral("revision")).toULongLong());
    const QVariantMap ice = FindBySymbol(
            model.completions(), QStringLiteral("surface.ice"));
    okay &= Check(
            expandedSurface.value(QStringLiteral("accepted")).toBool() &&
                    expandedSurface.value(QStringLiteral("source")).toString() ==
                            surfaceComparison + QStringLiteral("surface.") &&
                    model.completions().size() == 31 &&
                    ice.value(QStringLiteral("kind")).toString() ==
                            QStringLiteral("enum") &&
                    ice.value(QStringLiteral("value")).toDouble() == 3.0 &&
                    ice.value(QStringLiteral("insertText")).toString() ==
                            QStringLiteral("ice"),
            "surface enum did not expand to its 31 direct values");

    const QString booleanComparison =
            QStringLiteral("car.freewheel = ");
    model.updateDocumentState(booleanComparison, booleanComparison.size());
    okay &= Check(
            model.completions().size() == 2 &&
                    !FindBySymbol(model.completions(), QStringLiteral("true"))
                             .isEmpty() &&
                    !FindBySymbol(model.completions(), QStringLiteral("false"))
                             .isEmpty(),
            "boolean comparison did not offer true and false constants");

    model.setSource(QStringLiteral("km"));
    model.setCursorPosition(2);
    const QVariantMap kmh =
            FindBySymbol(model.completions(), QStringLiteral("kmh"));
    okay &= Check(
            kmh.value(QStringLiteral("insertText")).toString() ==
                            QStringLiteral("kmh()") &&
                    kmh.value(QStringLiteral("replaceStart")).toInt() == 0 &&
                    kmh.value(QStringLiteral("replaceLength")).toInt() == 2 &&
                    kmh.value(QStringLiteral("cursorOffset")).toInt() == 4,
            "function completion edit is not IDE-style");

    model.updateDocumentState(QStringLiteral("kmh"), 3);
    const QVariantMap pairedFunction = model.functionCallEdit(3);
    okay &= Check(
            pairedFunction.value(QStringLiteral("source")).toString() ==
                            QStringLiteral("kmh()") &&
                    pairedFunction.value(QStringLiteral("cursorPosition"))
                                    .toInt() == 4,
            "typed function call did not add a closing parenthesis");

    model.updateDocumentState(QStringLiteral("kmh()"), 4);
    const QVariantMap argumentCar =
            FindBySymbol(model.completions(), QStringLiteral("car"));
    const QVariantMap insertedArgumentObject = model.acceptCompletion(
            argumentCar.value(QStringLiteral("completionId")).toString(),
            argumentCar.value(QStringLiteral("revision")).toULongLong());
    const QVariantMap argumentSpeed =
            FindBySymbol(model.completions(), QStringLiteral("car.speed"));
    const QVariantMap insertedArgumentMember = model.acceptCompletion(
            argumentSpeed.value(QStringLiteral("completionId")).toString(),
            argumentSpeed.value(QStringLiteral("revision")).toULongLong());
    okay &= Check(
            insertedArgumentObject.value(QStringLiteral("accepted")).toBool() &&
                    insertedArgumentObject.value(QStringLiteral("source"))
                                    .toString() ==
                            QStringLiteral("kmh(car.)") &&
                    insertedArgumentObject.value(
                            QStringLiteral("cursorPosition")).toInt() == 8 &&
                    insertedArgumentMember.value(QStringLiteral("accepted"))
                            .toBool() &&
                    insertedArgumentMember.value(QStringLiteral("source"))
                                    .toString() ==
                            QStringLiteral("kmh(car.speed)"),
            "picker completion removed an auto-paired function parenthesis");

    model.updateDocumentState(QStringLiteral("car"), 3);
    okay &= Check(model.functionCallEdit(3).isEmpty(),
                  "non-function identifier received function parentheses");

    model.setSource(QStringLiteral("distance(car.pos, "));
    model.setCursorPosition(model.source().size());
    okay &= Check(model.parameterHint().value(QStringLiteral("activeParameter"))
                                          .toInt() == 1 &&
                          model.parameterHint()
                                          .value(QStringLiteral("signature"))
                                          .toString() ==
                                  QStringLiteral("distance(vector, vector)"),
                  "distance parameter hint does not track the active argument");
    okay &= Check(
            FindBySymbol(model.completions(), QStringLiteral("bf_target_point"))
                    .isEmpty(),
            "point target leaked into completion for another target type");

    model.setEvaluationTargetId(QStringLiteral("point-target"));
    const QVariantMap pointVectorCompletion = FindBySymbol(
            model.completions(), QStringLiteral("bf_target_point"));
    okay &= Check(pointVectorCompletion.value(QStringLiteral("insertText"))
                                  .toString() ==
                          QStringLiteral("variable(bf_target_point)"),
                  "point-target vector completion is missing");
    model.setSource(QStringLiteral("variable("));
    model.setCursorPosition(model.source().size());
    okay &= Check(!FindBySymbol(model.completions(),
                                QStringLiteral("bf_target_point"))
                           .isEmpty(),
                  "external variable completion is missing in variable()");
    return okay;
}

bool TestLineTogglesAndGateMode() {
    forevertas::app::ConditionEditorModel model;
    model.setSource(QStringLiteral("iterations = 1\niterations = 2"));
    const QVariantMap disabledEdit = model.toggleLine(1, model.source().size());
    bool okay =
            Check(disabledEdit.value(QStringLiteral("source"))
                                  .toString()
                                  .startsWith(QStringLiteral("// ")) &&
                          disabledEdit.value(QStringLiteral("cursorPosition"))
                                          .toInt() == model.source().size() + 3,
                  "line toggle did not preserve the cursor/source");
    model.setSource(disabledEdit.value(QStringLiteral("source")).toString());
    okay &= Check(model.valid() && model.gateCount() == 1 &&
                          model.lineStates()[0]
                                          .toMap()
                                          .value(QStringLiteral("state"))
                                          .toString() ==
                                  QStringLiteral("disabled") &&
                          !model.lineStates()[0]
                                   .toMap()
                                   .value(QStringLiteral("enabled"))
                                   .toBool(),
                  "disabled line was not represented as an inactive gate");

    model.setGateMode(QStringLiteral("or"));
    okay &= Check(model.gateMode() == QStringLiteral("or") &&
                          model.statusText().startsWith(QStringLiteral("OR")),
                  "OR mode did not update the editor summary");
    const QVariantMap enabledEdit = model.toggleLine(1, 3);
    okay &= Check(enabledEdit.value(QStringLiteral("source"))
                          .toString()
                          .startsWith(QStringLiteral("iterations")),
                  "disabled line could not be re-enabled");
    return okay;
}

bool TestCatalogueAvailability() {
    forevertas::app::ConditionEditorModel model;
    const QVariantMap speed =
            FindBySymbol(model.catalogue(), QStringLiteral("car.speed"));
    const QVariantMap kmh =
            FindBySymbol(model.catalogue(), QStringLiteral("kmh"));
    const QVariantMap ice =
            FindBySymbol(model.catalogue(), QStringLiteral("surface.ice"));
    QVariantMap target =
            FindBySymbol(model.catalogue(), QStringLiteral("bf_target_point"));
    QVariantMap variableFunction =
            FindBySymbol(model.catalogue(), QStringLiteral("variable"));
    bool okay = Check(
            speed.value(QStringLiteral("kind")).toString() ==
                            QStringLiteral("number") &&
                    speed.value(QStringLiteral("type")).toString() ==
                            QStringLiteral("number") &&
                    kmh.value(QStringLiteral("signature")).toString() ==
                            QStringLiteral("kmh(number)") &&
                    ice.value(QStringLiteral("kind")).toString() ==
                            QStringLiteral("enum") &&
                    ice.value(QStringLiteral("type")).toString() ==
                            QStringLiteral("surface") &&
                    ice.value(QStringLiteral("value")).toDouble() == 3.0,
            "catalogue exposed internal scalar terminology");
    okay &=
            Check(!target.isEmpty() &&
                          !target.value(QStringLiteral("available")).toBool() &&
                          !target.value(QStringLiteral("unavailableReason"))
                                   .toString()
                                   .isEmpty(),
                  "browser did not explain unavailable point target");
    okay &= Check(!variableFunction.value(QStringLiteral("available")).toBool(),
                  "browser offered variable() with no external values");
    model.setEvaluationTargetId(QStringLiteral("point-target"));
    target = FindBySymbol(model.catalogue(), QStringLiteral("bf_target_point"));
    okay &= Check(target.value(QStringLiteral("available")).toBool() &&
                          target.value(QStringLiteral("kind")).toString() ==
                                  QStringLiteral("vector") &&
                          target.value(QStringLiteral("type")).toString() ==
                                  QStringLiteral("vector"),
                  "browser did not expose the active point target as a vector");
    variableFunction =
            FindBySymbol(model.catalogue(), QStringLiteral("variable"));
    okay &= Check(variableFunction.value(QStringLiteral("available")).toBool(),
                  "browser did not enable variable() for Point Target");

    const QVariantList wheelMatches =
            model.searchCatalogue(QStringLiteral("wheel surface"));
    okay &= Check(!wheelMatches.isEmpty() &&
                          wheelMatches[0]
                                  .toMap()
                                  .value(QStringLiteral("symbol"))
                                  .toString()
                                  .contains(QStringLiteral("wheels")),
                  "catalogue search does not use documentation/category text");
    return okay;
}

QTextCharFormat FormatAt(const QTextBlock &block, int position) {
    for (const QTextLayout::FormatRange &range : block.layout()->formats()) {
        if (position >= range.start && position < range.start + range.length) {
            return range.format;
        }
    }
    return {};
}

bool TestHighlighting() {
    QTextDocument document;
    forevertas::app::ConditionSyntaxHighlighter highlighter;
    highlighter.setDocument(&document);
    const QColor symbol(QStringLiteral("#11aa44"));
    const QColor previous(QStringLiteral("#667788"));
    const QColor function(QStringLiteral("#2288ee"));
    const QColor number(QStringLiteral("#dd9900"));
    const QColor op(QStringLiteral("#aabbcc"));
    const QColor error(QStringLiteral("#ee3344"));
    highlighter.setPalette(symbol, previous, function, number, op, error);
    const QString source = QStringLiteral(
            "car.prev.speed + kmh(car.speed) >= surface.ice");
    document.setPlainText(source);
    highlighter.setDiagnostics({{0, 0, 3}});
    highlighter.rehighlight();

    const QTextBlock block = document.firstBlock();
    const QTextCharFormat previousFormat = FormatAt(block, 0);
    const QTextCharFormat functionFormat =
            FormatAt(block, source.indexOf(QStringLiteral("kmh")));
    const QTextCharFormat symbolFormat =
            FormatAt(block, source.indexOf(QStringLiteral("car.speed")));
    const QTextCharFormat operatorFormat =
            FormatAt(block, source.indexOf(QStringLiteral(">=")));
    const QTextCharFormat numberFormat =
            FormatAt(block, source.indexOf(QStringLiteral("surface.ice")));
    bool okay = Check(
            previousFormat.foreground().color() == previous &&
                    previousFormat.underlineStyle() ==
                            QTextCharFormat::WaveUnderline &&
                    previousFormat.underlineColor() == error,
            "diagnostic underline did not merge with previous-state syntax");
    okay &= Check(functionFormat.foreground().color() == function,
                  "function syntax color is missing");
    okay &= Check(symbolFormat.foreground().color() == symbol,
                  "symbol syntax color is missing");
    okay &= Check(operatorFormat.foreground().color() == op,
                  "operator syntax color is missing");
    okay &= Check(numberFormat.foreground().color() == number,
                  "numeric constant syntax color is missing");
    document.setPlainText(QStringLiteral("// car.speed >= 12"));
    highlighter.rehighlight();
    const QTextCharFormat disabledFormat = FormatAt(document.firstBlock(), 0);
    okay &= Check(disabledFormat.foreground().color() == previous,
                  "disabled gate did not use the muted syntax color");
    return okay;
}

bool TestScriptFileStore() {
    QTemporaryDir directory;
    if (!Check(directory.isValid(),
               "could not create a temporary script library")) {
        return false;
    }

    forevertas::app::ScriptFileStore store(directory.path());
    const QVariantMap saved = store.save(QStringLiteral("conditions"),
                                         QStringLiteral("fast finish"),
                                         QStringLiteral("car.speed >= 10"));
    bool okay =
            Check(saved.value(QStringLiteral("ok")).toBool() &&
                          saved.value(QStringLiteral("name")).toString() ==
                                  QStringLiteral("fast finish.txt") &&
                          store.files(QStringLiteral("conditions")).size() == 1,
                  "named condition script was not saved as .txt");

    const QVariantMap collision = store.save(QStringLiteral("conditions"),
                                             QStringLiteral("fast finish.txt"),
                                             QStringLiteral("car.speed >= 20"));
    okay &= Check(!collision.value(QStringLiteral("ok")).toBool() &&
                          collision.value(QStringLiteral("exists")).toBool(),
                  "existing script was overwritten without confirmation");
    const QVariantMap replaced = store.save(
            QStringLiteral("conditions"), QStringLiteral("fast finish"),
            QStringLiteral("car.speed >= 20"), true);
    const QVariantMap loaded = store.load(QStringLiteral("conditions"),
                                          QStringLiteral("fast finish.txt"));
    okay &= Check(replaced.value(QStringLiteral("ok")).toBool() &&
                          loaded.value(QStringLiteral("ok")).toBool() &&
                          loaded.value(QStringLiteral("content")).toString() ==
                                  QStringLiteral("car.speed >= 20"),
                  "confirmed script replacement did not round-trip");

    const QVariantMap inputSaved =
            store.save(QStringLiteral("inputs"), QStringLiteral("opening"),
                       QStringLiteral("0.00 press up"));
    okay &= Check(inputSaved.value(QStringLiteral("ok")).toBool() &&
                          store.files(QStringLiteral("inputs")).size() == 1 &&
                          store.files(QStringLiteral("conditions")).size() == 1,
                  "condition and input script libraries were not separated");
    okay &= Check(!store.save(QStringLiteral("conditions"),
                              QStringLiteral("../escape"), QStringLiteral("x"))
                                  .value(QStringLiteral("ok"))
                                  .toBool() &&
                          store.files(QStringLiteral("unknown")).isEmpty(),
                  "script library accepted an unsafe name or unknown kind");
    return okay;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    return TestAnalysis() && TestCompletionAndHints() &&
                           TestLineTogglesAndGateMode() &&
                           TestCatalogueAvailability() && TestHighlighting() &&
                           TestScriptFileStore()
                   ? 0
                   : 1;
}
