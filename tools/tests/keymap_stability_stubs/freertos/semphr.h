#pragma once
#include <mutex>
using SemaphoreHandle_t = std::recursive_mutex*;
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { static std::recursive_mutex m; return &m; }
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t m, unsigned) { m->lock(); return 1; }
inline int xSemaphoreGiveRecursive(SemaphoreHandle_t m) { m->unlock(); return 1; }
