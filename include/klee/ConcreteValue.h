#ifndef KLEE_CONCRETE_VALUE_
#define KLEE_CONCRETE_VALUE_

#include <llvm/ADT/APInt.h>
#include <experimental/optional>

namespace klee {
// wrapper around APInt that remembers the signdness
class ConcreteValue {

    // FIXME: turn this info an identifier like: (line, col, seq),
    // so that we can use that in sliced programs
    std::experimental::optional<llvm::APInt> pointer; // if set, value is offset

    llvm::APInt value;

    bool issigned{false};

public:
    ConcreteValue(unsigned numBits, uint64_t val, bool isSigned)
    : value(numBits, val, isSigned), issigned(isSigned) {}

    ConcreteValue(const llvm::APInt& val, bool isSigned)
    : value(val), issigned(isSigned) {}

    ConcreteValue(llvm::APInt&& val, bool isSigned)
    : value(std::move(val)), issigned(isSigned) {}

    ConcreteValue(const llvm::APInt& obj, const llvm::APInt& off)
    : pointer(obj), value(off) {}

    ConcreteValue(const llvm::APInt&& obj, const llvm::APInt&& off)
    : pointer(std::move(obj)), value(std::move(off)) {}

    bool isSigned() const { return issigned; }
    uint64_t getZExtValue() const { return value.getZExtValue(); }
    // makes sense also for unsigned
    uint64_t getSExtValue() const { return value.getSExtValue(); }

    unsigned getBitWidth() const { return value.getBitWidth(); }
    // WARNING: not efficient
    std::string toString() const { return value.toString(10, issigned); }

    bool isPointer() const { return static_cast<bool>(pointer); }
    llvm::APInt& getPointer() { return *pointer; }
    const llvm::APInt& getPointer() const { return *pointer; }

    llvm::APInt& getValue() { return value; }
    const llvm::APInt& getValue() const { return value; }

    void setValue(const llvm::APInt& v) { value = v; }
    void setValue(llvm::APInt&& v) { value = std::move(v); }

    void setPointer(const llvm::APInt& p) { pointer = p; }
    void setPointer(llvm::APInt&& p) { pointer = std::move(p); }
};


class NamedConcreteValue : public ConcreteValue {
    const std::string name;

public:

    NamedConcreteValue(unsigned numBits, uint64_t val,
                       bool isSigned, const std::string& nm = "")
    : ConcreteValue(numBits, val, isSigned), name(nm) {}

    NamedConcreteValue(const llvm::APInt& val, bool isSigned,
                       const std::string& nm = "")
    : ConcreteValue(val, isSigned), name(nm) {}

    NamedConcreteValue(llvm::APInt&& val, bool isSigned,
                       const std::string& nm = "")
    : ConcreteValue(std::move(val), isSigned), name(nm) {}

    NamedConcreteValue(const llvm::APInt& obj, const llvm::APInt& off,
                       const std::string& nm = "")
    : ConcreteValue(obj, off), name(nm) {}

    NamedConcreteValue(const llvm::APInt&& obj, const llvm::APInt&& off,
                       const std::string& nm = "")
    : ConcreteValue(std::move(obj), std::move(off)), name(nm) {}

    const std::string& getName() const { return name; }
    // debug info (optional)
    unsigned line{0};
    unsigned col{0};
};

} // namespace klee

#endif
