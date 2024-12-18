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
const std::string ProgressRecorder::memoryDirName{"States"};
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
    ostr << "  {";
    roundActions.at(i)->toJson(ostr);
    ostr << "}" << (i + 1U < roundActions.size() ? ",\n" : "\n");
  }
  ostr << "]\n";
  roundActions.clear();
}

void ProgressRecorder::onInsertInfo(int nodeID, const PTreeNode *const node) {
  int parentID = node->parent == nullptr ? -1 : instance().nodeIDs.at(node->parent);
  
  std::string pathName = memoryDir + "/" + std::to_string(nodeID) + ".json";
  std::ofstream ostr(pathName.c_str());

  InsertInfo insertInfo(node, nodeID);

  instance().recordInfo(nodeID, parentID, node->state->addressSpace.objects);

  ostr << "{\n";
  insertInfo.toJson(ostr);
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
                                    const PTreeNode *const child) {
  roundActions.push_back(
      std::make_unique<InsertEdge>(nodeIDs.at(parent), nodeIDs.at(child)));
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
  ostr << "\n    \"stack\":[";
  for (std::size_t i = 0U; i != stack.size(); ++i)
    if (stack.at(i).callPathNode->callSite != nullptr) {
      const InstructionInfo *const callerInfo{stack.at(i).caller->info};
      ostr << "[\"" << callerInfo->file << "\"," << callerInfo->line << ","
           << callerInfo->column << "," << callerInfo->assemblyLine << "]"
           << (i + 1U < stack.size() ? ", " : "");
    }
  ostr << "]\n  ";
}

static void constraints2json(std::ostream &ostr,
                             const ConstraintSet constraints) {
  ostr << "  \"constraints\": [\n";
  for (auto it = constraints.begin(); it != constraints.end();) {
    ostr << "    \"" << expr2str(*it) << "\"";
    ++it;
    ostr << (it != constraints.end() ? ",\n" : "\n");
  }
  ostr << "  ]";
}

void bytesMap2Json(std::ostream &ostr, const BytesMap &bytesMap) {
  std::string indent16 = std::string(16, ' ');
  std::string indent14 = std::string(14, ' ');
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

  return getUpdateAdditions(parentUpdates, childUpdates);
}

Updates ProgressRecorder::getUpdateAdditions(const Updates& parentUpdates, const Updates& childUpdates) {
    Updates additions;

    // Count occurrences of each update in the parent and child multisets
    std::map<std::tuple<std::string, std::string>, int> parentCounts;
    for (const auto& update : parentUpdates) {
        parentCounts[update]++;
    }

    std::map<std::tuple<std::string, std::string>, int> childCounts;
    for (const auto& update : childUpdates) {
        childCounts[update]++;
    }

    for (const auto& childUpdate : childCounts) {
        const auto& update = childUpdate.first;
        int childCount = childUpdate.second;
        int parentCount = parentCounts.count(update) ? parentCounts[update] : 0;

        // If child has more occurrences than the parent, add the difference to additions
        int diffCount = childCount - parentCount;
        for (int i = 0; i < diffCount; ++i) {
            additions.insert(update);
        }
    }

    return additions;
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
  return getVecDiff2(parentBytes, bytes);
}

