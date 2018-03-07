//===-- BitArray.h ----------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_BITARRAY_H
#define KLEE_BITARRAY_H

namespace klee {

  // XXX would be nice not to have
  // two allocations here for allocated
  // BitArrays
class BitArray {
private:
  uint32_t *bits;
  unsigned _size;
  
protected:
  static uint32_t length(unsigned size) { return (size+31)/32; }

public:
  BitArray() : bits(0), _size(0) {}
  explicit BitArray(unsigned size, bool value = false) : bits(new uint32_t[length(size)]), _size(size) {
    memset(bits, value?0xFF:0, sizeof(*bits)*length(size));
  }
  BitArray(const BitArray &b) : bits(new uint32_t[length(b._size)]), _size(b._size) {
    memcpy(bits, b.bits, sizeof(*bits)*length(_size));
  }
  ~BitArray() { delete[] bits; }

  unsigned size() const {
    return _size;
  }

  void resize(unsigned newSize, bool value = false) {
    uint32_t *oldBits = bits;
    if (newSize)
      bits = new uint32_t[length(newSize)];
    memcpy(bits, oldBits, sizeof(*bits)*length(std::min(_size, newSize)));
    for (unsigned i = _size; i < newSize; i++)
      set(i, value);
    _size = newSize;
    if (oldBits)
      delete[] oldBits;
  }

  bool get(unsigned idx) const { return (bool) ((bits[idx/32]>>(idx&0x1F))&1); }
  void set(unsigned idx) { bits[idx/32] |= 1<<(idx&0x1F); }
  void unset(unsigned idx) { bits[idx/32] &= ~(1<<(idx&0x1F)); }
  void set(unsigned idx, bool value) { if (value) set(idx); else unset(idx); }
};

} // End klee namespace

#endif /* KLEE_BITARRAY_H */
