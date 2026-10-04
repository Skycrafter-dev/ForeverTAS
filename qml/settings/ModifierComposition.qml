import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

ColumnLayout {
    id: root

    property var controller
    property var viewer
    property var options: []
    property var passes: []
    property int activePassIndex: 0
    readonly property int passCount: passes.length
    readonly property int renderedPassCount: passRepeater.count
    readonly property int passModelCount: passModel.count
    readonly property var firstRenderedPass:
        activePassIndex >= 0 && passRepeater.count > 0
            ? passRepeater.itemAt(0) : null
    readonly property int firstPassOptionCount:
        firstRenderedPass ? firstRenderedPass.optionCount : -1
    readonly property string firstPassSelectedId:
        firstRenderedPass ? firstRenderedPass.selectedId : ""
    readonly property bool firstPassSettingsLoaded:
        firstRenderedPass ? firstRenderedPass.settingsLoaded : false
    readonly property string firstPassSettingsObjectName:
        firstRenderedPass ? firstRenderedPass.settingsObjectName : ""
    readonly property var firstPassSettingsItem:
        firstRenderedPass ? firstRenderedPass.settingsItem : null
    readonly property bool firstPassSlotStyled:
        firstRenderedPass ? firstRenderedPass.slotStyled : false
    property int rebuildCount: 0
    signal passActivated()

    function synchronizePassModel() {
        if (passModel.count > passes.length)
            passModel.remove(passes.length, passModel.count - passes.length)

        for (let index = passModel.count; index < passes.length; ++index) {
            passModel.append({
                passId: passes[index].id,
                passEnabled: passes[index].enabled !== false,
                passSettings: passes[index].settings
            })
        }
        for (let index = 0; index < passes.length; ++index) {
            passModel.setProperty(index, "passId", passes[index].id)
            passModel.setProperty(index, "passEnabled", passes[index].enabled !== false)
            passModel.setProperty(index, "passSettings", passes[index].settings)
        }
    }

    function addPass(id) {
        if (controller.running || optionIndex(id) < 0)
            return

        controller.addModifierPass(id)
        synchronizePassModel()
        selectPass(passes.length - 1)
    }

    function selectPass(index) {
        if (index < 0 || index >= passCount)
            return
        activePassIndex = index
        passActivated()
    }

    function movePass(offset) {
        synchronizePassModel()
        const destination = activePassIndex + offset
        if (controller.running || activePassIndex < 0
                || destination < 0 || destination >= passes.length)
            return

        controller.moveModifierPass(activePassIndex, destination)
        passModel.move(activePassIndex, destination, 1)
        activePassIndex = destination
    }

    function removePass() {
        if (controller.running || activePassIndex < 0 || passCount === 0)
            return

        const removedIndex = activePassIndex
        synchronizePassModel()
        controller.removeModifierPass(removedIndex)
        passModel.remove(removedIndex)
        activePassIndex = Math.min(removedIndex, passCount - 1)
        passActivated()
    }

    function optionIndex(id) {
        for (let index = 0; index < options.length; ++index) {
            if (options[index].id === id)
                return index

        }
        return -1
    }

    function option(id) {
        const index = optionIndex(id)
        return index >= 0 ? options[index] : null
    }

    objectName: "modifierComposition"
    Layout.fillWidth: true
    spacing: 8
    onPassCountChanged: {
        activePassIndex = passCount === 0 ? -1
            : Math.max(0, Math.min(activePassIndex, passCount - 1))
    }
    onPassesChanged: Qt.callLater(synchronizePassModel)
    Component.onCompleted: synchronizePassModel()

    ListModel {
        id: passModel

        dynamicRoles: true
    }

    Repeater {
        id: passRepeater

        model: passModel

        delegate: ColumnLayout {
            id: passDelegate

            required property int index
            required property string passId
            required property bool passEnabled
            required property var passSettings

            readonly property int optionCount: passTypeCombo.count
            readonly property string selectedId:
                passTypeCombo.currentValue.toString()
            readonly property bool settingsLoaded:
                passSettingsLoader.status === Loader.Ready
                && passSettingsLoader.item !== null
            readonly property string settingsObjectName:
                settingsLoaded ? passSettingsLoader.item.objectName : ""
            readonly property var settingsItem: passSettingsLoader.item
            readonly property bool slotStyled: passTypeCombo.slotStyled

            objectName: "modifierPass" + index
            visible: root.activePassIndex === index
            Layout.fillWidth: true
            spacing: 6
            onPassSettingsChanged: {
                if (passSettingsLoader.item)
                    passSettingsLoader.item.settings = passSettings

            }

            ThemeControls.ThemedCheckBox {
                objectName: "modifierPassEnabled" + index
                text: qsTr("Enabled")
                checked: passEnabled
                enabled: !root.controller.running
                onToggled: root.controller.setModifierPassEnabled(
                               passDelegate.index, checked)
            }

            StyledComboBox {
                id: passTypeCombo

                objectName: "modifierPassCombo" + index
                Layout.fillWidth: true
                Accessible.name: qsTr("Pass type")
                model: root.options
                textRole: "label"
                valueRole: "id"
                currentIndex: root.optionIndex(passId)
                enabled: !root.controller.running
                onActivated: selectedIndex =>
                    root.controller.setModifierPassId(
                        passDelegate.index, valueAt(selectedIndex).toString())
            }

            Loader {
                id: passSettingsLoader

                readonly property var descriptor: root.option(passId)

                Layout.fillWidth: true
                active: descriptor !== null
                source: active ? descriptor.settingsComponent : ""
                onLoaded: {
                    if (!item)
                        return

                    item.settings = passSettings
                    if ("viewer" in item)
                        item.viewer = root.viewer

                    item.updateSetting = function(key, value) {
                        root.controller.setModifierPassSetting(
                            passDelegate.index, key, value)
                    }
                }
                onDescriptorChanged: {
                    if (item)
                        item.settings = passSettings

                }
            }

            Binding {
                target: passSettingsLoader.item
                property: "running"
                value: root.controller.running
                when: passSettingsLoader.item !== null
            }

        }

    }

}
