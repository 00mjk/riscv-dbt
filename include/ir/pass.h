#ifndef IR_PASS_H
#define IR_PASS_H

#include "ir/instruction.h"

namespace ir::pass {

class Pass {
private:
    void run_recurse(Instruction* inst);

protected:
    // Before visiting the tree.
    virtual void start() {}
    // After visiting the tree.
    virtual void finish() {}
    // Before visiting children of the instruction. Returning true will abort children visit.
    virtual bool before(Instruction*) { return false; }
    // After all children has been visited.
    virtual void after(Instruction*) {}

public:
    void run(Graph& buffer);
};

class Printer: public Pass {
public:
    static const char* opcode_name(Opcode opcode);
    static const char* type_name(Type type);

protected:
    // Used for numbering the output of instructions.
    uint64_t _index;
    virtual void start() override { _index = 0; }
    virtual void after(Instruction* inst) override;
};

class Dot_printer: public Printer {
protected:
    virtual void start() override;
    virtual void finish() override;
    virtual void after(Instruction* inst) override;
};

}

#endif