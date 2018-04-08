//===-- Assignment.h --------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_UTIL_ASSIGNMENT_H
#define KLEE_UTIL_ASSIGNMENT_H

#include <map>

#include "klee/util/ExprEvaluator.h"

// FIXME: Rename?

namespace klee {
  class Array;

  class ArrayModel {
  public:
    virtual uint8_t get(unsigned index) const = 0;
    virtual std::map<uint32_t, uint8_t> asMap() const = 0;
    virtual void dump() const = 0;
  };

  class VectorArrayModel : public ArrayModel {
  public:
    std::vector<uint8_t> content;
    VectorArrayModel(std::vector<uint8_t> content) : content(content) {}
    uint8_t get(unsigned index) const override {
      return content[index];
    }
    std::map<uint32_t, uint8_t> asMap() const override {
      std::map<uint32_t, uint8_t> retMap;
      for (unsigned i = 0; i < content.size(); i++) {
        retMap[i] = content[i];
      }
      return retMap;
    }
    void dump() const override {
      for (unsigned i = 0; i < content.size(); ++i)
        llvm::errs() << content[i] << ",";
    }
  };

  class CompactArrayModel : public ArrayModel {
  private:
    std::vector<std::pair<uint32_t, uint32_t> > skipRanges;
    std::vector<uint8_t> values;
    friend class MapArrayModel;
  public:
    uint8_t get(unsigned index) const override {
      // TODO better search
#if 0
      for (unsigned i = skipRanges.size() - 1; i > 0; i--) {
        auto rangeStart = skipRanges[i].first;
        auto rangeSkip = skipRanges[i].second;
        if (index > rangeStart) {
          if (index < rangeStart + rangeSkip) {
            // it's within skipped range, so it can be anything, for example 0
            return 0;
          } else {
            return values[index + rangeSkip];
          }
        }
      }
#endif
      if (index < values.size())
        return values[index];
      return 0;
    }

    std::map<uint32_t, uint8_t> asMap() const override {
      std::map<uint32_t, uint8_t> retMap;
      unsigned index = 0;
      unsigned valueIndex = 0;
      for (unsigned i = 0; i < skipRanges.size(); i++) {
        for (; index < skipRanges[i].first; index++, valueIndex++) {
          retMap[index] = values[valueIndex];
        }
        index = skipRanges[i].second;
      }
      for (; valueIndex < values.size(); index++, valueIndex++) {
        retMap[index] = values[valueIndex];
      }
      return retMap;
    }

    std::vector<uint8_t> asVector() const {
      std::vector<uint8_t> result;
      // TODO reserve
      unsigned index = 0;
      for (unsigned i = 0; i < skipRanges.size(); i++) {
        for (; index < skipRanges[i].first; index++) {
          result.push_back(values[index]);
        }
        result.resize(result.size() + skipRanges[i].second - index);
      }
      for (; index < values.size(); index++) {
        result.push_back(values[index]);
      }
      return result;
    }

    void dump() const override {
      // TODO
    }
  };

  class MapArrayModel : public ArrayModel {
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
    MapArrayModel(const ArrayModel &other) {
      content = other.asMap();
    }
    MapArrayModel(const std::vector<uint8_t> &other) {
      for (unsigned i = 0; i < other.size(); i++) {
        content[i] = other[i];
      }
    }
    std::map<uint32_t, uint8_t> asMap() const override {
      return content;
    }
    uint8_t get(unsigned index) const override {
      auto it = content.find(index);
      if (it != content.end())
        return it->second;
      return 0;
    }

    void add(uint32_t index, uint8_t value) {
      content[index] = value;
    }
    void toCompact(CompactArrayModel& model) const {
#if 0
      unsigned skipRangeCount = 0;
      unsigned valueCount = 0;
      uint8_t *values = 0;
      std::pair<uint32_t, uint32_t> *skipRanges = 0;
      std::map<uint32_t, uint8_t>::const_iterator it = content.begin();
      while (it != content.end()) {
        std::pair<uint32_t, uint8_t> current = *it;
        if (++it != content.end()) {
          std::pair<uint32_t, uint8_t> next = *it;
          unsigned difference = next.first - current.first;
          if (shouldSkip(difference)) {
            skipRangeCount++;
          }
          valueCount++;
        }
      }
      if (valueCount) {
        if (skipRangeCount)
          skipRanges = new std::pair<uint32_t, uint32_t>[skipRangeCount];
        values = new uint8_t[valueCount](); // initialized to zeros by ()
        unsigned skipRangeIndex = 0;
        unsigned skipValue = 0;
        unsigned valueIndex = 0;
        it = content.begin();
        while (it != content.end()) {
          std::pair<uint32_t, uint8_t> current = *it;
          if (++it != content.end()) {
            std::pair<uint32_t, uint8_t> next = *it;
            unsigned difference = next.first - current.first;
            if (shouldSkip(difference)) {
              skipValue += difference;
              skipRanges[skipRangeIndex] = std::make_pair(current.first, skipValue);
              skipRangeIndex++;
            }
          }
          values[valueIndex++] = current.second;
        }
      }
#else
      for (const auto pair : content) {
        if (pair.first >= model.values.size())
          model.values.resize(pair.first + 1);
        model.values[pair.first] = pair.second;
      }
#endif
    }

    void dump() const override {
      // TODO
    }
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

    bindings_ty bindings;

  public:
    Assignment(const bindings_ty bindings) : bindings(bindings) {}
    Assignment(const map_bindings_ty models) {
      for (const auto &pair : models) {
        auto &item = bindings[pair.first];
        pair.second.toCompact(item);
      }
    }

    uint8_t getValue(const Array *mo, unsigned index) const;
    ref<Expr> evaluate(const Array *mo, unsigned index) const;
    ref<Expr> evaluate(ref<Expr> e) const;
    void createConstraintsFromAssignment(std::vector<ref<Expr> > &out) const;

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
        return ReadExpr::create(UpdateList(array, 0), 
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

#endif
