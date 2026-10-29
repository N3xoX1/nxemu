// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_video_core/host1x/host1x.h"
namespace Tegra {
class GPU {
public:
    Host1x::Host1x& Host1x() { return host; }
private:
    Host1x::Host1x host;
};
}
