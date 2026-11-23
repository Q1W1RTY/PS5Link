import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material
import "controls" as C

Dialog {
    id: dialog
    property alias text: label.text
    property var callback
    property var rejectCallback
    property bool newDialogOpen: false
    property Item restoreFocusItem
    parent: Overlay.overlay
    x: Math.round((root.width - width) / 2)
    y: Math.round((root.height - height) / 2)
    modal: true
    padding: 24
    Material.roundedScale: Material.MediumScale
    onOpened: label.forceActiveFocus(Qt.TabFocusReason)
    onAccepted: {
        newDialogOpen = true;
        restoreFocus();
        callback();
    }
    onClosed: if(!newDialogOpen) { restoreFocus() }

    onRejected: {
        if(rejectCallback)
        {
            newDialogOpen = true;
            restoreFocus();
            rejectCallback();
        }
    }

    function restoreFocus() {
        if (restoreFocusItem)
            restoreFocusItem.forceActiveFocus(Qt.TabFocusReason);
        label.focus = false;
    }

    background: Rectangle {
        radius: 22
        color: "#232328"
        border.color: "#383842"
        border.width: 1
    }

    Component.onCompleted: {
        header.horizontalAlignment = Text.AlignHCenter;
        header.background = null;
    }

    ColumnLayout {
        spacing: 20

        Label {
            id: label
            color: "#F0F1F3"
            font.pixelSize: 16
            Keys.onEscapePressed: dialog.reject()
            Keys.onReturnPressed: dialog.accept()
        }

        RowLayout {
            Layout.alignment: Qt.AlignCenter
            spacing: 14

            Button {
                text: qsTr("Yes")
                leftPadding: 46
                rightPadding: 20
                implicitHeight: 40
                onClicked: dialog.accept()
                background: Rectangle {
                    radius: 14
                    color: parent.down ? "#0768CC" : parent.hovered ? "#2494FF" : "#0A84FF"
                    border.color: "#48A4FF"
                    border.width: 1
                }

                Image {
                    anchors {
                        left: parent.left
                        verticalCenter: parent.verticalCenter
                        leftMargin: 14
                    }
                    width: 22
                    height: 22
                    sourceSize: Qt.size(width, height)
                    source: root.controllerButton("cross")
                }
            }

            Button {
                text: qsTr("No")
                leftPadding: 46
                rightPadding: 20
                implicitHeight: 40
                onClicked: dialog.reject()
                background: Rectangle {
                    radius: 14
                    color: parent.down ? "#25252C" : parent.hovered ? "#3E3E48" : "#2E2E36"
                    border.color: "#40404B"
                    border.width: 1
                }

                Image {
                    anchors {
                        left: parent.left
                        verticalCenter: parent.verticalCenter
                        leftMargin: 14
                    }
                    width: 22
                    height: 22
                    sourceSize: Qt.size(width, height)
                    source: root.controllerButton("moon")
                }
            }
        }
    }
}
