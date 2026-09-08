#pragma once

#ifndef SIMULATOR_BUILD

void hexhoundInitBacklightHardware();
void hexhoundSetBacklight(bool on);

#else

inline void hexhoundInitBacklightHardware() {}
inline void hexhoundSetBacklight(bool) {}

#endif
