import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "settings"

// Edit > Preferences > Remote (docs/REMOTE.md): this computer (what it can
// search on, what it lends), your own machines (a room they join with its
// code), your groups, and the public pool.
ScrollView {
    id: root
    objectName: "remoteScroll"

    // RemoteSearch (null in tests that do not make one).
    property var remote: null
    readonly property var groups: remote === null ? [] : remote.groups.filter(group => !group.isPublic)
    readonly property var pool: {
        const found = remote === null ? [] : remote.groups.filter(group => group.isPublic)
        return found.length > 0 ? found[0] : null
    }

    clip: true
    contentWidth: availableWidth

    // A small heading inside a section.
    component Heading: Label {
        Layout.fillWidth: true
        Layout.topMargin: 4
        font.pixelSize: 13
        font.weight: Font.DemiBold
        color: AppTheme.text
    }
    component Note: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: 12
        color: AppTheme.textMuted
    }
    component Divider: Rectangle {
        Layout.fillWidth: true
        Layout.topMargin: 6
        Layout.bottomMargin: 6
        implicitHeight: 1
        color: AppTheme.border
    }
    // A thing this computer has: a dot (green: ForeverTAS searches on it), its name, what it is.
    component Device: RowLayout {
        property string name
        property string detail
        property string status
        property bool ready: true
        Layout.fillWidth: true
        spacing: 10
        Rectangle {
            implicitWidth: 8
            implicitHeight: 8
            radius: 4
            color: parent.ready ? AppTheme.success : AppTheme.textFaint
        }
        Label {
            text: parent.name
            color: AppTheme.text
            elide: Text.ElideRight
            Layout.maximumWidth: 320
        }
        Label {
            text: parent.detail
            color: AppTheme.textMuted
            font.pixelSize: 12
        }
        Label {
            Layout.fillWidth: true
            visible: text.length > 0
            text: parent.status
            color: AppTheme.textFaint
            font.pixelSize: 12
            elide: Text.ElideRight
        }
    }
    // A group (or the public pool): what it has, lending to it, searching with it, its invite, who lent most.
    component GroupCard: Rectangle {
        id: card
        required property var group
        objectName: "group_" + group.id
        Layout.fillWidth: true
        implicitHeight: cardColumn.implicitHeight + 24
        radius: 8
        color: AppTheme.surfaceAlternate
        border.width: 1
        border.color: AppTheme.border

        ColumnLayout {
            id: cardColumn
            anchors.fill: parent
            anchors.margins: 12
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Label {
                    visible: !card.group.isPublic
                    text: card.group.name
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    color: AppTheme.text
                }
                Label {
                    Layout.fillWidth: true
                    text: [card.group.machines === 1 ? qsTr("1 machine lent") : qsTr("%1 machines lent").arg(card.group.machines),
                           card.group.searches === 1 ? qsTr("1 search running") : qsTr("%1 searches running").arg(card.group.searches),
                           card.group.isPublic ? ""
                               : card.group.members === 1 ? qsTr("1 member") : qsTr("%1 members").arg(card.group.members)]
                          .filter(part => part.length > 0).join("  ·  ")
                    color: AppTheme.textMuted
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                ThemedButton {
                    objectName: "leaveGroupButton_" + card.group.id
                    text: qsTr("Leave")
                    onClicked: root.remote.leaveGroup(card.group.id)
                }
            }
            SettingSwitch {
                objectName: "lendCheck_" + card.group.id
                label: qsTr("Lend this computer") + (card.group.lend
                       ? "  —  " + (!root.remote.canLend ? qsTr("nothing to lend: see This computer")
                                  : card.group.lending ? qsTr("lent now")
                                  : qsTr("waits while this computer searches on its GPU")) : "")
                checked: card.group.lend
                running: !root.remote.canLend && !card.group.lend
                onToggled: checked => root.remote.setLending(card.group.id, checked)
            }
            SettingSwitch {
                objectName: "searchCheck_" + card.group.id
                label: qsTr("Search with it") + (card.group.search
                       ? "  —  " + (card.group.connected ? qsTr("your searches use its machines") : qsTr("connecting...")) : "")
                checked: card.group.search
                onToggled: checked => root.remote.setSearching(card.group.id, checked)
            }
            RowLayout {
                Layout.fillWidth: true
                visible: card.group.invite.length > 0
                spacing: 8
                Label {
                    text: qsTr("Invite")
                    color: AppTheme.textMuted
                    font.pixelSize: 12
                }
                TextField {
                    id: inviteField
                    objectName: "inviteField_" + card.group.id
                    Layout.fillWidth: true
                    readOnly: true
                    selectByMouse: true
                    font.family: "monospace"
                    text: card.group.invite
                }
                ThemedButton {
                    text: qsTr("Copy")
                    onClicked: {
                        inviteField.selectAll()
                        inviteField.copy()
                        inviteField.deselect()
                    }
                }
                ThemedButton {
                    objectName: "newInviteButton_" + card.group.id
                    visible: card.group.owner
                    text: qsTr("New invite")
                    onClicked: root.remote.newInvite(card.group.id)
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: card.group.credits.length > 0
                spacing: 2
                Label {
                    text: qsTr("Who lent most")
                    font.pixelSize: 12
                    color: AppTheme.textMuted
                }
                Repeater {
                    model: card.group.credits.slice(0, 5)
                    delegate: RowLayout {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        Label {
                            Layout.preferredWidth: 22
                            text: (index + 1) + "."
                            color: AppTheme.textFaint
                            font.pixelSize: 12
                        }
                        Label {
                            Layout.fillWidth: true
                            text: modelData.name + (modelData.you ? qsTr(" (you)") : "")
                            font.weight: modelData.you ? Font.DemiBold : Font.Normal
                            color: AppTheme.text
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                        Label {
                            text: qsTr("%1 attempts").arg(Math.round(modelData.attempts).toLocaleString(Qt.locale(), "f", 0))
                            color: AppTheme.textMuted
                            font.pixelSize: 12
                        }
                        Label {
                            Layout.preferredWidth: 64
                            horizontalAlignment: Text.AlignRight
                            text: qsTr("%1 h").arg(modelData.hours.toFixed(1))
                            color: AppTheme.textMuted
                            font.pixelSize: 12
                        }
                    }
                }
            }
        }
    }

    ColumnLayout {
        objectName: "remoteSection"
        width: root.availableWidth
        spacing: 14

        Note {
            text: qsTr("Search on other computers, and lend this one. Other machines search on their own GPU or CPU and their own copy of the game; every best they find is checked here before it counts.")
        }

        // ---- this computer ----
        ConfigurationSection {
            title: qsTr("This computer")

            Heading { text: qsTr("Graphics cards") }
            Repeater {
                model: root.remote === null ? [] : root.remote.localGpus
                delegate: Device {
                    required property var modelData
                    name: modelData.name.length > 0 ? modelData.name : modelData.backend
                    detail: modelData.backend
                    ready: modelData.ready
                    status: modelData.ready ? "" : modelData.status
                }
            }
            Note {
                visible: root.remote !== null && root.remote.localGpus.length === 0
                text: qsTr("None that ForeverTAS can search on.")
            }
            Heading { text: qsTr("Processor") }
            Device {
                name: root.remote === null ? "" : root.remote.cpuName
                detail: root.remote === null ? "" : qsTr("%1 threads").arg(root.remote.cpuThreads)
            }
            Divider {}
            Heading { text: qsTr("What it lends to groups") }
            ThemedCheckBox {
                objectName: "lendGpuCheck"
                text: qsTr("Its graphics card")
                enabled: root.remote !== null && root.remote.hasGpu
                checked: root.remote !== null && root.remote.lendGpu && root.remote.hasGpu
                onToggled: root.remote.lendGpu = checked
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Label {
                    text: qsTr("CPU threads")
                    color: AppTheme.text
                }
                SpinBox {
                    objectName: "lendCpuThreadsBox"
                    from: 0
                    to: root.remote === null ? 0 : root.remote.cpuThreads
                    editable: true
                    value: root.remote === null ? 0 : root.remote.lendCpuThreads
                    onValueModified: root.remote.lendCpuThreads = value
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("of %1").arg(root.remote === null ? 0 : root.remote.cpuThreads)
                    color: AppTheme.textMuted
                }
            }
            Note {
                text: qsTr("Lending pauses while this computer searches on its own GPU. One search at a time: your groups first, the public pool when they need nothing.")
            }
        }

        // ---- your own machines ----
        ConfigurationSection {
            title: qsTr("Your own machines")
            description: qsTr("A Google Colab or Kaggle notebook, a rented GPU, another computer of yours: it joins with a code and searches for you.")

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Note {
                    text: qsTr("On Google Colab: the notebook asks for your Packs folder (in your Drive) and the code below, or groups' invites.")
                }
                ThemedButton {
                    objectName: "openColabButton"
                    text: qsTr("Open in Colab")
                    onClicked: Qt.openUrlExternally("https://colab.research.google.com/github/Skycrafter-dev/ForeverTAS/blob/v"
                                                    + Qt.application.version + "/worker/colab.ipynb")
                }
            }
            SettingSwitch {
                objectName: "remoteOpenSwitch"
                label: qsTr("Let machines join with a code")
                checked: root.remote !== null && root.remote.open
                running: root.remote === null
                onToggled: checked => { if (root.remote) root.remote.open = checked }
            }
            Note {
                objectName: "remoteStatus"
                visible: text.length > 0
                text: root.remote === null ? "" : root.remote.status
            }
            Label {
                objectName: "remoteCode"
                Layout.alignment: Qt.AlignHCenter
                visible: text.length > 0
                text: root.remote === null ? "" : root.remote.code
                font.family: "monospace"
                font.pixelSize: 28
                font.weight: Font.DemiBold
                font.letterSpacing: 4
                color: AppTheme.text
            }
            Note {
                visible: remoteCommand.text.length > 0
                text: qsTr("On the other machine, with ForeverTAS and the game's Packs, run:")
            }
            RowLayout {
                Layout.fillWidth: true
                visible: remoteCommand.text.length > 0
                TextField {
                    id: remoteCommand
                    objectName: "remoteWorkerCommand"
                    Layout.fillWidth: true
                    readOnly: true
                    selectByMouse: true
                    font.family: "monospace"
                    font.pixelSize: 11
                    text: root.remote === null ? "" : root.remote.workerCommand
                }
                ThemedButton {
                    objectName: "copyRemoteCommandButton"
                    text: qsTr("Copy")
                    onClicked: {
                        remoteCommand.selectAll()
                        remoteCommand.copy()
                        remoteCommand.deselect()
                    }
                }
            }
            Heading {
                visible: root.remote !== null && root.remote.open
                text: qsTr("Machines")
            }
            Note {
                visible: root.remote !== null && root.remote.open && root.remote.roomMachines.length === 0
                text: qsTr("None has joined yet.")
            }
            Repeater {
                model: root.remote === null ? [] : root.remote.roomMachines
                delegate: Rectangle {
                    id: machineCard
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: machineColumn.implicitHeight + 20
                    radius: 8
                    color: AppTheme.surfaceAlternate
                    border.width: 1
                    border.color: AppTheme.border
                    ColumnLayout {
                        id: machineColumn
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 4
                        Label {
                            text: machineCard.modelData.name
                            font.weight: Font.DemiBold
                            color: AppTheme.text
                        }
                        Repeater {
                            model: machineCard.modelData.parts
                            delegate: ThemedCheckBox {
                                required property var modelData
                                objectName: "remoteWorker_" + modelData.key
                                text: modelData.name + "  ·  " + modelData.detail
                                checked: modelData.used
                                onToggled: root.remote.setUsed(modelData.key, checked)
                            }
                        }
                    }
                }
            }
            Note {
                visible: root.remote !== null && root.remote.roomMachines.length > 0
                text: qsTr("Ticked ones search for every search started here, beside this computer.")
            }
        }

        // ---- groups ----
        ConfigurationSection {
            title: qsTr("Groups")
            description: qsTr("Lend this computer to a group, or search with the computers its members lend.")

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Label {
                    text: qsTr("Your name in groups")
                    color: AppTheme.text
                }
                TextField {
                    objectName: "memberNameField"
                    Layout.fillWidth: true
                    selectByMouse: true
                    text: root.remote === null ? "" : root.remote.memberName
                    onEditingFinished: if (root.remote) root.remote.memberName = text
                }
            }
            Repeater {
                model: root.groups
                delegate: GroupCard {
                    required property var modelData
                    group: modelData
                }
            }
            Note {
                visible: root.groups.length === 0
                text: qsTr("You are in no group yet.")
            }
            Divider {}
            Heading { text: qsTr("New group") }
            RowLayout {
                Layout.fillWidth: true
                TextField {
                    id: newGroupName
                    objectName: "newGroupNameField"
                    Layout.fillWidth: true
                    selectByMouse: true
                    placeholderText: qsTr("Its name")
                }
                ThemedButton {
                    objectName: "createGroupButton"
                    text: qsTr("Create")
                    enabled: newGroupName.text.trim().length > 0
                    onClicked: {
                        root.remote.createGroup(newGroupName.text)
                        newGroupName.text = ""
                    }
                }
            }
            Heading { text: qsTr("Join a group") }
            RowLayout {
                Layout.fillWidth: true
                TextField {
                    id: inviteToJoin
                    objectName: "joinInviteField"
                    Layout.fillWidth: true
                    selectByMouse: true
                    font.family: "monospace"
                    placeholderText: qsTr("The invite someone gave you")
                }
                ThemedButton {
                    objectName: "joinGroupButton"
                    text: qsTr("Join")
                    enabled: inviteToJoin.text.trim().length > 0
                    onClicked: {
                        root.remote.joinGroup(inviteToJoin.text)
                        inviteToJoin.text = ""
                    }
                }
            }
        }

        // ---- the public pool ----
        ConfigurationSection {
            title: qsTr("Public pool")
            description: qsTr("Anyone lends, anyone searches.")

            Label {
                Layout.fillWidth: true
                text: qsTr("A search here sends its replay or challenge, inputs and settings to strangers' computers. Keep competitive routes in a group of your own.")
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: AppTheme.warning
            }
            Loader {
                Layout.fillWidth: true
                active: root.pool !== null
                visible: active
                sourceComponent: GroupCard { group: root.pool }
            }
            ThemedButton {
                objectName: "joinPublicButton"
                visible: root.pool === null
                text: qsTr("Join the public pool")
                onClicked: root.remote.joinGroup("public")
            }
        }

        Label {
            objectName: "groupMessage"
            Layout.fillWidth: true
            visible: text.length > 0
            text: root.remote === null ? "" : root.remote.groupMessage
            wrapMode: Text.WordWrap
            color: AppTheme.info
        }
    }
}
