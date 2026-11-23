import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

Item {
    id: dialog
    property alias header: headerLabel.text
    property alias title: titleLabel.text
    property alias buttonText: okButton.text
    property alias buttonEnabled: okButton.enabled
    property alias buttonVisible: okButton.visible
    property Item restoreFocusItem
    default property Item mainItem: null

    signal accepted()
    signal rejected()

    function close() {
        root.closeDialog();
    }

    Keys.onEscapePressed: close()

    Keys.onMenuPressed: {
        if (okButton.enabled)
            okButton.clicked()
    }

    StackView.onDeactivating: {
        restoreFocusItem = Window.window.activeFocusItem;
    }

    StackView.onActivated: {
        if (!restoreFocusItem) {
            let item = mainItem.nextItemInFocusChain();
            if (item)
                item.forceActiveFocus(Qt.TabFocusReason);
        } else {
            restoreFocusItem.forceActiveFocus(Qt.TabFocusReason);
            restoreFocusItem = null;
        }
    }

    onMainItemChanged: {
        if (mainItem) {
            mainItem.parent = contentItem;
            mainItem.anchors.fill = contentItem;
        }
    }

    // DS5Link outer shell background
    Rectangle {
        anchors.fill: parent
        color: "#18181B"
    }

    // DS5Link Header Card
    Rectangle {
        id: toolBar
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            topMargin: 16
            leftMargin: 20
            rightMargin: 20
        }
        height: Math.max(66, titleBlock.implicitHeight + 20)
        radius: 20
        color: "#232328"
        border.color: "#32323A"
        border.width: 1

        RowLayout {
            anchors {
                fill: parent
                leftMargin: 14
                rightMargin: 14
            }
            spacing: 14

            Button {
                Layout.preferredHeight: 38
                leftPadding: 14
                rightPadding: 16
                text: "❮  " + qsTr("Back")
                focusPolicy: Qt.NoFocus
                font.pixelSize: 13
                font.weight: Font.Medium
                onClicked: {
                    dialog.rejected();
                    dialog.close();
                }
                contentItem: Text {
                    text: parent.text
                    font: parent.font
                    color: "#F0F1F3"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 14
                    color: parent.down ? "#25252C" : parent.hovered ? "#3B3B45" : "#2C2C34"
                    border.color: "#3D3D47"
                    border.width: 1
                }
            }

            ColumnLayout {
                id: titleBlock
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 2
                Layout.alignment: Qt.AlignVCenter

                Label {
                    id: titleLabel
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    wrapMode: Text.Wrap
                    font.weight: Font.DemiBold
                    font.pixelSize: 19
                    color: "#FFFFFF"
                }

                Label {
                    id: headerLabel
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    wrapMode: Text.Wrap
                    visible: text.length > 0
                    font.pixelSize: 12
                    color: "#A5A5A5"
                }
            }

            // DS5Link active status badge pill
            Rectangle {
                radius: 11
                color: "#2C2C34"
                border.color: "#3C3C45"
                border.width: 1
                implicitHeight: 24
                implicitWidth: brandBadgeRow.implicitWidth + 18
                visible: toolBar.width >= 560

                RowLayout {
                    id: brandBadgeRow
                    anchors.centerIn: parent
                    spacing: 6

                    Rectangle {
                        width: 6
                        height: 6
                        radius: 3
                        color: "#0A84FF"
                    }

                    Label {
                        text: "PS5Link"
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: "#D0D2D6"
                    }
                }
            }

            Button {
                id: okButton
                Layout.preferredHeight: 38
                leftPadding: 16
                rightPadding: 16
                font.pixelSize: 14
                font.weight: Font.DemiBold
                focusPolicy: Qt.NoFocus
                onClicked: dialog.accepted()
                contentItem: Text {
                    text: okButton.text
                    font: okButton.font
                    color: okButton.enabled ? "#FFFFFF" : "#7A7A85"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 14
                    color: !okButton.enabled ? "#25252C"
                        : okButton.down ? "#0768CC"
                        : okButton.hovered ? "#2494FF"
                        : "#0A84FF"
                    border.color: okButton.enabled ? "#48A4FF" : "#34343E"
                    border.width: 1
                }
            }
        }
    }

    // DS5Link Inner Content Card
    Rectangle {
        anchors {
            top: toolBar.bottom
            left: parent.left
            right: parent.right
            bottom: parent.bottom
            topMargin: 12
            leftMargin: 20
            rightMargin: 20
            bottomMargin: 16
        }
        radius: 20
        color: "#232328"
        border.color: "#32323A"
        border.width: 1
        clip: true

        Item {
            id: contentItem
            anchors {
                fill: parent
                margins: 12
            }
        }
    }
}
