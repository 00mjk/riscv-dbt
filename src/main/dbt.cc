#include "emu/state.h"
#include "main/dbt.h"
#include "riscv/basic_block.h"
#include "riscv/context.h"
#include "riscv/decoder.h"
#include "riscv/instruction.h"
#include "riscv/opcode.h"
#include "util/assert.h"
#include "util/code_buffer.h"
#include "util/format.h"
#include "util/memory.h"
#include "x86/builder.h"
#include "x86/disassembler.h"
#include "x86/encoder.h"
#include "x86/instruction.h"
#include "x86/opcode.h"

// Shorthand for instruction coding.
using namespace x86::builder;

// A separate class is used instead of generating code directly in Dbt_runtime, so it is easier to define and use
// helper functions that are shared by many instructions.
class Dbt_compiler {
private:
    Dbt_runtime& runtime_;
    x86::Encoder encoder_;

    Dbt_compiler& operator <<(const x86::Instruction& inst);

    /* Helper functions */
    void emit_move(int rd, int rs);
    void emit_move32(int rd, int rs);
    void emit_load_immediate(int rd, riscv::reg_t imm);
    void emit_branch(riscv::Instruction inst, riscv::reg_t pc_diff, x86::Condition_code cc);

    /* Translated instructions */
    void emit_jalr(riscv::Instruction inst, riscv::reg_t pc_diff);
    void emit_jal(riscv::Instruction inst, riscv::reg_t pc_diff);

    void emit_addi(riscv::Instruction inst);
    void emit_andi(riscv::Instruction inst);
    void emit_add(riscv::Instruction inst);
    void emit_sub(riscv::Instruction inst);
    void emit_and(riscv::Instruction inst);
    void emit_addiw(riscv::Instruction inst);
    void emit_addw(riscv::Instruction inst);
    void emit_lui(riscv::Instruction inst);

public:
    Dbt_compiler(Dbt_runtime& runtime, util::Code_buffer& buffer): runtime_{runtime}, encoder_{buffer} {}
    void compile(emu::reg_t pc);
};

Dbt_runtime::Dbt_runtime(emu::State& state): state_ {state} {
    icache_tag_ = std::unique_ptr<emu::reg_t[]> { new emu::reg_t[4096] };
    icache_ = std::unique_ptr<std::byte*[]> { new std::byte*[4096] };
}

void Dbt_runtime::step(riscv::Context& context) {
    const emu::reg_t pc = context.pc;
    const ptrdiff_t tag = (pc >> 1) & 4095;

    // If the cache misses, compile the current block.
    if (UNLIKELY(icache_tag_[tag] != pc)) {
        compile(pc);
    }

    auto func = reinterpret_cast<void(*)(riscv::Context&)>(icache_[tag]);
    ASSERT(func);
    func(context);
    return;
}

void Dbt_runtime::compile(emu::reg_t pc) {
    const ptrdiff_t tag = (pc >> 1) & 4095;
    util::Code_buffer& buffer = inst_cache_[pc];

    // Reserve a page in case that the buffer is empty, it saves the code buffer from reallocating (which is expensive
    // as code buffer is backed up by mmap and munmap at the moment.
    // If buffer.size() is not zero, it means that we have compiled the code previously but it is not in the hot cache.
    if (buffer.size() == 0) {
        buffer.reserve(4096);
        Dbt_compiler compiler { *this, buffer };
        compiler.compile(pc);
    }

    // Update tag to reflect newly compiled code.
    icache_[tag] = buffer.data();
    icache_tag_[tag] = pc;
}

Dbt_compiler& Dbt_compiler::operator <<(const x86::Instruction& inst) {
    bool disassemble = runtime_.state_.disassemble;
    std::byte *pc;
    if (disassemble) {
        pc = encoder_.buffer().data() + encoder_.buffer().size();
    }
    encoder_.encode(inst);
    if (disassemble) {
        std::byte *new_pc = encoder_.buffer().data() + encoder_.buffer().size();
        x86::disassembler::print_instruction(
            reinterpret_cast<uintptr_t>(pc), reinterpret_cast<const char*>(pc), new_pc - pc, inst);
    }
    return *this;
}

