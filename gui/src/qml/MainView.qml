import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

import org.streetpea.chiaking

Pane {
    padding: 0
    font.family: Qt.application.font.family
    readonly property int edge: width < 500 ? 12 : 20
    id: consolePane
    background: Rectangle {
        color: "#18181B"
    }

    readonly property color shellBg: "#18181B"
    readonly property color cardBg: "#232328"
    readonly property color cardHoverBg: "#2A2A31"
    readonly property color cardBorder: "#32323A"
    readonly property color insetBg: "#141418"
    readonly property color pillBg: "#2C2C34"
    readonly property color pillHover: "#3B3B45"
    readonly property color pillBorder: "#3D3D47"
    readonly property color textPrimary: "#F0F1F3"
    readonly property color textSecondary: "#A5A5A5"
    readonly property color textMuted: "#9C9C9C"
    readonly property color accentBlue: "#0A84FF"
    readonly property color accentGreen: "#30D158"
    readonly property color accentAmber: "#FF9F0A"
    readonly property color accentDanger: "#FF453A"

    component DS5HeaderPill: Button {
        id: hBtn
        property bool isPrimary: false
        property bool isDanger: false
        property color statusDotColor: "transparent"
        implicitHeight: 34
        topPadding: 7
        bottomPadding: 7
        topInset: 0
        bottomInset: 0
        leftPadding: 14
        rightPadding: 14
        focusPolicy: Qt.TabFocus
        Accessible.name: text
        font.pixelSize: 13
        font.weight: isPrimary ? Font.DemiBold : Font.Medium
        contentItem: RowLayout {
            spacing: 7
            Rectangle {
                visible: hBtn.statusDotColor !== Qt.rgba(0, 0, 0, 0) && hBtn.statusDotColor !== "transparent"
                width: 7
                height: 7
                radius: 3.5
                color: hBtn.statusDotColor
                Layout.alignment: Qt.AlignVCenter
            }
            Text {
                text: hBtn.text
                font: hBtn.font
                color: hBtn.isPrimary ? "#FFFFFF"
                    : (hBtn.isDanger && hBtn.hovered) ? "#FF6961"
                    : consolePane.textPrimary
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                Layout.alignment: Qt.AlignVCenter
            }
        }
        background: Rectangle {
            radius: 14
            color: hBtn.isPrimary ? (hBtn.down ? "#0768CC" : hBtn.hovered ? "#2494FF" : consolePane.accentBlue)
                : hBtn.isDanger ? (hBtn.down ? "#552326" : hBtn.hovered ? "#3E2023" : consolePane.pillBg)
                : (hBtn.down ? "#25252C" : hBtn.hovered ? consolePane.pillHover : consolePane.pillBg)
            border.width: 1
            border.color: hBtn.visualFocus ? "#82BDFF" : hBtn.isPrimary ? "#48A4FF"
                : (hBtn.isDanger && hBtn.hovered) ? "#8A3836"
                : consolePane.pillBorder
        }
    }

    StackView.onActivated: {
        forceActiveFocus(Qt.TabFocusReason);
        if(!Chiaki.autoConnect && !root.initialAsk && !Chiaki.window.directStream)
        {
            root.initialAsk = true;
            if(Chiaki.settings.addSteamShortcutAsk && (typeof Chiaki.createSteamShortcut === "function"))
                root.showRemindDialog(qsTr("Official Steam artwork + controller layout"), qsTr("Would you like to either create a new non-Steam game for chiaki-ng\nor update an existing non-Steam game with the official artwork and controller layout?") + "\n\n" + qsTr("(Note: If you select no now and want to do this later, click the button or press R3 from the main menu.)"), false, () => root.showSteamShortcutDialog(true));
            else if(Chiaki.settings.remotePlayAsk)
            {
                if(!Chiaki.settings.psnRefreshToken || !Chiaki.settings.psnAuthToken || !Chiaki.settings.psnAuthTokenExpiry || !Chiaki.settings.psnAccountId)
                    root.showRemindDialog(qsTr("Remote Play via PSN"), qsTr("Would you like to connect to PSN?\nThis enables:\n- Automatic registration\n- Playing outside of your home network without port forwarding?") + "\n\n" + qsTr("(Note: If you select no now and want to do this later, go to the Config section of the settings.)"), true, () => root.showPSNTokenDialog(false));
                else
                    Chiaki.settings.remotePlayAsk = false;
            }
        }
    }
    Keys.onUpPressed: hostsView.decrementCurrentIndex()
    Keys.onDownPressed: hostsView.incrementCurrentIndex()
    Keys.onMenuPressed: settingsButton.clicked()
    Keys.onReturnPressed: if (hostsView.currentItem) hostsView.currentItem.connectToHost()
    Keys.onYesPressed: if (hostsView.currentItem) hostsView.currentItem.wakeUpHost()
    Keys.onNoPressed: if (hostsView.currentItem) hostsView.currentItem.deleteHost()
    Keys.onEscapePressed: root.showConfirmDialog(qsTr("Quit"), qsTr("Are you sure you want to quit?"), () => Qt.quit())
    Keys.onPressed: (event) => {
        if (event.modifiers)
            return;
        switch (event.key) {
        case Qt.Key_PageUp:
            if (hostsView.currentItem) hostsView.currentItem.setConsolePin();
            event.accepted = true;
            break;
        case Qt.Key_PageDown:
            if (Chiaki.settings.psnAuthToken) Chiaki.refreshPsnToken();
            event.accepted = true;
            break;
        case Qt.Key_F1:
            if (typeof Chiaki.createSteamShortcut === "function") root.showSteamShortcutDialog(false);
            event.accepted = true;
            break;
        case Qt.Key_F2:
            root.showManualHostDialog();
            event.accepted = true;
            break;
        }
    }

    Rectangle {
        id: toolBar
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: consolePane.edge
        width: Math.max(0, Math.min(760, parent.width - 2 * consolePane.edge))
        height: headerLayout.implicitHeight + 28
        radius: 20
        color: consolePane.cardBg
        border { color: consolePane.cardBorder; width: 1 }

        ColumnLayout {
            id: headerLayout
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 14 }
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Image {
                    Layout.preferredWidth: 48
                    Layout.preferredHeight: 48
                    source: "qrc:/ps5link/PS5Link.png"
                    fillMode: Image.PreserveAspectFit
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 3
                    Label {
                        text: "PS5Link"
                        font.pixelSize: 19
                        font.weight: Font.DemiBold
                        color: consolePane.textPrimary
                    }
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: Chiaki.settings.psnAuthToken
                            ? (Chiaki.discoveryEnabled ? qsTr("PSN and local discovery") : qsTr("Signed in to PSN"))
                            : (Chiaki.discoveryEnabled ? qsTr("Local discovery active") : qsTr("Local discovery paused"))
                        font.pixelSize: 12
                        color: consolePane.textSecondary
                    }
                }

                DS5HeaderPill {
                    id: settingsButton
                    text: qsTr("Settings")
                    onClicked: root.showSettingsDialog()
                }
            }

            Flow {
                Layout.fillWidth: true
                Layout.preferredHeight: implicitHeight
                spacing: 6

                DS5HeaderPill {
                    text: qsTr("Add console")
                    onClicked: root.showManualHostDialog()
                }
                DS5HeaderPill {
                    text: Chiaki.discoveryEnabled ? qsTr("Discovery on") : qsTr("Discovery off")
                    statusDotColor: Chiaki.discoveryEnabled ? consolePane.accentGreen : "#868691"
                    onClicked: Chiaki.discoveryEnabled = !Chiaki.discoveryEnabled
                }
                DS5HeaderPill {
                    text: qsTr("Refresh PSN")
                    visible: !!Chiaki.settings.psnAuthToken
                    onClicked: Chiaki.refreshPsnToken()
                }
                DS5HeaderPill {
                    text: qsTr("Steam shortcut")
                    visible: typeof Chiaki.createSteamShortcut === "function"
                    onClicked: root.showSteamShortcutDialog(false)
                }
            }
        }
    }

    Label {
        id: sectionHeader
        anchors { top: toolBar.bottom; left: toolBar.left; topMargin: 15; leftMargin: 2 }
        text: qsTr("PlayStation consoles")
        font.pixelSize: 14
        font.weight: Font.DemiBold
        color: consolePane.textPrimary
    }

    ListView {
        id: hostsView
        keyNavigationWraps: true
        anchors {
            top: sectionHeader.bottom
            left: toolBar.left
            right: toolBar.right
            bottom: footerBar.top
            topMargin: 8
            bottomMargin: 8
        }
        spacing: 12
        clip: true
        model: {
            const hosts = Chiaki.hosts;
            const visibleHosts = [];
            for (let i = 0; i < hosts.length; ++i) {
                if (hosts[i].display)
                    visibleHosts.push({ host: hosts[i], hostIndex: i });
            }
            return visibleHosts;
        }
        delegate: ItemDelegate {
            id: delegate
            width: hostsView.width
            readonly property var host: modelData.host
            readonly property int hostIndex: modelData.hostIndex
            height: Math.max(96, consoleRow.implicitHeight + 28)
            padding: 14
            highlighted: ListView.isCurrentItem
            onClicked: connectToHost()

            function connectToHost() {
                if(delegate.host.discovered)
                    Chiaki.connectToHost(hostIndex, delegate.host.name);
                else
                    Chiaki.connectToHost(hostIndex);
            }

            function wakeUpHost() {
                if((!delegate.host.discovered && !delegate.host.duid) || delegate.host.state == "standby")
                    Chiaki.wakeUpHost(hostIndex);
            }

            function deleteHost() {
                if (delegate.host.manual)
                    root.showConfirmDialog(qsTr("Delete Console"), qsTr("Are you sure you want to delete this console?"), () => {Chiaki.deleteHost(hostIndex)});
                else if (delegate.host.discovered && !delegate.host.registered)
                    root.showConfirmDialog(qsTr("Hide Console"), qsTr("Are you sure you want to hide this console?") + "\n\n" + qsTr("Note: You can unhide from the Consoles section of the Settings under Hidden Consoles"), () => Chiaki.hideHost(delegate.host.mac, delegate.host.name));
            }

            function setConsolePin() {
                root.showConsolePinDialog(hostIndex);
            }

            background: Rectangle {
                radius: 20
                color: delegate.highlighted ? "#282C38" : (delegate.hovered ? consolePane.cardHoverBg : consolePane.cardBg)
                border.width: delegate.highlighted ? 2 : 1
                border.color: delegate.highlighted ? consolePane.accentBlue : consolePane.cardBorder

            }

            contentItem: RowLayout {
                id: consoleRow
                spacing: 12

                Rectangle {
                    Layout.preferredWidth: 56
                    Layout.preferredHeight: 56
                    radius: 16
                    border.color: "#42424C"
                    border.width: 1
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: "#34343E" }
                        GradientStop { position: 1.0; color: "#1E1E24" }
                    }

                    Image {
                        anchors.centerIn: parent
                        width: 48
                        height: 48
                        fillMode: Image.PreserveAspectFit
                        source: "image://svg/console-ps" + (delegate.host.ps5 ? "5" : "4") + (delegate.host.state == "standby" ? "#light_standby" : "#light_on")
                        sourceSize: Qt.size(width, height)
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 5

                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: delegate.host.name || (delegate.host.ps5 ? "PlayStation 5" : "PlayStation 4")
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: consolePane.textPrimary
                    }

                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        color: consolePane.textSecondary
                        font.pixelSize: 12
                        text: {
                            const system = delegate.host.ps5 ? "PS5" : "PS4";
                            if (delegate.host.duid && !delegate.host.discovered)
                                return system + "  ·  " + qsTr("Remote Play via PSN");
                            if (delegate.host.state === "standby")
                                return system + "  ·  " + qsTr("Rest mode");
                            if (delegate.host.state === "ready")
                                return system + "  ·  " + qsTr("Available");
                            return system + "  ·  " + (delegate.host.registered ? qsTr("Registered") : qsTr("Unregistered"));
                        }
                    }

                    Label {
                        visible: !!delegate.host.address || !!delegate.host.mac || (delegate.host.duid && delegate.host.discovered)
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        color: consolePane.textMuted
                        font.pixelSize: 11
                        text: {
                            let parts = [];
                            if (delegate.host.address)
                                parts.push(qsTr("Host: %1").arg(Chiaki.settings.streamerMode ? qsTr("hidden") : delegate.host.address));
                            if (delegate.host.mac)
                                parts.push(qsTr("ID: %1 (%2)").arg(Chiaki.settings.streamerMode ? qsTr("hidden") : delegate.host.mac).arg(delegate.host.registered ? qsTr("registered") : qsTr("unregistered")));
                            if (delegate.host.duid)
                                parts.push(delegate.host.discovered ? qsTr("Auto-Registration Available") : qsTr("PSN Cloud Link"));
                            else if (delegate.host.discovered)
                                parts.push(delegate.host.manual ? qsTr("LAN + Manual") : qsTr("LAN Discovered"));
                            else
                                parts.push(qsTr("Manual Host"));
                            return parts.join("   •   ");
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        visible: !delegate.host.duid && !!(delegate.host.app || delegate.host.titleId)
                        color: "#72B4FF"
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        text: {
                            let appParts = [];
                            if (delegate.host.app)
                                appParts.push(qsTr("Playing: %1").arg(delegate.host.app));
                            if (delegate.host.titleId)
                                appParts.push(qsTr("Title ID: %1").arg(delegate.host.titleId));
                            return appParts.join("  •  ");
                        }
                    }
                }

                GridLayout {
                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                    columns: consolePane.width < 430 ? 1 : 2
                    columnSpacing: 6
                    rowSpacing: 6

                    DS5HeaderPill {
                        text: (delegate.host.registered || delegate.host.duid) ? qsTr("Connect") : qsTr("Register")
                        isPrimary: true
                        onClicked: delegate.connectToHost()
                    }

                    DS5HeaderPill {
                        text: qsTr("Wake Up")
                        visible: delegate.host.registered && ((!delegate.host.duid && !delegate.host.discovered) || delegate.host.state == "standby")
                        onClicked: delegate.wakeUpHost()
                    }

                    DS5HeaderPill {
                        text: qsTr("PIN")
                        visible: delegate.host.registered
                        onClicked: delegate.setConsolePin()
                    }

                    DS5HeaderPill {
                        text: delegate.host.manual ? qsTr("Delete") : qsTr("Hide")
                        isDanger: true
                        visible: delegate.host.manual || (delegate.host.discovered && !delegate.host.registered)
                        onClicked: delegate.deleteHost()
                    }
                }
            }
        }
    }

    // Empty state DS5Link Card when no consoles are listed yet
    Rectangle {
        anchors { top: hostsView.top; horizontalCenter: hostsView.horizontalCenter }
        width: hostsView.width
        height: emptyBody.implicitHeight + 32
        radius: 22
        color: consolePane.cardBg
        border.color: consolePane.cardBorder
        border.width: 1
        visible: hostsView.count === 0

        ColumnLayout {
            id: emptyBody
            anchors.centerIn: parent
            spacing: 9
            width: parent.width - 32

            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 48
                Layout.preferredHeight: 48
                radius: 17
                color: "#2C2C34"
                border.color: "#3E3E48"
                border.width: 1

                Image {
                    anchors.centerIn: parent
                    width: 34
                    height: 34
                    fillMode: Image.PreserveAspectFit
                    source: "qrc:/ps5link/PS5Link.png"
                    sourceSize: Qt.size(width, height)
                }
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: Chiaki.discoveryEnabled ? qsTr("Searching for PlayStation 5...") : qsTr("No Consoles Added Yet")
                font.pixelSize: 17
                font.weight: Font.DemiBold
                color: "#FFFFFF"
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: Chiaki.settings.psnAuthToken
                    ? qsTr("Refresh PSN or add your console by IP address.")
                    : qsTr("Sign in to PSN in Settings or add your console by IP address.")
                font.pixelSize: 13
                color: consolePane.textSecondary
            }
        }
    }

    // Footer Bar
    RowLayout {
        id: footerBar
        anchors {
            left: toolBar.left
            right: toolBar.right
            bottom: parent.bottom
            leftMargin: 2
            rightMargin: 2
            bottomMargin: 14
        }

        Label {
            text: qsTr("Ctrl+O opens the in-game menu")
            font.pixelSize: 12
            color: consolePane.textMuted
        }

        Item { Layout.fillWidth: true }

        Rectangle {
            radius: 8
            color: "#232328"
            border.color: "#32323A"
            border.width: 1
            implicitHeight: 22
            implicitWidth: verLabel.implicitWidth + 16

            Label {
                id: verLabel
                anchors.centerIn: parent
                text: "v" + Qt.application.version
                font.pixelSize: 11
                color: consolePane.textSecondary
            }
        }
    }
}
