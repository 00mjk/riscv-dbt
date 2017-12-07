#ifndef RISCV_FRONTEND_H
#define RISCV_FRONTEND_H

#include "ir/instruction.h"

namespace riscv {

class Basic_block;

ir::Graph compile(const Basic_block& block);

} // riscv

#endif