#ifndef FOREVERTAS_APP_KEYBOARD_LAYOUT_H
#define FOREVERTAS_APP_KEYBOARD_LAYOUT_H

namespace forevertas::app {

// Whether the keyboard's letters sit like AZERTY's (the key QWERTY calls Q
// types A), as best the system says: the keyboard itself on Windows; the
// desktop's, X11's or the system's configured layout on Linux; the input
// language otherwise.
bool KeyboardLettersAreAzerty();

}  // namespace forevertas::app

#endif
