#pragma once
#include <QKeySequence>

namespace portwave {
// GUI-thread shortcut policy, shared by runtime bindings and staged settings.
// These helpers never change the OS keyboard layout or IME conversion state.
inline QKeySequence logSearchShortcut() {
    return QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F);
}
inline bool conflictsWithImeToggle(const QKeySequence& sequence) {
    return !sequence.isEmpty() &&
           sequence[0] == QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_F);
}
} // namespace portwave
