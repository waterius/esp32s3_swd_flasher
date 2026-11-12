// Поддержанные чипы (design §6, «Выбор»).
#pragma once
#include "core/chip.h"

namespace chips {

extern const chip::Desc py32f002b;

// Все описания по порядку; список для `chip` без аргумента.
extern const chip::Desc *const all[];
extern const uint8_t count;

// Описание по имени или nullptr.
const chip::Desc *find(const char *name);

}  // namespace chips
