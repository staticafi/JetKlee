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
using Updates = std::multiset<std::tuple<std::string, std::string>>;
using Bytes = std::vector<std::string>;
using BytesMap =
    std::map<std::string, std::vector<int>>; // value : list of offsets
using BytesDiff = std::tuple<BytesMap, BytesMap>;

struct Memory {
  const Bytes concreteStore;
  const Bytes concreteMask;
  const Bytes knownSymbolics;
};

enum ByteType {
    CONCRETE,
    SYMBOLIC,
    MASK
};

struct ByteInfo {
  int offset;
  bool isConcrete;
  bool isKnownSym;
  bool isUnflushed;
  klee::ref<klee::Expr> value;

  bool operator==(const ByteInfo &other) const {
    return offset == other.offset && isConcrete == other.isConcrete &&
           isKnownSym == other.isKnownSym && isUnflushed == other.isUnflushed &&
           value == other.value;
  }

  bool operator<(const ByteInfo &other) const {
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
};

struct pair_hash {
  template <class T1, class T2>
  std::size_t operator()(const std::pair<T1, T2> &pair) const {
    auto hash1 = std::hash<T1>{}(pair.first);
    auto hash2 = std::hash<T2>{}(pair.second);
    return hash1 ^ hash2;
  }
};
class ProgressRecorder {

  struct Action {
    virtual ~Action() {}
    virtual void toJson(std::ostream &ostr) = 0;
  };

  struct InsertInfo : public Action {
    InsertInfo(const PTreeNode *node_, int nodeID_)
        : node{node_}, nodeID{nodeID_} {}
    void toJson(std::ostream &ostr) override;
    const PTreeNode *node;
    int nodeID;
  };

  struct InsertNode : public Action {
    InsertNode(const PTreeNode *node_, int nodeID_, int stateID_,
               bool uniqueState_)
        : node{node_}, nodeID{nodeID_}, stateID{stateID_},
          uniqueState{uniqueState_} {}
    void toJson(std::ostream &ostr) override;
    const PTreeNode *node;
    int nodeID;
    int stateID;
    bool uniqueState;
  };

  struct InsertEdge : public Action {
    InsertEdge(const int parentID_, const int childID_)
        : parentID{parentID_}, childID{childID_} {}
    void toJson(std::ostream &ostr) override;
    const int parentID;
    const int childID;
  };

  struct EraseNode : public Action {
    explicit EraseNode(const int ID_) : ID{ID_} {}
    void toJson(std::ostream &ostr) override;
    const int ID;
  };

  std::string rootOutputDir;
  std::string treeDir;
  std::string memoryDir;

  int roundCounter;

  int nodeCounter;

  std::unordered_map<int, int> nodeJSONs;
  std::unordered_map<int, bool> nodeUniqueStates;

  std::unordered_map<int, int> accessCount;
  // list of Object ids for parent nodes
  std::unordered_map<int, std::set<int>> parentIDs;
  std::unordered_map<std::pair<int, int>, Updates, pair_hash> segmentUpdates;
  std::unordered_map<std::pair<int, int>, Updates, pair_hash> offsetUpdates;
  std::unordered_map<std::pair<int, int>, Memory, pair_hash> segmentMemory;
  std::unordered_map<std::pair<int, int>, Memory, pair_hash> offsetMemory;
  std::unordered_map<const PTreeNode *, int> nodeIDs;
  std::set<int> recordedNodesIDs;

  int stateCounter;
  std::unordered_map<const ExecutionState *, int> stateIDs;

  std::vector<std::unique_ptr<Action>> roundActions;

  ProgressRecorder();
  ProgressRecorder(ProgressRecorder const &) = default;
  ProgressRecorder &operator=(ProgressRecorder const &) = default;

public:
  static ProgressRecorder &instance();
  static const std::string rootDirName;
  static const std::string treeDirName;
  static const std::string memoryDirName;
  static const mode_t dirPermissions;

  static int getNodeCounter();

  bool start(const std::string &underDir, std::string fileName);
  void end();
  void stop();
  bool started() const;

  void onRoundBegin();
  void onRoundEnd();

  bool hasChanged(int nodeID, int parentID, const ObjectStatePlane *const segmentPlane,
                const ObjectStatePlane *const offsetPlane);
  Memory getMemory(const ObjectStatePlane *const plane);
  BytesDiff getByteDiff(const ObjectStatePlane *const plane,
                                  int nodeID, int parentID, bool isOffset, enum ByteType type);
  Updates getUpdateDiff(const UpdateList updateList, int nodeID, int parentID, bool isOffset, int planeID);
  Updates getUpdateAdditions(const Updates &parentUpdates, const Updates &childUpdates);
  void plane2json(std::ostream &ostr, const ObjectStatePlane *const plane,
                  int nodeID, int parentID, bool isOffset);
  void object2json(std::ostream &ostr, const MemoryObject *const obj,
                   const klee::ref<klee::ObjectState> &state, int nodeID,
                   int parentID);
  void recordInfo(int nodeID, int parentID, const MemoryMap objects);
  void recordPlanes(int nodeId,
                    const ObjectStatePlane *const segmentPlane,
                    const ObjectStatePlane *const offsetPlane);

  void deleteParentInfo(const int parentID);

  void onInsertInfo(int nodeID, const PTreeNode *node);
  void onInsertNode(const PTreeNode *node);
  void onInsertEdge(const PTreeNode *parent, const PTreeNode *child);
  void onEraseNode(const PTreeNode *node);

  const std::unordered_map<const PTreeNode *, int> &getNodeIDs() const {
    return nodeIDs;
  }
  const std::set<int> &getRecordedNodeIDs() const { return recordedNodesIDs; }
};

inline ProgressRecorder &recorder() { return ProgressRecorder::instance(); }

} // namespace klee

#endif
