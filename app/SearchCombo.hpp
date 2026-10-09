#pragma once
// A drop list one can search: typing in it lists the items whose text contains what is typed (any case, anywhere in the
// text), Enter or a click takes one (as a click in the list would: activated). Leaving it with text that names no item puts
// the chosen item's text back, unless free text is what it takes (a material written by name). Every drop list of things to
// choose from (bodies, components, materials, fans, joints, studies, versions) goes through it.
class QComboBox;

namespace search_combo {
void enable(QComboBox* combo, bool freeText = false);
}
