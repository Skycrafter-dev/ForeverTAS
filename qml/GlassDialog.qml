import QtQuick
import QtQuick.Controls

// A dialog on a glass sheet. The Basic style draws the title bar and the
// button box on opaque fills; here they sit on the glass too.
Dialog {
    id: control

    background: GlassPopupBackground {}

    header: Label {
        text: control.title
        visible: control.title.length > 0
        elide: Label.ElideRight
        color: AppTheme.text
        font.bold: true
        leftPadding: 16
        rightPadding: 16
        topPadding: 14
        bottomPadding: 4
    }
}
