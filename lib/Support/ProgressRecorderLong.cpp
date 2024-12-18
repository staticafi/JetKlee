//===-- ProgressRecorderLong.cpp ----------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Support/ProgressRecorderLong.h"
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

ProgressRecorderLong &ProgressRecorderLong::instance() {
  static ProgressRecorderLong rec;
  return rec;
}

const std::string ProgressRecorderLong::rootDirName{"__JetKleeProgressRecordingLong__"};
const std::string ProgressRecorderLong::treeDirName{"Tree"};
const std::string ProgressRecorderLong::memoryDirName{"States"};
const mode_t ProgressRecorderLong::dirPermissions{0775};

ProgressRecorderLong::ProgressRecorderLong()
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
  return mkdir(dir.c_str(), ProgressRecorderLong::dirPermissions) >= 0;
}

int ProgressRecorderLong::getNodeCounter() { return instance().nodeCounter; }

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

void ProgressRecorderLong::end() {
  std::string kleeOutDir = rootOutputDir;
  size_t lastSlashPos = kleeOutDir.find_last_of("/\\");
  if (lastSlashPos != std::string::npos) {
    kleeOutDir = kleeOutDir.substr(0, lastSlashPos);
  }

  const std::string assemblyFilePath = kleeOutDir + "/assembly.ll";
  copyFile(assemblyFilePath, rootOutputDir + "/source.ll");
}

