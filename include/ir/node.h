#ifndef IR_NODE_H
#define IR_NODE_H

#include <cstdint>
#include <utility>
#include <vector>

#include "util/assert.h"
#include "util/array_multiset.h"

namespace ir {

namespace pass {
class Pass;
}

enum class Type: uint8_t {
    none = 0,
    i1 = 1,
    i8 = 8,
    i16 = 16,
    i32 = 32,
    i64 = 64,
    memory = 0xFE,
    control = 0xFF,
};

[[maybe_unused]]
static size_t get_type_size(Type type) {
    return static_cast<uint8_t>(type);
}

enum class Opcode: uint8_t {
    /** Control flow opcodes **/
    // Input: None. Output: Memory.
    start,

    // Input: Control[]. Output: None.
    end,

    // Input: Control[]. Output: Memory.
    // attribute.pointer is used to reference the last node in the block, i.e. jmp/if.
    block,

    // Input: Memory, Value. Output: Control, Control.
    i_if,

    // Input: Control, Control. Output: Control.
    if_true,
    if_false,

    // Input: Memory. Output: Control.
    jmp,

    /** Opcodes with side-effects **/
    // Input: Memory. Output: Memory.
    emulate,

    /* Machine register load/store */
    // Input: Memory. Output: Memory, Value.
    load_register,

    // Input: Memory, Value. Output: Memory.
    store_register,

    /* Memory load/store */
    // Input: Memory, Value. Output: Memory, Value.
    load_memory,

    // Input: Memory, Value, Value. Output: Memory.
    store_memory,

    // Input: Memory[], Output: Memory
    fence,

    /** Pure opcodes **/

    // Input: None. Output: Value.
    constant,

    // Input: Value. Output: Value.
    cast,

    /*
     * Unary ops
     * Input: Value. Output: Value.
     */
    neg,
    i_not,

    /*
     * Binary ops
     * Input: Value, Value. Output: Value.
     */
    /* Arithmetic operations */
    add,
    sub,
    i_xor,
    i_or,
    i_and,

    /* Shift operations */
    shl,
    shr,
    sar,

    /* Compare */
    eq,
    ne,
    lt,
    ge,
    ltu,
    geu,

    /*
     * Ternary op
     * Input: Value, Value, Value. Output: Value.
     */
    mux,
};

[[maybe_unused]]
static bool is_pure_opcode(Opcode opcode) {
    return static_cast<uint8_t>(opcode) >= static_cast<uint8_t>(Opcode::constant);
}

[[maybe_unused]]
static bool is_binary_opcode(Opcode opcode) {
    uint8_t value = static_cast<uint8_t>(opcode);
    return value >= static_cast<uint8_t>(Opcode::add) && value <= static_cast<uint8_t>(Opcode::geu);
}

[[maybe_unused]]
static bool is_commutative_opcode(Opcode opcode) {
    switch(opcode) {
        case Opcode::add:
        case Opcode::i_xor:
        case Opcode::i_or:
        case Opcode::i_and:
        case Opcode::eq:
        case Opcode::ne:
            return true;
        default:
            return false;
    }
}

class Node;
class Value;
class Graph;

// Represents a value defined by a node. Note that the node may be null.
class Value {
private:
    Node* _node;
    size_t _index;
public:
    Value(): _node{nullptr}, _index{0} {}
    Value(Node* node, size_t index): _node{node}, _index{index} {}

    Node* node() const { return _node; }
    size_t index() const { return _index; }

    inline Type type() const;
    inline const util::Array_multiset<Node*>& references() const;

    explicit operator bool() { return _node != nullptr; }

    // Some frequently used utility function.
    inline Opcode opcode() const;
    inline bool is_const() const;
    inline uint64_t const_value() const;
};

[[maybe_unused]]
static bool operator ==(Value a, Value b) {
    return a.node() == b.node() && a.index() == b.index();
}

[[maybe_unused]]
static bool operator !=(Value a, Value b) { return !(a == b); }

class Node {
private:

    // Values that this node references.
    std::vector<Value> _operands;

    // Nodes that references the value of this node.
    std::vector<util::Array_multiset<Node*>> _references;

    // The output type of this node.
    std::vector<Type> _type;

    // Additional attributes for some nodes.
    union {
        uint64_t value;
        void *pointer;
    } _attribute;

    // Scratchpad for passes to store data temporarily.
    union {
        uint64_t value;
        void *pointer;
    } _scratchpad;

    // Opcode of the node.
    Opcode _opcode;

    // Whether the node is visited. For graph walking only.
    // 0 - not visited, 1 - visited, 2 - visiting.
    uint8_t _visited;

public:
    Node(Opcode opcode, std::vector<Type>&& type, std::vector<Value>&& operands);
    ~Node();

    // Disable copy construction and assignment. Node should live on heap.
    Node(const Node& node) = delete;
    Node(Node&& node) = delete;
    void operator =(const Node& node) = delete;
    void operator =(Node&& node) = delete;

private:
    void link();
    void unlink();

public:
    // Field accessors and mutators
    uint64_t scratchpad() const { return _scratchpad.value; }
    void scratchpad(uint64_t value) { _scratchpad.value = value; }
    void* scratchpad_pointer() const { return _scratchpad.pointer; }
    void scratchpad_pointer(void* pointer) { _scratchpad.pointer = pointer; }

    uint64_t attribute() const { return _attribute.value; }
    void attribute(uint64_t value) { _attribute.value = value; }
    void* attribute_pointer() const { return _attribute.pointer; }
    void attribute_pointer(void* pointer) { _attribute.pointer = pointer; }

    // A node can produce one or more values. The following functions allow access to these values.
    size_t value_count() const { return _type.size(); }
    Value value(size_t index) { return {this, index}; }

    Opcode opcode() const { return _opcode; }
    void opcode(Opcode opcode) { _opcode = opcode; }

    // Operand accessors and mutators
    const std::vector<Value>& operands() const { return _operands; }
    void operands(std::vector<Value>&& operands);
    size_t operand_count() const { return _operands.size(); }

    Value operand(size_t index) const {
        ASSERT(index < _operands.size());
        return _operands[index];
    }

    void operand_set(size_t index, Value value);
    void operand_add(Value value);
    void operand_swap(size_t first, size_t second) { std::swap(_operands[first], _operands[second]); }
    void operand_update(Value oldvalue, Value newvalue);

    friend Value;
    friend Graph;
    friend pass::Pass;
};

class Graph {
private:
    std::vector<Node*> _heap;
    Node* _start;
    Node* _root = nullptr;

public:
    Graph();
    Graph(const Graph&) = delete;
    Graph(Graph&&) = default;
    ~Graph();

    Graph& operator =(const Graph&) = delete;
    Graph& operator =(Graph&&);

    Node* manage(Node* node) {
        _heap.push_back(node);
        return node;
    }

    // Free up dead nodes. Not necessary during compilation, but useful for reducing footprint when graph needs to be
    // cached.
    void garbage_collect();

    Node* start() const { return _start; }

    Node* root() const { return _root; }
    void root(Node* root) { _root = root; }

    friend pass::Pass;
};

Type Value::type() const { return _node->_type[_index]; }
const util::Array_multiset<Node*>& Value::references() const { return _node->_references[_index]; }

Opcode Value::opcode() const { return _node->_opcode; }
bool Value::is_const() const { return _node->_opcode == Opcode::constant; }
uint64_t Value::const_value() const { return _node->attribute(); }

} // ir

#endif