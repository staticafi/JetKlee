//===-- Assignment.cpp ----------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Expr/Assignment.h"

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

ConstraintSet Assignment::createConstraintsFromAssignment() const {
  ConstraintSet result;
  for (const auto &binding : bindings) {
    const auto &array = binding.first;
    const auto &values = binding.second;

    for (const auto &pair : values.asMap()) {
      unsigned arrayIndex = pair.first;
      unsigned char value = pair.second;
      result.push_back(EqExpr::create(
          ReadExpr::create(UpdateList(array, 0),
                           ConstantExpr::alloc(arrayIndex, array->getDomain())),
          ConstantExpr::alloc(value, array->getRange())));
    }
  }
  return result;
}

void MapArrayModel::toCompact(CompactArrayModel& model) const {
  model.skipRanges.clear();
  model.values.clear();
  unsigned cursor = 0;
  for (const auto &item : content) {
    unsigned difference = item.first - cursor;
    if (shouldSkip(difference)) {
      model.skipRanges.push_back(std::make_pair(cursor, difference));
    } else {
      model.values.resize(model.values.size() + difference);
    }
    model.values.push_back(item.second);
    cursor = item.first + 1;
  }
}

uint8_t CompactArrayModel::get(unsigned index) const {
  unsigned skipTotal = 0;
  for (const auto &item : skipRanges) {
    unsigned skipStart = item.first;
    unsigned skipCount = item.second;
    unsigned skipEnd = skipStart + skipCount;
    if (index < skipEnd) {
      if (index >= skipStart) {
        // it's within skipped range, so it can be anything, for example 0
        return 0;
      } else {
        return values[index - skipTotal];
      }
    }
    skipTotal += skipCount;
  }
  if (index - skipTotal < values.size())
    return values[index - skipTotal];
  return 0;
}

std::map<uint32_t, uint8_t> CompactArrayModel::asMap() const {
  std::map<uint32_t, uint8_t> retMap;
  unsigned index = 0;
  unsigned cursor = 0;
  for (const auto &item : skipRanges) {
    for (; index < item.first; index++, cursor++) {
      retMap[index] = values[cursor];
    }
    index += item.second;
  }
  for (; cursor < values.size(); cursor++, index++) {
    retMap[index] = values[cursor];
  }
  return retMap;
}

std::vector<uint8_t> CompactArrayModel::asVector() const {
  std::vector<uint8_t> result;
  unsigned index = 0;
  unsigned cursor = 0;
  for (const auto &item : skipRanges) {
    for (; index < item.first; index++, cursor++) {
      result.push_back(values[cursor]);
    }
    index += item.second;
    result.resize(result.size() + item.second);
  }
  for (; cursor < values.size(); cursor++, index++) {
    result.push_back(values[cursor]);
  }
  return result;
}

void CompactArrayModel::dump() const {
  // TODO
}


void Assignment::addBinding(const Array*array,
                            const std::vector<unsigned char>& values) {
    assert(!hasBindings(array));

    MapArrayModel mapModel(values);
    auto &binding = bindings[array];
    mapModel.toCompact(binding);
}

}
