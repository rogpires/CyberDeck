#pragma once

#include <stdbool.h>
#include <stdint.h>

// Horizontal icon carousel (touch + arrow keys). Returns the chosen index, or -1 for back.
// labels[i] is shown under the icon; icon_index[i] selects an icon from icons.h.
// accents[i] is the 0xRRGGBB highlight color for item i.
int carousel_run(const char* const* labels, const int* icon_index, const uint32_t* accents, int count, int start, bool show_back);
