#include <cstring>

#include "emu/state.h"
#include "emu/unwind.h"
#include "ir/pass.h"
#include "main/ir_dbt.h"
#include "main/signal.h"
#include "riscv/basic_block.h"
#include "riscv/context.h"
#include "riscv/decoder.h"
#include "riscv/disassembler.h"
#include "riscv/frontend.h"
#include "riscv/instruction.h"
#include "riscv/opcode.h"
#include "util/assert.h"
#include "util/format.h"
#include "util/memory.h"
#include "x86/backend.h"

// Declare the exception handling registration functions.
extern "C" void __register_frame(void*);
extern "C" void __deregister_frame(void*);

_Unwind_Reason_Code ir_dbt_personality(
    [[maybe_unused]] int version,
    [[maybe_unused]] _Unwind_Action actions,
    [[maybe_unused]] uint64_t exception_class,
    [[maybe_unused]] struct _Unwind_Exception *exception_object,
    [[maybe_unused]] struct _Unwind_Context *context
) {
    return _URC_CONTINUE_UNWIND;
}

static void generate_eh_frame(std::byte* data) {
    // TODO: Create an dwarf generation to replace this hard-coded template.
    static const unsigned char cie_template[] = {
        // CIE
        // Length
        0x1C, 0x00, 0x00, 0x00,
        // CIE
        0x00, 0x00, 0x00, 0x00,
        // Version
        0x01,
        // Augmentation string
        'z', 'P', 'L', 0,
        // Instruction alignment factor = 1
        0x01,
        // Data alignment factor = -8
        0x78,
        // Return register number
        0x10,
        // Augmentation data
        0x0A, // Data for z
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // abs format, personality routine
        0x00, // abs format for LSDA
        // Instructions
        // def_cfa(rsp, 8)
        0x0c, 0x07, 0x08,
        // offset(rsp, cfa-8)
        0x90, 0x01,
        // Padding

        // FDE
        // Length
        0x24, 0x00, 0x00, 0x00,
        // CIE Pointer
        0x24, 0x00, 0x00, 0x00,
        // Initial location
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // Augumentation data
        0x8,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // LSDA
        // advance_loc(1)
        0x41,
        // def_cfa_offset(16)
        0x0E, 0x10,
        // offset(rbp, cfa-16)
        0x86, 0x02,
        // Padding
        0x00, 0x00,

        0x00, 0x00, 0x00, 0x00
    };

    uint8_t *cie = new uint8_t[sizeof(cie_template)];

    memcpy(cie, cie_template, sizeof(cie_template));
    util::write_as<uint64_t>(cie + 0x12, reinterpret_cast<uint64_t>(ir_dbt_personality));
    util::write_as<uint64_t>(cie + 0x28, reinterpret_cast<uint64_t>(data));
    util::write_as<uint64_t>(cie + 0x30, 4096);
    util::write_as<uint64_t>(cie + 0x39, 0);

    __register_frame(cie);
}

Ir_dbt::Ir_dbt(emu::State& state) noexcept: state_{state} {
    icache_tag_ = std::unique_ptr<emu::reg_t[]> { new emu::reg_t[4096] };
    icache_ = std::unique_ptr<std::byte*[]> { new std::byte*[4096] };
    for (size_t i = 0; i < 4096; i++) {
        icache_tag_[i] = 0;
    }
}

void Ir_dbt::step(riscv::Context& context) {
    const emu::reg_t pc = context.pc;
    const ptrdiff_t tag = (pc >> 1) & 4095;

    // If the cache misses, compile the current block.
    if (UNLIKELY(icache_tag_[tag] != pc)) {
        compile(pc);
    }

    auto func = reinterpret_cast<void(*)(riscv::Context&)>(icache_[tag]);
    ASSERT(func);
    func(context);

    // ir::pass::Evaluator{&context}.run(graph_cache_[pc]);
}

void Ir_dbt::compile(emu::reg_t pc) {
    const ptrdiff_t tag = (pc >> 1) & 4095;
    auto& code_buffer = inst_cache_[pc];

    if (!code_buffer.size()) {
        code_buffer.reserve(4096);

        ir::Graph& graph = graph_cache_[pc];
        riscv::Decoder decoder {&state_, pc};
        riscv::Basic_block basic_block = decoder.decode_basic_block();

        graph = riscv::compile(state_, basic_block);
        ir::pass::Register_access_elimination{66}.run(graph);
        ir::pass::Local_value_numbering{}.run(graph);

        if (state_.disassemble) {
            // ir::pass::Dot_printer{}.run(graph);
            util::log("Translating {:x} to {:x}\n", pc, (uintptr_t)code_buffer.data());
        }

        ir::pass::Block_marker{}.run(graph);
        graph.garbage_collect();

        x86::Backend{state_, code_buffer}.run(graph);
        generate_eh_frame(code_buffer.data());
    }

    // Update tag to reflect newly compiled code.
    icache_[tag] = code_buffer.data();
    icache_tag_[tag] = pc;
}