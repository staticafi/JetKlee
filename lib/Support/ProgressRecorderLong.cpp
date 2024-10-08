//===-- ProgressRecorderLong.cpp ----------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Support/ProgressRecorderLong.h"
#include "klee/Module/InstructionInfoTable.h"
#include "klee/Module/KInstruction.h"
#include "../Core/ExecutionState.h"
#include "../Core/PTree.h"
#include "../Core/CallPathManager.h"
#include "../Core/Memory.h"
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <assert.h>

namespace klee {

  static bool createDir(const std::string &dir) {
    return mkdir(dir.c_str(), 0775) >= 0;
  }

  static std::string expr2str(const ref<Expr> &e) {
    std::stringstream sstr;
    sstr << e;
    std::string expr;
    expr.reserve(sstr.str().size());
    bool lastIsSpace{ false };
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

  static void stack2json(std::ostream& ostr, const std::vector<StackFrame> stack){
    ostr << "\"stack\": [";
    for (std::size_t i = 0U; i != stack.size(); ++i)
      if (stack.at(i).callPathNode->callSite != nullptr) {
        const InstructionInfo *const callerInfo{ stack.at(i).caller->info };
        ostr << "[\"" << callerInfo->file << "\","
             << callerInfo->line << ","
             << callerInfo->column << ","
             << callerInfo->assemblyLine << "]"
             << (i + 1U < stack.size() ? "," : "");
      }
    ostr << "],\n";
  }

  static void constraints2json(std::ostream& ostr, const ConstraintSet constraints) {
    ostr << "    \"constraints\": [\n";
    for (auto it = constraints.begin(); it != constraints.end(); ++it)
      ostr << "      \"" << expr2str(*it) << "\"" << (std::next(it) != constraints.end() ? ",\n" : "\n");
    ostr << "    ]";
  }

  static void memoryObjs2json(std::ostream& ostr, const MemoryMap objects){
    ostr << "    \"objects\": [\n";
    for (auto it = objects.begin(); it != objects.end(); ) {
      ostr << "      { ";
      ostr << "\"objID\": " << it->first->id << ", ";
      ostr << "\"segment\": " << it->first->segment << ", ";
      ostr << "\"name\": \"" << it->first->name << "\", ";
      ostr << "\"size\": \"" << expr2str(it->first->size) << "\", ";
      ostr << "\"isLocal\": " << it->first->isLocal << ", ";
      ostr << "\"isGlobal\": " << it->first->isGlobal << ", ";
      ostr << "\"isFixed\": " << it->first->isFixed << ", ";
      ostr << "\"isUserSpecified\": " << it->first->isUserSpecified << ", ";
      ostr << "\"isLazy\": " << it->first->isLazyInitialized << ", ";
      ostr << "\"symAddress\": \"" << (it->first->symbolicAddress ? expr2str(*it->first->symbolicAddress) : "") << "\" ";
      ostr << "}";
      ++it;
      ostr << (it != objects.end() ? ",\n" : "\n");
    }
    ostr << "    ],\n";
  }

  static void plane2json(std::ostream& ostr, const ObjectStatePlane *const plane) {
    if (plane == nullptr) return;

    int id = plane->getParent() ? plane->getParent()->getObject()->id : -1;
    ostr << "\"memoryObjectID\": " << id << ", ";
    
    std::string name = plane->getUpdateList().root ? plane->getUpdateList().root->getName() : "";
    ostr << "\"rootObject\": " << "\"" << name << "\", ";
    ostr << "\"sizeBound\": " << plane->sizeBound << ", ";
    ostr << "\"initialized\": " << plane->initialized << ", ";
    ostr << "\"symbolic\": " << plane->symbolic << ", ";
    ostr << "\"initialValue\": " << (int)plane->initialValue << ",\n";

    ostr << "        \"bytes\": [\n";
    for (unsigned i = 0; i < plane->sizeBound; ++i) {
      ostr << "          {";
      ostr << "\"concrete\": " << plane->isByteConcrete(i) << ", ";
      ostr << "\"knownSym\": " << plane->isByteKnownSymbolic(i) << ", ";
      ostr << "\"unflushed\": " << plane->isByteUnflushed(i) << ", ";
      ostr << "\"value\": " << "\"" << expr2str(plane->read8(i)) << "\"";
      ostr << "}" << (i + 1U < plane->sizeBound ? ",\n" : "\n");
    }
    ostr << "        ],\n";

    ostr << "        \"updates\": [\n";
    for (const auto *un = plane->getUpdateList().head.get(); un; ) {
      ostr << "          {";
      ostr << "\"" << expr2str(un->index) << "\"" << " : " << "\"" << expr2str(un->value) << "\"";
      un = un->next.get();
      ostr << "}" << (un != nullptr ? ",\n" : "\n");
    }
    ostr << "        ]\n";
  }

  ProgressRecorderLong& ProgressRecorderLong::instance() {
    static ProgressRecorderLong rec;
    return rec;
  }

  static std::string replaceFileExtension(const std::string& fileName, const std::string& extension) {
    size_t dotPos = fileName.rfind('.');
    return fileName.substr(0, dotPos) + extension;
  }
  
  static void copyFile(const std::string& originalFile, const std::string& copyFile) {
    std::ifstream original(originalFile);
    if (original){
      std::ofstream copy(copyFile);
      if (copy){
        copy << original.rdbuf();
      }
    }
  }

  const std::string ProgressRecorderLong::rootDirName{ "__JetKleeProgressRecordingLong__" };

  ProgressRecorderLong::ProgressRecorderLong()
    : rootOutputDir{}

    , roundCounter{ 0 }

    , nodeCounter{ 0 }
    , nodeIDs{ { nullptr, 0 } }

    , stateCounter{ 0 }
    , stateIDs{ { nullptr, 0 } }

    , roundActions{}
  {}

  bool ProgressRecorderLong::start(const std::string &underDir,
                               std::string fileName) {
    if (!createDir(underDir))
        return false;
    rootOutputDir = underDir;

    std::string command = "llvm-dis " + fileName + " -o " + rootOutputDir + "/source.ll";
    std::system(command.c_str());

    const std::string cFile = replaceFileExtension(fileName, ".c");
    const std::string iFile = replaceFileExtension(fileName, ".i");
    copyFile(cFile, rootOutputDir + "/source.c");
    copyFile(iFile, rootOutputDir + "/source.i");

    return true;
  }

  void ProgressRecorderLong::stop() {
    rootOutputDir.clear();
  }

  bool ProgressRecorderLong::started() const {
    return !rootOutputDir.empty();
  }

  void ProgressRecorderLong::onRoundBegin() {
    ++roundCounter;
  }

  void ProgressRecorderLong::onRoundEnd() {
    if (roundActions.empty()) return;
    std::string pathName = rootOutputDir + "/" + std::to_string(roundCounter) + ".json";
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

  void ProgressRecorderLong::onInsertNode(const PTreeNode *const node) {
    int nodeID = ++nodeCounter;
    auto nr = nodeIDs.insert({ node, nodeID });
    assert(nr.second);
    bool uniqueState{ false };
    auto sit = stateIDs.find(node->state);
    if (sit == stateIDs.end()) {
        sit = stateIDs.insert({ node->state, ++stateCounter }).first;
        uniqueState = true;
    }
    roundActions.push_back(std::make_unique<InsertNode>(node, nr.first->second, sit->second, uniqueState));
  }

  void ProgressRecorderLong::onInsertEdge(const PTreeNode *const parent, const PTreeNode *const child, const uint8_t tag) {
    roundActions.push_back(std::make_unique<InsertEdge>(nodeIDs.at(parent), nodeIDs.at(child), tag));
  }

  void ProgressRecorderLong::onEraseNode(const PTreeNode *const node) {
    roundActions.push_back(std::make_unique<EraseNode>(nodeIDs.at(node)));
    nodeIDs.erase(node);
  }

  void ProgressRecorderLong::InsertNode::toJson(std::ostream& ostr) {
    ostr << "    \"action\": \"InsertNode\", ";
    ostr << "\"nodeID\": " << nodeID << ", ";
    ostr << "\"stateID\": " << stateID << ", ";
    ostr << "\"uniqueState\": " << uniqueState << ", ";
    const InstructionInfo *const instrInfo{ node->state->prevPC->info };
    ostr << "\"location\": ["
         << "\"" << instrInfo->file << "\","
         << instrInfo->line << ","
         << instrInfo->column << ","
         << instrInfo->assemblyLine << "], ";
    const InstructionInfo *const nextInstrInfo{ node->state->pc->info };
    ostr << "\"nextLocation\": ["
         << "\"" << nextInstrInfo->file << "\","
         << nextInstrInfo->line << ","
         << nextInstrInfo->column << ","
         << nextInstrInfo->assemblyLine << "], ";
    ostr << "\"incomingBBIndex\": " << node->state->incomingBBIndex << ", ";
    ostr << "\"depth\": " << node->state->depth << ", ";
    ostr << "\"coveredNew\": " << node->state->coveredNew << ", ";
    ostr << "\"forkDisabled\": " << node->state->forkDisabled << ", ";
    ostr << "\"instsSinceCovNew\": " << node->state->instsSinceCovNew << ", ";
    ostr << "\"nextID\": " << node->state->nextID << ", ";
    ostr << "\"steppedInstructions\": " << node->state->steppedInstructions << ", ";
    if (node->state->unwindingInformation != nullptr){
      ostr << "\"unwindingKind\": " << (node->state->unwindingInformation->getKind() == UnwindingInformation::Kind::SearchPhase ? "\"SearchPhase\"" : "\"CleanupPhase\"") << ", ";
      ostr << "\"unwindingException\": \"" << expr2str(node->state->unwindingInformation->exceptionObject) << "\", ";
    }

    stack2json(ostr, node->state->stack);
    constraints2json(ostr, node->state->constraints);
    
    if (!uniqueState) {
      ostr << "\n";
      return;
    }
    ostr << ",\n";

    ostr << "    \"nondetValues\": [\n";
    for (auto it = node->state->nondetValues.begin(); it != node->state->nondetValues.end(); ) {
      ostr << "      {";
      ostr << "\"value\": \"" << expr2str(it->value.getValue()) << "\", ";
      ostr << "\"segment\": " << expr2str(it->value.getSegment()) << ", ";
      ostr << "\"isSigned\": " << it->isSigned << ", ";
      ostr << "\"name\": \"" << it->name << "\"";
      ostr << "}";
      ++it;
      ostr << (it != node->state->nondetValues.end() ? ",\n" : "\n");
    }
    ostr << "    ], \n";

    memoryObjs2json(ostr, node->state->addressSpace.objects);
    ostr << "    \"objectStates\": [\n";
    for (auto it = node->state->addressSpace.objects.begin(); it != node->state->addressSpace.objects.end(); ) {
      ostr << "      {\n";
      ostr << "      \"objID\": " << it->first->id << ", ";
      ostr << "\"copyOnWriteOwner\": " << it->second->copyOnWriteOwner << ", ";
      ostr << "\"readOnly\": " << it->second->readOnly << ",\n";
      ostr << "      \"segmentPlane\": { ";
      plane2json(ostr, it->second->segmentPlane);
      ostr << "\n      },\n";
      ostr << "      \"offsetPlane\": { ";
      plane2json(ostr, it->second->offsetPlane);
      ostr << "      }";
      ostr << "\n      }";
      ++it;
      ostr << (it != node->state->addressSpace.objects.end() ? ",\n" : "\n");
    }
    ostr << "    ],\n";

    ostr << "    \"concreteAddressMap\": [\n";
    for (auto it = node->state->addressSpace.concreteAddressMap.begin(); it != node->state->addressSpace.concreteAddressMap.end(); ) {
      ostr << "      {";
      ostr << "\"address\": " << it->first << ", ";
      ostr << "\"segment\": " << it->second;
      ostr << "}";
      ++it;
      ostr << (it != node->state->addressSpace.concreteAddressMap.end() ? ",\n" : "\n");
    }
    ostr << "    ], \n";

    ostr << "    \"removedObjectsMap\": [\n";
    for (auto it = node->state->addressSpace.removedObjectsMap.begin(); it != node->state->addressSpace.removedObjectsMap.end(); ) {
      ostr << "      {";
      ostr << "\"segment\": " << it->first << "\", ";
      ostr << "\"symbolicArray\": \"" << expr2str(it->second) << "\"";
      ostr << "\n      }";
      ++it;
      ostr << (it != node->state->addressSpace.removedObjectsMap.end() ? ", " : "");
    }
    ostr << "    ], \n";

    ostr << "    \"lazyObjectsMap\": [\n";
    for (auto it = node->state->addressSpace.lazyObjectsMap.begin(); it != node->state->addressSpace.lazyObjectsMap.end(); ) {
      ostr << "      {";
      ostr << "\"pointerSegment\": " << it->first << ", ";
      ostr << "\"offsets\": [";
      for (auto a = it->second.begin(); a != it->second.end(); ) {
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
    for (auto it = node->state->cexPreferences.begin(); it != node->state->cexPreferences.end(); ) {
      ostr << "      \"" << expr2str(*it) << "\"";
      ++it;
      ostr << (it != node->state->cexPreferences.end() ? ", " : "");
    }
    ostr << "    ]\n";
  }

  void ProgressRecorderLong::InsertEdge::toJson(std::ostream& ostr) {
    ostr << "    \"action\": \"InsertEdge\", ";
    ostr << "\"parentID\": " << parentID << ", ";
    ostr << "\"childID\": " << childID << ", ";    
    ostr << "\"tag\": " << (int)tag << "\n";    
  }

  void ProgressRecorderLong::EraseNode::toJson(std::ostream& ostr) {
    ostr << "    \"action\": \"EraseNode\", ";
    ostr << "\"nodeID\": " << ID << "\n";
  }
}