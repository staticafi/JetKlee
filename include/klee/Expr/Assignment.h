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
    typedef std::map<const Array*, std::vector<unsigned char> > bindings_ty;

    bindings_ty bindings;

  public:
    Assignment(const std::vector<const Array*> &objects,
               std::vector< std::vector<unsigned char> > &values) {
      std::vector< std::vector<unsigned char> >::iterator valIt =
        values.begin();
      for (std::vector<const Array*>::const_iterator it = objects.begin(),
             ie = objects.end(); it != ie; ++it) {
        const Array *os = *it;
        std::vector<unsigned char> &arr = *valIt;
        bindings.insert(std::make_pair(os, arr));
        ++valIt;
      }
    }
    Assignment(const std::map<const Array*, std::vector<unsigned char> > &bindings)
      : bindings(bindings) {}

    uint8_t getValue(const Array *mo, unsigned index) const;
    ref<Expr> evaluate(const Array *mo, unsigned index) const;
    ref<Expr> evaluate(ref<Expr> e) const;
    ConstraintSet createConstraintsFromAssignment() const;

    template<typename InputIterator>
    bool satisfies(InputIterator begin, InputIterator end) const;
    void dump() const;
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
    bindings_ty::const_iterator it = bindings.find(array);
    if (it!=bindings.end() && index<it->second.size()) {
      return ConstantExpr::alloc(it->second[index], array->getRange());
    } else {
      return ConstantExpr::alloc(0, array->getRange());
    }
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
    if (it!=bindings.end() && index<it->second.size()) {
      return it->second[index];
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
