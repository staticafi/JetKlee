//===-- ProgressRecorder.cpp ----------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Support/ProgressRecorder.h"
#include "../Core/CallPathManager.h"
#include "../Core/ExecutionState.h"
#include "../Core/Memory.h"
#include "../Core/PTree.h"
#include "klee/Module/InstructionInfoTable.h"
#include "klee/Module/KInstruction.h"
#include "llvm/IR/Function.h"
#include <assert.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace klee {

ProgressRecorder &ProgressRecorder::instance() {
  static ProgressRecorder rec;
  return rec;
}

const std::string ProgressRecorder::rootDirName{"__JetKleeProgressRecording__"};
const std::string ProgressRecorder::treeDirName{"Tree"};
const std::string ProgressRecorder::memoryDirName{"Memory"};
const mode_t ProgressRecorder::dirPermissions{0775};

ProgressRecorder::ProgressRecorder()
    : rootOutputDir{}

      ,
      roundCounter{0}

      ,
      nodeCounter{0}, nodeIDs{{nullptr, 0}}

      ,
      stateCounter{0}, stateIDs{{nullptr, 0}}

      ,
      roundActions{} {}

static bool createDir(const std::string &dir) {
  return mkdir(dir.c_str(), ProgressRecorder::dirPermissions) >= 0;
}

int ProgressRecorder::getNodeCounter() { return instance().nodeCounter; }

static std::string replaceFileExtension(const std::string &fileName,
                                        const std::string &extension) {
  size_t dotPos = fileName.rfind('.');
  return fileName.substr(0, dotPos) + extension;
}

static void copyFile(const std::string &originalFile,
                     const std::string &copyFile) {
  std::ifstream original(originalFile);
  if (original) {
    std::ofstream copy(copyFile);
    if (copy) {
      copy << original.rdbuf();
    }
  }
}

void ProgressRecorder::end() {
  std::string kleeOutDir = rootOutputDir;
  size_t lastSlashPos = kleeOutDir.find_last_of("/\\");
  if (lastSlashPos != std::string::npos) {
    kleeOutDir = kleeOutDir.substr(0, lastSlashPos);
  }

  const std::string assemblyFilePath = kleeOutDir + "/assembly.ll";
  copyFile(assemblyFilePath, rootOutputDir + "/source.ll");
}

bool ProgressRecorder::start(const std::string &underDir,
                             std::string fileName) {
  if (!createDir(underDir))
    return false;
  rootOutputDir = underDir;

  treeDir = rootOutputDir + "/" + treeDirName;
  memoryDir = rootOutputDir + "/" + memoryDirName;
  
  if (!createDir(treeDir) || !createDir(memoryDir)) {
    return false;
  }

  const std::string cFile = replaceFileExtension(fileName, ".c");
  const std::string iFile = replaceFileExtension(fileName, ".i");
  copyFile(cFile, rootOutputDir + "/source.c");
  copyFile(iFile, rootOutputDir + "/source.i");

  return true;
}

void ProgressRecorder::stop() { rootOutputDir.clear(); }

bool ProgressRecorder::started() const { return !rootOutputDir.empty(); }

void ProgressRecorder::onRoundBegin() { ++roundCounter; }

void ProgressRecorder::onRoundEnd() {
  if (roundActions.empty())
    return;
  std::string pathName = treeDir + "/" + std::to_string(roundCounter) + ".json";
  std::ofstream ostr(pathName.c_str());
  ostr << "[\n";
  for (std::size_t i = 0U; i != roundActions.size(); ++i) {
    ostr << "  {\n";
    roundActions.at(i)->toJson(ostr);
    ostr << "  }" << (i + 1U < roundActions.size() ? ",\n" : "\n");
  }
  ostr << "]\n";
  roundActions.clear();
}

void ProgressRecorder::onInsertMemory(int nodeID, const PTreeNode *const node) {
  int parentID = node->parent == nullptr ? -1 : instance().nodeIDs.at(node->parent);
  
  std::string pathName = memoryDir + "/" + std::to_string(nodeID) + ".json";
  std::ofstream ostr(pathName.c_str());
  InsertMemory insertMemory(node, nodeID);

  instance().recordInfo(nodeID, parentID, node->state->addressSpace.objects);

  ostr << "{\n";
  insertMemory.toJson(ostr);
  ostr << "}";

  if (instance().accessCount[parentID] == 2) {
    instance().deleteParentInfo(parentID);
  }
}

