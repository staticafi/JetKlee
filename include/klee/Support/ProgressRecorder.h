//===-- ProgressRecorder.h --------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_PROGRESS_RECORDER_H
#define KLEE_PROGRESS_RECORDER_H

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <iosfwd>
#include "klee/ADT/Ref.h"
#include "klee/Expr/Expr.h"
#include "../../../lib/Core/Memory.h"
#include "../../../lib/Core/AddressSpace.h"


namespace klee {

  class PTreeNode;
  class ExecutionState;
  struct InstructionInfo;
  using Updates = std::set<std::tuple<std::string, std::string>>;

  struct ByteInfo {
    int offset;
    bool isConcrete;
    bool isKnownSym;
    bool isUnflushed;
    klee::ref<klee::Expr> value;

    bool operator==(const ByteInfo& other) const {
      return offset == other.offset &&
            isConcrete == other.isConcrete &&
            isKnownSym == other.isKnownSym &&
            isUnflushed == other.isUnflushed &&
            value == other.value;      
    }

    bool operator<(const ByteInfo& other) const {
      if (offset != other.offset) {
        return offset < other.offset;
      } else if (isConcrete != other.isConcrete) {
        return !isConcrete;
      } else if (isKnownSym != other.isKnownSym) {
        return !isKnownSym;
      } else if (isUnflushed != other.isUnflushed) {
        return !isUnflushed;
      } else {
        return value < other.value;
      }
    }
    // void toJson(std::ostream& ostr) const;
  };

  struct pair_hash {
    template <class T1, class T2>
    std::size_t operator () (const std::pair<T1,T2> &pair) const {
        auto hash1 = std::hash<T1>{}(pair.first);
        auto hash2 = std::hash<T2>{}(pair.second);
        return hash1 ^ hash2;
    }
};
  class ProgressRecorder {

    struct Action {
      virtual ~Action() {}
      virtual void toJson(std::ostream& ostr) = 0;
    };

    struct InsertNode : public Action {
      InsertNode(const PTreeNode *node_, int nodeID_, int stateID_, bool uniqueState_)
        : node{ node_ }, nodeID{ nodeID_ }, stateID{ stateID_ }, uniqueState{ uniqueState_ } {}
      void toJson(std::ostream& ostr) override;
      const PTreeNode *node;
      int nodeID;
      int stateID;
      bool uniqueState;
    };

    struct InsertEdge : public Action {
      InsertEdge(const int parentID_, const int childID_, uint8_t tag_)
        : parentID{ parentID_ }, childID{ childID_ }, tag{ tag_ } {}
      void toJson(std::ostream& ostr) override;
      const int parentID;
      const int childID;
      uint8_t tag;
    };

    struct EraseNode : public Action {
      explicit EraseNode(const int ID_) : ID{ ID_ } {}
      void toJson(std::ostream& ostr) override;
      const int ID;
    };

    std::string rootOutputDir;

    int roundCounter;

    int nodeCounter;
    
    std::unordered_map<const PTreeNode *, int> nodeIDs;
    std::unordered_map<int, int> nodeJSONs;

    std::unordered_map<int, int> accessCount;
    // list of Object ids for parent nodes
    std::unordered_map<int, std::set<int>> parentIds;
    std::unordered_map<int, Updates> updates;
    std::unordered_map<std::pair<int, int>, std::set<ByteInfo>, pair_hash> segmentBytes;
    std::unordered_map<std::pair<int, int>, std::set<ByteInfo>, pair_hash> offsetBytes;

    int stateCounter;
    std::unordered_map<const ExecutionState *, int> stateIDs;

    std::vector<std::unique_ptr<Action>> roundActions;

    ProgressRecorder();
    ProgressRecorder(ProgressRecorder const&) = default;
    ProgressRecorder& operator=(ProgressRecorder const&) = default;

  public:
    static ProgressRecorder& instance();
    static const std::string rootDirName;

    bool start(const std::string &underDir, std::string fileName);
    void stop();
    bool started() const;

    void onRoundBegin();
    void onRoundEnd();

    std::tuple<std::set<ByteInfo>, std::set<ByteInfo>> getByteDiff(const ObjectStatePlane *const plane, int nodeID, int parentID, bool isOffset);
    Updates getUpdateDiff(const UpdateList updateList, int nodeID, int parentID);
    void plane2json(std::ostream& ostr, const ObjectStatePlane *const plane, int nodeID, int parentID, bool isOffset);
    void object2json(std::ostream &ostr, const MemoryObject *const obj, const klee::ref<klee::ObjectState>& state, int nodeID, int parentID);
    void recordInfo(int nodeID, int parentID, const MemoryMap objects);

    void deleteParentInfo(const int parentID);
    void onInsertNode(const PTreeNode *node);
    void onInsertEdge(const PTreeNode *parent, const PTreeNode *child, uint8_t tag);
    void onEraseNode(const PTreeNode *node);
  };

  inline ProgressRecorder& recorder() { return ProgressRecorder::instance(); }

}

#endif