void ProgressRecorder::plane2json(std::ostream &ostr,
                                  const ObjectStatePlane *const plane,
                                  int nodeID, int parentID, bool isOffset) {
  if (plane == nullptr)
    return;

  int planeID = plane->getParent() ? plane->getParent()->getObject()->id : -1;
  ostr << "\"rootObject\": " << "\"" << plane->getUpdates().root->getName() << "\", ";
  ostr << "\"sizeBound\": " << plane->sizeBound << ", ";
  ostr << "\"initialized\": " << plane->initialized << ", ";
  ostr << "\"symbolic\": " << plane->symbolic << ", ";
  ostr << "\"initialValue\": " << (int)plane->initialValue;

  std::string indent12 = std::string(12, ' ');
  
  BytesDiff concreteStoreDiff = getByteDiff(plane, nodeID, parentID, isOffset, ByteType::CONCRETE);
  BytesMap concreteStoreAdd = std::get<0>(concreteStoreDiff);
  BytesMap concreteStoreDel = std::get<1>(concreteStoreDiff);

  bool addComma = false;

  if (!concreteStoreAdd.empty() || !concreteStoreDel.empty()) {
    ostr << ",\n          \"concreteStore\": {";

    if (!concreteStoreAdd.empty()) {
      ostr << "\n" << indent12 << "\"add\": [";
      bytesMap2Json(ostr, concreteStoreAdd);
      ostr << "]";
      addComma = true;
    }

    if (!concreteStoreDel.empty()) {
      if (addComma) {ostr << ",";}
      ostr << "\n" << indent12 << "\"del\": [";
      bytesMap2Json(ostr, concreteStoreDel);
      ostr << "]";
    }
    ostr << "\n          }";
  }

  addComma = false;

  BytesDiff concreteMaskDiff = getByteDiff(plane, nodeID, parentID, isOffset, ByteType::MASK);
  BytesMap concreteMaskAdd = std::get<0>(concreteMaskDiff);
  BytesMap concreteMaskDel = std::get<1>(concreteMaskDiff);

  if (!concreteMaskAdd.empty() || !concreteMaskDel.empty()) {
    ostr << ",\n          \"concreteMask\": {";

    if (!concreteMaskAdd.empty()) {
      ostr << "\n" << indent12 << "\"add\": [";
      bytesMap2Json(ostr, concreteMaskAdd);
      ostr << "]";
      addComma = true;
    }

    if (!concreteMaskDel.empty()) {
      if (addComma) {ostr << ",";}
      ostr << "\n" << indent12 << "\"del\": [";
      bytesMap2Json(ostr, concreteMaskDel);
      ostr << "]";
    }
    ostr << "\n          }";
  }

  addComma = false;

  BytesDiff knownSymbolicsDiff = getByteDiff(plane, nodeID, parentID, isOffset, ByteType::SYMBOLIC);
  BytesMap knownSymbolicsAdd = std::get<0>(knownSymbolicsDiff);
  BytesMap knownSymbolicsDel = std::get<1>(knownSymbolicsDiff);

  if (!knownSymbolicsAdd.empty() || !knownSymbolicsDel.empty()) {
    ostr << ",\n          \"knownSymbolics\": {";
    if (!knownSymbolicsAdd.empty()) {
      ostr << "\n" << indent12 << "\"add\": [";
      bytesMap2Json(ostr, knownSymbolicsAdd);
      ostr << "]";
      addComma = true;
    }

    if (!knownSymbolicsDel.empty()) {
      if (addComma) {ostr << ",";}
      ostr << "\n" << indent12 << "\"del\": [";
      bytesMap2Json(ostr, knownSymbolicsDel);
      ostr << "]";
    }
    ostr << "\n          }";
  }

  Updates updatesAdd = getUpdateDiff(plane->getUpdates(), nodeID, parentID, isOffset, planeID);
  if (!updatesAdd.empty()) {
    ostr << ",\n          \"updates\": [\n";
    for (auto it = updatesAdd.begin(); it != updatesAdd.end();) {
      const std::string& index = std::get<0>(*it);
      const std::string& value = std::get<1>(*it);
      ostr << "            {";
      ostr << "\"" << value << "\""
           << " : "
           << "\"" << index << "\"";
      ++it;
      ostr << "}" << (it != updatesAdd.end() ? ",\n" : "\n");
    }
    ostr << "          ]";
  }
}

static void instr2json(std::ostream &ostr, const InstructionInfo *const instr) {
  ostr << "\"" << instr->file << "\"," << instr->line << "," << instr->column
       << "," << instr->assemblyLine;
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
    if (plane->isByteKnownSymbolic(i)) {
      knownSymbolics.push_back(expr2str(plane->knownSymbolics[i]));
    }
  }
  return {concreteStore, concreteMask, knownSymbolics};
}

void ProgressRecorder::recordPlanes(int nodeID, const ObjectStatePlane *const segmentPlane, const ObjectStatePlane *const offsetPlane) {
  if (segmentPlane != nullptr) {
    // record plane's memory (concreteStore, concreteMask, knownSymbolics)
    int segmentPlaneID = segmentPlane->getParent() ? segmentPlane->getParent()->getObject()->id : -1;
    Memory currSegmentMemory = getMemory(segmentPlane);
    instance().segmentMemory.insert({{nodeID, segmentPlaneID}, currSegmentMemory});


    // record plane's updates
    Updates childUpdates;
    if (segmentPlane->getUpdates().head.get() != nullptr) {
      for (const auto *un = segmentPlane->getUpdates().head.get(); un; un = un->next.get()) {
        childUpdates.insert(std::make_tuple(expr2str(un->index), expr2str(un->value)));
      }
    }
    instance().segmentUpdates.insert({{nodeID, segmentPlaneID}, childUpdates});
  }

  if (offsetPlane != nullptr) {
    // record plane's memory (concreteStore, concreteMask, knownSymbolics)
    int offsetPlaneID = offsetPlane->getParent() ? offsetPlane->getParent()->getObject()->id : -1;
    Memory currOffsetMemory = getMemory(offsetPlane);
    instance().offsetMemory.insert({{nodeID, offsetPlaneID}, currOffsetMemory});

    // record plane's updates
    Updates childUpdates;
    if (offsetPlane->getUpdates().head.get() != nullptr) {
      for (const auto *un = offsetPlane->getUpdates().head.get(); un; un = un->next.get()) {
        childUpdates.insert(std::make_tuple(expr2str(un->index), expr2str(un->value)));
      }
    }
    instance().offsetUpdates.insert({{nodeID, offsetPlaneID}, childUpdates});
  }
}

