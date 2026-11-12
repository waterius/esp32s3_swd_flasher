#include "chips/chips.h"

#include <string.h>

namespace chips {

const chip::Desc *const all[] = {&py32f002b};
const uint8_t count = sizeof all / sizeof all[0];

const chip::Desc *find(const char *name) {
  for (uint8_t i = 0; i < count; i++)
    if (strcmp(all[i]->name, name) == 0) return all[i];
  return nullptr;
}

}  // namespace chips
