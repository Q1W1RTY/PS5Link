import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

Slider {
    id: control
    property bool firstInFocusChain: false
    property bool lastInFocusChain: false
    property bool sendOutput: false

    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        implicitWidth: 180
        implicitHeight: 6
        width: control.availableWidth
        height: implicitHeight
        radius: 3
        color: "#3D3D45"

        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            color: "#0A84FF"
            radius: 3
        }
    }

    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + control.availableHeight / 2 - height / 2
        implicitWidth: 18
        implicitHeight: 18
        radius: 9
        color: control.pressed ? "#DCEBFF" : "#F0F1F3"
        border.color: control.visualFocus ? "#0A84FF" : "#66FFFFFF"
        border.width: control.visualFocus ? 2 : 1
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
        }
    }
}
