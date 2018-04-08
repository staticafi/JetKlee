//===-- Assignment.cpp ----------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include "klee/util/Assignment.h"
namespace klee {

void Assignment::dump() const {
  if (bindings.size() == 0) {
    llvm::errs() << "No bindings\n";
    return;
  }
  for (bindings_ty::const_iterator i = bindings.begin(), e = bindings.end(); i != e;
       ++i) {
    llvm::errs() << (*i).first->name << "\n[";
    i->second.dump();
    llvm::errs() << "]\n";
  }
}

void Assignment::createConstraintsFromAssignment(
    std::vector<ref<Expr> > &out) const {
  assert(out.size() == 0 && "out should be empty");
  for (bindings_ty::const_iterator it = bindings.begin(), ie = bindings.end();
       it != ie; ++it) {
    const Array *array = it->first;
    const auto &values = it->second;
    for (const auto pair : values.asMap()) {
      unsigned arrayIndex = pair.first;
      unsigned char value = pair.second;
      out.push_back(EqExpr::create(
          ReadExpr::create(UpdateList(array, 0),
                           ConstantExpr::alloc(arrayIndex, array->getDomain())),
          ConstantExpr::alloc(value, array->getRange())));
    }
  }
}
}
