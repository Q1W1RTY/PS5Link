import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

Button {
    id: control
    property bool firstInFocusChain: false
    property bool lastInFocusChain: false
    property bool sendOutput: false

    leftPadding: 16
    rightPadding: 16
    topPadding: 9
    bottomPadding: 9
    font.weight: Font.Medium

    background: Rectangle {
        implicitWidth: 96
        implicitHeight: 38
        radius: 13
        color: !control.enabled ? "#222228"
            : control.visualFocus ? "#0A84FF"
            : control.down ? "#26262E"
            : control.hovered ? "#3E3E48"
            : "#303038"
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? "#72B4FF"
            : control.hovered ? "#4E4E5A"
            : "#3E3E48"
    }

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
            if (!firstInFocusChain) {
                let item = nextItemInFocusChain(false);
                if (item)
                    item.forceActiveFocus(Qt.TabFocusReason);
                if(!sendOutput)
                    event.accepted = true;
            }
            break;
        case Qt.Key_Down:
            if (!lastInFocusChain) {
                let item = nextItemInFocusChain();
                if (item)
                    item.forceActiveFocus(Qt.TabFocusReason);
                if(!sendOutput)
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
}