void ProgressRecorder::onInsertNode(const PTreeNode *const node) {
  int nodeID = ++nodeCounter;
  auto nr = nodeIDs.insert({node, nodeID});
  assert(nr.second);
  bool uniqueState{false};
  auto sit = stateIDs.find(node->state);
  if (sit == stateIDs.end()) {
    sit = stateIDs.insert({node->state, ++stateCounter}).first;
    uniqueState = true;
  }
  nodeJSONs.insert({nodeID, std::max(1, roundCounter)});
  roundActions.push_back(std::make_unique<InsertNode>(
      node, nr.first->second, sit->second, uniqueState));
}

void ProgressRecorder::onInsertEdge(const PTreeNode *const parent,
                                    const PTreeNode *const child,
                                    const uint8_t tag) {
  roundActions.push_back(
      std::make_unique<InsertEdge>(nodeIDs.at(parent), nodeIDs.at(child), tag));
}

void ProgressRecorder::onEraseNode(const PTreeNode *const node) {
  roundActions.push_back(std::make_unique<EraseNode>(nodeIDs.at(node)));
  nodeIDs.erase(node);
}

static std::string expr2str(const ref<Expr> &e) {
  std::stringstream sstr;
  sstr << e;
  std::string expr;
  expr.reserve(sstr.str().size());
  bool lastIsSpace{false};
  for (auto c : sstr.str())
    if (std::isspace(c)) {
      if (!lastIsSpace) {
        expr.push_back(' ');
        lastIsSpace = true;
      }
    } else {
      expr.push_back(c);
      lastIsSpace = false;
    }
  return expr;
}

static void stack2json(std::ostream &ostr,
                       const std::vector<StackFrame> stack) {
  ostr << "\"stack\": [";
  for (std::size_t i = 0U; i != stack.size(); ++i)
    if (stack.at(i).callPathNode->callSite != nullptr) {
      const InstructionInfo *const callerInfo{stack.at(i).caller->info};
      ostr << "[\"" << callerInfo->file << "\"," << callerInfo->line << ","
           << callerInfo->column << "," << callerInfo->assemblyLine << "]"
           << (i + 1U < stack.size() ? "," : "");
    }
  ostr << "],\n";
}

static void constraints2json(std::ostream &ostr,
                             const ConstraintSet constraints) {
  ostr << "    \"constraints\": [\n";
  for (auto it = constraints.begin(); it != constraints.end();) {
    ostr << "      \"" << expr2str(*it) << "\"";
    ++it;
    ostr << (it != constraints.end() ? ",\n" : "\n");
  }
  ostr << "    ]";
}

void bytesMapToJson(std::ostream &ostr, const BytesMap &bytesMap) {
  std::string indent16 = std::string(16, ' ');
  std::string indent14 = std::string(14, ' ');
  if (!bytesMap.empty())
    ostr << "\n" << indent16;
  for (auto bm = bytesMap.begin(); bm != bytesMap.end();) {
    ostr << "{\"" << bm->first << "\"" << ":[";
    for (auto index = bm->second.begin(); index != bm->second.end();) {
      ostr << *index;
      ++index;
      ostr << (index != bm->second.end() ? "," : "]}");
    }
    ++bm;
    ostr << (bm != bytesMap.end() ? ", " : "");
  }
  if (!bytesMap.empty())
    ostr << "\n" << indent14;
}

template <typename T>
std::tuple<std::set<T>, std::set<T>> getDiff(const std::set<T> &parent,
                                             const std::set<T> &child) {
  std::set<T> additions;
  std::set<T> deletions;

  std::set_difference(child.begin(), child.end(), parent.begin(), parent.end(),
                      std::inserter(additions, additions.begin()));
  std::set_difference(parent.begin(), parent.end(), child.begin(), child.end(),
                      std::inserter(deletions, deletions.begin()));

  return std::make_tuple(additions, deletions);
}

Updates ProgressRecorder::getUpdateDiff(const UpdateList updateList, int nodeID, int parentID, bool isOffset, int planeID) {
  Updates additions;
  if (planeID == -1)
    return additions;

  Updates childUpdates;
  Updates parentUpdates;
  const auto& updatesMap = isOffset ? instance().offsetUpdates : instance().segmentUpdates;

  auto itUpdates = updatesMap.find({nodeID, planeID});
  if (itUpdates != updatesMap.end()) {
    childUpdates = itUpdates->second;
  }

  itUpdates = updatesMap.find({parentID, planeID});
  if (itUpdates != updatesMap.end()) {
    parentUpdates = itUpdates->second;
  }

  std::tuple<Updates, Updates> diff = getDiff(parentUpdates, childUpdates);

  return std::get<0>(diff);
}

BytesMap bytesMapFromVector(const std::vector<std::string>& vec) {
    BytesMap result;

    for (size_t i = 0; i < vec.size(); ++i) {
        result[vec[i]].push_back(static_cast<int>(i));
    }

    return result;
}