#define memory_of_register(reg) (x86::Register::rbp + (offsetof(riscv::Context, registers) + sizeof(emu::reg_t) * reg - 0x80))
#define memory_of(name) (x86::Register::rbp + (offsetof(riscv::Context, name) - 0x80))

void Dbt_compiler::compile(emu::reg_t pc) {
    riscv::Decoder decoder { &runtime_.state_, pc };
    riscv::Basic_block block = decoder.decode_basic_block();

    if (runtime_.state_.disassemble) {
        util::log("Translating {:x} to {:x}\n", pc, reinterpret_cast<uintptr_t>(encoder_.buffer().data()));
    }

    // Prolog. We place context + 0x80 to rbp instead of context directly, as it allows all registers to be located
    // within int8 offset from rbp, so the assembly representation will uses a shorter encoding.
    *this << push(x86::Register::rbp);
    *this << lea(x86::Register::rbp, qword(x86::Register::rdi + 0x80));

    int pc_diff = 0;
    int instret_diff = 0;

    // We treat the last instruction differently.
    for (size_t i = 0; i < block.instructions.size() - 1; i++) {

        riscv::Instruction inst = block.instructions[i];
        riscv::Opcode opcode = inst.opcode();

        switch (opcode) {
            case riscv::Opcode::addi: emit_addi(inst); break;
            case riscv::Opcode::andi: emit_andi(inst); break;
            case riscv::Opcode::add: emit_add(inst); break;
            case riscv::Opcode::sub: emit_sub(inst); break;
            case riscv::Opcode::i_and: emit_and(inst); break;
            case riscv::Opcode::addiw: emit_addiw(inst); break;
            case riscv::Opcode::addw: emit_addw(inst); break;
            case riscv::Opcode::lui: emit_lui(inst); break;
            case riscv::Opcode::auipc: {
                // AUIPC is special: it needs pc_diff, so do not move it to a separate function.
                const int rd = inst.rd();
                if (rd == 0) break;
                *this << mov(x86::Register::rax, qword(memory_of(pc)));
                *this << add(x86::Register::rax, pc_diff + inst.imm());
                *this << mov(qword(memory_of_register(rd)), x86::Register::rax);
                break;
            }
            default:
                *this << mov(x86::Register::rsi, util::read_as<uint64_t>(&inst));
                *this << lea(x86::Register::rdi, qword(x86::Register::rbp - 0x80));
                *this << mov(x86::Register::rax, reinterpret_cast<uintptr_t>(riscv::step));
                *this << call(x86::Register::rax);
                break;
        }

        pc_diff += inst.length();
        instret_diff++;
    }

    riscv::Instruction inst = block.instructions.back();
    pc_diff += inst.length();
    instret_diff += 1;

    *this << add(qword(memory_of(instret)), instret_diff);

    switch (inst.opcode()) {
        case riscv::Opcode::jalr: emit_jalr(inst, pc_diff); break;
        case riscv::Opcode::jal: emit_jal(inst, pc_diff); break;
        case riscv::Opcode::beq: emit_branch(inst, pc_diff, x86::Condition_code::equal); break;
        case riscv::Opcode::bne: emit_branch(inst, pc_diff, x86::Condition_code::not_equal); break;
        case riscv::Opcode::blt: emit_branch(inst, pc_diff, x86::Condition_code::less); break;
        case riscv::Opcode::bge: emit_branch(inst, pc_diff, x86::Condition_code::greater_equal); break;
        case riscv::Opcode::bltu: emit_branch(inst, pc_diff, x86::Condition_code::below); break;
        case riscv::Opcode::bgeu: emit_branch(inst, pc_diff, x86::Condition_code::above_equal); break;
        case riscv::Opcode::fence_i: {
            void (*callback)(Dbt_runtime&) = [](Dbt_runtime& runtime) {
                for (int i = 0; i < 4096; i++)
                    runtime.icache_tag_[i] = 0;
                runtime.inst_cache_.clear();
            };
            *this << add(qword(memory_of(pc)), pc_diff);
            *this << mov(x86::Register::rdi, reinterpret_cast<uintptr_t>(&runtime_));
            *this << mov(x86::Register::rax, reinterpret_cast<uintptr_t>(callback));
            *this << pop(x86::Register::rbp);
            *this << jmp(x86::Register::rax);
            break;
        }
        default:
            *this << add(qword(memory_of(pc)), pc_diff);
            *this << mov(x86::Register::rsi, util::read_as<uint64_t>(&inst));
            *this << lea(x86::Register::rdi, qword(x86::Register::rbp - 0x80));
            *this << mov(x86::Register::rax, reinterpret_cast<uintptr_t>(riscv::step));
            *this << pop(x86::Register::rbp);
            *this << jmp(x86::Register::rax);
            break;
    }
}

