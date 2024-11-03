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
  bool isUniqueState = instance().nodeUniqueStates.at(nodeID);
  int parentID = node->parent == nullptr ? -1 : instance().nodeIDs.at(node->parent);
  
  if (isUniqueState) {  
    std::string pathName = memoryDir + "/" + std::to_string(nodeID) + ".json";
    std::ofstream ostr(pathName.c_str());
    InsertMemory insertMemory(node, nodeID);

    ostr << "{\n";
    insertMemory.toJson(ostr);
    ostr << "}";
  }

  klee_message("Recording memory info for node %d", nodeID);
  instance().recordInfo(nodeID, parentID, node->state->addressSpace.objects);

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
  nodeUniqueStates.insert({nodeID, uniqueState});
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

using GroupMap = std::map<std::tuple<bool, bool, bool>,
                          std::map<klee::ref<klee::Expr>, std::vector<int>>>;
GroupMap groupByteInfo(const std::set<ByteInfo> &byteInfo) {
  GroupMap groupedBytes;

  for (auto it = byteInfo.begin(); it != byteInfo.end(); ++it) {
    std::tuple<bool, bool, bool> flags =
        std::make_tuple(it->isConcrete, it->isKnownSym, it->isUnflushed);
    groupedBytes[flags][it->value].push_back(it->offset);
  }
  return groupedBytes;
}

void groupMapToJson(std::ostream &ostr, const GroupMap &groupMap,
                    std::string indent) {
  for (auto gm = groupMap.begin(); gm != groupMap.end();) {
    ostr << indent << "  [";
    bool isConcrete, isKnownSym, isUnflushed;
    std::tie(isConcrete, isKnownSym, isUnflushed) = gm->first;
    ostr << isConcrete << "," << isKnownSym << "," << isUnflushed << ",";

    // first element of the list is value, other elements are indices of bytes with this value
    // e.g. ["0",0,1,2,3,4,5] means that 0 is the value of bytes with offsets 0,1,2,3,4,5
    for (auto value = gm->second.begin(); value != gm->second.end();) {
      ostr << "[";
      ostr << "\"" << expr2str(value->first) << "\"" << ",";
      for (auto index = value->second.begin(); index != value->second.end();) {
        ostr << *index;
        ++index;
        ostr << (index != value->second.end() ? "," : "");
      }
      ++value;
      ostr << (value != gm->second.end() ? "]," : "]");
    }
    ++gm;
    ostr << (gm != groupMap.end() ? "],\n" : "]\n");
  }
}

void byteDiffToJson(std::ostream &ostr, const GroupMap &additions,
                    const GroupMap &deletions, int indentSize) {
  std::string indent(indentSize, ' ');

  ostr << indent << "\"add\": [\n";
  groupMapToJson(ostr, additions, indent);
  ostr << indent << "],\n";

  ostr << indent << "\"del\": [\n";
  groupMapToJson(ostr, deletions, indent);
  ostr << indent << "]\n";
}

std::set<ByteInfo> getByteInfo(const ObjectStatePlane *const plane) {
  std::set<ByteInfo> byteInfo;
  for (unsigned i = 0; i < plane->sizeBound; ++i) {
    ByteInfo byte;
    byte.offset = i;
    byte.isConcrete = plane->isByteConcrete(i);
    byte.isKnownSym = plane->isByteKnownSymbolic(i);
    byte.isUnflushed = plane->isByteUnflushed(i);
    byte.value = plane->read8(i);
    byteInfo.insert(byte);
  }
  return byteInfo;
}

Updates ProgressRecorder::getUpdateDiff(const UpdateList updateList, int nodeID, int parentID) {
  Updates additions;
  Updates childUpdates;
  Updates parentUpdates;

  if (updateList.head.get() == nullptr)
    return additions;

  for (const auto *un = updateList.head.get(); un; un = un->next.get()) {
    childUpdates.insert(std::make_tuple(expr2str(un->index), expr2str(un->value)));
  }

  auto itUpdates = instance().updates.find({parentID});
  if (itUpdates != instance().updates.end()) {
      parentUpdates = itUpdates->second;
    }

  std::tuple<Updates, Updates> diff = getDiff(parentUpdates, childUpdates);
  return std::get<0>(diff);
}

