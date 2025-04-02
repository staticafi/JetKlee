//===-- Assignment.h --------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_ASSIGNMENT_H
#define KLEE_ASSIGNMENT_H

#include "klee/Expr/Constraints.h"
#include "klee/Expr/ExprEvaluator.h"

#include <map>

namespace klee {
  class Array;

  class CompactArrayModel {
  private:
    std::vector<std::pair<uint32_t, uint32_t> > skipRanges;
    std::vector<uint8_t> values;
    friend class MapArrayModel;
  public:
    uint8_t get(unsigned index) const;
    std::map<uint32_t, uint8_t> asMap() const;
    std::vector<uint8_t> asVector() const;
    void dump() const;
  };

  class MapArrayModel {
  private:
    std::map<uint32_t, uint8_t> content;
    bool shouldSkip(unsigned difference) const {
      return difference > 1;
    }
  public:
    MapArrayModel() {}
    MapArrayModel(const MapArrayModel &other) {
      content = other.content;
    }
    MapArrayModel(const CompactArrayModel &other) {
      content = other.asMap();
    }
    MapArrayModel(const std::vector<uint8_t> &other) {
      for (unsigned i = 0; i < other.size(); i++) {
        content[i] = other[i];
      }
    }

    MapArrayModel& operator=(const MapArrayModel &other) {
      if (this != &other)
        content = other.content;

      return *this;
    }

    std::map<uint32_t, uint8_t> asMap() const {
      return content;
    }
    uint8_t get(unsigned index) const {
      auto it = content.find(index);
      if (it != content.end())
        return it->second;
      return 0;
    }

    void add(uint32_t index, uint8_t value) {
      content[index] = value;
    }

    void toCompact(CompactArrayModel& model) const;
  };

  class VectorAssignment {
  public:
    typedef std::map<const Array*, std::vector<unsigned char> > bindings_ty;

    bool allowFreeValues;
    bindings_ty bindings;
    
  public:
    VectorAssignment(bool _allowFreeValues=false)
      : allowFreeValues(_allowFreeValues) {}

    VectorAssignment(const std::vector<const Array*> &objects,
                     std::vector< std::vector<unsigned char> > &values,
                     bool _allowFreeValues = false)
    : allowFreeValues(_allowFreeValues) {
      std::vector<std::vector<unsigned char> >::iterator valIt = values.begin();
      for (std::vector<const Array*>::const_iterator it = objects.begin(),
           ie = objects.end(); it != ie; ++it) {
        const Array *os = *it;
        std::vector<unsigned char> &arr = *valIt;
        bindings.insert(std::make_pair(os, arr));
        ++valIt;
      }
    }

    ref<Expr> evaluate(const Array *mo, unsigned index) const;
    ref<Expr> evaluate(ref<Expr> e) const;
  };

  class Assignment {
  public:
    typedef std::map<const Array*, CompactArrayModel> bindings_ty;
    typedef std::map<const Array*, MapArrayModel> map_bindings_ty;

    Assignment() = default;
    Assignment(const bindings_ty bindings) : bindings(bindings) {}
    Assignment(const map_bindings_ty models) {
      for (const auto &pair : models) {
        auto &item = bindings[pair.first];
        pair.second.toCompact(item);
      }
    }

    CompactArrayModel& getBindings(const Array *a) {
        return bindings[a];
    }

    const CompactArrayModel *getBindingsOrNull(const Array *a) const {
        auto it = bindings.find(a);
        if (it == bindings.end())
            return nullptr;
        return &it->second;
    }

    bool hasBindings(const Array *a) const {
        return bindings.find(a) != bindings.end();
    }

    void addBinding(const Array*, const std::vector<unsigned char>& values);

    uint8_t getValue(const Array *mo, unsigned index) const;
    ref<Expr> evaluate(const Array *mo, unsigned index) const;
    ref<Expr> evaluate(ref<Expr> e) const;
    ConstraintSet createConstraintsFromAssignment() const;

    template<typename InputIterator>
    bool satisfies(InputIterator begin, InputIterator end) const;
    void dump() const;

  private:
    bindings_ty bindings;
  };

  template <typename T>
  class AssignmentEvaluator : public ExprEvaluator {
    const T &a;

  protected:
    ref<Expr> getInitialValue(const Array &mo, unsigned index) {
      return a.evaluate(&mo, index);
    }
    
  public:
    AssignmentEvaluator(const T &_a) : a(_a) {}
  };

  /***/

  inline ref<Expr> VectorAssignment::evaluate(const Array *array,
                                        unsigned index) const {
    assert(array);
    bindings_ty::const_iterator it = bindings.find(array);
    if (it!=bindings.end() && index<it->second.size()) {
      return ConstantExpr::alloc(it->second[index], array->getRange());
    } else {
      if (allowFreeValues) {
        return ReadExpr::create(UpdateList(array, ref<UpdateNode>(nullptr)),
                                ConstantExpr::alloc(index, array->getDomain()));
      } else {
        return ConstantExpr::alloc(0, array->getRange());
      }
    }
  }

    inline ref<Expr> Assignment::evaluate(const Array *array,
                                        unsigned index) const {
    assert(array);
    return ConstantExpr::alloc(getValue(array, index), array->getRange());
  }

  inline ref<Expr> VectorAssignment::evaluate(ref<Expr> e) const {
    AssignmentEvaluator<VectorAssignment> v(*this);
    return v.visit(e); 
  }

  inline ref<Expr> Assignment::evaluate(ref<Expr> e) const {
    AssignmentEvaluator<Assignment> v(*this);
    return v.visit(e);
  }

  inline uint8_t Assignment::getValue(const Array* array, unsigned index) const {
    bindings_ty::const_iterator it = bindings.find(array);
    if (it!=bindings.end()) {
      return it->second.get(index);
    }
    return 0;
  }

  template<typename InputIterator>
  inline bool Assignment::satisfies(InputIterator begin, InputIterator end) const {
    AssignmentEvaluator<Assignment> v(*this);
    for (; begin!=end; ++begin)
      if (!v.visit(*begin)->isTrue())
        return false;
    return true;
  }
}

#endif /* KLEE_ASSIGNMENT_H */
