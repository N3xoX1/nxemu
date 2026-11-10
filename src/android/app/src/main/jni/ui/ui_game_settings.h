#pragma once

#include <cstdint>

namespace UiGameSettings
{
bool CanEdit(uint64_t programId);
uint64_t RunningProgramId();
bool Open(uint64_t programId);
void Close();
bool Editing();

int ReadInt(const char * key);
bool ReadBool(const char * key);
void SetInt(const char * key, int value);
void SetBool(const char * key, bool value);

bool Overridden(const char * key);
int OverriddenCount();
void InheritAll();
} // namespace UiGameSettings
