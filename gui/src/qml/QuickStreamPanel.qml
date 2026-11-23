import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import org.streetpea.chiaking

FocusScope {
    id: panel
    Material.theme: Material.Dark
    Material.accent: panel.accent

    signal dismissed()
    signal disconnectRequested()
    signal displayRequested()
    signal advancedRequested()
    signal settingsRequested()
    property alias initialFocusItem: resumeButton

    readonly property color surface: "#18181B"
    readonly property color card: "#232328"
    readonly property color stroke: "#32323A"
    readonly property color ink: "#F0F1F3"
    readonly property color subtle: "#A5A5A5"
    readonly property color accent: "#0A84FF"
    readonly property color success: "#30D158"
    readonly property var session: Chiaki.session

    Keys.onEscapePressed: dismissed()
    Keys.onMenuPressed: dismissed()

    component PanelButton: Button {
        id: button
        property bool warning: false
        property bool primary: false
        implicitHeight: 38
        topInset: 0
        bottomInset: 0
        font.family: Qt.application.font.family
        leftPadding: 15
        rightPadding: 15
        focusPolicy: Qt.StrongFocus
        Keys.onReturnPressed: clicked()
        font.pixelSize: 13
        font.weight: primary ? Font.DemiBold : Font.Medium
        contentItem: Text {
            text: button.text
            font: button.font
            color: button.primary ? "#FFFFFF" : button.warning ? "#FF6961" : panel.ink
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            implicitWidth: 78
            radius: height / 2
            color: button.primary ? (button.down ? "#0768CC" : button.hovered ? "#2494FF" : panel.accent)
                : button.warning ? (button.hovered ? "#4A2225" : "#341C1E")
                : button.down ? "#25252C" : button.hovered ? "#3B3B45" : "#2C2C34"
            border.width: button.activeFocus ? 2 : 1
            border.color: button.activeFocus ? "#72B4FF"
                : button.primary ? "#48A4FF"
                : button.warning ? "#8A3836"
                : "#3D3D47"
            opacity: button.enabled ? 1 : 0.45
        }
    }

    component PanelSelect: ComboBox {
        id: select
        implicitHeight: 34
        leftPadding: 13
        rightPadding: 30
        font.family: Qt.application.font.family
        font.pixelSize: 13
        background: Rectangle {
            radius: height / 2
            color: select.hovered || select.popup.visible ? "#393940" : "#303037"
            border.color: select.activeFocus ? "#72B4FF" : "#414149"
        }
        contentItem: Text {
            text: select.displayText
            font: select.font
            color: panel.ink
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
    }

    component Section: Frame {
        padding: 14
        background: Rectangle {
            radius: 23
            color: panel.card
            border.color: panel.stroke
        }
    }

    component Caption: Label {
        color: panel.subtle
        font.pixelSize: 12
        font.weight: Font.DemiBold
    }

    Rectangle {
        anchors.fill: shell
        radius: 28
        color: panel.surface
        border.color: "#3C3C46"
        border.width: 1
    }

    ColumnLayout {
        id: shell
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Math.min(14, parent.height / 16)
        width: Math.max(1, Math.min(540, parent.width - 24))
        height: Math.min(parent.height - anchors.bottomMargin, content.implicitHeight + 30)
        spacing: 0

        ScrollView {
            id: viewport
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 14
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical: ScrollBar {
                width: 7
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                policy: ScrollBar.AsNeeded
                visible: size < 0.999
                contentItem: Rectangle {
                    radius: width / 2
                    color: parent.pressed ? "#64646E" : "#484850"
                }
            }
            clip: true

            ColumnLayout {
                id: content
                width: viewport.availableWidth
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 14

                    ColumnLayout {
                        spacing: 4
                        Label { text: "PS5Link"; color: "#FFFFFF"; font.pixelSize: 20; font.weight: Font.DemiBold }
                        Rectangle {
                            radius: 10
                            color: "#2C2C33"
                            border.color: "#3C3C45"
                            border.width: 1
                            implicitHeight: 22
                            implicitWidth: qspBadgeRow.implicitWidth + 16
                            RowLayout {
                                id: qspBadgeRow
                                anchors.centerIn: parent
                                spacing: 6
                                Rectangle {
                                    width: 6
                                    height: 6
                                    radius: 3
                                    color: panel.session && panel.session.connected ? panel.success : panel.accent
                                }
                                Label {
                                    text: panel.session && panel.session.connected ? qsTr("Remote Play connected") : qsTr("Remote Play")
                                    color: "#D0D2D6"
                                    font.pixelSize: 11
                                }
                            }
                        }
                    }
                    Item { Layout.fillWidth: true }
                    PanelButton {
                        id: resumeButton
                        text: qsTr("Resume  (Esc)")
                        primary: true
                        Layout.alignment: Qt.AlignRight
                        onClicked: panel.dismissed()
                        KeyNavigation.down: aspectBox
                        KeyNavigation.left: endButton
                        KeyNavigation.right: aspectBox
                    }
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 1
                    columnSpacing: 10
                    rowSpacing: 10

                    Section {
                        Layout.fillWidth: true
                        contentItem: ColumnLayout {
                            spacing: 8
                            Label { text: qsTr("Video"); color: panel.ink; font.pixelSize: 17; font.weight: Font.DemiBold }
                            GridLayout {
                                Layout.fillWidth: true
                                columns: 2
                                columnSpacing: 10
                                rowSpacing: 4
                                Caption { text: qsTr("Aspect") }
                                PanelSelect {
                                    id: aspectBox
                                    Layout.fillWidth: true
                                    Accessible.name: qsTr("Video aspect")
                                    model: [qsTr("Fit"), qsTr("Fill"), qsTr("Stretch")]
                                    currentIndex: Chiaki.window.videoMode === ChiakiWindow.VideoMode.Zoom ? 1
                                        : Chiaki.window.videoMode === ChiakiWindow.VideoMode.Stretch ? 2 : 0
                                    onActivated: index => {
                                        Chiaki.window.videoMode = [ChiakiWindow.VideoMode.Normal,
                                            ChiakiWindow.VideoMode.Zoom, ChiakiWindow.VideoMode.Stretch][index]
                                    }
                                    KeyNavigation.up: resumeButton
                                    KeyNavigation.down: zoomSlider.visible ? zoomSlider : presetBox
                                    KeyNavigation.right: volumeControl
                                }
                                Caption { text: qsTr("Zoom"); visible: zoomSlider.visible }
                                Slider {
                                    id: zoomSlider
                                    Layout.fillWidth: true
                                    visible: Chiaki.window.videoMode === ChiakiWindow.VideoMode.Zoom
                                    Accessible.name: qsTr("Zoom")
                                    from: -1
                                    to: 4
                                    stepSize: 0.01
                                    value: Chiaki.window.ZoomFactor
                                    onMoved: {
                                        Chiaki.window.ZoomFactor = value
                                        Chiaki.settings.sZoomFactor = value
                                    }
                                    KeyNavigation.up: aspectBox
                                    KeyNavigation.down: presetBox
                                }
                                Caption { text: qsTr("Preset") }
                                PanelSelect {
                                    id: presetBox
                                    Layout.fillWidth: true
                                    Accessible.name: qsTr("Video preset")
                                    model: [qsTr("Fast"), qsTr("Default"), qsTr("High Quality"), qsTr("HQ + Spatial"),
                                        qsTr("HQ + Advanced"), qsTr("Custom")]
                                    currentIndex: Chiaki.window.videoPreset
                                    onActivated: index => {
                                        Chiaki.window.videoPreset = index
                                        Chiaki.settings.videoPreset = index
                                    }
                                    KeyNavigation.up: zoomSlider.visible ? zoomSlider : aspectBox
                                    KeyNavigation.down: vsrBox.visible ? vsrBox : displayButton
                                    KeyNavigation.right: micButton.enabled ? micButton : statsButton
                                }
                                Caption { visible: vsrBox.visible; text: qsTr("RTX VSR") }
                                PanelSelect {
                                    id: vsrBox
                                    visible: Chiaki.window.rtxVsrAvailable
                                    Layout.fillWidth: true
                                    Accessible.name: qsTr("RTX Video Super Resolution")
                                    model: [qsTr("Off"), qsTr("Low"), qsTr("Medium"), qsTr("High"), qsTr("Ultra")]
                                    currentIndex: Chiaki.window.rtxVsrQuality
                                    onActivated: index => Chiaki.window.rtxVsrQuality = index
                                    ToolTip.visible: hovered && currentIndex > 0
                                    ToolTip.text: Chiaki.window.rtxVsrActive ? qsTr("RTX VSR active") : qsTr("Waiting for eligible video")
                                    KeyNavigation.up: presetBox
                                    KeyNavigation.down: displayButton
                                    KeyNavigation.right: micButton.enabled ? micButton : statsButton
                                }
                            }
                        }
                    }

                    Section {
                        Layout.fillWidth: true
                        contentItem: ColumnLayout {
                            spacing: 8
                            Label { text: qsTr("Session"); color: panel.ink; font.pixelSize: 17; font.weight: Font.DemiBold }
                            Caption {
                                Layout.fillWidth: true
                                visible: !!panel.session
                                wrapMode: Text.WordWrap
                                text: panel.session
                                    ? qsTr("%1 Mbps · %2% packet loss")
                                        .arg(panel.session.measuredBitrate.toFixed(1))
                                        .arg((panel.session.averagePacketLoss * 100).toFixed(2))
                                    : ""
                            }
                            RowLayout {
                                Caption { text: qsTr("Volume") }
                                Slider {
                                    id: volumeControl
                                    Layout.fillWidth: true
                                    from: 0
                                    to: 128
                                    stepSize: 1
                                    value: Chiaki.settings.audioVolume
                                    onMoved: Chiaki.settings.audioVolume = value
                                    Accessible.name: qsTr("Remote Play volume")
                                    KeyNavigation.up: resumeButton
                                    KeyNavigation.down: micButton.enabled ? micButton : statsButton
                                }
                                Label {
                                    Layout.minimumWidth: 42
                                    horizontalAlignment: Text.AlignRight
                                    color: panel.ink
                                    text: qsTr("%1%").arg(Math.round(volumeControl.value * 100 / 128))
                                }
                            }
                            Flow {
                                Layout.fillWidth: true
                                Layout.preferredHeight: implicitHeight
                                spacing: 8
                                PanelButton {
                                    id: micButton
                                    enabled: !!panel.session && panel.session.connected
                                    text: panel.session && !panel.session.muted ? qsTr("Mic on") : qsTr("Mic muted")
                                    onClicked: if (panel.session) panel.session.muted = !panel.session.muted
                                    KeyNavigation.up: volumeControl
                                    KeyNavigation.left: presetBox
                                    KeyNavigation.right: statsButton
                                    KeyNavigation.down: displayButton
                                }
                                PanelButton {
                                    id: statsButton
                                    text: Chiaki.settings.showStreamStats ? qsTr("Stats on") : qsTr("Stats off")
                                    onClicked: Chiaki.settings.showStreamStats = !Chiaki.settings.showStreamStats
                                    KeyNavigation.up: volumeControl
                                    KeyNavigation.left: micButton.enabled ? micButton : presetBox
                                    KeyNavigation.down: endButton
                                }
                            }
                            Caption {
                                Layout.fillWidth: true
                                text: panel.session && panel.session.recoveryStats ? panel.session.recoveryStats : ""
                                visible: text.length > 0
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }

                Flow {
                    Layout.fillWidth: true
                    Layout.preferredHeight: implicitHeight
                    spacing: 8
                    PanelButton {
                        id: displayButton
                        text: qsTr("Display settings")
                        onClicked: panel.displayRequested()
                        KeyNavigation.up: vsrBox.visible ? vsrBox : presetBox
                        KeyNavigation.right: allSettingsButton
                        KeyNavigation.down: resumeButton
                    }
                    PanelButton {
                        id: allSettingsButton
                        text: qsTr("All settings")
                        onClicked: panel.settingsRequested()
                        KeyNavigation.up: vsrBox.visible ? vsrBox : presetBox
                        KeyNavigation.left: displayButton
                        KeyNavigation.right: advancedButton.visible ? advancedButton : endButton
                        KeyNavigation.down: resumeButton
                    }
                    PanelButton {
                        id: advancedButton
                        visible: Chiaki.window.videoPreset === ChiakiWindow.VideoPreset.Custom
                        text: qsTr("Advanced shaders")
                        onClicked: panel.advancedRequested()
                        KeyNavigation.up: statsButton
                        KeyNavigation.left: allSettingsButton
                        KeyNavigation.right: endButton
                        KeyNavigation.down: resumeButton
                    }
                    PanelButton {
                        id: endButton
                        text: qsTr("Disconnect")
                        warning: true
                        onClicked: panel.disconnectRequested()
                        KeyNavigation.up: statsButton
                        KeyNavigation.left: advancedButton.visible ? advancedButton : allSettingsButton
                        KeyNavigation.right: resumeButton
                        KeyNavigation.down: resumeButton
                    }
                }
            }
        }
    }
}