BytesDiff getVecDiff2(
    const std::vector<std::string>& parentValues,
    const std::vector<std::string>& childValues
) {
    
    auto parentBytesMap = bytesMapFromVector(parentValues);
    auto childBytesMap = bytesMapFromVector(childValues);

    BytesMap added;
    BytesMap deleted;

    // Collect all unique keys from both maps
    std::set<std::string> allKeys;
    for (const auto& pair : parentBytesMap) {
        allKeys.insert(pair.first);
    }
    for (const auto& pair : childBytesMap) {
        allKeys.insert(pair.first);
    }

    // Iterate over each key to compute differences
    for (const auto& key : allKeys) {
        std::set<int> firstSet, secondSet;

        // Convert vectors to sets for set operations
        if (parentBytesMap.count(key)) {
            firstSet.insert(parentBytesMap.at(key).begin(), parentBytesMap.at(key).end());
        }
        if (childBytesMap.count(key)) {
            secondSet.insert(childBytesMap.at(key).begin(), childBytesMap.at(key).end());
        }

        // Compute added values: values in secondSet but not in firstSet
        std::vector<int> addedValues;
        std::set_difference(
            secondSet.begin(), secondSet.end(),
            firstSet.begin(), firstSet.end(),
            std::back_inserter(addedValues)
        );
        if (!addedValues.empty()) {
            added[key] = addedValues;
        }

        // Compute deleted values: values in firstSet but not in secondSet
        std::vector<int> deletedValues;
        std::set_difference(
            firstSet.begin(), firstSet.end(),
            secondSet.begin(), secondSet.end(),
            std::back_inserter(deletedValues)
        );
        if (!deletedValues.empty()) {
            deleted[key] = deletedValues;
        }
    }

    return std::make_tuple(added, deleted);
}

BytesDiff getVecDiff(const std::vector<std::string>& parentValues, const std::vector<std::string>& childValues) {
    BytesMap bytesAdd;
    BytesMap bytesDel;

    // Track indices of elements in child that are missing in parent (additions)
    for (size_t i = 0; i < childValues.size(); ++i) {
        if (std::find(parentValues.begin(), parentValues.end(), childValues[i]) == parentValues.end()) {
            auto x = childValues[i];
            bytesAdd[childValues[i]].push_back(i);

        }
    }
    
    // Track indices of elements in parent that are missing in child (deletions)
    for (size_t i = 0; i < parentValues.size(); ++i) {
        if (std::find(childValues.begin(), childValues.end(), parentValues[i]) == childValues.end()) {
            auto x = parentValues[i];
            bytesDel[parentValues[i]].push_back(i);
        }
    }
    return std::make_tuple(bytesAdd, bytesDel);
}

BytesDiff ProgressRecorder::getByteDiff(const ObjectStatePlane *const plane,
                                  int nodeID, int parentID, bool isOffset, enum ByteType type) {
  BytesDiff diff;
  if (plane == nullptr)
    return diff;
  int planeID = plane->getParent() ? plane->getParent()->getObject()->id : -1;

  Bytes bytes;
  Bytes parentBytes;
  const auto& memoryMap = isOffset ? instance().offsetMemory : instance().segmentMemory;

  auto itBytes = memoryMap.find({nodeID, planeID});
  if (itBytes != memoryMap.end()) {
    switch (type)
    {
    case ByteType::CONCRETE:
      bytes = itBytes->second.concreteStore;
      break;
    case ByteType::SYMBOLIC:
      bytes = itBytes->second.knownSymbolics;
      break;
    case ByteType::MASK:
      bytes = itBytes->second.concreteMask;
      break;
    }
  }

  itBytes = memoryMap.find({parentID, planeID});
  if (itBytes != memoryMap.end()) {
        switch (type)
    {
    case ByteType::CONCRETE:
      parentBytes = itBytes->second.concreteStore;
      break;
    case ByteType::SYMBOLIC:
      parentBytes = itBytes->second.knownSymbolics;
      break;
    case ByteType::MASK:
      parentBytes = itBytes->second.concreteMask;
      break;
    }
  }

  if (planeID == 11) {
  klee_message("Node ID: %d", nodeID);
  klee_message("Plane ID: %d", planeID);
  klee_message("Parent bytes: %d", parentBytes.size());
  klee_message("Child bytes length: %d", bytes.size());
  for (auto i = 0U; i < bytes.size(); ++i)
  {
    klee_message("Child bytes[%d]: %s", i, bytes[i].c_str());
  }
  klee_message("--------------------");
  klee_message("Parent bytes length: %d", parentBytes.size());
  for (auto i = 0U; i < parentBytes.size(); ++i)
  {
    klee_message("Parent bytes[%d]: %s", i, parentBytes[i].c_str());
  }
  klee_message("--------------------");
  }

  diff = getVecDiff2(parentBytes, bytes);

  if (planeID == 11) {
    // auto add = std::get<0>(diff);
    auto del = std::get<1>(diff);
    klee_message("Size of del: %d", del.size());

    for (auto it = del.begin(); it != del.end(); ++it) {
      klee_message("Deleted: %s", it->first.c_str());
      for (auto i = 0U; i < it->second.size(); ++i)
      {
        klee_message("Index: %d", it->second[i]);
      }
    }
  }
  return diff;
}