void Dbt_compiler::emit_move(int rd, int rs) {
    if (rd == 0 || rd == rs) {
        // We would like at least one x86 instruction to be generated for an instruction. Therefore if the instruction
        // turns out to be no-op, we also generate a no-op.
        *this << nop();
        return;
    }

    if (rs == 0) {
        emit_load_immediate(rd, 0);
        return;
    }

    *this << mov(x86::Register::rax, qword(memory_of_register(rs)));
    *this << mov(qword(memory_of_register(rd)), x86::Register::rax);
}

void Dbt_compiler::emit_move32(int rd, int rs) {
    if (rd == 0) {
        *this << nop();
        return;
    }

    if (rs == 0) {
        emit_load_immediate(rd, 0);
        return;
    }

    *this << movsx(x86::Register::rax, dword(memory_of_register(rs)));
    *this << mov(qword(memory_of_register(rd)), x86::Register::rax);
}

void Dbt_compiler::emit_load_immediate(int rd, riscv::reg_t imm) {
    if (rd == 0) {
        *this << nop();
        return;
    }

    *this << mov(qword(memory_of_register(rd)), imm);
}

void Dbt_compiler::emit_branch(riscv::Instruction inst, riscv::reg_t pc_diff, x86::Condition_code cc) {
    const int rs1 = inst.rs1();
    const int rs2 = inst.rs2();

    if (rs1 == rs2) {
        bool result = cc == x86::Condition_code::equal ||
                      cc == x86::Condition_code::greater_equal ||
                      cc == x86::Condition_code::above_equal;

        if (result) {
            *this << add(qword(memory_of(pc)), pc_diff - inst.length() + inst.imm());
        } else {
            *this << add(qword(memory_of(pc)), pc_diff);
        }

        *this << pop(x86::Register::rbp);
        *this << ret();
        return;
    }

    // Compare and set flags.
    // If either operand is 0, it should be treated specially.
    if (rs2 == 0) {
        *this << cmp(qword(memory_of_register(rs1)), 0);
    } else if (rs1 == 0) {

        // Switch around condition code in this case.
        switch (cc) {
            case x86::Condition_code::less: cc = x86::Condition_code::greater; break;
            case x86::Condition_code::greater_equal: cc = x86::Condition_code::less_equal; break;
            case x86::Condition_code::below: cc = x86::Condition_code::above; break;
            case x86::Condition_code::above_equal: cc = x86::Condition_code::below_equal; break;
            default: break;
        }

        *this << cmp(qword(memory_of_register(rs2)), 0);
    } else {
        *this << mov(x86::Register::rax, qword(memory_of_register(rs1)));
        *this << cmp(x86::Register::rax, qword(memory_of_register(rs2)));
    }

    // If flag set, then change rax to offset of new target
    *this << mov(x86::Register::rdx, pc_diff - inst.length() + inst.imm());
    *this << mov(x86::Register::rax, pc_diff);
    *this << cmovcc(cc, x86::Register::rax, x86::Register::rdx);

    // Update pc
    *this << add(qword(memory_of(pc)), x86::Register::rax);

    *this << pop(x86::Register::rbp);
    *this << ret();
}

void Dbt_compiler::emit_jalr(riscv::Instruction inst, riscv::reg_t pc_diff) {
    const int rd = inst.rd();
    const int rs1 = inst.rs1();
    riscv::reg_t imm = inst.imm();

    if (rd != 0) {
        *this << mov(x86::Register::rdx, qword(memory_of(pc)));
    }

    *this << mov(x86::Register::rax, qword(memory_of_register(rs1)));

    if (imm != 0) {
        *this << add(x86::Register::rax, imm);
    }

    *this << i_and(x86::Register::rax, ~1);
    *this << mov(qword(memory_of(pc)), x86::Register::rax);

    if (rd != 0) {
        *this << add(x86::Register::rdx, pc_diff);
        *this << mov(qword(memory_of_register(rd)), x86::Register::rdx);
    }

    *this << pop(x86::Register::rbp);
    *this << ret();
}

