import QtQuick

// Background for dialogs, menus, lists and popovers: a glass sheet that
// follows the popup around (popups are not part of the panel layout). It
// blurs the panels under it as well as the race view, and stays dark enough
// to read whatever the glass look is set to.
Glass {
    trackPosition: true
    seesPanels: true
    radius: 14 * AppTheme.cornerScale
    tintAmount: Math.min(0.85, Math.max(0.55, AppTheme.darkening + 0.25))
    shadowAlpha: 0.5
    shadowRadius: 26
    shadowOffset: 12
}