void ProgressRecorder::plane2json(std::ostream &ostr,
                                  const ObjectStatePlane *const plane,
                                  int nodeID, int parentID, bool isOffset) {
  if (plane == nullptr)
    return;

  int planeID = plane->getParent() ? plane->getParent()->getObject()->id : -1;
  ostr << "\"objID\": " << planeID << ", ";
  ostr << "\"rootObject\": " << "\"" << plane->getUpdates().root->getName() << "\", ";
  ostr << "\"sizeBound\": " << plane->sizeBound << ", ";
  ostr << "\"initialized\": " << plane->initialized << ", ";
  ostr << "\"symbolic\": " << plane->symbolic << ", ";
  ostr << "\"initialValue\": " << (int)plane->initialValue;

  

  std::string indent14 = std::string(14, ' ');
  
  BytesDiff concreteStoreDiff = getByteDiff(plane, nodeID, parentID, isOffset, ByteType::CONCRETE);
  BytesMap concreteStoreAdd = std::get<0>(concreteStoreDiff);
  BytesMap concreteStoreDel = std::get<1>(concreteStoreDiff);

  ostr << ",\n            \"concreteStore\": {";
  ostr << "\n" << indent14 << "\"add\": [";
  bytesMapToJson(ostr, concreteStoreAdd);
  ostr << "],";

  ostr << "\n" << indent14 << "\"del\": [";
  bytesMapToJson(ostr, concreteStoreDel);
  ostr << "]\n            }";

  BytesDiff concreteMaskDiff = getByteDiff(plane, nodeID, parentID, isOffset, ByteType::MASK);
  BytesMap concreteMaskAdd = std::get<0>(concreteMaskDiff);
  BytesMap concreteMaskDel = std::get<1>(concreteMaskDiff);

  ostr << ",\n            \"concreteMask\": {";
  ostr << "\n" << indent14 << "\"add\": [";
  bytesMapToJson(ostr, concreteMaskAdd);
  ostr << "],";

  ostr << "\n" << indent14 << "\"del\": [";
  bytesMapToJson(ostr, concreteMaskDel);
  ostr << "]\n            }";

  BytesDiff knownSymbolicsDiff = getByteDiff(plane, nodeID, parentID, isOffset, ByteType::SYMBOLIC);
  BytesMap knownSymbolicsAdd = std::get<0>(knownSymbolicsDiff);
  BytesMap knownSymbolicsDel = std::get<1>(knownSymbolicsDiff);

  ostr << ",\n            \"knownSymbolics\": {";
  ostr << "\n" << indent14 << "\"add\": [";
  bytesMapToJson(ostr, knownSymbolicsAdd);
  ostr << "],";

  ostr << "\n" << indent14 << "\"del\": [";
  bytesMapToJson(ostr, knownSymbolicsDel);
  ostr << "]\n            }";

  {
    ostr << ",\n            \"concreteStoreLong\": [ ";
    bool start = true;
    for (auto x : plane->concreteStore)
    {
      if (start) start = false; else ostr << ",";
      ostr << (unsigned int)x;
    }
    ostr << "]";
  }
  {
    ostr << ",\n            \"concreteMaskLong\": [ ";
    bool start = true;
    for (auto i = 0U;  i < plane->concreteMask.size(); ++i)
    {
      if (start) start = false; else ostr << ",";
      ostr << (unsigned int)plane->isByteConcrete(i);
    }
    ostr << "]";
  }
  {
    ostr << ",\n            \"knownSymbolicsLong\": [ ";
    bool start = true;
    for (auto i = 0U;  i < plane->sizeBound; ++i)
    {
      if (start) start = false; else ostr << ",";
      ostr << "\"";
      if (plane->isByteKnownSymbolic(i))
        ostr << expr2str(plane->knownSymbolics[i]);
      ostr << "\"";
    }
    ostr << "]";
  }

  Updates updatesAdd = getUpdateDiff(plane->getUpdateList(), nodeID, parentID, isOffset, planeID);
  if (!updatesAdd.empty()) {
    ostr << ",\n            \"updates\": [\n";
    for (auto it = updatesAdd.begin(); it != updatesAdd.end();) {
      const std::string& offset = std::get<0>(*it);
      const std::string& value = std::get<1>(*it);

      ostr << "              {";
      ostr << "\"" << value << "\""
           << " : "
           << "\"" << offset << "\"";
      ++it;
      ostr << "}" << (it != updatesAdd.end() ? ",\n" : "\n");
    }
    ostr << "            ]";

    ostr << ",\n            \"constantValues\": [\n";
    ostr << indent14;
    const Array *array = plane->getUpdateList().root;
    for (auto it = array->constantValues.begin(); it != array->constantValues.end();) {
      ostr << "\"" << expr2str(*it) << "\"";
      ++it;
      ostr << (it != array->constantValues.end() ? "," : "");
    }
    ostr << "]";
  }
}

