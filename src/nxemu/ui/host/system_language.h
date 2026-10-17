#pragma once
#include <nxemu-os/os_types.h>

// The console language nearest to the one the computer is used in. False
// when the computer does not say, or says one the console does not have.
bool SystemConsoleLanguage(Language & language);
