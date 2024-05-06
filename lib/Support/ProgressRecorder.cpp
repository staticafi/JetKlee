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
  return mkdir(dir.c_str(), 0775) >= 0;
}

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

bool ProgressRecorder::start(const std::string &underDir,
                             std::string fileName) {
  if (!createDir(underDir))
    return false;
  rootOutputDir = underDir;

  char bcFilePath[PATH_MAX];
  if (realpath(fileName.c_str(), bcFilePath)) {
    std::string bcFilePathStr(bcFilePath);
    std::string command =
        "llvm-dis " + bcFilePathStr + " -o " + rootOutputDir + "/source.ll";
    std::system(command.c_str());
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
  std::string pathName =
      rootOutputDir + "/" + std::to_string(roundCounter) + ".json";
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

void ObjectInfo::toJson(std::ostream &ostr) const {
  ostr << "        { ";
  ostr << "\"objID\": " << objID << ", ";
  ostr << "\"segment\": " << segment << ", ";
  ostr << "\"name\": \"" << name << "\", ";
  ostr << "\"size\": \"" << expr2str(size) << "\", ";
  ostr << "\"isLocal\": " << isLocal << ", ";
  ostr << "\"isGlobal\": " << isGlobal << ", ";
  ostr << "\"isFixed\": " << isFixed << ", ";
  ostr << "\"isUserSpec\": " << isUserSpecified << ", ";
  ostr << "\"isLazy\": " << isLazyInitialized << ", ";
  ostr << "\"symAddress\": \""
       << (symbolicAddress ? expr2str(*symbolicAddress) : "") << "\" ";
  ostr << "}";
}

template <typename T>
std::tuple<std::set<T>, std::set<T>> getDiff(const std::set<T> &parent, const std::set<T> &child) {
  std::set<T> additions;
  std::set<T> deletions;

  std::set_difference(child.begin(), child.end(), parent.begin(), parent.end(), std::inserter(additions, additions.begin()));
  std::set_difference(parent.begin(), parent.end(), child.begin(), child.end(), std::inserter(deletions, deletions.begin()));

  return std::make_tuple(additions, deletions);
}

using GroupMap = std::map<std::tuple<bool, bool, bool>, std::map<klee::ref<klee::Expr>, std::vector<int>>>;
GroupMap groupByteInfo(const std::set<ByteInfo> &byteInfo) {
  GroupMap groupedBytes;
  for (auto b = byteInfo.begin(); b != byteInfo.end(); ++b) {
    std::tuple<bool, bool, bool> flags = std::make_tuple(b->isConcrete, b->isKnownSym, b->isUnflushed);
    groupedBytes[flags][b->value].push_back(b->offset);
  }
  return groupedBytes;
}

void groupMapToJson(std::ostream &ostr, const GroupMap &groupMap, std::string indent) {
  for (auto it = groupMap.begin(); it != groupMap.end();) {
    ostr << indent << "  [";
    bool isConcrete, isKnownSym, isUnflushed;
    std::tie(isConcrete, isKnownSym, isUnflushed) = it->first;
    ostr << isConcrete << "," << isKnownSym << "," << isUnflushed << ",";

    // first element is value, other elements are bytes
    for (auto val = it->second.begin(); val != it->second.end();) {
      ostr << "[";
      ostr << "\"" << expr2str(val->first) << "\"" << ",";
      for (auto b = val->second.begin(); b != val->second.end();) {
        ostr << *b;
        ++b;
        ostr << (b != val->second.end() ? "," : "");
      }
      ++val;
      ostr << (val != it->second.end() ? "]," : "]");
    }
    ++it;
    ostr << (it != groupMap.end() ? "],\n" : "]\n");
  }
}

void byteDiffToJson(std::ostream &ostr, const GroupMap &additions, const GroupMap &deletions, int indentSize) {
  std::string indent(indentSize, ' ');
  
  ostr << indent << "\"add\": [\n";
  groupMapToJson(ostr, additions, indent);
  ostr << indent << "],\n";

  ostr << indent << "\"del\": [\n";
  groupMapToJson(ostr, deletions, indent);
  ostr << indent << "]\n";
}

void objDiffToJson(std::ostream &ostr, const std::set<ObjectInfo> &additions, const std::set<ObjectInfo> &deletions, int indentSize) {
  std::string indent(indentSize, ' ');

  ostr << indent << "\"add\": [\n";
  for (auto it = additions.begin(); it != additions.end();) {
    it->toJson(ostr);
    ++it;
    ostr << (it != additions.end() ? ",\n" : "\n");
  }
  ostr << indent << "],\n";

  ostr << indent << "\"del\": [\n";
  for (auto it = deletions.begin(); it != deletions.end();) {
    it->toJson(ostr);
    ++it;
    ostr << (it != deletions.end() ? ",\n" : "\n");
  }
  ostr << indent << "]\n";
}

std::set<ObjectInfo> getObjectInfo(const MemoryMap &objects) {
  std::set<ObjectInfo> objectsInfo;

  for (auto it = objects.begin(); it != objects.end(); ++it) {
    ObjectInfo info;
    info.objID = it->first->id;
    info.segment = it->first->segment;
    info.size = it->first->size;
    info.isLocal = it->first->isLocal;
    info.isGlobal = it->first->isGlobal;
    info.isFixed = it->first->isFixed;
    info.isUserSpecified = it->first->isUserSpecified;
    info.isLazyInitialized = it->first->isLazyInitialized;
    info.symbolicAddress = it->first->symbolicAddress;
    objectsInfo.insert(info);
  }
  return objectsInfo;
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

void ProgressRecorder::plane2json(std::ostream &ostr,
                                  const ObjectStatePlane *const plane,
                                  int nodeID, int parentID, bool isOffset) {
  if (plane == nullptr)
    return;
  int planeID = plane->getParent() ? plane->getParent()->getObject()->id : -1;

  std::set<ByteInfo> bytes = getByteInfo(plane);
  std::set<ByteInfo> parentBytes;

  if (isOffset) {
    instance().offsetBytes.insert({{nodeID, planeID}, bytes});
    auto itBytes = instance().offsetBytes.find({parentID, planeID});

    if (itBytes != instance().offsetBytes.end()) {
      parentBytes = itBytes->second;
    }
  } else {
    instance().segmentBytes.insert({{nodeID, planeID}, bytes});
    auto itBytes = instance().segmentBytes.find({parentID, planeID});

    if (itBytes != instance().segmentBytes.end()) {
      parentBytes = itBytes->second;
    }
  }

  ostr << "\"memoryObjectID\": " << planeID << ", ";

  std::string name =
      plane->getUpdateList().root ? plane->getUpdateList().root->getName() : "";
  ostr << "\"rootObject\": "
       << "\"" << name << "\", ";
  ostr << "\"sizeBound\": " << plane->sizeBound << ", ";
  ostr << "\"initialized\": " << plane->initialized << ", ";
  ostr << "\"symbolic\": " << plane->symbolic << ", ";
  ostr << "\"initialValue\": " << (int)plane->initialValue;

  std::tuple<std::set<ByteInfo>, std::set<ByteInfo>> diff = getDiff(parentBytes, bytes);
  GroupMap groupedAdd = groupByteInfo(std::get<0>(diff));
  GroupMap groupedDel = groupByteInfo(std::get<1>(diff));

  if (!(groupedAdd.empty() && groupedDel.empty())) {
    ostr << ",\n        \"bytes\": {\n";
    byteDiffToJson(ostr, groupedAdd, groupedDel, 10);
    ostr << "        }";
  }
 
  if (plane->getUpdateList().head.get() != nullptr) {
    ostr << ",\n        \"updates\": [\n";
    for (const auto *un = plane->getUpdateList().head.get(); un;) {
      ostr << "        {";
      ostr << "\"" << expr2str(un->index) << "\""
          << " : "
          << "\"" << expr2str(un->value) << "\"";
      un = un->next.get();
      ostr << "}" << (un != nullptr ? ",\n" : "\n");
    }
    ostr << "        ]";
  }
}

void ProgressRecorder::objects2json(std::ostream &ostr, const MemoryMap objects,
                                    int nodeID, int parentID) {
  instance().accessCount[parentID] += 1;
  std::set<ObjectInfo> childObjectInfo = getObjectInfo(objects);
  std::set<ObjectInfo> parentObjectInfo;
  instance().objectStates.insert({nodeID, childObjectInfo});

  auto it = instance().objectStates.find(parentID);
  if (it != instance().objectStates.end()) {
    parentObjectInfo = it->second;
  }
  std::tuple<std::set<ObjectInfo>, std::set<ObjectInfo>> diff = getDiff(parentObjectInfo, childObjectInfo);
  std::set<ObjectInfo> additions = std::get<0>(diff);
  std::set<ObjectInfo> deletions = std::get<1>(diff);
  if (!(additions.empty() && deletions.empty())) {
    ostr << "    \"objects\": {\n";
    objDiffToJson(ostr, additions, deletions, 6);
    ostr << "    },\n";
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

void ProgressRecorder::recordInfo(int nodeID, int parentID, const MemoryMap objects) {
  instance().accessCount[parentID] += 1;
  std::set<ObjectInfo> childObjectInfo = getObjectInfo(objects);
  instance().objectStates.insert({nodeID, childObjectInfo});

  for (auto it = objects.begin(); it != objects.end(); ++it) {
    if (it->second->segmentPlane != nullptr) {
      int planeID = it->second->segmentPlane->getParent() ? it->second->segmentPlane->getParent()->getObject()->id : -1;
      std::set<ByteInfo> bytes = getByteInfo(it->second->segmentPlane);
      instance().segmentBytes.insert({{nodeID, planeID}, bytes});
    }
    if (it->second->offsetPlane != nullptr) {
      int planeID = it->second->offsetPlane->getParent() ? it->second->offsetPlane->getParent()->getObject()->id : -1;
      std::set<ByteInfo> bytes = getByteInfo(it->second->offsetPlane);
      instance().offsetBytes.insert({{nodeID, planeID}, bytes});
    }
  }
  if (instance().accessCount[parentID] == 2) {
    instance().deleteParentInfo(parentID);
  }
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

  const InstructionInfo *const instrInfo{node->state->prevPC->info};
  ostr << "\"location\": [";
  instr2json(ostr, instrInfo);
  ostr << "], ";
  const InstructionInfo *const nextInstrInfo{node->state->pc->info};
  ostr << "\"nextLocation\": [";
  instr2json(ostr, nextInstrInfo);
  ostr << "], ";

  ostr << "\"incomingBBIndex\": " << node->state->incomingBBIndex << ", ";
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

  if (!uniqueState) {
    ostr << "\n";
    instance().recordInfo(nodeID, parentID, node->state->addressSpace.objects);
    return;
  }
  ostr << ",\n";

  nondetValues2json(ostr, node->state->nondetValues);
  instance().objects2json(ostr, node->state->addressSpace.objects, nodeID,
                          parentID);

  ostr << "    \"objectStates\": [\n";
  for (auto it = node->state->addressSpace.objects.begin();
       it != node->state->addressSpace.objects.end();) {
    ostr << "      {\n";
    ostr << "      \"objID\": " << it->first->id << ", ";
    ostr << "\"copyOnWriteOwner\": " << it->second->copyOnWriteOwner << ", ";
    ostr << "\"readOnly\": " << it->second->readOnly << ",\n";

    ostr << "      \"segmentPlane\": {";
    instance().plane2json(ostr, it->second->segmentPlane, nodeID, parentID, false);
    ostr << "\n      },\n";

    ostr << "      \"offsetPlane\": {";
    instance().plane2json(ostr, it->second->offsetPlane, nodeID, parentID, true);
    ostr << "\n      }";
    ostr << "\n      }";
    ++it;
    ostr << (it != node->state->addressSpace.objects.end() ? ",\n" : "\n");
  }
  ostr << "    ],\n";

  ostr << "    \"concreteAddressMap\": [\n";
  for (auto it = node->state->addressSpace.concreteAddressMap.begin();
       it != node->state->addressSpace.concreteAddressMap.end();) {
    ostr << "      {";
    ostr << "\"address\": " << it->first << ", ";
    ostr << "\"segment\": " << it->second;
    ostr << "}";
    ++it;
    ostr << (it != node->state->addressSpace.concreteAddressMap.end() ? ",\n"
                                                                      : "\n");
  }
  ostr << "    ], \n";

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

  if (instance().accessCount[parentID] == 2) {
    instance().deleteParentInfo(parentID);
  }
}

void ProgressRecorder::deleteParentInfo(const int parentID) {
  nodeJSONs.erase(parentID);
  accessCount.erase(parentID);
  objectStates.erase(parentID);

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