static void instr2json(std::ostream &ostr, const InstructionInfo *const instr) {
  ostr << "\"" << instr->file << "\"," << instr->line << "," << instr->column
       << "," << instr->assemblyLine;
}

static void
nondetValues2json(std::ostream &ostr,
                  std::vector<ExecutionState::NondetValue> nondetValues) {
  ostr << "    \"nondetValues\": [\n";
  for (auto it = nondetValues.begin(); it != nondetValues.end();) {
    ostr << "      {";
    ostr << "\"value\": \"" << expr2str(it->value.getValue()) << "\", ";
    ostr << "\"segment\": " << expr2str(it->value.getSegment()) << ", ";
    ostr << "\"isSigned\": " << it->isSigned << ", ";
    ostr << "\"name\": \"" << it->name << "\"";
    ostr << "}";
    ++it;
    ostr << (it != nondetValues.end() ? ",\n" : "\n");
  }
  ostr << "    ], \n";
}

Memory ProgressRecorder::getMemory(const ObjectStatePlane *const plane) {
  Bytes concreteStore;
  Bytes concreteMask;
  Bytes knownSymbolics;

  for (std::size_t i = 0U; i < plane->concreteStore.size(); ++i) {
    std::string value = std::to_string((unsigned int) plane->concreteStore[i]);
    concreteStore.push_back(value);
  }

  for (std::size_t i = 0U; i < plane->concreteMask.size(); ++i) {
    std::string value = std::to_string((unsigned int) plane->isByteConcrete(i));
    concreteMask.push_back(value);
  }

  for (std::size_t i = 0U; i < plane->sizeBound; ++i)
  {
    std::string value = "";
    if (plane->isByteKnownSymbolic(i)) {
      value = expr2str(plane->knownSymbolics[i]);
    }
    knownSymbolics.push_back(value);
  }

  return {concreteStore, concreteMask, knownSymbolics};
}

void ProgressRecorder::recordPlanes(int nodeId, const ObjectStatePlane *const segmentPlane, const ObjectStatePlane *const offsetPlane) {
  if (segmentPlane != nullptr) {
    // record plane's memory (concreteStore, concreteMask, knownSymbolics)
    int segmentPlaneId = segmentPlane->getParent() ? segmentPlane->getParent()->getObject()->id : -1;
    Memory currSegmentMemory = getMemory(segmentPlane);
    instance().segmentMemory.insert({{nodeId, segmentPlaneId}, currSegmentMemory});


    // record plane's updates
    Updates childUpdates;
    if (segmentPlane->getUpdateList().head.get() != nullptr) {
      for (const auto *un = segmentPlane->getUpdateList().head.get(); un; un = un->next.get()) {
        childUpdates.insert(std::make_tuple(expr2str(un->index), expr2str(un->value)));
      }
    }
    instance().segmentUpdates.insert({{nodeId, segmentPlaneId}, childUpdates});
  }

  if (offsetPlane != nullptr) {
    // record plane's memory (concreteStore, concreteMask, knownSymbolics)
    int offsetPlaneId = offsetPlane->getParent() ? offsetPlane->getParent()->getObject()->id : -1;
    Memory currOffsetMemory = getMemory(offsetPlane);
    instance().offsetMemory.insert({{nodeId, offsetPlaneId}, currOffsetMemory});

    // record plane's updates
    Updates childUpdates;
    if (offsetPlane->getUpdateList().head.get() != nullptr) {
      for (const auto *un = offsetPlane->getUpdateList().head.get(); un; un = un->next.get()) {
        childUpdates.insert(std::make_tuple(expr2str(un->index), expr2str(un->value)));
      }
    }
    instance().offsetUpdates.insert({{nodeId, offsetPlaneId}, childUpdates});
  }
}

