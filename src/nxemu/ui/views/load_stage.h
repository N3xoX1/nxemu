#pragma once

// What NXCoreSetting::DiskCacheLoadStage reports while a game loads.
enum class LoadStage
{
    Game = 0,
    Shaders = 1,
    Launch = 2,
    ReadProfile = 3,
    ImportProfile = 4,
    BuildProfile = 5,
};