void Dbt_compiler::emit_jal(riscv::Instruction inst, riscv::reg_t pc_diff) {
    const int rd = inst.rd();

    if (rd != 0) {
        *this << mov(x86::Register::rax, qword(memory_of(pc)));
    }

    *this << add(qword(memory_of(pc)), pc_diff - inst.length() + inst.imm());

    if (rd != 0) {
        *this << add(x86::Register::rax, pc_diff);
        *this << mov(qword(memory_of_register(rd)), x86::Register::rax);
    }

    *this << pop(x86::Register::rbp);
    *this << ret();
}

void Dbt_compiler::emit_addi(riscv::Instruction inst) {
    int rd = inst.rd();
    int rs1 = inst.rs1();
    riscv::reg_t imm = inst.imm();

    if (rd == 0) {
        *this << nop();
        return;
    }

    if (rs1 == 0) {
        emit_load_immediate(rd, imm);
        return;
    }

    if (imm == 0) {
        emit_move(rd, rs1);
        return;
    }

    if (rd == rs1) {
        *this << add(qword(memory_of_register(rd)), imm);
        return;
    }

    *this << mov(x86::Register::rax, qword(memory_of_register(rs1)));
    *this << add(x86::Register::rax, imm);
    *this << mov(qword(memory_of_register(rd)), x86::Register::rax);
}

void Dbt_compiler::emit_andi(riscv::Instruction inst) {
    int rd = inst.rd();
    int rs1 = inst.rs1();
    riscv::reg_t imm = inst.imm();

    if (rd == 0) {
        *this << nop();
        return;
    }

    if (rs1 == 0) {
        emit_load_immediate(rd, 0);
        return;
    }

    if (imm == 0) {
        emit_load_immediate(rd, 0);
        return;
    }

    if (imm == static_cast<riscv::reg_t>(-1)) {
        emit_move(rd, rs1);
        return;
    }

    if (rd == rs1) {
        *this << i_and(qword(memory_of_register(rd)), imm);
        return;
    }

    *this << mov(x86::Register::rax, qword(memory_of_register(rs1)));
    *this << i_and(x86::Register::rax, imm);
    *this << mov(qword(memory_of_register(rd)), x86::Register::rax);
}

void Dbt_compiler::emit_add(riscv::Instruction inst) {
    int rd = inst.rd();
    int rs1 = inst.rs1();
    int rs2 = inst.rs2();

    if (rd == 0) {
        *this << nop();
        return;
    }

    if (rs1 == 0) {
        emit_move(rd, rs2);
        return;
    }

    if (rs2 == 0) {
        emit_move(rd, rs1);
        return;
    }

    // Add one variable to itself can be efficiently implemented as an in-place shift.
    if (rd == rs1 && rd == rs2) {
        *this << shl(qword(memory_of_register(rd)), 1);
        return;
    }

    if (rd == rs1) {
        *this << mov(x86::Register::rax, qword(memory_of_register(rs2)));
        *this << add(qword(memory_of_register(rd)), x86::Register::rax);
        return;
    }

    if (rd == rs2) {
        *this << mov(x86::Register::rax, qword(memory_of_register(rs1)));
        *this << add(qword(memory_of_register(rd)), x86::Register::rax);
        return;
    }

    if (rs1 == rs2) {
        *this << mov(x86::Register::rax, qword(memory_of_register(rs1)));
        *this << add(x86::Register::rax, x86::Register::rax);
        *this << mov(qword(memory_of_register(rd)), x86::Register::rax);
        return;
    }

    *this << mov(x86::Register::rax, qword(memory_of_register(rs1)));
    *this << add(x86::Register::rax, qword(memory_of_register(rs2)));
    *this << mov(qword(memory_of_register(rd)), x86::Register::rax);
}