void ProgressRecorder::recordInfo(int nodeID, int parentID,
                                  const MemoryMap objects) {
  instance().accessCount[parentID] += 1;
  instance().recordedNodesIDs.insert(nodeID);
  std::set<int> currentIds;

  for (auto it = objects.begin(); it != objects.end(); ++it) {
    currentIds.insert(it->first->id);
    instance().recordPlanes(nodeID, it->second->segmentPlane, it->second->offsetPlane);
  }

  instance().parentIds.insert({nodeID, currentIds});
}

void ProgressRecorder::object2json(std::ostream &ostr, const MemoryObject *const obj, const klee::ref<klee::ObjectState>& state, int nodeID, int parentID) {
  // OBJECT INFO
  ostr << "        {";
  ostr << "\"objID\": " << obj->id << ", ";
  ostr << "\"segment\": " << obj->segment << ", ";
  ostr << "\"name\": \"" << obj->name << "\", ";
  ostr << "\"size\": \"" << expr2str(obj->size) << "\", ";
  ostr << "\"isLocal\": " << obj->isLocal << ", ";
  ostr << "\"isGlobal\": " << obj->isGlobal << ", ";
  ostr << "\"isFixed\": " << obj->isFixed << ", ";
  ostr << "\"isUserSpec\": " << obj->isUserSpecified << ", ";
  ostr << "\"isLazy\": " << obj->isLazyInitialized << ", ";
  ostr << "\"symAddress\": \""
      << (obj->symbolicAddress ? expr2str(*obj->symbolicAddress) : "") << "\", ";
  
  // OBJECT STATE INFO
  ostr << "\"copyOnWriteOwner\": " << state->copyOnWriteOwner << ", ";
  ostr << "\"readOnly\": " << state->readOnly << ",\n";

  ostr << "          \"allocSite\": { ";
  if (obj->allocSite != nullptr) {
    std::string result;
    llvm::raw_string_ostream info(result);
    if (const llvm::Instruction *instr = llvm::dyn_cast<llvm::Instruction>(obj->allocSite)) {
      info << "\"scope\": \"function\", \"name\": \"" << instr->getParent()->getParent()->getName() << "\", \"code\": \"" << *instr << "\" ";
    } else if (const llvm::GlobalValue *gv = dyn_cast<llvm::GlobalValue>(obj->allocSite)) {
      info << "\"scope\": \"static\", \"name\": \"" << gv->getName() << "\" ";
    } else {
      info << "\"scope\": \"value\", \"code\": \"" << *obj->allocSite << "\" ";
    }
    info.flush();
    ostr << result;
  }
  ostr << "},\n";

  ostr << "          \"segmentPlane\": {";
  instance().plane2json(ostr, state->segmentPlane, nodeID, parentID,
  false); ostr << "\n          },\n";

  ostr << "          \"offsetPlane\": {";
  instance().plane2json(ostr, state->offsetPlane, nodeID, parentID,
  true); ostr << "\n          }";
  
  ostr << "\n        }";
}

void ProgressRecorder::InsertNode::toJson(std::ostream &ostr) {
  int parentID =
      node->parent == nullptr ? -1 : instance().nodeIDs.at(node->parent);

  ostr << "    \"action\": \"InsertNode\", ";
  ostr << "\"nodeID\": " << nodeID << ", ";
  ostr << "\"stateID\": " << stateID << ", ";
  ostr << "\"uniqueState\": " << uniqueState << ", ";
  ostr << "\"parentID\": " << parentID << ", ";
  ostr << "\"parentJSON\": "
       << (node->parent == nullptr ? -1 : instance().nodeJSONs.at(parentID))
       << ", ";
  ostr << "\"memoryJSON\": " << (uniqueState ? nodeID : nodeID - 1) << ", ";

  const InstructionInfo *const instrInfo{node->state->prevPC->info};
  ostr << "\"location\": [";
  instr2json(ostr, instrInfo);
  ostr << "], ";
  const InstructionInfo *const nextInstrInfo{node->state->pc->info};
  ostr << "\"nextLocation\": [";
  instr2json(ostr, nextInstrInfo);
  ostr << "], ";

  ostr << "\"depth\": " << node->state->depth << ", ";
  ostr << "\"coveredNew\": " << node->state->coveredNew << ", ";
  ostr << "\"forkDisabled\": " << node->state->forkDisabled << ", ";
  ostr << "\"instsSinceCovNew\": " << node->state->instsSinceCovNew << ", ";
  ostr << "\"nextID\": " << node->state->nextID << ", ";
  ostr << "\"steppedInstructions\": " << node->state->steppedInstructions
       << ", ";

  if (node->state->unwindingInformation != nullptr) {
    ostr << "\"unwindingKind\": "
         << (node->state->unwindingInformation->getKind() ==
                     UnwindingInformation::Kind::SearchPhase
                 ? "\"SearchPhase\""
                 : "\"CleanupPhase\"")
         << ", ";
    ostr << "\"unwindingException\": \""
         << expr2str(node->state->unwindingInformation->exceptionObject)
         << "\", ";
  }

  stack2json(ostr, node->state->stack);
  constraints2json(ostr, node->state->constraints);
}

