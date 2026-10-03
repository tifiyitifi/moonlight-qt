import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2
import QtQuick.Window 2.2

import ComputerModel 1.0
import ComputerManager 1.0
import SdlGamepadKeyNavigation 1.0

// Per-PC settings. Structured like SettingsView.qml so that future per-PC
// options can simply be appended as another GroupBox, without touching
// PcView.qml or main.qml.
Flickable {
    id: pcSettingsPage
    objectName: qsTr("PC Settings")

    property int computerIndex
    property string computerUuid
    property ComputerModel computerModel : createModel()

    // Mask currently applied to the PC. Tracked separately so we can tell our
    // own updates apart from anything the backend pushes back.
    property int allowedMask: 15

    // Suppresses the change handlers while we populate the checkboxes
    property bool initializing: true

    // Suppresses the change handlers while we restore the previous selection
    property bool restoringSelection: false

    // Expanded state of the note explaining what the connection type selection
    // can and cannot do. Deliberately not persisted: this page is created anew
    // every time PC Settings is opened, and StackView.onActivated derives it
    // from the mask, so the note is expanded exactly when a selection is
    // narrowed and stays folded while everything is allowed.
    property bool scopeNoteExpanded: false

    readonly property int nltWired: 1
    readonly property int nltWireless: 2
    readonly property int nltVirtual: 4
    readonly property int nltOther: 8
    readonly property int nltAll: 15

    boundsBehavior: Flickable.OvershootBounds

    contentWidth: settingsColumn.width
    contentHeight: settingsColumn.height

    ScrollBar.vertical: ScrollBar {
        anchors {
            left: parent.right
            leftMargin: -10
        }
    }

    function isChildOfFlickable(item) {
        while (item) {
            if (item.parent === contentItem) {
                return true
            }

            item = item.parent
        }
        return false
    }

    NumberAnimation on contentY {
        id: autoScrollAnimation
        duration: 100
    }

    Window.onActiveFocusItemChanged: {
        var item = Window.activeFocusItem
        if (item) {
            // Ignore non-child elements like the toolbar buttons
            if (!isChildOfFlickable(item)) {
                return
            }

            // Map the focus item's position into our content item's coordinate space
            var pos = item.mapToItem(contentItem, 0, 0)

            // Ensure some extra space is visible around the element we're scrolling to
            var scrollMargin = height > 100 ? 50 : 0

            if (pos.y - scrollMargin < contentY) {
                autoScrollAnimation.from = contentY
                autoScrollAnimation.to = Math.max(pos.y - scrollMargin, 0)
                autoScrollAnimation.start()
            }
            else if (pos.y + item.height + scrollMargin > contentY + height) {
                autoScrollAnimation.from = contentY
                autoScrollAnimation.to = Math.min(pos.y + item.height + scrollMargin - height, contentY + height, contentHeight - height)
                autoScrollAnimation.start()
            }
        }
    }

    function createModel()
    {
        // Parent to the page itself so the model is destroyed together with it.
        // Parenting to the StackView (via `parent`) would leak one model per
        // page open, each still listening to ComputerManager signals.
        var model = Qt.createQmlObject('import ComputerModel 1.0; ComputerModel {}', pcSettingsPage, '')
        model.initialize(ComputerManager)
        return model
    }

    function resolveCurrentComputer()
    {
        // Prefer the stable uuid: the positional index may already point at a
        // different PC if the host list was reordered between the menu click
        // and this page's creation.
        if (computerUuid.length > 0) {
            return computerModel.setCurrentIndexByUuid(computerUuid)
        }

        computerModel.currentIndex = computerIndex
        return computerModel.currentIndex >= 0
    }

    // computerIndex/computerUuid are assigned by createObject(), which happens
    // after the computerModel binding above has already been evaluated, so the
    // selection is only known to be correct from here on.
    Component.onCompleted: {
        resolveCurrentComputer()
    }

    StackView.onActivated: {
        // This enables Tab and BackTab based navigation rather than arrow keys.
        SdlGamepadKeyNavigation.setUiNavMode(true)

        // Populate the checkboxes from the stored mask. Re-arm the guard here
        // because onActivated also fires when returning from a page (such as
        // SettingsView) that was pushed on top of this one. Re-resolve by uuid
        // first: the host list may have been refreshed while we were away.
        initializing = true
        var resolved = true
        if (computerUuid.length > 0) {
            resolved = computerModel.setCurrentIndexByUuid(computerUuid)
        }
        // No positional fallback here on purpose. A uuid that no longer
        // resolves means this host is gone, or its identity changed, and
        // computerIndex by then names whichever PC occupies that slot -
        // selecting it would show and edit a different host's mask. Give up
        // instead and let the branch below present the default selection.
        if (!resolved) {
            // The host is gone. Show the default selection rather than an
            // all-unchecked state that would silently drop the next
            // applyMask() on the floor, since
            // setConnectionOptionsForCurrentComputer() ignores invalid indices.
            allowedMask = nltAll
            allowWired.checked = true
            allowWireless.checked = true
            allowVirtual.checked = true
            allowOther.checked = true
            noTypesSelectedWarning.visible = false
            initializing = false
        } else {
            allowedMask = computerModel.currentAllowedLinkTypes
            // A zero mask means the model has no selection (or a corrupted
            // backend value). Never present that as "nothing selected".
            if (allowedMask === 0) {
                allowedMask = nltAll
            }
            allowWired.checked = (allowedMask & nltWired) !== 0
            allowWireless.checked = (allowedMask & nltWireless) !== 0
            allowVirtual.checked = (allowedMask & nltVirtual) !== 0
            allowOther.checked = (allowedMask & nltOther) !== 0
            noTypesSelectedWarning.visible = false
            initializing = false
        }

        // Derived from the mask rather than remembered, so the note always
        // matches the selection it is explaining: expanded when something is
        // deselected, folded when everything is allowed. This also runs when
        // returning from a page pushed on top of this one, which is the point -
        // the mask may have been changed elsewhere in the meantime.
        scopeNoteExpanded = allowedMask !== nltAll

        // Highlight the first item if a gamepad is connected
        if (SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            allowWired.forceActiveFocus(Qt.TabFocus)
        }
    }

    StackView.onDeactivating: {
        SdlGamepadKeyNavigation.setUiNavMode(false)
    }

    // Recomputes the mask from the checkboxes and pushes it to the backend
    function applyMask() {
        if (initializing || restoringSelection) {
            return
        }

        var mask = 0
        if (allowWired.checked) {
            mask |= nltWired
        }
        if (allowWireless.checked) {
            mask |= nltWireless
        }
        if (allowVirtual.checked) {
            mask |= nltVirtual
        }
        if (allowOther.checked) {
            mask |= nltOther
        }

        if (mask === 0) {
            // Deselecting the last remaining type would make this PC
            // unreachable, so refuse that one toggle: put the mask back the
            // way it was before this click, which re-checks only the type the
            // user just cleared and leaves the ones they disabled earlier
            // alone. Restoring the default (all types) here would silently
            // re-enable those, and the backend writes the change straight
            // through. allowedMask is exactly the pre-toggle state, because the
            // early return below keeps it in sync with the checkboxes. Setting
            // `checked` re-enters this function synchronously, so the guard
            // must be up first.
            restoringSelection = true
            allowWired.checked = (allowedMask & nltWired) !== 0
            allowWireless.checked = (allowedMask & nltWireless) !== 0
            allowVirtual.checked = (allowedMask & nltVirtual) !== 0
            allowOther.checked = (allowedMask & nltOther) !== 0
            restoringSelection = false
            noTypesSelectedWarning.visible = true
            mask = allowedMask
        } else {
            noTypesSelectedWarning.visible = false
        }

        if (mask === allowedMask) {
            return
        }

        if (computerModel.currentIndex < 0) {
            // No host selected (deleted or not yet populated). The backend
            // call below would silently do nothing, so don't advance
            // allowedMask either, or the next toggle would compare against
            // a value that was never applied.
            return
        }

        allowedMask = mask
        // Narrowing the selection is exactly when the note is relevant, so
        // follow it: expanding on the toggle that deselects a type, and
        // folding again once everything is re-enabled. Doing this here rather
        // than binding to allowedMask keeps a manual fold from being undone by
        // an unrelated toggle that leaves the mask unchanged - applyMask()
        // returns early in that case.
        scopeNoteExpanded = (mask !== nltAll)
        computerModel.setConnectionOptionsForCurrentComputer(mask)
    }

    Column {
        id: settingsColumn
        padding: 10
        width: pcSettingsPage.width
        spacing: 15

        GroupBox {
            id: thisPcGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("This PC") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    id: pcNameLabel
                    text: pcSettingsPage.computerModel.currentName.length > 0
                          ? pcSettingsPage.computerModel.currentName
                          : qsTr("Unknown PC")
                    font.pointSize: 16
                    font.bold: true
                    wrapMode: Text.Wrap
                    elide: Text.ElideRight
                }

                Label {
                    width: parent.width
                    id: pcStatusDesc
                    text: qsTr("Moonlight uses a single network connection to reach a PC. If this PC is reachable over more than one, the settings below control which of them Moonlight is allowed to use.")
                    font.pointSize: 9
                    wrapMode: Text.Wrap
                }
            }
        }

        GroupBox {
            id: connectionGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("Connection") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    id: connTypeTitle
                    text: qsTr("Connection Type")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                Label {
                    width: parent.width
                    id: connTypeDesc
                    // Kept to what holds in every network layout. Whether the
                    // selection also keeps the traffic off the deselected
                    // adapter depends on the topology, which is what the
                    // expandable note below spells out - so this label promises
                    // nothing about the adapter, and the previous wording here
                    // ("will not probe or start a stream over it") was the one
                    // that only held when the two links are on different
                    // subnets.
                    text: qsTr("Deselect a connection type to stop Moonlight from using it for this PC. Leave everything selected to keep the default behavior of using whichever connection responds first. If deselecting leaves this PC with no address at all, it is reported as offline rather than falling back to a connection type you removed. The paths currently available are listed below.")
                    font.pointSize: 9
                    wrapMode: Text.Wrap
                }

                CheckBox {
                    id: allowWired
                    width: parent.width
                    text: qsTr("Ethernet (Wired)")
                    font.pointSize: 12
                    onCheckedChanged: pcSettingsPage.applyMask()
                }

                CheckBox {
                    id: allowWireless
                    width: parent.width
                    text: qsTr("Wi-Fi (Wireless)")
                    font.pointSize: 12
                    onCheckedChanged: pcSettingsPage.applyMask()
                }

                CheckBox {
                    id: allowVirtual
                    width: parent.width
                    text: qsTr("VPN / Virtual Adapter")
                    font.pointSize: 12
                    onCheckedChanged: pcSettingsPage.applyMask()
                }

                CheckBox {
                    id: allowOther
                    width: parent.width
                    text: qsTr("Other / Unknown")
                    font.pointSize: 12
                    onCheckedChanged: pcSettingsPage.applyMask()

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Allow any network adapter that Moonlight does not recognize. Leave this selected unless you are certain no other adapter is involved.")
                }

                Label {
                    width: parent.width
                    id: noTypesSelectedWarning
                    visible: false
                    text: qsTr("At least one connection type must stay selected, otherwise this PC could not be reached.")
                    font.pointSize: 9
                    font.italic: true
                    wrapMode: Text.Wrap
                }

                // Explains the limit of the checkboxes above, which the
                // checkboxes alone cannot express. The mask gates ADDRESSES,
                // not adapters: an address with no route over an allowed
                // connection type is dropped, and an address that survives is
                // pinned for the stream (see the RTSP pin in
                // Session::startConnectionAsync()). Whether that also keeps the
                // bytes off the deselected adapter depends on the topology:
                //
                //  - Different subnets, e.g. a direct cable between the two PCs
                //    (which normally falls back to a 169.254.x.x link-local
                //    address): the deselected address has no route over ANY of
                //    our adapters, so it is dropped outright and the selection
                //    is fully in effect.
                //
                //  - Both PCs on the same router and on the same Wi-Fi: every
                //    address of the host is reachable over our wired adapter as
                //    well, because the wired NIC is on-link for that whole
                //    subnet, so all of them survive and the kernel picks the
                //    adapter by interface metric instead. Nothing in the
                //    connection path binds an outgoing interface - NvHTTP binds
                //    no local address and moonlight-common-c asks the kernel
                //    for the source address - so the selection can only be
                //    reported here, never enforced. That is the case
                //    conflictWarning below is for, and the only two remedies
                //    are the ones the user controls outside Moonlight.
                //
                // Hidden while everything is selected, since there is nothing to
                // disambiguate then, and expanded by default whenever something
                // is deselected (see scopeNoteExpanded).
                Label {
                    id: scopeNoteHeader
                    width: parent.width
                    visible: pcSettingsPage.allowedMask !== pcSettingsPage.nltAll
                    text: (pcSettingsPage.scopeNoteExpanded ? "\u25be " : "\u25b8 ")
                          + qsTr("What this selection does and does not control")
                    font.pointSize: 10
                    font.bold: true
                    color: scopeNoteHit.containsMouse ? "#e0e0e0" : "#c0c0c0"
                    wrapMode: Text.Wrap

                    MouseArea {
                        id: scopeNoteHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: pcSettingsPage.scopeNoteExpanded = !pcSettingsPage.scopeNoteExpanded
                    }
                }

                Label {
                    width: parent.width
                    id: scopeNoteBody
                    visible: scopeNoteHeader.visible && pcSettingsPage.scopeNoteExpanded
                    // One qsTr() around a single literal, not a concatenation of
                    // three: lupdate only extracts the string it can see as one
                    // literal argument, so a concatenated argument would leave
                    // this untranslatable in every language.
                    text: qsTr("The selection filters this PC's addresses, not the network adapter the traffic leaves through. An address Moonlight may not use is never probed, and the stream is pinned to the address that did answer. Whether that also keeps the traffic off a deselected adapter depends on your network layout.\n\nDifferent networks - the selection holds. With a direct Ethernet cable between the two PCs (which normally falls back to a 169.254.x.x link-local address), this PC's Wi-Fi address has no route over your wired adapter at all, so it is dropped and discovery, the control channel and the stream all stay on the wired address.\n\nSame subnet - the selection cannot be enforced. When both PCs are plugged into the same router and are also on the same Wi-Fi, both of this PC's addresses are reachable over your wired adapter, so both of them survive the selection and Windows decides which adapter carries the traffic by interface metric. Moonlight reports that below instead of overriding it: lower the Interface Metric of the adapter you want to use, or put the two networks on separate subnets.")
                    font.pointSize: 9
                    wrapMode: Text.Wrap
                }

                Label {
                    width: parent.width
                    id: routesTitle
                    text: qsTr("Paths currently available to this PC:")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                Label {
                    width: parent.width
                    id: routesDesc
                    text: pcSettingsPage.computerModel.currentActiveRoute.length > 0
                          ? pcSettingsPage.computerModel.currentActiveRoute
                          : qsTr("This PC is offline, so no path is currently available.")
                    font.pointSize: 9
                    wrapMode: Text.Wrap
                }

                Label {
                    width: parent.width
                    id: conflictWarning
                    visible: pcSettingsPage.computerModel.currentConnectionConflict.length > 0
                    text: pcSettingsPage.computerModel.currentConnectionConflict
                    font.pointSize: 9
                    font.bold: true
                    color: "#D08080"
                    wrapMode: Text.Wrap
                }

                Label {
                    width: parent.width
                    id: unenforceableWarning
                    visible: pcSettingsPage.computerModel.currentUnenforceableLinkFilter.length > 0
                    text: pcSettingsPage.computerModel.currentUnenforceableLinkFilter
                    font.pointSize: 9
                    font.bold: true
                    color: "#D08080"
                    wrapMode: Text.Wrap
                }

                // The other side of the same story, and the reason it cannot
                // share the label above: there the mask is not in effect on a PC
                // that is still reachable, here it is very much in effect on a PC
                // that is not - every address it can be reached on needs a
                // connection type the user deselected, so the poller probes
                // nothing and the PC shows as offline. Says nothing about any
                // stream, because there is not one.
                Label {
                    width: parent.width
                    id: maskExcludedWarning
                    visible: pcSettingsPage.computerModel.currentMaskExcludedLinkFilter.length > 0
                    text: pcSettingsPage.computerModel.currentMaskExcludedLinkFilter
                    font.pointSize: 9
                    font.bold: true
                    color: "#D08080"
                    wrapMode: Text.Wrap
                }

                // And the third state, which is a side effect of enforcing the
                // selection rather than a statement about it: the selection
                // approved an address, that address is not answering, and the
                // name is not used as a fallback because the selection cannot be
                // applied to it. Says nothing about connection types, because
                // nothing there is wrong - it is the address that is out of date.
                Label {
                    width: parent.width
                    id: nameSuppressedWarning
                    visible: pcSettingsPage.computerModel.currentNameSuppressedLinkFilter.length > 0
                    text: pcSettingsPage.computerModel.currentNameSuppressedLinkFilter
                    font.pointSize: 9
                    font.bold: true
                    color: "#D08080"
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