std::tuple<std::set<ByteInfo>, std::set<ByteInfo>> ProgressRecorder::getByteDiff(const ObjectStatePlane *const plane,
                                  int nodeID, int parentID, bool isOffset) {
  std::tuple<std::set<ByteInfo>, std::set<ByteInfo>> diff;
  if (plane == nullptr)
    return diff;
  int planeID = plane->getParent() ? plane->getParent()->getObject()->id : -1;

  std::set<ByteInfo> bytes = getByteInfo(plane);
  std::set<ByteInfo> parentBytes;

  if (isOffset) {
    auto itBytes = instance().offsetBytes.find({parentID, planeID});
    if (itBytes != instance().offsetBytes.end()) {
      parentBytes = itBytes->second;
    }
  } else {
    auto itBytes = instance().segmentBytes.find({parentID, planeID});
    if (itBytes != instance().segmentBytes.end()) {
      parentBytes = itBytes->second;
    }
  }

  diff = getDiff(parentBytes, bytes);
  return diff;
}

void ProgressRecorder::plane2json(std::ostream &ostr,
                                  const ObjectStatePlane *const plane,
                                  int nodeID, int parentID, bool isOffset) {
  if (plane == nullptr)
    return;

  int objID = plane->getParent() ? plane->getParent()->getObject()->id : -1;
  ostr << "\"objID\": " << objID << ", ";
  ostr << "\"rootObject\": " << "\"" << plane->getUpdates().root->getName() << "\", ";
  ostr << "\"sizeBound\": " << plane->sizeBound << ", ";
  ostr << "\"initialized\": " << plane->initialized << ", ";
  ostr << "\"symbolic\": " << plane->symbolic << ", ";
  ostr << "\"initialValue\": " << (int)plane->initialValue;

  std::tuple<std::set<ByteInfo>, std::set<ByteInfo>> diff =
      getByteDiff(plane, nodeID, parentID, isOffset);
  GroupMap groupedAdd = groupByteInfo(std::get<0>(diff));
  GroupMap groupedDel = groupByteInfo(std::get<1>(diff));

  if (!(groupedAdd.empty() && groupedDel.empty())) {
    ostr << ",\n            \"bytes\": {\n";
    byteDiffToJson(ostr, groupedAdd, groupedDel, 14);
    ostr << "            }";
  }

  Updates updatesAdd = getUpdateDiff(plane->getUpdateList(), nodeID, parentID);
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

void ProgressRecorder::recordPlane(int nodeId, const ObjectStatePlane *const plane, int parentID, bool isSegment) {
  if (plane == nullptr) {
    return;
  }

  int planeID = plane->getParent() ? plane->getParent()->getObject()->id : -1;

  std::set<ByteInfo> bytes = getByteInfo(plane);
  if (isSegment) {
    instance().segmentBytes.insert({{nodeId, planeID}, bytes});
  } else {
    instance().offsetBytes.insert({{nodeId, planeID}, bytes});
  }

  Updates childUpdates;
  if (plane->getUpdateList().head.get() != nullptr) {
    for (const auto *un = plane->getUpdateList().head.get(); un; un = un->next.get()) {
      childUpdates.insert(std::make_tuple(expr2str(un->index), expr2str(un->value)));
    }
  }

  instance().updates.insert({nodeId, childUpdates});
}

void ProgressRecorder::recordInfo(int nodeID, int parentID,
                                  const MemoryMap objects) {
  instance().accessCount[parentID] += 1;
  instance().recordedNodesIDs.insert(nodeID);
  std::set<int> currentIds;

  for (auto it = objects.begin(); it != objects.end(); ++it) {
    currentIds.insert(it->first->id);
    instance().recordPlane(nodeID, it->second->segmentPlane, parentID, true);
    instance().recordPlane(nodeID, it->second->offsetPlane, parentID, false);
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
    
    auto segmentDiff = instance().getByteDiff(it->second->segmentPlane, nodeID, parentID, false);
    auto offsetDiff = instance().getByteDiff(it->second->offsetPlane, nodeID, parentID, true);
    
    if (additionsIds.find(it->first->id) == additionsIds.end() &&
        (!std::get<0>(segmentDiff).empty() || !std::get<1>(segmentDiff).empty() || 
         !std::get<0>(offsetDiff).empty() || !std::get<1>(offsetDiff).empty())) {
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

void ProgressRecorder::deleteParentInfo(const int parentID) {
  parentIds.erase(parentID);
  nodeJSONs.erase(parentID);
  accessCount.erase(parentID);
  updates.erase(parentID);

  for (auto it = segmentBytes.begin(); it != segmentBytes.end();) {
    if (it->first.first == parentID) {
      it = instance().segmentBytes.erase(it);
    } else {
      ++it;
    }
  }

  for (auto it = offsetBytes.begin(); it != offsetBytes.end();) {
    if (it->first.first == parentID) {
      it = instance().offsetBytes.erase(it);
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