void ProgressRecorder::InsertMemory::toJson(std::ostream &ostr) {
  int parentID =
      node->parent == nullptr ? -1 : instance().nodeIDs.at(node->parent);

  ostr << "    \"action\": \"InsertMemory\", ";
  ostr << "\"nodeID\": " << nodeID << ",\n";

  nondetValues2json(ostr, node->state->nondetValues);

  std::set<int> currentParentIds;
  auto it = instance().parentIds.find(parentID);
  if (it != instance().parentIds.end()) {
    currentParentIds = it->second;
  }

  std::set<int> childIds;
  std::set<int> changesIds;

  for (auto it = node->state->addressSpace.objects.begin();
       it != node->state->addressSpace.objects.end();) {
    childIds.insert(it->first->id);
    ++it;
  }

  std::tuple<std::set<int>, std::set<int>> diff =
      getDiff(currentParentIds, childIds);
  std::set<int> additionsIds = std::get<0>(diff);
  std::set<int> deletionsIds = std::get<1>(diff);

  bool addComma = false;

  ostr << "    \"objects\": {\n";

  ostr << "      \"added\": [";
  for (auto it = node->state->addressSpace.objects.begin();
       it != node->state->addressSpace.objects.end(); ++it) {
    if (additionsIds.find(it->first->id) != additionsIds.end()) {
      ostr << (addComma ? ",\n" : "\n");
      addComma = true;
      instance().object2json(ostr, it->first, it->second, nodeID, parentID);
    }
  }
  ostr << "\n      ],\n";

  addComma = false;

  ostr << "      \"changed\": [";
  for (auto it = node->state->addressSpace.objects.begin();
       it != node->state->addressSpace.objects.end(); ++it) {
    
    if (additionsIds.find(it->first->id) == additionsIds.end() &&
        instance().hasChanged(nodeID, parentID, it->second->segmentPlane, it->second->offsetPlane)) {
      ostr << (addComma ? ",\n" : "\n");
      addComma = true;
      instance().object2json(ostr, it->first, it->second, nodeID, parentID);
    }
  }
  ostr << "\n      ],\n";

  ostr << "      \"deleted\": [\n";
  for (auto it = deletionsIds.begin(); it != deletionsIds.end();) {
    ostr << "        " << *it;
    ++it;
    ostr << (it != deletionsIds.end() ? ", " : "\n");
  }
  ostr << "      ]\n";

  ostr << "    },\n";

  ostr << "    \"removedObjectsMap\": [\n";
  for (auto it = node->state->addressSpace.removedObjectsMap.begin();
       it != node->state->addressSpace.removedObjectsMap.end();) {
    ostr << "      {";
    ostr << "\"segment\": " << it->first << "\", ";
    ostr << "\"symbolicArray\": \"" << expr2str(it->second) << "\"";
    ostr << "\n      }";
    ++it;
    ostr << (it != node->state->addressSpace.removedObjectsMap.end() ? ", "
                                                                     : "");
  }
  ostr << "    ], \n";

  ostr << "    \"lazyObjectsMap\": [\n";
  for (auto it = node->state->addressSpace.lazyObjectsMap.begin();
       it != node->state->addressSpace.lazyObjectsMap.end();) {
    ostr << "      {";
    ostr << "\"pointerSegment\": " << it->first << ", ";
    ostr << "\"offsets\": [";
    for (auto a = it->second.begin(); a != it->second.end();) {
      ostr << expr2str(*a);
      ++a;
      ostr << (a != it->second.end() ? ", " : "");
    }
    ostr << "    ], \n";
    ostr << "}";
    ++it;
    ostr << (it != node->state->addressSpace.lazyObjectsMap.end() ? ", " : "");
  }
  ostr << "    ], \n";

  ostr << "    \"symbolics\": [\n";
  for (auto it = node->state->symbolics.begin();
       it != node->state->symbolics.end();) {
    ostr << "      {\n";
    ostr << "        \"objID\": " << it->first->id << ", ";
    ostr << "\"name\": \"" << it->second->name << "\", ";
    ostr << "\"size\": " << it->second->size << ", ";
    ostr << "\"domain\": " << it->second->domain << ", ";
    ostr << "\"range\": " << it->second->range << ", ";
    ostr << "\"isSymbolicArray\": " << it->second->isSymbolicArray() << ", ";
    ostr << "\"constantValues\": [";
    for (auto a = it->second->constantValues.begin();
         a != it->second->constantValues.end();) {
      ostr << "\"" << expr2str(*a) << "\"";
      ++a;
      ostr << (a != it->second->constantValues.end() ? ", " : "");
    }
    ostr << "] \n";
    ostr << "      }";
    ++it;
    ostr << (it != node->state->symbolics.end() ? ",\n" : "\n");
  }
  ostr << "    ], \n";

  ostr << "    \"cexPreferences\": [\n";
  for (auto it = node->state->cexPreferences.begin();
       it != node->state->cexPreferences.end();) {
    ostr << "      \"" << expr2str(*it) << "\"";
    ++it;
    ostr << (it != node->state->cexPreferences.end() ? ", " : "");
  }
  ostr << "    ]\n";
}

