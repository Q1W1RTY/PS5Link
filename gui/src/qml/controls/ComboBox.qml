import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

ComboBox {
    id: control
    property bool firstInFocusChain: false
    property bool lastInFocusChain: false
    implicitContentWidthPolicy: ComboBox.WidestText
    leftPadding: 14
    rightPadding: 32
    ToolTip.visible: hovered && !popup.visible && displayText.length > 0
    ToolTip.text: displayText
    ToolTip.delay: 550

    background: Rectangle {
        implicitWidth: 160
        implicitHeight: 38
        radius: 12
        color: !control.enabled ? "#202026"
            : control.visualFocus ? "#2E3648"
            : control.hovered ? "#3A3A44"
            : "#2E2E36"
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? "#0A84FF" : "#40404B"
    }

    Keys.onPressed: (event) => {
        switch (event.key) {
        case Qt.Key_Up:
            if (!popup.visible) {
                let item = nextItemInFocusChain(false);
                if (!firstInFocusChain && item)
                    item.forceActiveFocus(Qt.TabFocusReason);
                event.accepted = true;
            }
            break;
        case Qt.Key_Down:
            if (!popup.visible) {
                let item = nextItemInFocusChain();
                if (!lastInFocusChain && item)
                    item.forceActiveFocus(Qt.TabFocusReason);
                event.accepted = true;
            }
            break;
        case Qt.Key_Return:
            if (popup.visible) {
                activated(highlightedIndex);
                popup.close();
            } else {
                popup.open();
            }
            event.accepted = true;
            break;
        }
    }

    Keys.onReleased: (event) => {
        if (event.key == Qt.Key_Return)
            event.accepted = true;
    }
}
