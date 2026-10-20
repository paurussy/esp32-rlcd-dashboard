/*
  Arduino.h - Minimal host stub so src/ui.cpp builds on a PC
*/
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

using std::max;
using std::min;

#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