bool ProgressRecorder::hasChanged(int nodeID, int parentID, 
                                  const ObjectStatePlane* const segmentPlane, 
                                  const ObjectStatePlane* const offsetPlane) {
    // Get diffs for each ByteType (CONCRETE, SYMBOLIC, MASK) for segment and offset planes
    auto segmentConcreteDiff = instance().getByteDiff(segmentPlane, nodeID, parentID, false, ByteType::CONCRETE);
    auto offsetConcreteDiff = instance().getByteDiff(offsetPlane, nodeID, parentID, true, ByteType::CONCRETE);
    
    auto segmentSymbolicDiff = instance().getByteDiff(segmentPlane, nodeID, parentID, false, ByteType::SYMBOLIC);
    auto offsetSymbolicDiff = instance().getByteDiff(offsetPlane, nodeID, parentID, true, ByteType::SYMBOLIC);
    
    auto segmentMaskDiff = instance().getByteDiff(segmentPlane, nodeID, parentID, false, ByteType::MASK);
    auto offsetMaskDiff = instance().getByteDiff(offsetPlane, nodeID, parentID, true, ByteType::MASK);

    bool hasSegmentConcreteChanges = !std::get<0>(segmentConcreteDiff).empty() || !std::get<1>(segmentConcreteDiff).empty();
    bool hasOffsetConcreteChanges = !std::get<0>(offsetConcreteDiff).empty() || !std::get<1>(offsetConcreteDiff).empty();
    
    bool hasSegmentSymbolicChanges = !std::get<0>(segmentSymbolicDiff).empty() || !std::get<1>(segmentSymbolicDiff).empty();
    bool hasOffsetSymbolicChanges = !std::get<0>(offsetSymbolicDiff).empty() || !std::get<1>(offsetSymbolicDiff).empty();
    
    bool hasSegmentMaskChanges = !std::get<0>(segmentMaskDiff).empty() || !std::get<1>(segmentMaskDiff).empty();
    bool hasOffsetMaskChanges = !std::get<0>(offsetMaskDiff).empty() || !std::get<1>(offsetMaskDiff).empty();

    return hasSegmentConcreteChanges || hasOffsetConcreteChanges ||
           hasSegmentSymbolicChanges || hasOffsetSymbolicChanges ||
           hasSegmentMaskChanges || hasOffsetMaskChanges;
}


void ProgressRecorder::deleteParentInfo(const int parentID) {
  parentIds.erase(parentID);
  nodeJSONs.erase(parentID);
  accessCount.erase(parentID);

  for (auto it = segmentUpdates.begin(); it != segmentUpdates.end();) {
    if (it->first.first == parentID) {
      it = segmentUpdates.erase(it);
    } else {
      ++it;
    }
  }

  for (auto it = offsetUpdates.begin(); it != offsetUpdates.end();) {
    if (it->first.first == parentID) {
      it = offsetUpdates.erase(it);
    } else {
      ++it;
    }
  }

  for (auto it = segmentMemory.begin(); it != segmentMemory.end();) {
    if (it->first.first == parentID) {
      it = segmentMemory.erase(it);
    } else {
      ++it;
    }
  }

  for (auto it = offsetMemory.begin(); it != offsetMemory.end();) {
    if (it->first.first == parentID) {
      it = offsetMemory.erase(it);
    } else {
      ++it;
    }
  }
}

void ProgressRecorder::InsertEdge::toJson(std::ostream &ostr) {
  ostr << "    \"action\": \"InsertEdge\", ";
  ostr << "\"parentID\": " << parentID << ", ";
  ostr << "\"childID\": " << childID << ", ";
  ostr << "\"tag\": " << (int)tag << "\n";
}

void ProgressRecorder::EraseNode::toJson(std::ostream &ostr) {
  ostr << "    \"action\": \"EraseNode\", ";
  ostr << "\"nodeID\": " << ID << "\n";
}
} // namespace klee