void Dbt_compiler::emit_sub(riscv::Instruction inst) {
    int rd = inst.rd();
    int rs1 = inst.rs1();
    int rs2 = inst.rs2();

    if (rd == 0) {
        *this << nop();
        return;
    }

    // rd = rs1 - 0
    if (rs2 == 0) {
        emit_move(rd, rs1);
        return;
    }

    // rd = rs1 - rs1 = 0
    if (rs1 == rs2) {
        emit_load_immediate(rd, 0);
        return;
    }

    // rd -= rs2
    if (rd == rs1) {
        *this << mov(x86::Register::rax, qword(memory_of_register(rs2)));
        *this << sub(qword(memory_of_register(rd)), x86::Register::rax);
        return;
    }

    // rd = -rd
    if (rd == rs2 && rs1 == 0) {
        *this << neg(qword(memory_of_register(rd)));
        return;
    }

    // rd = -rs2
    if (rs1 == 0) {
        *this << mov(x86::Register::rax, qword(memory_of_register(rs2)));
        *this << neg(x86::Register::rax);
        *this << mov(qword(memory_of_register(rd)), x86::Register::rax);
        return;
    }

    *this << mov(x86::Register::rax, qword(memory_of_register(rs1)));
    *this << sub(x86::Register::rax, qword(memory_of_register(rs2)));
    *this << mov(qword(memory_of_register(rd)), x86::Register::rax);
}

void Dbt_compiler::emit_and(riscv::Instruction inst) {
    int rd = inst.rd();
    int rs1 = inst.rs1();
    int rs2 = inst.rs2();

    if (rd == 0) {
        *this << nop();
        return;
    }

    if (rs1 == 0 || rs2 == 0) {
        emit_load_immediate(rd, 0);
        return;
    }

    if (rs1 == rs2) {
        emit_move(rd, rs1);
        return;
    }

    if (rd == rs1) {
        *this << mov(x86::Register::rax, qword(memory_of_register(rs2)));
        *this << i_and(qword(memory_of_register(rd)), x86::Register::rax);
        return;
    }

    if (rd == rs2) {
        *this << mov(x86::Register::rax, qword(memory_of_register(rs1)));
        *this << i_and(qword(memory_of_register(rd)), x86::Register::rax);
        return;
    }

    *this << mov(x86::Register::rax, qword(memory_of_register(rs1)));
    *this << i_and(x86::Register::rax, qword(memory_of_register(rs2)));
    *this << mov(qword(memory_of_register(rd)), x86::Register::rax);
}

void Dbt_compiler::emit_addiw(riscv::Instruction inst) {
    int rd = inst.rd();
    int rs1 = inst.rs1();
    riscv::reg_t imm = inst.imm();

    if (rd == 0) {
        *this << nop();
        return;
    }

    if (rs1 == 0) {
        emit_load_immediate(rd, imm);
        return;
    }

    if (imm == 0) {
        emit_move32(rd, rs1);
        return;
    }

    *this << mov(x86::Register::eax, dword(memory_of_register(rs1)));
    *this << add(x86::Register::eax, imm);
    *this << cdqe();
    *this << mov(qword(memory_of_register(rd)), x86::Register::rax);
}

void Dbt_compiler::emit_addw(riscv::Instruction inst) {
    int rd = inst.rd();
    int rs1 = inst.rs1();
    int rs2 = inst.rs2();

    if (rd == 0) {
        return;
    }

    if (rs1 == 0) {
        emit_move32(rd, rs2);
        return;
    }

    if (rs2 == 0) {
        emit_move32(rd, rs1);
        return;
    }

    if (rs1 == rs2) {
        // ADDW rd, rs1, rs1
        *this << mov(x86::Register::eax, dword(memory_of_register(rs1)));
        *this << add(x86::Register::eax, x86::Register::eax);
    } else {
        *this << mov(x86::Register::eax, dword(memory_of_register(rs1)));
        *this << add(x86::Register::eax, dword(memory_of_register(rs2)));
    }

    *this << cdqe();
    *this << mov(qword(memory_of_register(rd)), x86::Register::rax);
}

void Dbt_compiler::emit_lui(riscv::Instruction inst) {
    emit_load_immediate(inst.rd(), inst.imm());
}