void ProgressRecorder::recordInfo(int nodeID, int parentID,
                                  const MemoryMap objects) {
  instance().accessCount[parentID] += 1;
  instance().recordedNodesIDs.insert(nodeID);
  std::set<int> currentIDs;

  for (auto it = objects.begin(); it != objects.end(); ++it) {
    currentIDs.insert(it->first->id);
    instance().recordPlanes(nodeID, it->second->segmentPlane, it->second->offsetPlane);
  }

  instance().parentIDs.insert({nodeID, currentIDs});
}

void ProgressRecorder::object2json(std::ostream &ostr, const MemoryObject *const obj, const klee::ref<klee::ObjectState>& state, int nodeID, int parentID) {
  // OBJECT INFO
  ostr << "      {";
  ostr << "\"objID\": " << obj->id << ", ";
  ostr << "\"segment\": " << obj->segment << ", ";
  ostr << "\"name\": \"" << obj->name << "\", ";
  ostr << "\"size\": \"" << expr2str(obj->size) << "\", ";
  ostr << "\"isLocal\": " << obj->isLocal << ", ";
  ostr << "\"isFixed\": " << obj->isFixed << ", ";
  ostr << "\"isUserSpec\": " << obj->isUserSpecified << ", ";
  ostr << "\"isLazy\": " << obj->isLazyInitialized << ", ";
  
  // OBJECT STATE INFO
  ostr << "\"copyOnWriteOwner\": " << state->copyOnWriteOwner << ", ";
  ostr << "\"readOnly\": " << state->readOnly << ",\n";

  ostr << "        \"allocSite\": {";
  if (obj->allocSite != nullptr) {
    std::string result;
    llvm::raw_string_ostream info(result);
    if (const llvm::Instruction *instr = llvm::dyn_cast<llvm::Instruction>(obj->allocSite)) {
      info << "\"scope\": \"function\", \"name\": \"" << instr->getParent()->getParent()->getName() << "\", \"code\": \"" << *instr << "\"";
    } else if (const llvm::GlobalValue *gv = dyn_cast<llvm::GlobalValue>(obj->allocSite)) {
      info << "\"scope\": \"static\", \"name\": \"" << gv->getName() << "\"";
    } else {
      info << "\"scope\": \"value\", \"code\": \"" << *obj->allocSite << "\"";
    }
    info.flush();
    ostr << result;
  }
  ostr << "},\n";

  ostr << "        \"segmentPlane\": {";
  instance().plane2json(ostr, state->segmentPlane, nodeID, parentID,
  false); ostr << "\n        },\n";

  ostr << "        \"offsetPlane\": {";
  instance().plane2json(ostr, state->offsetPlane, nodeID, parentID,
  true); ostr << "\n        }";
  
  ostr << "\n      }";
}

void ProgressRecorder::InsertNode::toJson(std::ostream &ostr) {
  ostr << "\"action\": \"InsertNode\", ";
  ostr << "\"nodeID\": " << nodeID << ", ";
  ostr << "\"stateID\": " << stateID << ", ";
  ostr << "\"uniqueState\": " << uniqueState << ", ";
  ostr << "\"depth\": " << node->state->depth << ", ";

  const InstructionInfo *const instrInfo{node->state->prevPC->info};
  ostr << "\"firstLocation\": [";
  instr2json(ostr, instrInfo);
  ostr << "], ";
  stack2json(ostr, node->state->stack);
}

