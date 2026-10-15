#include "stratakv/version.h"

#ifndef STRATAKV_VERSION
#define STRATAKV_VERSION "0.0.0-unknown"
#endif

namespace stratakv {

const char* Version() { return STRATAKV_VERSION; }

}  // namespace stratakv
