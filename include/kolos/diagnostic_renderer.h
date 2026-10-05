#pragma once
#include <kolos/ahb_import.h>
namespace kolos {
// Synchronous clear including ownership/layout transitions; no slot selection.
void draw(Context& v, Imported& b, uint32_t frame);
}