void ProgressRecorder::InsertInfo::toJson(std::ostream &ostr) {
  int parentID =
      node->parent == nullptr ? -1 : instance().nodeIDs.at(node->parent);

  ostr << "  \"action\": \"InsertInfo\", ";
  ostr << "\"nodeID\": " << nodeID << ",";
  ostr << "\"parentIter\": "
       << (node->parent == nullptr ? -1 : instance().nodeJSONs.at(parentID))
       << ",\n";

  const InstructionInfo *const instrInfo{node->state->prevPC->info};
  ostr << "  \"lastLocation\": [";
  instr2json(ostr, instrInfo);
  ostr << "], ";
  ostr << "\"coveredNew\": " << node->state->coveredNew << ", ";
  ostr << "\"instsSinceCovNew\": " << node->state->instsSinceCovNew << ", ";
  ostr << "\"steppedInstructions\": " << node->state->steppedInstructions << ",\n";
  ostr << "\"forkDisabled\": " << node->state->forkDisabled << ", ";
  constraints2json(ostr, node->state->constraints);
  ostr << ",\n";

  std::set<int> currentParentIDs;
  auto it = instance().parentIDs.find(parentID);
  if (it != instance().parentIDs.end()) {
    currentParentIDs = it->second;
  }

  std::set<int> childIDs;
  std::set<int> changesIDs;

  for (auto it = node->state->addressSpace.objects.begin();
       it != node->state->addressSpace.objects.end();) {
    childIDs.insert(it->first->id);
    ++it;
  }

  std::tuple<std::set<int>, std::set<int>> diff =
      getDiff(currentParentIDs, childIDs);
  std::set<int> additionsIDs = std::get<0>(diff);
  std::set<int> deletionsIDs = std::get<1>(diff);

  bool addComma = false;

  ostr << "  \"objects\": {\n";
 
  ostr << "    \"added\": [";
  for (auto it = node->state->addressSpace.objects.begin();
       it != node->state->addressSpace.objects.end(); ++it) {
    if (additionsIDs.find(it->first->id) != additionsIDs.end()) {
      ostr << (addComma ? ",\n" : "\n");
      addComma = true;
      instance().object2json(ostr, it->first, it->second, nodeID, parentID);
    }
  }
  ostr << "\n    ],\n";

  addComma = false;

  ostr << "    \"changed\": [";
  for (auto it = node->state->addressSpace.objects.begin();
       it != node->state->addressSpace.objects.end(); ++it) {
    
    if (additionsIDs.find(it->first->id) == additionsIDs.end() &&
        instance().hasChanged(nodeID, parentID, it->second->segmentPlane, it->second->offsetPlane)) {
      ostr << (addComma ? ",\n" : "\n");
      addComma = true;
      instance().object2json(ostr, it->first, it->second, nodeID, parentID);
    }
  }
  ostr << "\n    ],\n";

  ostr << "    \"deleted\": [\n";
  for (auto it = deletionsIDs.begin(); it != deletionsIDs.end();) {
    ostr << "      " <<  *it;
    ++it;
    ostr << (it != deletionsIDs.end() ? ",\n" : "\n");
  }
  ostr << "    ]\n";

  ostr << "  }\n";
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

    Updates segmentUpdatesDiff;
    Updates offsetUpdatesDiff;
    if (segmentPlane != nullptr) {
      segmentUpdatesDiff = instance().getUpdateDiff(segmentPlane->getUpdates(), nodeID, parentID, false, segmentPlane->getParent() ? segmentPlane->getParent()->getObject()->id : -1);
    }
    if (offsetPlane != nullptr) {
      offsetUpdatesDiff = instance().getUpdateDiff(offsetPlane->getUpdates(), nodeID, parentID, true, offsetPlane->getParent() ? offsetPlane->getParent()->getObject()->id : -1);
    }

    bool hasSegmentConcreteChanges = !std::get<0>(segmentConcreteDiff).empty() || !std::get<1>(segmentConcreteDiff).empty();
    bool hasOffsetConcreteChanges = !std::get<0>(offsetConcreteDiff).empty() || !std::get<1>(offsetConcreteDiff).empty();
    
    bool hasSegmentSymbolicChanges = !std::get<0>(segmentSymbolicDiff).empty() || !std::get<1>(segmentSymbolicDiff).empty();
    bool hasOffsetSymbolicChanges = !std::get<0>(offsetSymbolicDiff).empty() || !std::get<1>(offsetSymbolicDiff).empty();
    
    bool hasSegmentMaskChanges = !std::get<0>(segmentMaskDiff).empty() || !std::get<1>(segmentMaskDiff).empty();
    bool hasOffsetMaskChanges = !std::get<0>(offsetMaskDiff).empty() || !std::get<1>(offsetMaskDiff).empty();

    bool hasSegmentUpdates = !segmentUpdatesDiff.empty();
    bool hasOffsetUpdates = !offsetUpdatesDiff.empty();

    return hasSegmentConcreteChanges || hasOffsetConcreteChanges ||
           hasSegmentSymbolicChanges || hasOffsetSymbolicChanges ||
           hasSegmentMaskChanges || hasOffsetMaskChanges ||
            hasSegmentUpdates || hasOffsetUpdates;
}


void ProgressRecorder::deleteParentInfo(const int parentID) {
  parentIDs.erase(parentID);
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
  ostr << "\"action\": \"InsertEdge\", ";
  ostr << "\"nodeID\": " << childID << ", ";
  ostr << "\"parentID\": " << parentID;
}

void ProgressRecorder::EraseNode::toJson(std::ostream &ostr) {
  ostr << "\"action\": \"EraseNode\", ";
  ostr << "\"nodeID\": " << ID;
}
} // namespace klee
