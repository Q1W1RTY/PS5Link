import QtCore
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs

import org.streetpea.chiaking

import "controls" as C

DialogView {
    enum Console {
        PS4,
        PS5
    }
    property int selectedConsole: SettingsDialog.Console.PS5
    property bool quitControllerMapping: true
    id: dialog
    property bool fromStream: false
    property bool generalAdvanced: false
    Component.onCompleted: if (fromStream) Chiaki.window.grabInput()
    Component.onDestruction: if (fromStream && Chiaki.window) Chiaki.window.releaseInput()
    StackView.onRemoved: if (fromStream && typeof Chiaki.window.returnToExternalStream === "function")
        Chiaki.window.returnToExternalStream()
    title: qsTr("Settings")
    header: qsTr("* Defaults in () to right of value or marked with (Default)")
    buttonVisible: false
    function flickContainsItem(flick, item) {
        let current = item;
        while (current) {
            if (current === flick || current === flick.contentItem)
                return true;
            current = current.parent;
        }
        return false;
    }
    function ensureItemVisibleInFlick(flick, item) {
        if (!flick || !item || item === flick || !flickContainsItem(flick, item))
            return;
        const position = item.mapToItem(flick.contentItem, 0, 0);
        const right = position.x + item.width;
        const bottom = position.y + item.height;
        if (position.x < flick.contentX || item.width > flick.width)
            flick.contentX = Math.max(0, position.x);
        else if (right > flick.contentX + flick.width)
            flick.contentX = Math.min(flick.contentWidth - flick.width, right - flick.width);
        if (position.y < flick.contentY || item.height > flick.height)
            flick.contentY = Math.max(0, position.y);
        else if (bottom > flick.contentY + flick.height)
            flick.contentY = Math.min(flick.contentHeight - flick.height, bottom - flick.height);
    }
    function itemIsInsideNestedScrollable(flick, item) {
        let current = item ? item.parent : null;
        while (current && current !== flick && current !== flick.contentItem) {
            if (current.contentY !== undefined && current.contentHeight !== undefined && current !== flick)
                return true;
            current = current.parent;
        }
        return false;
    }
    function nestedScrollableTarget(flick, item) {
        let current = item;
        while (current && current !== flick && current !== flick.contentItem) {
            if (current.contentY !== undefined && current.contentHeight !== undefined && current !== flick)
                return current;
            current = current.parent;
        }
        return item;
    }
    function ensureActiveFocusVisible() {
        const flick = activeSettingsFlick();
        const window = dialog.Window.window;
        if (!flick || !window || !window.activeFocusItem)
            return;
        const target = itemIsInsideNestedScrollable(flick, window.activeFocusItem)
            ? nestedScrollableTarget(flick, window.activeFocusItem)
            : window.activeFocusItem;
        ensureItemVisibleInFlick(flick, target);
    }
    function scrollFlickKeepingItemVisible(flick, item, delta) {
        if (!flick || !item)
            return;
        const maxContentY = Math.max(0, flick.contentHeight - flick.height);
        if (maxContentY <= 0)
            return;
        flick.contentY = Math.max(0, Math.min(maxContentY, flick.contentY + delta));
    }
    function focusCurrentTabFirstItem() {
        let item = null;
        switch (bar.currentIndex) {
        case 0: item = disconnectAction; break;
        case 1: item = hwDecoderCombo; break;
        case 2: item = consoleSelection; break;
        case 3: item = audioOutDevice; break;
        case 4: item = registerNewButton; break;
        case 5: item = resetAllKeys; break;
        case 6: item = controllerMappingChange; break;
        case 7: item = firstRemoteFocusableItem(); break;
        case 8: item = profile; break;
        }
        if (item)
            item.forceActiveFocus(Qt.TabFocusReason);
    }
    function activeSettingsFlick() {
        switch (bar.currentIndex) {
        case 0: return generalFlick;
        case 1: return videoFlick;
        case 2: return streamFlick;
        case 3: return audiowifiFlick;
        case 4: return consolesFlick;
        case 5: return keysFlick;
        case 6: return controllersFlick;
        case 7: return remoteFlick;
        case 8: return configFlick;
        default: return null;
        }
    }

    function firstRemoteFocusableItem() {
        if (openPsnLogin.visible)
            return openPsnLogin;
        if (resetPsnTokens.visible)
            return resetPsnTokens;
        if (holePunchGuessingCheckbox.visible)
            return holePunchGuessingCheckbox;
        if (portGuessCountSlider.visible)
            return portGuessCountSlider;
        if (portGuessSocketSlider.visible)
            return portGuessSocketSlider;
        return remoteFlick;
    }

    component GeneralCard: ColumnLayout {
        Layout.fillWidth: true
        spacing: 8
    }

    component GeneralRow: GridLayout {
        id: row
        property string label
        property string hint
        default property alias editor: slot.data
        columns: width >= 480 ? 2 : 1
        columnSpacing: 14
        rowSpacing: 3
        Layout.fillWidth: true

        ColumnLayout {
            Layout.fillWidth: true
            Layout.preferredWidth: row.columns === 2 ? row.width * 0.44 : row.width
            spacing: 2
            Label {
                Layout.fillWidth: true
                text: row.label
                font.pixelSize: 14
                color: "#F0F1F3"
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: text.length !== 0
                text: row.hint
                font.pixelSize: 12
                color: "#AAABB4"
                wrapMode: Text.Wrap
            }
        }

        RowLayout {
            id: slot
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: row.columns === 2 ? row.width * 0.56 - row.columnSpacing : row.width
            spacing: 8
        }
    }

    component GeneralToggle: GeneralRow {
        id: row
        property alias checked: check.checked
        signal toggled(bool enabled)
        C.CheckBox {
            id: check
            Accessible.name: row.label
            onToggled: row.toggled(checked)
        }
    }

    component SettingsPicker: C.ComboBox {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.maximumWidth: 360
    }

    component SettingsSlider: C.Slider {
        Layout.preferredWidth: Math.max(72, Math.min(250, parent ? parent.width - 104 : 250))
        Layout.minimumWidth: 0
    }

    component SettingsFlow: Flow {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.preferredHeight: implicitHeight
        spacing: 8
    }
    Keys.onPressed: (event) => {
        if (event.modifiers)
            return;
        switch (event.key) {
        case Qt.Key_PageUp:
            bar.currentIndex = Math.max(0, bar.currentIndex - 1);
            event.accepted = true;
            break;
        case Qt.Key_PageDown:
            bar.currentIndex = Math.min(tabs.count - 1, bar.currentIndex + 1);
            event.accepted = true;
            break;
        case Qt.Key_Up:
        {
            const flick = activeSettingsFlick();
            if (!flick || flick.contentHeight <= flick.height || flick.contentY <= 0.001)
                return;
            flick.flick(0, 500);
            event.accepted = true;
            break;
        }
        case Qt.Key_Down:
        {
            const flick = activeSettingsFlick();
            if (!flick || flick.contentHeight <= flick.height || flick.contentY >= flick.contentHeight - flick.height - 0.001)
                return;
            flick.flick(0, -500);
            event.accepted = true;
            break;
        }
        }
    }

    Pane {
        padding: 0
        font.pixelSize: 15
        background: null

        Rectangle {
            id: bar
            property int currentIndex: 0
            height: tabGrid.implicitHeight + 8
            radius: 15
            color: "#151519"
            border.color: "#2E2E36"
            border.width: 1
            anchors {
                top: parent.top
                left: parent.left
                right: parent.right
                topMargin: 4
                leftMargin: 6
                rightMargin: 6
            }

            GridLayout {
                id: tabGrid
                anchors.fill: parent
                anchors.margins: 4
                columns: width >= 940 ? 9 : width >= 555 ? 5 : 3
                uniformCellWidths: true
                uniformCellHeights: true
                columnSpacing: 4
                rowSpacing: 4

                Repeater {
                    id: tabs
                    model: [qsTr("General"), qsTr("Video"), qsTr("Stream"),
                            qsTr("Audio/Wifi"), qsTr("Consoles"), qsTr("Keys"),
                            qsTr("Controllers"), qsTr("Remote"), qsTr("Config")]
                    Button {
                        id: tab
                        required property int index
                        required property string modelData
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredHeight: 34
                        leftPadding: 4
                        rightPadding: 4
                        topPadding: 2
                        bottomPadding: 2
                        topInset: 0
                        bottomInset: 0
                        text: modelData
                        checked: bar.currentIndex === index
                        font.pixelSize: 13
                        focusPolicy: Qt.NoFocus
                        Accessible.role: Accessible.PageTab
                        onClicked: bar.currentIndex = index
                        ToolTip.visible: tab.hovered && contentItem.truncated
                        ToolTip.text: tab.text
                        contentItem: Text {
                            text: tab.text
                            color: tab.checked ? "#FFFFFF" : "#BFC0CA"
                            font: tab.font
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: 10
                            color: tab.checked ? "#343B4A" : tab.hovered ? "#26262E" : "transparent"
                        }
                    }
                }
            }
        }

        StackLayout {
            anchors {
                top: bar.bottom
                left: parent.left
                right: parent.right
                bottom: parent.bottom
            }
            currentIndex: bar.currentIndex
            onCurrentIndexChanged: {
                dialog.focusCurrentTabFirstItem()
                Qt.callLater(dialog.ensureActiveFocusVisible)
            }

            Item {
                Flickable {
                    id: generalFlick
                    anchors.fill: parent
                    clip: true
                    contentWidth: width
                    contentHeight: generalLayout.height + 24
                    flickableDirection: Flickable.VerticalFlick
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    ColumnLayout {
                        id: generalLayout
                        width: Math.max(0, Math.min(900, generalFlick.width - 36))
                        y: 12
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 10

                        Frame {
                            Layout.fillWidth: true
                            padding: 12
                            background: Rectangle {
                                radius: 17
                                color: "#1C1C21"
                                border.color: "#33333B"
                            }
                            contentItem: GeneralCard {
                                GeneralRow {
                                    label: qsTr("Action On Disconnect:")
                                    C.ComboBox {
                                        id: disconnectAction
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        Layout.maximumWidth: 300
                                        firstInFocusChain: true
                                        Accessible.name: qsTr("Action On Disconnect:")
                                        model: [qsTr("Do Nothing"), qsTr("Enter Sleep Mode"), qsTr("Ask")]
                                        currentIndex: Chiaki.settings.disconnectAction
                                        onActivated: index => Chiaki.settings.disconnectAction = index
                                    }
                                }
                                GeneralRow {
                                    label: qsTr("Action On Suspend:")
                                    C.ComboBox {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        Layout.maximumWidth: 300
                                        Accessible.name: qsTr("Action On Suspend:")
                                        model: [qsTr("Do Nothing"), qsTr("Enter Sleep Mode")]
                                        currentIndex: Chiaki.settings.suspendAction
                                        onActivated: index => Chiaki.settings.suspendAction = index
                                    }
                                }
                                GeneralRow {
                                    label: qsTr("Audio/Video:")
                                    C.ComboBox {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        Layout.maximumWidth: 300
                                        Accessible.name: qsTr("Audio/Video:")
                                        model: [qsTr("Audio and Video Enabled"), qsTr("Audio Disabled"),
                                            qsTr("Video Disabled"), qsTr("Audio and Video Disabled")]
                                        currentIndex: Chiaki.settings.audioVideoDisabled
                                        onActivated: index => Chiaki.settings.audioVideoDisabled = index
                                    }
                                }
                                GeneralRow {
                                    label: qsTr("UI Theme:")
                                    visible: Chiaki.settings.themeMode !== undefined
                                    C.ComboBox {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        Layout.maximumWidth: 300
                                        Accessible.name: qsTr("UI Theme:")
                                        model: [qsTr("Dark"), qsTr("Light"), qsTr("System Auto")]
                                        currentIndex: Chiaki.settings.themeMode ?? 0
                                        onActivated: index => Chiaki.settings.themeMode = index
                                    }
                                }
                                GeneralToggle {
                                    label: qsTr("Prevent PC/Display Sleep:")
                                    visible: Chiaki.settings.preventDisplaySleep !== undefined
                                    checked: Chiaki.settings.preventDisplaySleep === true
                                    onToggled: enabled => Chiaki.settings.preventDisplaySleep = enabled
                                }
                                GeneralToggle {
                                    label: qsTr("Streamer Mode (Hides Info)")
                                    checked: Chiaki.settings.streamerMode
                                    onToggled: enabled => Chiaki.settings.streamerMode = enabled
                                }
                            }
                        }

                        C.Button {
                            id: advancedButton
                            text: qsTr("Advanced")
                            Layout.fillWidth: true
                            Accessible.name: text
                            onClicked: dialog.generalAdvanced = !dialog.generalAdvanced
                            contentItem: RowLayout {
                                Label {
                                    Layout.fillWidth: true
                                    text: advancedButton.text
                                    font.pixelSize: 14
                                    font.weight: Font.DemiBold
                                }
                                Label {
                                    text: dialog.generalAdvanced ? "⌃" : "⌄"
                                    font.pixelSize: 16
                                }
                            }
                        }

                        Frame {
                            visible: dialog.generalAdvanced
                            Layout.fillWidth: true
                            padding: 12
                            background: Rectangle {
                                radius: 17
                                color: "#1C1C21"
                                border.color: "#33333B"
                            }
                            contentItem: GeneralCard {
                                GeneralToggle {
                                    label: qsTr("Steam Deck Haptics:")
                                    hint: qsTr("True haptics for SteamDeck, better quality but noisier")
                                    visible: Chiaki.settings.steamDeckHaptics !== undefined
                                    checked: Chiaki.settings.steamDeckHaptics === true
                                    onToggled: enabled => Chiaki.settings.steamDeckHaptics = enabled
                                }
                                GeneralToggle {
                                    label: qsTr("Steam Deck Vertical:")
                                    hint: qsTr("Use Steam Deck in vertical orientation (motion controls)")
                                    visible: Chiaki.settings.verticalDeck !== undefined
                                    checked: Chiaki.settings.verticalDeck === true
                                    onToggled: enabled => Chiaki.settings.verticalDeck = enabled
                                }
                                GeneralRow {
                                    label: qsTr("Performance Mode:")
                                    visible: Chiaki.settings.performanceMode !== undefined
                                    C.ComboBox {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        Layout.maximumWidth: 300
                                        Accessible.name: qsTr("Performance Mode:")
                                        model: [qsTr("Balanced"), qsTr("Turbo (1 ms timer + high priority)")]
                                        currentIndex: Chiaki.settings.performanceMode ?? 1
                                        onActivated: index => Chiaki.settings.performanceMode = index
                                    }
                                }
                                GeneralToggle {
                                    label: qsTr("Optimize Wi-Fi for Streaming:")
                                    visible: Chiaki.settings.wifiStreamingMode !== undefined
                                    checked: Chiaki.settings.wifiStreamingMode === true
                                    onToggled: enabled => Chiaki.settings.wifiStreamingMode = enabled
                                }
                                GeneralToggle {
                                    label: qsTr("Gamepad Quit Hotkey (Options + Share):")
                                    visible: Chiaki.settings.gamepadQuitHotkey !== undefined
                                    checked: Chiaki.settings.gamepadQuitHotkey === true
                                    onToggled: enabled => Chiaki.settings.gamepadQuitHotkey = enabled
                                }
                                GeneralToggle {
                                    label: qsTr("Overlay Volume Controls Windows System Volume:")
                                    visible: Chiaki.settings.volumeControlsSystem !== undefined
                                    checked: Chiaki.settings.volumeControlsSystem === true
                                    onToggled: enabled => Chiaki.settings.volumeControlsSystem = enabled
                                }
                                GeneralRow {
                                    label: qsTr("DX12 Quick Top Bar:")
                                    hint: qsTr("Hover top edge")
                                    visible: Chiaki.settings.hideOnScreenControls !== undefined
                                    Flow {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        Layout.preferredHeight: implicitHeight
                                        spacing: 6
                                        C.CheckBox {
                                            text: qsTr("Hide Bar")
                                            checked: Chiaki.settings.hideOnScreenControls === true
                                            onToggled: Chiaki.settings.hideOnScreenControls = checked
                                        }
                                        C.CheckBox {
                                            text: qsTr("Mic")
                                            enabled: Chiaki.settings.hideOnScreenControls !== true
                                            checked: Chiaki.settings.showMicButton === true
                                            onToggled: Chiaki.settings.showMicButton = checked
                                        }
                                        C.CheckBox {
                                            text: qsTr("Aspect")
                                            enabled: Chiaki.settings.hideOnScreenControls !== true
                                            checked: Chiaki.settings.showAspectButton === true
                                            onToggled: Chiaki.settings.showAspectButton = checked
                                        }
                                        C.CheckBox {
                                            text: qsTr("Vol -/+")
                                            enabled: Chiaki.settings.hideOnScreenControls !== true
                                            checked: Chiaki.settings.showVolumeButtons === true
                                            onToggled: Chiaki.settings.showVolumeButtons = checked
                                        }
                                    }
                                }
                                GeneralToggle {
                                    label: qsTr("Always Request Keyframe on Packet/Frame Gap:")
                                    hint: qsTr("Faster artifact clear")
                                    visible: Chiaki.settings.alwaysRequestIdr !== undefined
                                    checked: Chiaki.settings.alwaysRequestIdr === true
                                    onToggled: enabled => Chiaki.settings.alwaysRequestIdr = enabled
                                }
                                GeneralRow {
                                    label: qsTr("Log Directory:")
                                    Label {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        font.pixelSize: 12
                                        elide: Text.ElideMiddle
                                        text: Chiaki.settings.logDirectory
                                        HoverHandler { id: logPathHover }
                                        ToolTip.visible: logPathHover.hovered && truncated
                                        ToolTip.text: text
                                    }
                                    C.Button {
                                        text: qsTr("Open")
                                        onClicked: {
                                            const path = Chiaki.settings.logDirectory;
                                            Qt.openUrlExternally((path.startsWith("/") ? "file://" : "file:///") + encodeURI(path));
                                        }
                                    }
                                }
                                GeneralToggle {
                                    id: streamMenu
                                    label: qsTr("Stream Menu Shortcut Enabled")
                                    checked: Chiaki.settings.streamMenuEnabled
                                    onToggled: enabled => Chiaki.settings.streamMenuEnabled = enabled
                                }
                                GeneralRow {
                                    label: qsTr("Stream Menu Combo:")
                                    visible: Chiaki.settings.streamMenuEnabled
                                    GridLayout {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        columns: 2
                                        rowSpacing: 5
                                        columnSpacing: 5
                                        Repeater {
                                            model: 4
                                            C.ComboBox {
                                                required property int index
                                                readonly property string setting: "streamMenuShortcut" + (index + 1)
                                                Layout.fillWidth: true
                                                Layout.minimumWidth: 0
                                                model: [qsTr("Not Used"), qsTr("Cross"), qsTr("Moon"), qsTr("Box"), qsTr("Pyramid"),
                                                    qsTr("Dpad Left"), qsTr("Dpad Right"), qsTr("Dpad Up"), qsTr("Dpad Down"),
                                                    qsTr("L1"), qsTr("R1"), qsTr("L3"), qsTr("R3"),
                                                    qsTr("Options"), qsTr("Share"), qsTr("Touchpad"), qsTr("PS")]
                                                currentIndex: Chiaki.settings[setting]
                                                onActivated: value => Chiaki.settings[setting] = value
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }


            Item {
                // Video
                Flickable {
                    id: videoFlick
                    anchors {
                        fill: parent
                        topMargin: 20
                        bottomMargin: 20
                    }
                    clip: true
                    contentWidth: width
                    contentHeight: videoGrid.height + videoGrid.y + 12
                    flickableDirection: Flickable.AutoFlickIfNeeded
                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AlwaysOn
                        visible: videoFlick.contentHeight > videoFlick.height
                    }
                    ColumnLayout {
                        id: videoGrid
                        width: Math.max(0, Math.min(900, videoFlick.width - 36))
                        anchors {
                            top: parent.top
                            horizontalCenter: parent.horizontalCenter
                        }

                        spacing: 9

                        GeneralRow {
                            label: qsTr("Hardware Decoder:")
                            hint: qsTr("(Auto)")
                            RowLayout {

                                spacing: 12

                                SettingsPicker {
                                    id: hwDecoderCombo

                                    model: Chiaki.settings.availableDecoders
                                    currentIndex: Math.max(0, model.indexOf(Chiaki.settings.decoder))
                                    KeyNavigation.priority: KeyNavigation.BeforeItem
                                    KeyNavigation.up: hwDecoderCombo
                                    KeyNavigation.right: zeroCopyCheck
                                    KeyNavigation.down: windowTypeCombo
                                    onActivated: (index) => Chiaki.settings.decoder = index ? model[index] : ""
                                }

                                Label {
                                    text: qsTr("Zero-Copy")
                                }

                                C.CheckBox {
                                    id: zeroCopyCheck
                                    KeyNavigation.priority: KeyNavigation.BeforeItem
                                    KeyNavigation.up: zeroCopyCheck
                                    KeyNavigation.left: hwDecoderCombo
                                    KeyNavigation.down: windowTypeCombo
                                    checked: Chiaki.settings.useZeroCopy
                                    onToggled: Chiaki.settings.useZeroCopy = checked
                                }
                            }
                        }

                        GeneralRow {
                            label: qsTr("Window Type:")
                            hint: qsTr("(Fullscreen)")
                            SettingsPicker {
                                Accessible.name: qsTr("Window Type:")
                                id: windowTypeCombo

                                popup.width: Math.min(dialog.width - 20, 700)
                                model: [qsTr("Stream Resolution"), qsTr("Custom Resolution"), qsTr("Adjust Resolution Manually"), qsTr("Fullscreen"), qsTr("Zoom [adjust zoom using slider in stream menu]"), qsTr("Stretch")]
                                currentIndex: Chiaki.settings.windowType
                                onActivated: (index) => Chiaki.settings.windowType = index;
                            }
                        }

                        GeneralRow {
                            label: qsTr("Custom Resolution Width")
                            visible: Chiaki.settings.windowType == 1
                            hint: qsTr("(1920)")
                            C.TextField {
                                Accessible.name: qsTr("Custom Resolution Width")
                                Layout.fillWidth: true
                                Layout.maximumWidth: 300
                                Layout.minimumWidth: 0
                                                id: customResolutionWidth

                                                visible: Chiaki.settings.windowType == 1
                                                text: Chiaki.settings.customResolutionWidth
                                                Material.accent: text && !validate() ? Material.Red : undefined
                                                onEditingFinished: {
                                                    if (validate()) {
                                                        Chiaki.settings.customResolutionWidth = parseInt(text);
                                                    } else {
                                                        Chiaki.settings.customResolutionWidth = 0;
                                                        text = "";
                                                    }
                                                }
                                                function validate() {
                                                    var num = parseInt(text);
                                                    return num >= 0 && num <= 9999;
                                                }
                                            }
                        }

                        GeneralRow {
                            label: qsTr("Custom Resolution Height")
                            visible: Chiaki.settings.windowType == 1
                            hint: qsTr("(1080)")
                            C.TextField {
                                Accessible.name: qsTr("Custom Resolution Height")
                                Layout.fillWidth: true
                                Layout.maximumWidth: 300
                                Layout.minimumWidth: 0
                                                id: customResolutionHeight

                                                visible: Chiaki.settings.windowType == 1
                                                text: Chiaki.settings.customResolutionHeight
                                                Material.accent: text && !validate() ? Material.Red : undefined
                                                onEditingFinished: {
                                                    if (validate()) {
                                                        Chiaki.settings.customResolutionHeight = parseInt(text);
                                                    } else {
                                                        Chiaki.settings.customResolutionHeight = 0;
                                                        text = "";
                                                    }
                                                }
                                                function validate() {
                                                    var num = parseInt(text);
                                                    return num >= 0 && num <= 9999;
                                                }
                                            }
                        }

                        GeneralRow {
                            label: qsTr("Toggle Fullscreen on Double-click:")
                            hint: qsTr("(Unchecked)")
                            C.CheckBox {
                                Accessible.name: qsTr("Toggle Fullscreen on Double-click:")
                                checked: Chiaki.settings.fullscreenDoubleClick
                                onToggled: Chiaki.settings.fullscreenDoubleClick = checked
                            }
                        }

                        GeneralRow {
                            label: qsTr("Hide Cursor during Stream")
                            hint: qsTr("(Checked)")
                            C.CheckBox {
                                Accessible.name: qsTr("Hide Cursor during Stream")
                                checked: Chiaki.settings.hideCursor
                                onToggled: Chiaki.settings.hideCursor = !Chiaki.settings.hideCursor
                            }
                        }

                        GeneralRow {
                            label: qsTr("Vertical Sync:")
                            hint: qsTr("(Unchecked)")
                            C.CheckBox {
                                Accessible.name: qsTr("Vertical Sync:")
                                checked: Chiaki.settings.vSyncEnabled
                                onClicked: {
                                    Chiaki.settings.vSyncEnabled = checked
                                    if (Chiaki.window.runtimeRendererBackend === 1 && Chiaki.settings.restartApplication())
                                        Qt.quit()
                                }
                            }
                        }

                        GeneralRow {
                            label: qsTr("Render Preset:")
                            hint: qsTr("(High Quality)")
                            SettingsPicker {
                                Accessible.name: qsTr("Render Preset:")

                                model: [qsTr("Fast"), qsTr("Default"), qsTr("High Quality"), qsTr("High Quality + Spatial Upscaling"), qsTr("High Quality + Advanced Spatial Upscaling"), qsTr("Custom")]
                                currentIndex: Chiaki.settings.videoPreset
                                onActivated: (index) => {
                                    Chiaki.settings.videoPreset = index;
                                    switch (index) {
                                    case 0: Chiaki.window.videoPreset = ChiakiWindow.VideoPreset.Fast; break;
                                    case 1: Chiaki.window.videoPreset = ChiakiWindow.VideoPreset.Default; break;
                                    case 2: Chiaki.window.videoPreset = ChiakiWindow.VideoPreset.HighQuality; break;
                                    case 3: Chiaki.window.videoPreset = ChiakiWindow.VideoPreset.HighQualitySpatial; break;
                                    case 4: Chiaki.window.videoPreset = ChiakiWindow.VideoPreset.HighQualityAdvancedSpatial; break;
                                    case 5: Chiaki.window.videoPreset = ChiakiWindow.VideoPreset.Custom; break;
                                    }
                                }
                            }
                        }

                        GeneralRow {
                            label: qsTr("Frame Delivery:")
                            hint: qsTr("(Direct Mapping)")
                            SettingsPicker {
                                Accessible.name: qsTr("Frame Delivery:")

                                model: [qsTr("Direct Mapping"), qsTr("Frame Queue")]
                                currentIndex: Chiaki.settings.directFrameMapping ? 0 : 1
                                onActivated: (index) => {
                                    const direct = index === 0;
                                    if (direct === Chiaki.settings.directFrameMapping)
                                        return;
                                    Chiaki.settings.directFrameMapping = direct;
                                    if (Chiaki.settings.restartApplication())
                                        Qt.quit();
                                }
                            }
                        }

                        GeneralRow {
                            label: qsTr("Frame Mixer:")
                            hint: qsTr("(None)")
                            SettingsPicker {
                                Accessible.name: qsTr("Frame Mixer:")

                                model: [qsTr("None"), qsTr("Oversample"), qsTr("Hermite"), qsTr("Linear"), qsTr("Cubic")]
                                enabled: !Chiaki.settings.directFrameMapping
                                currentIndex: Chiaki.settings.directFrameMapping ? 0 : Chiaki.settings.placeboFrameMixer
                                onActivated: index => Chiaki.settings.placeboFrameMixer = index
                            }
                        }

                        GeneralRow {
                            label: qsTr("Renderer Backend:")
                            hint: qsTr("(Vulkan)")
                            SettingsPicker {
                                Accessible.name: qsTr("Renderer Backend:")

                                model: [qsTr("Vulkan"), qsTr("OpenGL")]
                                currentIndex: Chiaki.settings.rendererBackend
                                onActivated: (index) => {
                                    if (index === Chiaki.settings.rendererBackend)
                                        return;
                                    Chiaki.settings.rendererBackend = index;
                                    if (Chiaki.settings.restartApplication())
                                        Qt.quit();
                                }
                            }
                        }

                        GeneralRow {
                            label: qsTr("RTX Video Super Resolution")
                            visible: Chiaki.window.rtxVsrAvailable
                            hint: Chiaki.window.rtxVsrActive ? qsTr("Active") : ""
                            SettingsPicker {
                                Accessible.name: qsTr("RTX Video Super Resolution")

                                visible: Chiaki.window.rtxVsrAvailable
                                model: [qsTr("Off"), qsTr("Low"), qsTr("Medium"), qsTr("High"), qsTr("Ultra")]
                                currentIndex: Chiaki.window.rtxVsrQuality
                                onActivated: index => Chiaki.window.rtxVsrQuality = index
                            }
                        }

                        GeneralRow {
                            label: qsTr("Direct3D 12 (next session)")
                            visible: Chiaki.settings.dx12Enabled !== undefined
                            hint: qsTr("H.265 (SDR / HDR)")
                            C.CheckBox {
                                Accessible.name: qsTr("Direct3D 12 (next session)")
                                visible: Chiaki.settings.dx12Enabled !== undefined
                                checked: Chiaki.settings.dx12Enabled === true
                                onToggled: Chiaki.settings.dx12Enabled = checked
                            }
                        }

                        GeneralRow {
                            label: qsTr("Frame generation")
                            visible: Chiaki.settings.dx12Enabled === true
                            hint: qsTr("120 Hz or faster")
                            SettingsPicker {
                                Accessible.name: qsTr("Frame generation")
                                visible: Chiaki.settings.dx12Enabled === true
                                model: [qsTr("Off"), qsTr("Motion interpolation ×2"), qsTr("NVIDIA DLSS Frame Generation ×2"), qsTr("AMD FSR 3.1 Frame Generation ×2")]
                                currentIndex: Chiaki.settings.dx12FrameGeneration ?? 0
                                onActivated: index => Chiaki.settings.dx12FrameGeneration = index
                            }
                        }

                        GeneralRow {
                            label: qsTr("DX12 Window Mode")
                            visible: Chiaki.settings.dx12Enabled === true && Chiaki.settings.dx12WindowMode !== undefined
                            hint: qsTr("F11 / Alt+Enter")
                            SettingsPicker {
                                Accessible.name: qsTr("DX12 Window Mode")
                                visible: Chiaki.settings.dx12Enabled === true && Chiaki.settings.dx12WindowMode !== undefined
                                model: [qsTr("Fullscreen"), qsTr("Borderless Fullscreen"), qsTr("Windowed (remember size)")]
                                currentIndex: Chiaki.settings.dx12WindowMode ?? 0
                                onActivated: index => Chiaki.settings.dx12WindowMode = index
                            }
                        }

                        GeneralRow {
                            label: qsTr("Ambient Background Style")
                            visible: Chiaki.settings.dx12Enabled === true && Chiaki.settings.dx12AmbientStyle !== undefined
                            hint: qsTr("Used with Ambient aspect")
                            SettingsPicker {
                                Accessible.name: qsTr("Ambient Background Style")
                                visible: Chiaki.settings.dx12Enabled === true && Chiaki.settings.dx12AmbientStyle !== undefined
                                model: [qsTr("Ambient Colors"), qsTr("Blurred Video"), qsTr("Edge Stretch")]
                                currentIndex: Chiaki.settings.dx12AmbientStyle ?? 2
                                onActivated: index => Chiaki.settings.dx12AmbientStyle = index
                            }
                        }

                        GeneralRow {
                            label: qsTr("Error Concealment (Hold Clean Frame)")
                            visible: Chiaki.settings.dx12Enabled === true && Chiaki.settings.dx12ErrorConcealment !== undefined
                            hint: qsTr("Hides macroblocks on loss")
                            C.CheckBox {
                                Accessible.name: qsTr("Error Concealment (Hold Clean Frame)")
                                visible: Chiaki.settings.dx12Enabled === true && Chiaki.settings.dx12ErrorConcealment !== undefined
                                checked: Chiaki.settings.dx12ErrorConcealment === true
                                onToggled: Chiaki.settings.dx12ErrorConcealment = checked
                            }
                        }

                        GeneralRow {
                            label: qsTr("Vulkan Deferred Swap:")
                            visible: Chiaki.settings.rendererBackend == 0
                            hint: qsTr("(Unchecked)")
                            C.CheckBox {
                                Accessible.name: qsTr("Vulkan Deferred Swap:")
                                checked: Chiaki.settings.vulkanDeferredSwap
                                onToggled: Chiaki.settings.vulkanDeferredSwap = checked
                                visible: Chiaki.settings.rendererBackend == 0
                            }
                        }

                        GeneralRow {
                            label: qsTr("Custom Renderer Settings")
                            visible: Chiaki.window.videoPreset == ChiakiWindow.VideoPreset.Custom
                            C.Button {
                                id: customRendererSettings
                                text: qsTr("Open")
                                onClicked: root.showPlaceboSettingsDialog()
                                Material.roundedScale: Material.SmallScale
                                visible: Chiaki.window.videoPreset == ChiakiWindow.VideoPreset.Custom
                            }
                        }

                        GeneralRow {
                            label: qsTr("Display Settings")
                            C.Button {
                                id: displaySettings
                                text: qsTr("Open")
                                onClicked: root.showDisplaySettingsDialog()
                                Material.roundedScale: Material.SmallScale
                                lastInFocusChain: true
                            }
                        }
                    }
                }
            }

            Item {
                // Stream
                Flickable {
                    id: streamFlick
                    anchors {
                        fill: parent
                        topMargin: 20
                        bottomMargin: 20
                    }
                    clip: true
                    contentWidth: Math.max(width, streamGrid.width)
                    contentHeight: streamGrid.height
                    flickableDirection: Flickable.AutoFlickIfNeeded
                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AlwaysOn
                        visible: streamFlick.contentHeight > streamFlick.height
                    }
                    GridLayout {
                        id: streamGrid
                        width: Math.max(360, Math.min(900, streamFlick.width - 30))
                        readonly property real labelWidth: width >= 540 ? 132 : 104
                        readonly property real fieldWidth: (width - labelWidth - 2 * columnSpacing) / 2
                        anchors {
                            top: parent.top
                            horizontalCenter: parent.horizontalCenter
                        }
                        columns: 3
                        rowSpacing: 10
                        columnSpacing: 12

                        Label {
                            Layout.alignment: Qt.AlignRight
                        Layout.preferredWidth: streamGrid.labelWidth
                            Layout.maximumWidth: streamGrid.labelWidth
                            horizontalAlignment: Text.AlignRight
                            wrapMode: Text.Wrap
                            text: qsTr("Settings for:")
                        }


                    C.ComboBox {
                        id: consoleSelection
                        Layout.preferredWidth: streamGrid.fieldWidth
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: streamGrid.fieldWidth
                        Layout.alignment: Qt.AlignLeft
                        model: [qsTr("PS4"), qsTr("PS5")]
                        currentIndex: selectedConsole
                        onActivated: (index) => selectedConsole = index
                        firstInFocusChain: true
                        KeyNavigation.priority: {
                            if(!popup.visible)
                                KeyNavigation.BeforeItem
                            else
                                KeyNavigation.AfterItem
                        }
                        KeyNavigation.down: {
                            if(selectedConsole == SettingsDialog.Console.PS4)
                                resolutionLocalPS4
                            else
                                resolutionLocalPS5

                        }

                    }

                    Item {}
                    Item {}

                    Label {
                        Layout.alignment: Qt.AlignCenter
                        text: qsTr("Local")
                    }

                    Label {
                        Layout.alignment: Qt.AlignCenter
                        text: qsTr("Remote")
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        Layout.preferredWidth: streamGrid.labelWidth
                        Layout.maximumWidth: streamGrid.labelWidth
                        horizontalAlignment: Text.AlignRight
                        wrapMode: Text.Wrap
                        text: qsTr("Resolution:")
                    }

                    C.ComboBox {
                        id: resolutionLocalPS4
                        Layout.preferredWidth: streamGrid.fieldWidth
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: streamGrid.fieldWidth
                        model: [qsTr("360p"), qsTr("540p"), qsTr("720p (Default)"), qsTr("1080p (PS5 and PS4 Pro)")]
                        currentIndex: Chiaki.settings.resolutionLocalPS4 - 1
                        onActivated: (index) => {
                            Chiaki.settings.resolutionLocalPS4 = index + 1
                            Chiaki.settings.bitrateLocalPS4 = 0
                        }
                        visible: selectedConsole == SettingsDialog.Console.PS4
                        KeyNavigation.right: resolutionRemotePS4
                        KeyNavigation.down: fpsLocalPS4
                        KeyNavigation.up: consoleSelection
                        KeyNavigation.priority: {
                            if(!popup.visible)
                                KeyNavigation.BeforeItem
                            else
                                KeyNavigation.AfterItem
                        }
                    }

                    C.ComboBox {
                        id: resolutionRemotePS4
                        Layout.preferredWidth: streamGrid.fieldWidth
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: streamGrid.fieldWidth
                        model: [qsTr("360p"), qsTr("540p"), qsTr("720p (Default)"), qsTr("1080p (PS5 and PS4 Pro)")]
                        currentIndex: Chiaki.settings.resolutionRemotePS4 - 1
                        onActivated: (index) => {
                            Chiaki.settings.resolutionRemotePS4 = index + 1
                            Chiaki.settings.bitrateRemotePS4 = 0
                        }
                        visible: selectedConsole == SettingsDialog.Console.PS4
                        KeyNavigation.left: resolutionLocalPS4
                        KeyNavigation.down: fpsRemotePS4
                        KeyNavigation.up: consoleSelection
                        KeyNavigation.priority: {
                            if(!popup.visible)
                                KeyNavigation.BeforeItem
                            else
                                KeyNavigation.AfterItem
                        }
                    }

                    C.ComboBox {
                        id: resolutionLocalPS5
                        Layout.preferredWidth: streamGrid.fieldWidth
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: streamGrid.fieldWidth
                        model: [qsTr("360p"), qsTr("540p"), qsTr("720p"), qsTr("1080p (Default)")]
                        currentIndex: Chiaki.settings.resolutionLocalPS5 - 1
                        onActivated: (index) => {
                            Chiaki.settings.resolutionLocalPS5 = index + 1
                            Chiaki.settings.bitrateLocalPS5 = 0
                        }
                        visible: selectedConsole == SettingsDialog.Console.PS5
                        KeyNavigation.right: resolutionRemotePS5
                        KeyNavigation.up: consoleSelection
                        KeyNavigation.down: fpsLocalPS5
                        KeyNavigation.priority: {
                            if(!popup.visible)
                                KeyNavigation.BeforeItem
                            else
                                KeyNavigation.AfterItem
                        }
                    }

                    C.ComboBox {
                        id: resolutionRemotePS5
                        Layout.preferredWidth: streamGrid.fieldWidth
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: streamGrid.fieldWidth
                        model: [qsTr("360p"), qsTr("540p"), qsTr("720p (Default)"), qsTr("1080p")]
                        currentIndex: Chiaki.settings.resolutionRemotePS5 - 1
                        onActivated: (index) => {
                            Chiaki.settings.resolutionRemotePS5 = index + 1
                            Chiaki.settings.bitrateRemotePS5 = 0
                        }
                        visible: selectedConsole == SettingsDialog.Console.PS5
                        KeyNavigation.left: resolutionLocalPS5
                        KeyNavigation.up: consoleSelection
                        KeyNavigation.down: fpsRemotePS5
                        KeyNavigation.priority: {
                            if(!popup.visible)
                                KeyNavigation.BeforeItem
                            else
                                KeyNavigation.AfterItem
                        }
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        Layout.preferredWidth: streamGrid.labelWidth
                        Layout.maximumWidth: streamGrid.labelWidth
                        horizontalAlignment: Text.AlignRight
                        wrapMode: Text.Wrap
                        text: qsTr("FPS:")
                    }

                    C.ComboBox {
                        id: fpsLocalPS4
                        Layout.preferredWidth: streamGrid.fieldWidth
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: streamGrid.fieldWidth
                        model: [qsTr("30 fps"), qsTr("60 fps (Default)")]
                        currentIndex: (Chiaki.settings.fpsLocalPS4 / 30) - 1
                        onActivated: (index) => Chiaki.settings.fpsLocalPS4 = (index + 1) * 30
                        visible: selectedConsole == SettingsDialog.Console.PS4
                        KeyNavigation.up: resolutionLocalPS4
                        KeyNavigation.right: fpsRemotePS4
                        KeyNavigation.down: bitrateLocalPS4
                        KeyNavigation.priority: {
                            if(!popup.visible)
                                KeyNavigation.BeforeItem
                            else
                                KeyNavigation.AfterItem
                        }
                    }

                    C.ComboBox {
                        id: fpsRemotePS4
                        Layout.preferredWidth: streamGrid.fieldWidth
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: streamGrid.fieldWidth
                        model: [qsTr("30 fps"), qsTr("60 fps (Default)")]
                        currentIndex: (Chiaki.settings.fpsRemotePS4 / 30) - 1
                        onActivated: (index) => Chiaki.settings.fpsRemotePS4 = (index + 1) * 30
                        visible: selectedConsole == SettingsDialog.Console.PS4
                        KeyNavigation.up: resolutionRemotePS4
                        KeyNavigation.left: fpsLocalPS4
                        KeyNavigation.down: bitrateRemotePS4
                        KeyNavigation.priority: {
                            if(!popup.visible)
                                KeyNavigation.BeforeItem
                            else
                                KeyNavigation.AfterItem
                        }
                    }

                    C.ComboBox {
                        id: fpsLocalPS5
                        Layout.preferredWidth: streamGrid.fieldWidth
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: streamGrid.fieldWidth
                        model: [qsTr("30 fps"), qsTr("60 fps (Default)")]
                        currentIndex: (Chiaki.settings.fpsLocalPS5 / 30) - 1
                        onActivated: (index) => Chiaki.settings.fpsLocalPS5 = (index + 1) * 30
                        visible: selectedConsole == SettingsDialog.Console.PS5
                        KeyNavigation.up: resolutionLocalPS5
                        KeyNavigation.right: fpsRemotePS5
                        KeyNavigation.down: bitrateLocalPS5
                        KeyNavigation.priority: {
                            if(!popup.visible)
                                KeyNavigation.BeforeItem
                            else
                                KeyNavigation.AfterItem
                        }
                    }

                    C.ComboBox {
                        id: fpsRemotePS5
                        Layout.preferredWidth: streamGrid.fieldWidth
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: streamGrid.fieldWidth
                        model: [qsTr("30 fps"), qsTr("60 fps (Default)")]
                        currentIndex: (Chiaki.settings.fpsRemotePS5 / 30) - 1
                        onActivated: (index) => Chiaki.settings.fpsRemotePS5 = (index + 1) * 30
                        visible: selectedConsole == SettingsDialog.Console.PS5
                        KeyNavigation.up: resolutionRemotePS5
                        KeyNavigation.left: fpsLocalPS5
                        KeyNavigation.down: bitrateRemotePS5
                        KeyNavigation.priority: {
                            if(!popup.visible)
                                KeyNavigation.BeforeItem
                            else
                                KeyNavigation.AfterItem
                        }
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        Layout.preferredWidth: streamGrid.labelWidth
                        Layout.maximumWidth: streamGrid.labelWidth
                        horizontalAlignment: Text.AlignRight
                        wrapMode: Text.Wrap
                        text: qsTr("Bitrate:")
                    }

                    C.Slider {
                        id: bitrateLocalPS4
                        visible: selectedConsole == SettingsDialog.Console.PS4
                        property var bitrate: {
                            var rate = 0;
                            switch (Chiaki.settings.resolutionLocalPS4) {
                            case 1: rate = 2; break; // 360p
                            case 2: rate = 6; break; // 540p
                            case 3: rate = 10; break; // 720p
                            case 4: rate = 15; break; // 1080p
                            }
                            return rate;
                        }
                        Layout.preferredWidth: streamGrid.fieldWidth
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: streamGrid.fieldWidth
                        from: 2
                        to: 100
                        stepSize: 1
                        value: Chiaki.settings.bitrateLocalPS4 / 1000 ? (Chiaki.settings.bitrateLocalPS4 / 1000) : bitrate
                        onMoved: Chiaki.settings.bitrateLocalPS4 = value * 1000;
                        KeyNavigation.up: fpsLocalPS4
                        KeyNavigation.priority: KeyNavigation.BeforeItem
                        Layout.preferredHeight: 56
                        bottomPadding: 22
                        Label {
                            anchors {
                                bottom: parent.bottom
                                left: parent.left
                                right: parent.right
                            }
                            font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            text: parent.value + qsTr(" Mbps") + qsTr(" (%1 Mbps)").arg(parent.bitrate.toFixed(0))
                        }
                    }

                    C.Slider {
                        id: bitrateRemotePS4
                        visible: selectedConsole == SettingsDialog.Console.PS4
                        property var bitrate: {
                            var rate = 0;
                            switch (Chiaki.settings.resolutionRemotePS4) {
                            case 1: rate = 2; break; // 360p
                            case 2: rate = 6; break; // 540p
                            case 3: rate = 10; break; // 720p
                            case 4: rate = 15; break; // 1080p
                            }
                            return rate;
                        }
                        Layout.preferredWidth: streamGrid.fieldWidth
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: streamGrid.fieldWidth
                        from: 2
                        to: 100
                        stepSize: 1
                        value: Chiaki.settings.bitrateRemotePS4 / 1000 ? (Chiaki.settings.bitrateRemotePS4 / 1000) : bitrate
                        onMoved: Chiaki.settings.bitrateRemotePS4 = value * 1000;
                        KeyNavigation.up: fpsRemotePS4
                        KeyNavigation.priority: KeyNavigation.BeforeItem
                        lastInFocusChain: true

                        Layout.preferredHeight: 56
                        bottomPadding: 22
                        Label {
                            anchors {
                                bottom: parent.bottom
                                left: parent.left
                                right: parent.right
                            }
                            font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            text: parent.value + qsTr(" Mbps") + qsTr(" (%1 Mbps)").arg(parent.bitrate.toFixed(0))
                        }
                    }

                    C.Slider {
                        id: bitrateLocalPS5
                        visible: selectedConsole == SettingsDialog.Console.PS5
                        property var bitrate: {
                            var rate = 0;
                            switch (Chiaki.settings.resolutionLocalPS5) {
                            case 1: rate = 2; break; // 360p
                            case 2: rate = 6; break; // 540p
                            case 3: rate = 10; break; // 720p
                            case 4: rate = 15; break; // 1080p
                            }
                            return rate;
                        }
                        Layout.preferredWidth: streamGrid.fieldWidth
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: streamGrid.fieldWidth
                        from: 2
                        to: 100
                        stepSize: 1
                        value: Chiaki.settings.bitrateLocalPS5 / 1000 ? (Chiaki.settings.bitrateLocalPS5 / 1000) : bitrate
                        onMoved: Chiaki.settings.bitrateLocalPS5 = value * 1000;
                        KeyNavigation.up: fpsLocalPS5
                        KeyNavigation.down: codecLocalPS5
                        KeyNavigation.priority: KeyNavigation.BeforeItem

                        Layout.preferredHeight: 56
                        bottomPadding: 22
                        Label {
                            anchors {
                                bottom: parent.bottom
                                left: parent.left
                                right: parent.right
                            }
                            font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            text: parent.value + qsTr(" Mbps") + qsTr(" (%1 Mbps)").arg(parent.bitrate.toFixed(0))
                        }
                    }

                    C.Slider {
                        id: bitrateRemotePS5
                        visible: selectedConsole == SettingsDialog.Console.PS5
                        property var bitrate: {
                            var rate = 0;
                            switch (Chiaki.settings.resolutionRemotePS5) {
                            case 1: rate = 2; break; // 360p
                            case 2: rate = 6; break; // 540p
                            case 3: rate = 10; break; // 720p
                            case 4: rate = 15; break; // 1080p
                            }
                            return rate;
                        }
                        Layout.preferredWidth: streamGrid.fieldWidth
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: streamGrid.fieldWidth
                        from: 2
                        to: 100
                        stepSize: 1
                        value: Chiaki.settings.bitrateRemotePS5 / 1000 ? (Chiaki.settings.bitrateRemotePS5 / 1000) : bitrate
                        onMoved: Chiaki.settings.bitrateRemotePS5 = value * 1000;
                        KeyNavigation.up: fpsRemotePS5
                        KeyNavigation.down: codecRemotePS5
                        KeyNavigation.priority: KeyNavigation.BeforeItem
                        lastInFocusChain: true

                        Layout.preferredHeight: 56
                        bottomPadding: 22
                        Label {
                            anchors {
                                bottom: parent.bottom
                                left: parent.left
                                right: parent.right
                            }
                            font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            text: parent.value + qsTr(" Mbps") + qsTr(" (%1 Mbps)").arg(parent.bitrate.toFixed(0))
                        }
                    }

                    Label {
                        Layout.alignment: Qt.AlignRight
                        Layout.preferredWidth: streamGrid.labelWidth
                        Layout.maximumWidth: streamGrid.labelWidth
                        horizontalAlignment: Text.AlignRight
                        wrapMode: Text.Wrap
                        text: qsTr("Codec:")
                        visible: selectedConsole == SettingsDialog.Console.PS5
                    }

                    C.ComboBox {
                        id: codecLocalPS5
                        Layout.preferredWidth: streamGrid.fieldWidth
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: streamGrid.fieldWidth
                        property bool openGlBackend: Chiaki.settings.rendererBackend === 1
						model: openGlBackend ? [qsTr("H264"), qsTr("H265 (Default)")] : [qsTr("H264"), qsTr("H265 (Default)"), qsTr("H265 HDR")]
                        currentIndex: Chiaki.settings.codecLocalPS5
                        onActivated: (index) => Chiaki.settings.codecLocalPS5 = index
                        visible: selectedConsole == SettingsDialog.Console.PS5
                        Keys.onReturnPressed: {
                            if (popup.visible) {
                                activated(highlightedIndex);
                                popup.close();
                            } else
                                popup.open();
                        }
                        KeyNavigation.up: bitrateLocalPS5
                        KeyNavigation.right: codecRemotePS5
                        KeyNavigation.down: codecLocalPS5
                        KeyNavigation.priority: {
                            if(!popup.visible)
                                KeyNavigation.BeforeItem
                            else
                                KeyNavigation.AfterItem
                        }
                    }

                        C.ComboBox {
                            id: codecRemotePS5
                            Layout.preferredWidth: streamGrid.fieldWidth
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: streamGrid.fieldWidth
                        property bool openGlBackend: Chiaki.settings.rendererBackend === 1
                        model: openGlBackend ? [qsTr("H264"), qsTr("H265 (Default)")] : [qsTr("H264"), qsTr("H265 (Default)"), qsTr("H265 HDR")]
                            currentIndex: Chiaki.settings.codecRemotePS5
                            onActivated: (index) => Chiaki.settings.codecRemotePS5 = index
                            visible: selectedConsole == SettingsDialog.Console.PS5
                            lastInFocusChain: true
                            Keys.onReturnPressed: {
                                if (popup.visible) {
                                    activated(highlightedIndex);
                                    popup.close();
                                } else
                                    popup.open();
                            }
                            KeyNavigation.up: bitrateRemotePS5
                            KeyNavigation.left: codecLocalPS5
                            KeyNavigation.priority: {
                                if(!popup.visible)
                                    KeyNavigation.BeforeItem
                                else
                                    KeyNavigation.AfterItem
                            }
                        }
                    }
                }
            }

            Item {
                // Audio and Wifi
                Flickable {
                    id: audiowifiFlick
                    anchors {
                        fill: parent
                        topMargin: 20
                        bottomMargin: 20
                    }
                    clip: true
                    contentWidth: width
                    contentHeight: audiowifigrid.height + audiowifigrid.y + 12
                    flickableDirection: Flickable.AutoFlickIfNeeded
                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                    ScrollBar.vertical: ScrollBar {
                        id: audiowifiScrollbar
                        policy: ScrollBar.AlwaysOn
                        visible: audiowifiFlick.contentHeight > audiowifiFlick.height
                    }
                    ColumnLayout {
                        id: audiowifigrid
                        width: Math.max(0, Math.min(900, audiowifiFlick.width - 36))

                        spacing: 9

                        onVisibleChanged: if (visible) Chiaki.settings.refreshAudioDevices()

                        anchors {
                            top: parent.top
                            horizontalCenter: parent.horizontalCenter
                        }
                        GeneralRow {
                            label: qsTr("Output Device:")
                            hint: qsTr("(Auto)")
                            SettingsPicker {
                                Accessible.name: qsTr("Output Device:")
                                id: audioOutDevice

                                popup.x: (width - popup.width) / 2
                                popup.width: Math.min(dialog.width - 20, 700)
                                popup.font.pixelSize: 16
                                firstInFocusChain: true
                                model: [qsTr("Auto")].concat(Chiaki.settings.availableAudioOutDevices)
                                currentIndex: Math.max(0, model.indexOf(Chiaki.settings.audioOutDevice))
                                onActivated: (index) => Chiaki.settings.audioOutDevice = index ? model[index] : ""
                                KeyNavigation.down: audioInDevice
                                KeyNavigation.priority: popup.visible ? KeyNavigation.AfterItem : KeyNavigation.BeforeItem
                            }
                        }

                        GeneralRow {
                            label: qsTr("Input Device:")
                            hint: qsTr("(Auto)")
                            SettingsPicker {
                                Accessible.name: qsTr("Input Device:")
                                id: audioInDevice

                                popup.x: (width - popup.width) / 2
                                popup.width: Math.min(dialog.width - 20, 700)
                                popup.font.pixelSize: 16
                                model: [qsTr("Auto")].concat(Chiaki.settings.availableAudioInDevices)
                                currentIndex: Math.max(0, model.indexOf(Chiaki.settings.audioInDevice))
                                onActivated: (index) => Chiaki.settings.audioInDevice = index ? model[index] : ""
                                KeyNavigation.up: audioOutDevice
                                KeyNavigation.down: audioBufferSizeSlider
                                KeyNavigation.priority: popup.visible ? KeyNavigation.AfterItem : KeyNavigation.BeforeItem
                            }
                        }

                        GeneralRow {
                            label: qsTr("Audio Buffer Size:")
                            hint: qsTr("(50 ms)")
                            SettingsSlider {
                                Accessible.name: qsTr("Audio Buffer Size:")
                                id: audioBufferSizeSlider

                                from: 1
                                to: 10
                                stepSize: 1
                                value: Chiaki.settings.audioBufferSize / 1920 ? (Chiaki.settings.audioBufferSize / 1920) : 5
                                onMoved: Chiaki.settings.audioBufferSize = value * 1920;
                                KeyNavigation.up: audioInDevice

                                Label {
                                    anchors {
                                        left: parent.right
                                        verticalCenter: parent.verticalCenter
                                        leftMargin: 10
                                    }
                                    text: {
                                        (parent.value * 10).toFixed(0) + qsTr(" ms")
                                    }
                                }
                            }
                        }

                        GeneralRow {
                            label: qsTr("Audio Buffer Preset:")
                            hint: qsTr("Raise if audio crackles")
                            SettingsFlow {
                                spacing: 6
                                C.Button {
                                    text: qsTr("Low (20 ms)")
                                    onClicked: Chiaki.settings.audioBufferSize = 2 * 1920
                                    Material.roundedScale: Material.SmallScale
                                }
                                C.Button {
                                    text: qsTr("Medium (50 ms)")
                                    onClicked: Chiaki.settings.audioBufferSize = 5 * 1920
                                    Material.roundedScale: Material.SmallScale
                                }
                                C.Button {
                                    text: qsTr("Large (80 ms)")
                                    onClicked: Chiaki.settings.audioBufferSize = 8 * 1920
                                    Material.roundedScale: Material.SmallScale
                                }
                                C.Button {
                                    text: qsTr("Very Large (100 ms)")
                                    onClicked: Chiaki.settings.audioBufferSize = 10 * 1920
                                    Material.roundedScale: Material.SmallScale
                                }
                            }
                        }

                        GeneralRow {
                            label: qsTr("Audio Volume:")
                            hint: qsTr("(100%)")
                            SettingsSlider {
                                Accessible.name: qsTr("Audio Volume:")

                                from: 0
                                to: 128
                                stepSize: 1
                                value: Chiaki.settings.audioVolume
                                onMoved: Chiaki.settings.audioVolume = value

                                Label {
                                    anchors {
                                        left: parent.right
                                        verticalCenter: parent.verticalCenter
                                        leftMargin: 10
                                    }
                                    text: {
                                        ((parent.value / 128.0) * 100).toFixed(0) + qsTr("% volume")
                                    }
                                }
                            }
                        }

                        GeneralRow {
                            label: qsTr("Start Mic Unmuted:")
                            hint: qsTr("(Unchecked)")
                            C.CheckBox {
                                Accessible.name: qsTr("Start Mic Unmuted:")
                                checked: Chiaki.settings.startMicUnmuted
                                onToggled: Chiaki.settings.startMicUnmuted = checked
                            }
                        }

                        GeneralRow {
                            label: qsTr("Speech Processing:")
                            visible: typeof Chiaki.settings.speechProcessing !== "undefined"
                            hint: qsTr("(Unchecked)")
                            C.CheckBox {
                                Accessible.name: qsTr("Speech Processing:")
                                text: qsTr("Noise suppression + echo cancellation")
                                checked: Chiaki.settings.speechProcessing
                                onToggled: Chiaki.settings.speechProcessing = !Chiaki.settings.speechProcessing
                                visible: typeof Chiaki.settings.speechProcessing !== "undefined"
                            }
                        }

                        GeneralRow {
                            label: qsTr("Noise To Suppress:")
                            visible: if (typeof Chiaki.settings.speechProcessing !== "undefined") {Chiaki.settings.speechProcessing} else {false}
                            hint: qsTr("(6 dB)")
                            SettingsSlider {
                                Accessible.name: qsTr("Noise To Suppress:")

                                from: 0
                                to: 60
                                stepSize: 1
                                visible: if (typeof Chiaki.settings.speechProcessing !== "undefined") {Chiaki.settings.speechProcessing} else {false}
                                value: Chiaki.settings.noiseSuppressLevel
                                onMoved: Chiaki.settings.noiseSuppressLevel = value

                                Label {
                                    anchors {
                                        left: parent.right
                                        verticalCenter: parent.verticalCenter
                                        leftMargin: 10
                                    }
                                    text: qsTr("%1 dB").arg(parent.value)
                                }
                            }
                        }

                        GeneralRow {
                            label: qsTr("Echo To Suppress:")
                            visible: if (typeof Chiaki.settings.speechProcessing !== "undefined") {Chiaki.settings.speechProcessing} else {false}
                            hint: qsTr("(30 dB)")
                            SettingsSlider {
                                Accessible.name: qsTr("Echo To Suppress:")

                                from: 0
                                to: 60
                                stepSize: 1
                                value: Chiaki.settings.echoSuppressLevel
                                visible: if (typeof Chiaki.settings.speechProcessing !== "undefined") {Chiaki.settings.speechProcessing} else {false}
                                onMoved: Chiaki.settings.echoSuppressLevel = value

                                Label {
                                    anchors {
                                        left: parent.right
                                        verticalCenter: parent.verticalCenter
                                        leftMargin: 10
                                    }
                                    text: qsTr("%1 dB").arg(parent.value)
                                }
                            }
                        }

                        GeneralRow {
                            label: qsTr("Weak Wifi Notification:")
                            hint: qsTr("(3%)")
                            SettingsSlider {
                                Accessible.name: qsTr("Weak Wifi Notification:")

                                from: 0
                                to: 100
                                stepSize: 1
                                value: Chiaki.settings.wifiDroppedNotif
                                onMoved: Chiaki.settings.wifiDroppedNotif = value

                                Label {
                                    anchors {
                                        left: parent.right
                                        verticalCenter: parent.verticalCenter
                                        leftMargin: 10
                                    }
                                    text: qsTr(">= %1% dropped packets").arg(parent.value)
                                }
                            }
                        }

                        GeneralRow {
                            label: qsTr("Packet Loss Reported Max:")
                            hint: qsTr("(5%)")
                            SettingsSlider {
                                Accessible.name: qsTr("Packet Loss Reported Max:")

                                from: 0
                                to: 100
                                stepSize: 1
                                value: Chiaki.settings.packetLossReportedMax
                                onMoved: Chiaki.settings.packetLossReportedMax = value

                                Label {
                                    anchors {
                                        left: parent.right
                                        verticalCenter: parent.verticalCenter
                                        leftMargin: 10
                                    }
                                    text: qsTr("%1% packet loss").arg(parent.value)
                                }
                            }
                        }

                        GeneralRow {
                            label: qsTr("Request Keyframe After Unrecoverable Video Loss")
                            hint: qsTr("(Unchecked)")
                            C.CheckBox {
                                Accessible.name: qsTr("Request Keyframe After Unrecoverable Video Loss")
                                checked: Chiaki.settings.iDROnFECFailureEnabled
                                onToggled: Chiaki.settings.iDROnFECFailureEnabled = !Chiaki.settings.iDROnFECFailureEnabled
                            }
                        }

                        GeneralRow {
                            label: qsTr("Show Stream Stats During Gameplay")
                            hint: qsTr("(Unchecked)")
                            C.CheckBox {
                                Accessible.name: qsTr("Show Stream Stats During Gameplay")
                                lastInFocusChain: true
                                checked: Chiaki.settings.showStreamStats
                                onToggled: Chiaki.settings.showStreamStats = !Chiaki.settings.showStreamStats
                            }
                        }
                    }
                }
            }

            Item {
                // Consoles
                Flickable {
                    id: consolesFlick
                    anchors {
                        fill: parent
                        topMargin: 20
                        bottomMargin: 20
                    }
                    clip: true
                    contentWidth: Math.max(width, consolesLayout.width)
                    contentHeight: consolesLayout.height
                    flickableDirection: Flickable.AutoFlickIfNeeded
                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AlwaysOn
                        visible: consolesFlick.contentHeight > consolesFlick.height
                    }
                    ColumnLayout {
                        id: consolesLayout
                        width: Math.max(360, Math.min(900, consolesFlick.width - 30))
                        anchors {
                            top: parent.top
                            horizontalCenter: parent.horizontalCenter
                        }
                        spacing: 10

                        C.Button {
                            id: registerNewButton
                            Layout.alignment: Qt.AlignHCenter
                            Layout.topMargin: 10
                            firstInFocusChain: true
                            text: qsTr("Register New")
                            onClicked: root.showRegistDialog("255.255.255.255", true)
                            Material.roundedScale: Material.SmallScale
                        }

                        Label {
                            id: consolesLabel
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Registered Consoles")
                            font.bold: true
                        }

                        ListView {
                            id: consolesView
                            Layout.fillWidth: true
                            Layout.preferredHeight: 170
                            keyNavigationEnabled: false
                            ScrollBar.vertical: ScrollBar {
                                id: consolesScrollbar
                                policy: ScrollBar.AlwaysOn
                                visible: consolesView.contentHeight > consolesView.height
                            }
                            clip: true
                            model: Chiaki.settings.registeredHosts
                            delegate: ItemDelegate {
                                id: registeredConsole
                                readonly property bool compact: width < 560
                                text: "%1 (%2, %3)".arg(Chiaki.settings.streamerMode ? qsTr("hidden") : modelData.mac).arg(modelData.ps5 ? "PS5" : "PS4").arg(modelData.name)
                                height: 80
                                width: ListView.view.width
                                leftPadding: compact ? 12 : autoConnectButton.width + 30
                                rightPadding: compact ? 12 : deleteConsole.width + 30
                                bottomPadding: compact ? 40 : topPadding
                                contentItem: Label {
                                    text: registeredConsole.text
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                }
                                ToolTip.visible: hovered && contentItem.truncated
                                ToolTip.text: text

                                CheckBox {
                                    property bool firstInFocusChain: false
                                    property bool lastInFocusChain: false
                                    property bool lastDownInFocusChain: index > consolesView.count + hiddenConsolesView.count - 2

                                    id: autoConnectButton
                                    anchors {
                                        left: parent.left
                                        verticalCenter: registeredConsole.compact ? undefined : parent.verticalCenter
                                        bottom: registeredConsole.compact ? parent.bottom : undefined
                                        leftMargin: 12
                                    }
                                    text: qsTr("Auto-Connect")
                                    checked: Chiaki.settings.autoConnectMac == modelData.mac
                                    onToggled: Chiaki.settings.autoConnectMac = checked ? modelData.mac : "";

                                    Keys.onPressed: (event) => {
                                        switch (event.key) {
                                        case Qt.Key_Right:
                                            if (!lastInFocusChain) {
                                                let item = nextItemInFocusChain();
                                                if (item)
                                                    item.forceActiveFocus(Qt.TabFocusReason);
                                                event.accepted = true;
                                            }
                                            break;
                                        case Qt.Key_Up:
                                            if (!firstInFocusChain) {
                                                let item = nextItemInFocusChain(false);
                                                if (item) {
                                                    item.forceActiveFocus(Qt.TabFocusReason);
                                                    dialog.ensureItemVisibleInFlick(consolesView, item);
                                                }
                                                let count = index > 0 ? 2: 0;
                                                for(var i = 0; i < count; i++)
                                                {
                                                    let item2 = item.nextItemInFocusChain(false);
                                                    if (item)
                                                    {
                                                        item.forceActiveFocus(Qt.TabFocusReason);
                                                        dialog.ensureItemVisibleInFlick(consolesView, item);
                                                        item = item2;
                                                    }
                                                }
                                                event.accepted = true;
                                            }
                                            break;
                                        case Qt.Key_Down:
                                            if (!lastDownInFocusChain) {
                                                let item = nextItemInFocusChain();
                                                if (item) {
                                                    item.forceActiveFocus(Qt.TabFocusReason);
                                                    dialog.ensureItemVisibleInFlick(consolesView, item);
                                                }
                                                let count = 2;
                                                for(var i = 0; i < count; i++)
                                                {
                                                    let item2 = item.nextItemInFocusChain();
                                                    if (item)
                                                    {
                                                        item.forceActiveFocus(Qt.TabFocusReason);
                                                        dialog.ensureItemVisibleInFlick(consolesView, item);
                                                        item = item2;
                                                    }
                                                }
                                                event.accepted = true;
                                            }
                                            break;
                                        case Qt.Key_Return:
                                            if (visualFocus) {
                                                toggle();
                                                toggled();
                                            }
                                            event.accepted = true;
                                            break;
                                        }
                                    }
                                }

                                Button {
                                    id: deleteConsole
                                    property bool firstInFocusChain: false
                                    property bool lastInFocusChain: index > consolesView.count + hiddenConsolesView.count - 2
                                    Material.background: visualFocus ? Material.accent : undefined

                                    Component.onDestruction: {
                                        if (visualFocus) {
                                            let item = nextItemInFocusChain();
                                            if (item)
                                                item.forceActiveFocus(Qt.TabFocusReason);
                                        }
                                    }
                                    Keys.onPressed: (event) => {
                                        switch (event.key) {
                                            case Qt.Key_Left:
                                                if (!firstInFocusChain) {
                                                    let item = nextItemInFocusChain(false);
                                                    if (item)
                                                        item.forceActiveFocus(Qt.TabFocusReason);
                                                    event.accepted = true;
                                                }
                                                break;
                                            case Qt.Key_Up:
                                                if (!firstInFocusChain)
                                                {
                                                    let item = nextItemInFocusChain(false);
                                                    if (item) {
                                                        item.forceActiveFocus(Qt.TabFocusReason);
                                                        dialog.ensureItemVisibleInFlick(consolesView, item);
                                                    }
                                                    let count = 2;
                                                    for(var i = 0; i < count; i++)
                                                    {
                                                        let item2 = item.nextItemInFocusChain(false);
                                                        if (item)
                                                        {
                                                            item.forceActiveFocus(Qt.TabFocusReason);
                                                            dialog.ensureItemVisibleInFlick(consolesView, item);
                                                            item = item2;
                                                        }
                                                    }
                                                    event.accepted = true;
                                                }
                                                break;
                                            case Qt.Key_Down:
                                                if (!lastInFocusChain) {
                                                    let item = nextItemInFocusChain();
                                                    if (item) {
                                                        item.forceActiveFocus(Qt.TabFocusReason);
                                                        dialog.ensureItemVisibleInFlick(consolesView, item);
                                                    }
                                                    let count = index < consolesView.count - 1 ? 2: 0;
                                                    for(var i = 0; i < count; i++)
                                                    {
                                                        let item2 = item.nextItemInFocusChain();
                                                        if (item)
                                                        {
                                                            item.forceActiveFocus(Qt.TabFocusReason);
                                                            dialog.ensureItemVisibleInFlick(consolesView, item);
                                                            item = item2;
                                                        }
                                                    }
                                                    event.accepted = true;
                                                }
                                                break;
                                            case Qt.Key_Return:
                                                if (visualFocus) {
                                                    clicked();
                                                }
                                                event.accepted = true;
                                                break;
                                        }
                                    }
                                    anchors {
                                        right: parent.right
                                        verticalCenter: registeredConsole.compact ? undefined : parent.verticalCenter
                                        bottom: registeredConsole.compact ? parent.bottom : undefined
                                        rightMargin: 12
                                    }
                                    text: qsTr("Delete")
                                    onClicked: root.showConfirmDialog(qsTr("Delete Console"), qsTr("Are you sure you want to delete this console?"), () => Chiaki.settings.deleteRegisteredHost(index));
                                    Material.roundedScale: Material.SmallScale
                                    Material.accent: Material.Red
                                }
                            }
                        }

                        Label {
                            id: hiddenConsolesLabel
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Hidden Consoles")
                            font.bold: true
                        }
                        ListView {
                            id: hiddenConsolesView
                            Layout.alignment: Qt.AlignHCenter
                            keyNavigationEnabled: false
                            Layout.fillWidth: true
                            Layout.preferredHeight: 170
                            clip: true
                            ScrollBar.vertical: ScrollBar {
                                id: hiddenConsolesScrollbar
                                policy: ScrollBar.AlwaysOn
                                visible: hiddenConsolesView.contentHeight > hiddenConsolesView.height
                            }
                            model: Chiaki.hiddenHosts
                            delegate: ItemDelegate {
                                id: hiddenConsole
                                text: "%1 (%2)".arg(Chiaki.settings.streamerMode ? qsTr("hidden") : modelData.mac).arg(modelData.name)
                                height: 80
                                width: ListView.view.width
                                rightPadding: unhideConsole.width + 28
                                contentItem: Label {
                                    text: hiddenConsole.text
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                }
                                ToolTip.visible: hovered && contentItem.truncated
                                ToolTip.text: text

                                Button {
                                    id: unhideConsole
                                    property bool firstInFocusChain: false
                                    property bool lastInFocusChain: index > hiddenConsolesView.count - 2
                                    Material.background: visualFocus ? Material.accent : undefined

                                    Component.onDestruction: {
                                        if (visualFocus) {
                                            let item = nextItemInFocusChain();
                                            if (item)
                                                item.forceActiveFocus(Qt.TabFocusReason);
                                        }
                                    }
                                    Keys.onPressed: (event) => {
                                        switch (event.key) {
                                            case Qt.Key_Up:
                                                if (!firstInFocusChain)
                                                {
                                                    let item = nextItemInFocusChain(false);
                                                    if (item) {
                                                        item.forceActiveFocus(Qt.TabFocusReason);
                                                        dialog.ensureItemVisibleInFlick(hiddenConsolesView, item);
                                                    }
                                                    event.accepted = true;
                                                }
                                                break;
                                            case Qt.Key_Down:
                                                if (!lastInFocusChain) {
                                                    let item = nextItemInFocusChain();
                                                    if (item) {
                                                        item.forceActiveFocus(Qt.TabFocusReason);
                                                        dialog.ensureItemVisibleInFlick(hiddenConsolesView, item);
                                                    }
                                                    event.accepted = true;
                                                }
                                                break;
                                            case Qt.Key_Return:
                                                if (visualFocus) {
                                                    clicked();
                                                }
                                                event.accepted = true;
                                                break;
                                        }
                                    }
                                    anchors {
                                        right: parent.right
                                        verticalCenter: parent.verticalCenter
                                        rightMargin: 20
                                    }
                                    text: qsTr("Unhide")
                                    onClicked: root.showConfirmDialog(qsTr("Unhide Console"), qsTr("Are you sure you want to unhide this console?"), () => Chiaki.unhideHost(modelData.mac));
                                    Material.roundedScale: Material.SmallScale
                                    Material.accent: Material.Red
                                }
                            }
                        }
                    }
                }
            }

            Item {
                // Keys
                id: controllerMapping
                Flickable {
                    id: keysFlick
                    anchors {
                        fill: parent
                        topMargin: 20
                        bottomMargin: 20
                    }
                    clip: true
                    contentWidth: Math.max(width, keysGrid.width)
                    contentHeight: keysGrid.height
                    flickableDirection: Flickable.AutoFlickIfNeeded
                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                    function ensureItemVisible(item) {
                        dialog.ensureItemVisibleInFlick(keysFlick, item);
                    }
                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AlwaysOn
                        visible: keysFlick.contentHeight > keysFlick.height
                    }
                    ColumnLayout {
                        id: keysGrid
                        width: Math.max(0, Math.min(900, keysFlick.width - 28))
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 12

                        SettingsFlow {
                            C.Button {
                                id: resetAllKeys
                                text: qsTr("Reset All Keys")
                                firstInFocusChain: true
                                onClicked: Chiaki.settings.clearKeyMapping()
                            }
                            C.CheckBox {
                                text: qsTr("Keyboard as controller")
                                checked: Chiaki.settings.keyboardEnabled
                                onToggled: Chiaki.settings.keyboardEnabled = checked
                            }
                            C.CheckBox {
                                text: qsTr("Enable Mouse Touchpad")
                                checked: Chiaki.settings.mouseTouchEnabled
                                onToggled: Chiaki.settings.mouseTouchEnabled = checked
                            }
                            C.Button {
                                visible: typeof Chiaki.settings.applyPxPlayKeyboardPreset === "function"
                                text: qsTr("Apply PXPlay Preset (WASD + IJKL)")
                                onClicked: Chiaki.settings.applyPxPlayKeyboardPreset()
                            }
                        }
                        GridLayout {
                            id: mappingsGrid
                            Layout.fillWidth: true
                            columns: width >= 760 ? 3 : width >= 460 ? 2 : 1
                            columnSpacing: 16
                            rowSpacing: 4
                            Repeater {
                                id: chiakiKeys
                                model: Chiaki.settings.controllerMapping

                                RowLayout {
                                        property alias mappingButton: keyButton
                                    Layout.fillWidth: true
                                    spacing: 6

                                    Label {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        wrapMode: Text.Wrap
                                        text: modelData.buttonName
                                    }

                                    Button {
                                        id: keyButton
                                        Layout.preferredWidth: 90
                                        Layout.preferredHeight: 36
                                        text: modelData.keyName
                                        Material.roundedScale: Material.SmallScale
                                        Material.background: visualFocus ? Material.accent : undefined
                                        onActiveFocusChanged: if (activeFocus) keysFlick.ensureItemVisible(this)
                                        Component.onDestruction: {
                                            if (visualFocus) {
                                                let item = nextItemInFocusChain();
                                                if (item)
                                                    item.forceActiveFocus(Qt.TabFocusReason);
                                            }
                                        }
                                        onClicked: {
                                            keyDialog.show({
                                                value: modelData.buttonValue,
                                                mappingIndex: index,
                                                callback: (name) => text = name,
                                            });
                                        }
                                        Keys.onPressed: (event) => {
                                            const columns = mappingsGrid.columns;
                                            const column = index % columns;
                                            let target;
                                            switch (event.key) {
                                            case Qt.Key_Left:
                                                if (column > 0)
                                                    target = chiakiKeys.itemAt(index - 1)?.mappingButton;
                                                break;
                                            case Qt.Key_Right:
                                                if (column + 1 < columns)
                                                    target = chiakiKeys.itemAt(index + 1)?.mappingButton;
                                                break;
                                            case Qt.Key_Up:
                                                target = index < columns
                                                    ? resetAllKeys : chiakiKeys.itemAt(index - columns)?.mappingButton;
                                                break;
                                            case Qt.Key_Down:
                                                target = chiakiKeys.itemAt(index + columns)?.mappingButton;
                                                break;
                                            case Qt.Key_Return:
                                                if (visualFocus) {
                                                    clicked();
                                                    event.accepted = true;
                                                }
                                                return;
                                            }
                                            if (target) {
                                                target.forceActiveFocus(Qt.TabFocusReason);
                                                event.accepted = true;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Item {
                // Controllers
                Flickable {
                    id: controllersFlick
                    anchors {
                        fill: parent
                        topMargin: 20
                        bottomMargin: 20
                    }
                    clip: true
                    contentWidth: Math.max(width, controllersLayout.width)
                    contentHeight: controllersLayout.height
                    flickableDirection: Flickable.AutoFlickIfNeeded
                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                    ScrollBar.vertical: ScrollBar {
                        id: controllersScrollbar
                        policy: ScrollBar.AlwaysOn
                        visible: controllersFlick.contentHeight > controllersFlick.height
                    }
                    ColumnLayout {
                        id: controllersLayout
                        width: Math.max(0, Math.min(900, controllersFlick.width - 36))
                        anchors {
                            top: parent.top
                            horizontalCenter: parent.horizontalCenter
                        }
                        spacing: 10
                        C.Button {
                            Layout.alignment: Qt.AlignHCenter
                            id: controllerMappingChange
                            firstInFocusChain: true
                            text: qsTr("Change Controller Mapping")
                            onClicked: controllerMappingDialog.show({
                                reset: false
                            });
                        }
                        C.Button {
                            Layout.alignment: Qt.AlignHCenter
                            id: controllerMappingReset
                            text: qsTr("Reset Controller Mapping")
                            onClicked: controllerMappingDialog.show({
                                reset: true
                            });
                        }

                        GeneralRow {
                            label: qsTr("Background Controller Events:")
                            hint: qsTr("Process controller input when application is in background") + " " + qsTr("(Checked)")
                            C.CheckBox {
                                Accessible.name: qsTr("Background Controller Events:")
                                id: backgroundController
                                checked: {
                                    Chiaki.settings.allowJoystickBackgroundEvents
                                }
                                onToggled: Chiaki.settings.allowJoystickBackgroundEvents = checked
                            }
                        }
                        GeneralRow {
                            label: qsTr("Controller Polling Rate:")
                            visible: Chiaki.settings.controllerPollingMs !== undefined
                            SettingsPicker {
                                Accessible.name: qsTr("Controller Polling Rate:")
                                model: [qsTr("Normal (125 Hz / 8 ms)"), qsTr("High (250 Hz / 4 ms)"), qsTr("Very High (500 Hz / 2 ms)"), qsTr("Ultra (1000 Hz / 1 ms)")]
                                currentIndex: {
                                    const ms = Chiaki.settings.controllerPollingMs ?? 4;
                                    if (ms <= 1) return 3;
                                    if (ms <= 2) return 2;
                                    if (ms <= 4) return 1;
                                    return 0;
                                }
                                onActivated: index => {
                                    const rates = [8, 4, 2, 1];
                                    Chiaki.settings.controllerPollingMs = rates[index];
                                }
                            }
                        }
                        GeneralRow {
                            label: qsTr("DX12 Screen Touchpad Width:")
                            visible: Chiaki.settings.dx12TouchpadWidth !== undefined
                            SettingsPicker {
                                Accessible.name: qsTr("DX12 Screen Touchpad Width:")
                                model: [qsTr("Full Width (100%)"), qsTr("Wide (75%)"), qsTr("Center (50%)")]
                                currentIndex: {
                                    const w = Chiaki.settings.dx12TouchpadWidth ?? 100;
                                    if (w <= 55) return 2;
                                    if (w <= 80) return 1;
                                    return 0;
                                }
                                onActivated: index => {
                                    const widths = [100, 75, 50];
                                    Chiaki.settings.dx12TouchpadWidth = widths[index];
                                }
                            }
                        }
                        GeneralToggle {
                            label: qsTr("Enable Vibration (Rumble)")
                            visible: Chiaki.settings.enableRumble !== undefined
                            checked: Chiaki.settings.enableRumble === true
                            onToggled: enabled => Chiaki.settings.enableRumble = enabled
                        }
                        GeneralToggle {
                            label: qsTr("Adaptive Triggers (DualSense)")
                            visible: Chiaki.settings.enableRumble !== undefined
                            checked: Chiaki.settings.enableAdaptiveTriggers === true
                            onToggled: enabled => Chiaki.settings.enableAdaptiveTriggers = enabled
                        }
                        GeneralToggle {
                            label: qsTr("Enable Gyro Reset")
                            visible: Chiaki.settings.enableRumble !== undefined
                            checked: Chiaki.settings.enableGyroReset === true
                            onToggled: enabled => Chiaki.settings.enableGyroReset = enabled
                        }
                        GeneralRow {
                            label: qsTr("Console Lightbar Colors:")
                            visible: Chiaki.settings.lightbarMode !== undefined
                            SettingsPicker {
                                Accessible.name: qsTr("Console Lightbar Colors:")
                                model: [qsTr("Off"), qsTr("Controller Only"), qsTr("Overlay HUD Only"), qsTr("Controller + Overlay")]
                                currentIndex: Chiaki.settings.lightbarMode ?? 1
                                onActivated: index => Chiaki.settings.lightbarMode = index
                            }
                        }
                        GeneralRow {
                            label: qsTr("Dpad Touchpad Emulation")
                            hint: qsTr("(Checked)")
                            C.CheckBox {
                                Accessible.name: qsTr("Dpad Touchpad Emulation")
                                id: dpadTouch
                                checked: Chiaki.settings.dpadTouchEnabled
                                onToggled: Chiaki.settings.dpadTouchEnabled = !Chiaki.settings.dpadTouchEnabled
                                KeyNavigation.priority: KeyNavigation.BeforeItem
                                KeyNavigation.up: backgroundController
                                KeyNavigation.left: dpadTouch
                                KeyNavigation.right: dpadTouch
                                KeyNavigation.down: {
                                    if(touchIncrement.visible)
                                        touchIncrement
                                    else
                                        posButtons
                                }
                            }
                        }

                        GeneralRow {
                            label: qsTr("Dpad Touch Increment:")
                            hint: qsTr("(0.3 mm)")
                            id: touchIncrementLayout
                            visible: Chiaki.settings.dpadTouchEnabled
                            SettingsSlider {
                                Accessible.name: qsTr("Dpad Touch Increment:")
                                id: touchIncrement
                                from: 1
                                to: 1079
                                stepSize: 1
                                value: Chiaki.settings.dpadTouchIncrement
                                onMoved: Chiaki.settings.dpadTouchIncrement = value

                                Label {
                                    anchors {
                                        left: parent.right
                                        verticalCenter: parent.verticalCenter
                                        leftMargin: 10
                                    }
                                    text: qsTr("%1 mm").arg(parent.value / 100)
                                }
                                KeyNavigation.priority: KeyNavigation.BeforeItem
                                KeyNavigation.up: dpadTouch
                                KeyNavigation.down: {
                                    if(dpadShortcut1.visible)
                                        dpadShortcut1
                                    else
                                        posButtons;
                                }
                            }
                        }
                        GeneralRow {
                            label: qsTr("Dpad Regular/Touch Combo:")
                            hint: qsTr("(L1+R1+dpad Up)")
                            visible: Chiaki.settings.dpadTouchEnabled
                            SettingsFlow {
                                C.ComboBox {
                                    width: Math.min(132, parent.width)
                                    id: dpadShortcut1
                                    implicitContentWidthPolicy: ComboBox.WidestText
                                    firstInFocusChain: false
                                    model: [qsTr("Not Used"), qsTr("Cross"), qsTr("Moon"), qsTr("Box"), qsTr("Pyramid"), qsTr("Dpad Left"), qsTr("Dpad Right"), qsTr("Dpad Up"), qsTr("Dpad Down"), qsTr("L1"), qsTr("R1"), qsTr("L3"), qsTr("R3"), qsTr("Options"), qsTr("Share"), qsTr("Touchpad"), qsTr("PS")]
                                    currentIndex: Chiaki.settings.dpadTouchShortcut1
                                    onActivated: index => Chiaki.settings.dpadTouchShortcut1 = index
                                    KeyNavigation.priority: {
                                        if(!popup.visible)
                                            KeyNavigation.BeforeItem
                                        else
                                            KeyNavigation.AfterItem
                                    }
                                    KeyNavigation.up: touchIncrement
                                    KeyNavigation.down: posButtons
                                    KeyNavigation.left: dpadShortcut1
                                    KeyNavigation.right: dpadShortcut2
                                }
                                C.ComboBox {
                                    width: Math.min(132, parent.width)
                                    id: dpadShortcut2
                                    implicitContentWidthPolicy: ComboBox.WidestText
                                    firstInFocusChain: false
                                    model: [qsTr("Not Used"), qsTr("Cross"), qsTr("Moon"), qsTr("Box"), qsTr("Pyramid"), qsTr("Dpad Left"), qsTr("Dpad Right"), qsTr("Dpad Up"), qsTr("Dpad Down"), qsTr("L1"), qsTr("R1"), qsTr("L3"), qsTr("R3"), qsTr("Options"), qsTr("Share"), qsTr("Touchpad"), qsTr("PS")]
                                    currentIndex: Chiaki.settings.dpadTouchShortcut2
                                    onActivated: index => Chiaki.settings.dpadTouchShortcut2 = index
                                    KeyNavigation.priority: {
                                        if(!popup.visible)
                                            KeyNavigation.BeforeItem
                                        else
                                            KeyNavigation.AfterItem
                                    }
                                    KeyNavigation.up: touchIncrement
                                    KeyNavigation.down: posButtons
                                    KeyNavigation.left: dpadShortcut1
                                    KeyNavigation.right: dpadShortcut3
                                }
                                C.ComboBox {
                                    width: Math.min(132, parent.width)
                                    id: dpadShortcut3
                                    implicitContentWidthPolicy: ComboBox.WidestText
                                    firstInFocusChain: false
                                    model: [qsTr("Not Used"), qsTr("Cross"), qsTr("Moon"), qsTr("Box"), qsTr("Pyramid"), qsTr("Dpad Left"), qsTr("Dpad Right"), qsTr("Dpad Up"), qsTr("Dpad Down"), qsTr("L1"), qsTr("R1"), qsTr("L3"), qsTr("R3"), qsTr("Options"), qsTr("Share"), qsTr("Touchpad"), qsTr("PS")]
                                    currentIndex: Chiaki.settings.dpadTouchShortcut3
                                    onActivated: index => Chiaki.settings.dpadTouchShortcut3 = index
                                    KeyNavigation.priority: {
                                        if(!popup.visible)
                                            KeyNavigation.BeforeItem
                                        else
                                            KeyNavigation.AfterItem
                                    }
                                    KeyNavigation.up: touchIncrement
                                    KeyNavigation.down: posButtons
                                    KeyNavigation.left: dpadShortcut2
                                    KeyNavigation.right: dpadShortcut4
                                }
                                C.ComboBox {
                                    width: Math.min(132, parent.width)
                                    id: dpadShortcut4
                                    implicitContentWidthPolicy: ComboBox.WidestText
                                    firstInFocusChain: false
                                    model: [qsTr("Not Used"), qsTr("Cross"), qsTr("Moon"), qsTr("Box"), qsTr("Pyramid"), qsTr("Dpad Left"), qsTr("Dpad Right"), qsTr("Dpad Up"), qsTr("Dpad Down"), qsTr("L1"), qsTr("R1"), qsTr("L3"), qsTr("R3"), qsTr("Options"), qsTr("Share"), qsTr("Touchpad"), qsTr("PS")]
                                    currentIndex: Chiaki.settings.dpadTouchShortcut4
                                    onActivated: index => Chiaki.settings.dpadTouchShortcut4 = index
                                    KeyNavigation.priority: {
                                        if(!popup.visible)
                                            KeyNavigation.BeforeItem
                                        else
                                            KeyNavigation.AfterItem
                                    }
                                    KeyNavigation.up: touchIncrement
                                    KeyNavigation.down: posButtons
                                    KeyNavigation.left: dpadShortcut3
                                    KeyNavigation.right: dpadShortcut4
                                }
                            }
                        }
                        GeneralRow {
                            label: qsTr("Buttons By Position:")
                            hint: qsTr("Use buttons by position instead of by label") + " " + qsTr("(Unchecked)")
                            C.CheckBox {
                                Accessible.name: qsTr("Buttons By Position:")
                                id: posButtons
                                checked: Chiaki.settings.buttonsByPosition
                                onToggled: Chiaki.settings.buttonsByPosition = checked
                                KeyNavigation.priority: KeyNavigation.BeforeItem
                                KeyNavigation.up: {
                                    if(dpadShortcut1.visible)
                                        dpadShortcut1
                                    else
                                        dpadTouch;
                                }
                                KeyNavigation.down: rumbleHaptics
                                KeyNavigation.left: posButtons
                                KeyNavigation.right: posButtons
                            }
                        }
                        GeneralRow {
                            label: qsTr("Rumble Haptics:")
                            hint: qsTr("(Normal)")
                            SettingsPicker {
                                Accessible.name: qsTr("Rumble Haptics:")
                                id: rumbleHaptics
                                model: [qsTr("Off"), qsTr("Very Weak"), qsTr("Weak"), qsTr("Normal"), qsTr("Strong"), qsTr("Very Strong")]
                                currentIndex: Chiaki.settings.rumbleHapticsIntensity
                                onActivated: (index) => Chiaki.settings.rumbleHapticsIntensity = index;
                            }
                        }
                        GeneralRow {
                            label: qsTr("True Haptics Intensity:")
                            hint: qsTr("(console setting)")
                            SettingsSlider {
                                Accessible.name: qsTr("True Haptics Intensity:")
                                id: hapticOverride
                                from: 0
                                to: 2
                                stepSize: 0.1
                                value: Chiaki.settings.hapticOverride
                                onMoved: Chiaki.settings.hapticOverride = value;
                                lastInFocusChain: true
                                Label {
                                    anchors {
                                        left: parent.right
                                        verticalCenter: parent.verticalCenter
                                        leftMargin: 10
                                    }
                                    text: {
                                        if(parent.value > 0.99 && parent.value < 1.01)
                                            qsTr("console setting")
                                        else
                                            (parent.value * 100).toFixed(0) + qsTr(" % console setting")
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Item {
                // Remote (PSN and Hole Punching)
                Flickable {
                    id: remoteFlick
                    anchors {
                        fill: parent
                        topMargin: 20
                        bottomMargin: 20
                    }
                    clip: true
                    contentWidth: width
                    contentHeight: remoteGrid.y + remoteGrid.height
                    flickableDirection: Flickable.AutoFlickIfNeeded
                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AlwaysOn
                        visible: remoteFlick.contentHeight > remoteFlick.height
                    }
                    ColumnLayout {
                        id: remoteGrid
                        width: Math.max(0, Math.min(900, remoteFlick.width - 36))
                        anchors {
                            top: parent.top
                            horizontalCenter: parent.horizontalCenter
                            topMargin: 30
                        }

                        spacing: 9

                        C.Button {
                            id: openPsnLogin
                            firstInFocusChain: visible
                            Layout.alignment: Qt.AlignHCenter

                            text: qsTr("Login to PSN")
                            onClicked: {
                                root.showPSNTokenDialog(false)
                            }
                            Material.roundedScale: Material.SmallScale
                            visible: !Chiaki.settings.psnRefreshToken || !Chiaki.settings.psnAuthToken || !Chiaki.settings.psnAuthTokenExpiry || !Chiaki.settings.psnAccountId
                        }

                        C.Button {
                            id: resetPsnTokens
                            text: qsTr("Clear PSN Token")
                            firstInFocusChain: !openPsnLogin.visible
                            Layout.alignment: Qt.AlignHCenter

                            onClicked: {
                                Chiaki.settings.psnRefreshToken = ""
                                Chiaki.settings.psnAuthToken = ""
                                Chiaki.settings.psnAuthTokenExpiry = ""
                                Chiaki.settings.psnAccountId = ""
                                openPsnLogin.forceActiveFocus(Qt.TabFocusReason);
                            }
                            Material.roundedScale: Material.SmallScale
                            visible: Chiaki.settings.psnRefreshToken && Chiaki.settings.psnAuthToken && Chiaki.settings.psnAuthTokenExpiry && Chiaki.settings.psnAccountId
                        }

                        GeneralRow {
                            label: qsTr("Hole Punching Port Guessing:")
                            hint: qsTr("(Unchecked)")
                            C.CheckBox {
                                Accessible.name: qsTr("Hole Punching Port Guessing:")
                                id: holePunchGuessingCheckbox
                                text: qsTr("Force STUN port guessing")
                                checked: Chiaki.settings.portGuessingEnabled
                                onToggled: Chiaki.settings.portGuessingEnabled = checked
                            }
                        }

                        GeneralRow {
                            label: qsTr("Port Guess Count:")
                            hint: qsTr("(75)")
                            SettingsSlider {
                                Accessible.name: qsTr("Port Guess Count:")
                                id: portGuessCountSlider

                                from: 0
                                to: 75
                                stepSize: 1
                                value: Chiaki.settings.portGuessCount
                                onMoved: Chiaki.settings.portGuessCount = value

                                Label {
                                    anchors {
                                        left: parent.right
                                        verticalCenter: parent.verticalCenter
                                        leftMargin: 10
                                    }
                                    text: parent.value + qsTr(" guesses")
                                }
                            }
                        }

                        GeneralRow {
                            label: qsTr("Port Guess Socket Count:")
                            hint: qsTr("(250)")
                            SettingsSlider {
                                Accessible.name: qsTr("Port Guess Socket Count:")
                                id: portGuessSocketSlider

                                from: 0
                                to: 500
                                stepSize: 1
                                value: Chiaki.settings.portGuessSocketCount
                                onMoved: Chiaki.settings.portGuessSocketCount = value
                                lastInFocusChain: true

                                Label {
                                    anchors {
                                        left: parent.right
                                        verticalCenter: parent.verticalCenter
                                        leftMargin: 10
                                    }
                                    text: parent.value + qsTr(" sockets")
                                }
                            }
                        }
                    }
                }
            }

            Item {
                // Config (Options such as Import/Export and Logging)
                Flickable {
                    id: configFlick
                    anchors {
                        fill: parent
                        topMargin: 20
                        bottomMargin: 20
                    }
                    clip: true
                    contentWidth: Math.max(width, configGrid.width)
                    contentHeight: configGrid.y + configGrid.height
                    flickableDirection: Flickable.AutoFlickIfNeeded
                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AlwaysOn
                        visible: configFlick.contentHeight > configFlick.height
                    }
                    GridLayout {
                        id: configGrid
                        width: Math.max(360, Math.min(900, configFlick.width - 30))
                        anchors {
                            top: parent.top
                            horizontalCenter: parent.horizontalCenter
                            topMargin: 30
                        }
                        columns: 1
                        rowSpacing: 10
                        columnSpacing: 10

                        Label {
                            text: {
                                if(Chiaki.settings.currentProfile)
                                    qsTr("Current Profile: ") + Chiaki.settings.currentProfile
                                else
                                    qsTr("Current Profile: default")
                            }
                        }

                        C.Button {
                            id: profile
                            firstInFocusChain: true
                            text: qsTr("Manage Profiles")
                            onClicked: {
                                root.showProfileDialog()
                            }
                            Material.roundedScale: Material.SmallScale
                        }

                    C.Button {
                        id: exportButton
                        text: qsTr("Export settings to file")
                        onClicked: {
                            Chiaki.settings.exportSettings();
                        }
                        Material.roundedScale: Material.SmallScale
                    }

                    C.Button {
                        id: importButton
                        text: qsTr("Import settings from file")
                        onClicked: {
                            Chiaki.settings.importSettings();
                        }
                        Material.roundedScale: Material.SmallScale
                    }

                    C.Button {
                        id: aboutButton
                        text: qsTr("About %1-ng").arg(Qt.application.name)
                        onClicked: aboutDialog.open()
                        Material.roundedScale: Material.SmallScale
                    }

                    SettingsFlow {
                        spacing: 8
                        visible: typeof Chiaki.settings.openDataFolder === "function"
                        C.Button {
                            text: qsTr("Open Data Folder")
                            onClicked: Chiaki.settings.openDataFolder()
                            Material.roundedScale: Material.SmallScale
                        }
                        C.Button {
                            text: qsTr("Open Logs Folder")
                            onClicked: Chiaki.settings.openLogsFolder()
                            Material.roundedScale: Material.SmallScale
                        }
                        C.Button {
                            text: qsTr("Open Controllers Folder")
                            onClicked: Chiaki.settings.openControllersFolder()
                            Material.roundedScale: Material.SmallScale
                        }
                        C.Button {
                            text: qsTr("Open Shaders Folder")
                            onClicked: Chiaki.settings.openShadersFolder()
                            Material.roundedScale: Material.SmallScale
                        }
                    }

                    SettingsFlow {
                        spacing: 8
                        visible: typeof Chiaki.settings.resetVideoSettings === "function"
                        C.Button {
                            text: qsTr("Reset Video Settings")
                            onClicked: Chiaki.settings.resetVideoSettings()
                            Material.roundedScale: Material.SmallScale
                        }
                        C.Button {
                            text: qsTr("Reset Stream Settings")
                            onClicked: Chiaki.settings.resetStreamSettings()
                            Material.roundedScale: Material.SmallScale
                        }
                    }

                    C.CheckBox {
                        text: qsTr("Sanitize Logs (checked)")
                        checked: Chiaki.settings.logSanitize
                        onToggled: Chiaki.settings.logSanitize = checked
                    }

                        C.CheckBox {
                            text: qsTr("Verbose Logging (unchecked)")
                            checked: Chiaki.settings.logVerbose
                            lastInFocusChain: true
                            onToggled: Chiaki.settings.logVerbose = checked
                        }
                    }
                }
            }
        }

        Connections {
            target: dialog.Window.window
            function onActiveFocusItemChanged() {
                dialog.ensureActiveFocusVisible()
            }
        }

        Item {
            Timer {
                id: openTimer
                interval: 100
                running: false
                onTriggered: {
                    if(controllerMappingDialog.resetMapping)
                    {
                        if(!Chiaki.controllerMappingDefaultMapping)
                        {
                            quitControllerMapping = false;
                            Chiaki.controllerMappingReset();
                        }
                        controllerMappingDialog.close();
                    }
                    else
                    {
                        controllerMappingChange.forceActiveFocus(Qt.TabFocusReason);
                        root.showControllerMappingDialog();
                        quitControllerMapping = false;
                        controllerMappingDialog.resetFocus = false;
                        controllerMappingDialog.close();
                    }
                }
            }
        }

        Dialog {
            id: aboutDialog
            parent: Overlay.overlay
            x: Math.round((root.width - width) / 2)
            y: Math.round((root.height - height) / 2)
            title: qsTr("About %1-ng").arg(Qt.application.name)
            modal: true
            standardButtons: Dialog.Ok
            Material.roundedScale: Material.MediumScale
            onAboutToHide: aboutButton.forceActiveFocus(Qt.TabFocusReason)

            RowLayout {
                spacing: 50
                onVisibleChanged: if (visible) forceActiveFocus(Qt.TabFocusReason)
                Keys.onReturnPressed: aboutDialog.close()
                Keys.onEscapePressed: aboutDialog.close()

                Image {
                    Layout.preferredWidth: 200
                    fillMode: Image.PreserveAspectFit
                    verticalAlignment: Image.AlignTop
                    source: "qrc:icons/chiaking-logo.svg"
                }

                Label {
                    Layout.preferredWidth: 400
                    verticalAlignment: Text.AlignTop
                    wrapMode: Text.Wrap
                    text: qsTr("<h1>chiaki-ng</h1> by Street Pea, version %1\n                        <h2>Fork of Chiaki</h2> by Florian Markl at version 2.1.1\n\n                        <p>This program is free software: you can redistribute it and/or modify\n                        it under the terms of the GNU Affero General Public License version 3\n                        as published by the Free Software Foundation.</p>\n\n                        <p>This program is distributed in the hope that it will be useful,\n                        but WITHOUT ANY WARRANTY; without even the implied warranty of\n                        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n                        GNU General Public License for more details.</p>").arg(Qt.application.version)
                }
            }
        }

        Dialog {
            id: keyDialog
            focus: false
            property int buttonValue
            property var buttonCallback
            property var keysIndex
            parent: Overlay.overlay
            x: Math.round((root.width - width) / 2)
            y: Math.round((root.height - height) / 2)
            title: qsTr("Key Capture")
            modal: true
            standardButtons: Dialog.Close
            closePolicy: Popup.CloseOnPressOutside
            onOpened: keyLabel.forceActiveFocus(Qt.TabFocusReason)
            onClosed: {
                let item = chiakiKeys.itemAt(keysIndex)
                if(item)
                {
                    let item2 = item.children[1];
                    if(item2)
                        item2.forceActiveFocus(Qt.TabFocusReason);
                }
                keyLabel.focus = false;
                focus = false;
            }
            Material.roundedScale: Material.MediumScale

            function show(opts) {
                buttonValue = opts.value;
                buttonCallback = opts.callback;
                keysIndex = opts.mappingIndex;
                open();
            }

            Label {
                id: keyLabel
                focus: true
                text: qsTr("Press any key to configure button or click close")
                Keys.onReleased: (event) => {
                    var name = Chiaki.settings.changeControllerKey(keyDialog.buttonValue, event.key);
                    keyDialog.buttonCallback(name);
                    keyDialog.close();
                }
            }
        }

        Dialog {
            id: controllerMappingDialog
            property bool resetFocus: true
            property bool resetMapping: false
            parent: Overlay.overlay
            x: Math.round((root.width - width) / 2)
            y: Math.round((root.height - height) / 2)
            title: qsTr("Controller Capture")
            modal: true
            standardButtons: Dialog.Close
            closePolicy: Popup.CloseOnPressOutside
            onOpened: {
                controllerLabel.forceActiveFocus(Qt.TabFocusReason);
                Chiaki.beginControllerMapping(resetMapping);
            }
            onClosed: {
                if(resetFocus)
                {
                    if(resetMapping)
                        controllerMappingReset.forceActiveFocus(Qt.TabFocusReason);
                    else
                        controllerMappingChange.forceActiveFocus(Qt.TabFocusReason);
                    focus = false;
                }
                else
                {
                    resetFocus = true;
                    focus = false;
                }
                if(quitControllerMapping)
                    Chiaki.controllerMappingQuit();
                else
                    quitControllerMapping = true;
            }
            Material.roundedScale: Material.MediumScale

            function show(opts) {
                resetMapping = opts.reset;
                open();
            }
            Label {
                id: controllerLabel
                text: qsTr("Choose the controller by pressing any button on the controller")
            }
        }

        Dialog {
            id: steamControllerMappingDialog
            property bool resetMapping: false
            parent: Overlay.overlay
            x: Math.round((root.width - width) / 2)
            y: Math.round((root.height - height) / 2)
            title: qsTr("Controller Managed by Steam")
            modal: true
            standardButtons: Dialog.Close
            closePolicy: Popup.NoAutoClose
            onOpened: {
                steamLabel.forceActiveFocus(Qt.TabFocusReason);
            }
            onClosed: {
                if(resetMapping)
                    controllerMappingReset.forceActiveFocus(Qt.TabFocusReason);
                else
                    controllerMappingChange.forceActiveFocus(Qt.TabFocusReason);
                focus = false;
            }
            Material.roundedScale: Material.MediumScale

            Label {
                id: steamLabel
                wrapMode: TextEdit.Wrap
                text: qsTr("This controller is managed by Steam.\nPlease use Steam to map controller or disable Steam Input for the controller before mapping here.")
                Keys.onReturnPressed: steamControllerMappingDialog.close();
                Keys.onEscapePressed: steamControllerMappingDialog.close();
            }
        }

        Connections {
            target: Chiaki

            function onControllerMappingInProgressChanged()
            {
                if(Chiaki.controllerMappingInProgress)
                    openTimer.start();
            }

            function onControllerMappingSteamControllerSelected()
            {
                controllerMappingDialog.resetFocus = false;
                quitControllerMapping = false;
                steamControllerMappingDialog.resetMapping = controllerMappingDialog.resetMapping;
                controllerMappingDialog.close();
                steamControllerMappingDialog.open();
            }
        }
    }
}
