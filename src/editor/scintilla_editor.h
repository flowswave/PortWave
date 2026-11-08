#pragma once
#include "editor/editor_interface.h"
#include <memory>

namespace portwave {
std::unique_ptr<EditorInterface> makeEditor(QWidget* parent, bool cppHighlight = false);
}
