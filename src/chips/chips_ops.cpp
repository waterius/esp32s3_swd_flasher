#include "chips/chips.h"
#include "chips/ops.h"

namespace chips {

const Ops *ops_for(const chip::Desc &desc) {
  if (&desc == &py32f002b) return &py32f002b_ops;
  return nullptr;
}

}  // namespace chips
