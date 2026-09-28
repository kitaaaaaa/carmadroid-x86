#pragma once
#include "common.h"

namespace content {
extern bool g_restore;  // --censored turns this off
void apply();           // call after libParsons.so is loaded, before it runs
}