bool ProgressRecorderLong::start(const std::string &underDir,
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

void ProgressRecorderLong::stop() { rootOutputDir.clear(); }

bool ProgressRecorderLong::started() const { return !rootOutputDir.empty(); }

void ProgressRecorderLong::onRoundBegin() { ++roundCounter; }

void ProgressRecorderLong::onRoundEnd() {
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

void ProgressRecorderLong::onInsertInfo(int nodeID, const PTreeNode *const node) {
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

void ProgressRecorderLong::onInsertNode(const PTreeNode *const node) {
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

void ProgressRecorderLong::onInsertEdge(const PTreeNode *const parent,
                                    const PTreeNode *const child) {
  nodeIDs.at(parent);
  nodeIDs.at(child);
  roundActions.push_back(
      std::make_unique<InsertEdge>(nodeIDs.at(parent), nodeIDs.at(child)));
}

void ProgressRecorderLong::onEraseNode(const PTreeNode *const node) {
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

void ProgressRecorderLong::plane2json(std::ostream &ostr,
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

  std::string indent12 = std::string(12, ' ');
  
  {
    if (plane->concreteStore.size() > 0) {
    ostr << ",\n        \"concreteStoreLong\": [";
    bool start = true;
    for (auto x : plane->concreteStore)
    {
      if (start) start = false; else ostr << ",";
      ostr << (unsigned int)x;
    }
    ostr << "]";
    }
  }
  {
    if (plane->concreteMask.size() > 0) {
    ostr << ",\n        \"concreteMaskLong\": [";
    bool start = true;
    for (auto i = 0U;  i < plane->concreteMask.size(); ++i)
    {
      if (start) start = false; else ostr << ",";
      ostr << (unsigned int)plane->isByteConcrete(i);
    }
    ostr << "]";
    }
  }
  {
    ostr << ",\n        \"knownSymbolicsLong\": [";
    bool start = true;
    for (auto i = 0U;  i < plane->sizeBound; ++i)
    {
      if (plane->isByteKnownSymbolic(i)) {
        if (start) start = false; else ostr << ",";
        ostr << "\"";        
        ostr << expr2str(plane->knownSymbolics[i]);
        ostr << "\"";
      }
    }
    ostr << "]";
  }
  {
  if (plane->getUpdates().getSize() > 0) {
    ostr << ",\n        \"updatesLong\": [";
    bool start = true;
    for (const auto *un = plane->getUpdates().head.get(); un; un = un->next.get()) {
      if (start) start = false; else ostr << ",";
      ostr << "{";
      ostr << "\"index\": " << "\"" << expr2str(un->index) << "\", ";
      ostr << "\"value\": " << "\"" << expr2str(un->value) << "\"";
      ostr << "}";
    }
    ostr << "]";
    }
  }
}

static void instr2json(std::ostream &ostr, const InstructionInfo *const instr) {
  ostr << "\"" << instr->file << "\"," << instr->line << "," << instr->column
       << "," << instr->assemblyLine;
}

void ProgressRecorderLong::recordInfo(int nodeID, int parentID,
                                  const MemoryMap objects) {
  instance().accessCount[parentID] += 1;
  instance().recordedNodesIDs.insert(nodeID);
  std::set<int> currentIDs;

  for (auto it = objects.begin(); it != objects.end(); ++it) {
    currentIDs.insert(it->first->id);
  }

  instance().parentIDs.insert({nodeID, currentIDs});
}

void ProgressRecorderLong::object2json(std::ostream &ostr, const MemoryObject *const obj, const klee::ref<klee::ObjectState>& state, int nodeID, int parentID) {
  // OBJECT INFO
  ostr << "    {";
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

  ostr << "      \"allocSite\": {";
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

  ostr << "      \"segmentPlane\": {";
  instance().plane2json(ostr, state->segmentPlane, nodeID, parentID,
  false); ostr << "\n      },\n";

  ostr << "      \"offsetPlane\": {";
  instance().plane2json(ostr, state->offsetPlane, nodeID, parentID,
  true); ostr << "\n      }";
  
  ostr << "\n    }";
}

void ProgressRecorderLong::InsertNode::toJson(std::ostream &ostr) {
  ostr << "\"action\": \"InsertNode\", ";
  ostr << "\"nodeID\": " << nodeID << ", ";
  ostr << "\"stateID\": " << stateID << ", ";
  ostr << "\"uniqueState\": " << uniqueState << ", ";

  const InstructionInfo *const instrInfo{node->state->prevPC->info};
  ostr << "\"firstLocation\": [";
  instr2json(ostr, instrInfo);
  ostr << "], ";
  stack2json(ostr, node->state->stack);
}

void ProgressRecorderLong::InsertInfo::toJson(std::ostream &ostr) {
  int parentID =
      node->parent == nullptr ? -1 : instance().nodeIDs.at(node->parent);

  ostr << "  \"action\": \"InsertInfo\", ";
  ostr << "\"nodeID\": " << nodeID << ",";
  ostr << "\"stateID\": " << node->state->id << ", ";
  ostr << "\"parentID\": " << parentID << ", ";
  ostr << "\"parentJSON\": "
       << (node->parent == nullptr ? -1 : instance().nodeJSONs.at(parentID))
       << ",\n";

  const InstructionInfo *const instrInfo{node->state->prevPC->info};
  ostr << "  \"lastLocation\": [";
  instr2json(ostr, instrInfo);
  ostr << "], ";
  ostr << "\"depth\": " << node->state->depth << ", ";
  ostr << "\"coveredNew\": " << node->state->coveredNew << ", ";
  ostr << "\"forkDisabled\": " << node->state->forkDisabled << ", ";
  ostr << "\"instsSinceCovNew\": " << node->state->instsSinceCovNew << ", ";
  ostr << "\"steppedInstructions\": " << node->state->steppedInstructions << ",\n";
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

  ostr << "  \"objects\": [\n";
  for (auto it = node->state->addressSpace.objects.begin();
       it != node->state->addressSpace.objects.end();) {
    instance().object2json(ostr, it->first, it->second, nodeID, parentID);
    ++it;
    ostr << (it != node->state->addressSpace.objects.end() ? ",\n" : "\n");
  }
  ostr << "  ]\n";
}

void ProgressRecorderLong::deleteParentInfo(const int parentID) {
  parentIDs.erase(parentID);
  nodeJSONs.erase(parentID);
  accessCount.erase(parentID);
}

void ProgressRecorderLong::InsertEdge::toJson(std::ostream &ostr) {
  ostr << "\"action\": \"InsertEdge\", ";
  ostr << "\"parentID\": " << parentID << ", ";
  ostr << "\"childID\": " << childID;
}

void ProgressRecorderLong::EraseNode::toJson(std::ostream &ostr) {
  ostr << "\"action\": \"EraseNode\", ";
  ostr << "\"nodeID\": " << ID;
}
} // namespace klee
