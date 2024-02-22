//===-- ProgressRecorder.cpp ----------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Support/ProgressRecorder.h"
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

    ostr << "      \"bytes\": [\n";
    for (unsigned i = 0; i < plane->sizeBound; ++i) {
      ostr << "        {";
      ostr << "\"concrete\": " << plane->isByteConcrete(i) << ", ";
      ostr << "\"knownSym\": " << plane->isByteKnownSymbolic(i) << ", ";
      ostr << "\"unflushed\": " << plane->isByteUnflushed(i) << ", ";
      ostr << "\"value\": " << "\"" << expr2str(plane->read8(i)) << "\"";
      ostr << "}" << (i + 1U < plane->sizeBound ? ",\n" : "\n");
    }
    ostr << "      ],\n";

    ostr << "      \"updates\": [\n";
    for (const auto *un = plane->getUpdateList().head.get(); un; ) {
      ostr << "        {";
      ostr << "\"" << expr2str(un->index) << "\"" << " : " << "\"" << expr2str(un->value) << "\"" << "\n";
      un = un->next.get();
      ostr << "        }" << (un != nullptr ? ",\n" : "\n");
    }
    ostr << "      ]\n";
  }

  ProgressRecorder& ProgressRecorder::instance() {
    static ProgressRecorder rec;
    return rec;
  }

  const std::string ProgressRecorder::rootDirName{ "__JetKleeProgressRecording__" };

  ProgressRecorder::ProgressRecorder()
    : rootOutputDir{}

    , roundCounter{ 0 }

    , nodeCounter{ 0 }
    , nodeIDs{ { nullptr, 0 } }

    , stateCounter{ 0 }
    , stateIDs{ { nullptr, 0 } }

    , roundActions{}
  {}
 
  bool ProgressRecorder::start(const std::string &underDir, std::string fileName) {
    if (!createDir(underDir))
        return false;
    rootOutputDir = underDir;

    char bcFilePath[PATH_MAX];
    if (realpath(fileName.c_str(), bcFilePath)){
      std::string bcFilePathStr(bcFilePath);
      std::string command = "llvm-dis " + bcFilePathStr + " -o " + rootOutputDir + "/source.ll";
      std::system(command.c_str());
    }
  
    // file.bc -> file.c
    fileName.erase(fileName.size() - 2, 1);
    char cFilePath[PATH_MAX];
    
    if (realpath(fileName.c_str(), cFilePath)){
      std::ifstream sourceFile(cFilePath);
      std::ofstream destinationFile(rootOutputDir + "/source.c");
      if (sourceFile && destinationFile){
        destinationFile << sourceFile.rdbuf();
      }
    }

    return true;
  }

  void ProgressRecorder::stop() {
    rootOutputDir.clear();
  }

  bool ProgressRecorder::started() const {
    return !rootOutputDir.empty();
  }

  void ProgressRecorder::onRoundBegin() {
    ++roundCounter;
  }

  void ProgressRecorder::onRoundEnd() {
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

  void ProgressRecorder::onInsertNode(const PTreeNode *const node) {
    auto nr = nodeIDs.insert({ node, ++nodeCounter });
    assert(nr.second);
    bool uniqueState{ false };
    auto sit = stateIDs.find(node->state);
    if (sit == stateIDs.end()) {
        sit = stateIDs.insert({ node->state, ++stateCounter }).first;
        uniqueState = true;
    }
    roundActions.push_back(std::make_unique<InsertNode>(node, nr.first->second, sit->second, uniqueState));
  }

  void ProgressRecorder::onInsertEdge(const PTreeNode *const parent, const PTreeNode *const child, const uint8_t tag) {
    roundActions.push_back(std::make_unique<InsertEdge>(nodeIDs.at(parent), nodeIDs.at(child), tag));
  }

  void ProgressRecorder::onEraseNode(const PTreeNode *const node) {
    roundActions.push_back(std::make_unique<EraseNode>(nodeIDs.at(node)));
    nodeIDs.erase(node);
  }

  void ProgressRecorder::InsertNode::toJson(std::ostream& ostr) {
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

    stack2json(ostr, node->state->stack);
    constraints2json(ostr, node->state->constraints);
    
    if (!uniqueState) {
      ostr << "\n";
      return;
    }
    ostr << ",\n";

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
      ostr << "    {";
      ostr << "      \"address\": " << it->first << ", ";
      ostr << "\"segment\": " << it->second;
      ostr << "}";
      ++it;
      ostr << (it != node->state->addressSpace.concreteAddressMap.end() ? ",\n" : "\n");
    }
    ostr << "    ], \n";

    ostr << "    \"removedObjectsMap\": [\n";
    for (auto it = node->state->addressSpace.removedObjectsMap.begin(); it != node->state->addressSpace.removedObjectsMap.end(); ) {
      ostr << "      {\n";
      ostr << "      \"segment\": " << it->first << "\", ";
      ostr << "\"symbolicArray\": \"" << expr2str(it->second) << "\"";
      ostr << "\n      }";
      ++it;
      ostr << (it != node->state->addressSpace.removedObjectsMap.end() ? ", " : "");
    }
    ostr << "    ], \n";

    ostr << "    \"lazyObjectsMap\": [\n";
    for (auto it = node->state->addressSpace.lazyObjectsMap.begin(); it != node->state->addressSpace.lazyObjectsMap.end(); ) {
      ostr << "      {\n";
      ostr << "      \"pointerSegment\": " << it->first << "\", ";
      ostr << "\"offsets\": [\n";
      for (auto a = it->second.begin(); a != it->second.end(); ) {
        ostr << expr2str(*a);
        ++a;
        ostr << (a != it->second.end() ? ", " : "");
      }
      ostr << "    ], \n";
      ostr << "\n      }";
      ++it;
      ostr << (it != node->state->addressSpace.lazyObjectsMap.end() ? ", " : "");
    }
    ostr << "    ], \n";

    ostr << "    \"nondetValues\": [\n";
    for (auto it = node->state->nondetValues.begin(); it != node->state->nondetValues.end(); ) {
      ostr << "      {\n";
      ostr << "        \"value\": \"" << expr2str(it->value.getValue()) << "\", ";
      ostr << "\"offset\": \"" << expr2str(it->value.getOffset()) << "\", ";
      ostr << "\"segment\": \"" << expr2str(it->value.getSegment()) << "\", ";
      ostr << "\"isSigned\": \"" << it->isSigned << "\", ";
      ostr << "\"name\": \"" << it->name << "\"";
      ostr << "\n      }";
      ++it;
      ostr << (it != node->state->nondetValues.end() ? ",\n" : "\n");
    }
    ostr << "    ], \n";

    // ostr << "    \"symbolics\": [\n";
    // for (auto it = node->state->symbolics.begin(); it != node->state->symbolics.end(); ) {
    //   ostr << "\"objID\": " << it->first->id << ", ";
    //   for (auto a = it->second.begin(); a != it->second.end(); ) {
    //     ostr << expr2str(*a);
    //     ++a;
    //     ostr << (a != it->second.end() ? ", " : "");
    //   }
    //   ++it;
    //   ostr << (it != node->state->symbolics.end() ? ",\n" : "\n");
    // }
    // ostr << "    ], \n";

    ostr << "    \"arrayNames\": [\n";
    for (auto it = node->state->arrayNames.begin(); it != node->state->arrayNames.end(); ) {
      ostr << "      \"" << *it << "\"";
      ++it;
      ostr << (it != node->state->arrayNames.end() ? ", " : "");
    }
    ostr << "    ]\n";
  }

  void ProgressRecorder::InsertEdge::toJson(std::ostream& ostr) {
    ostr << "    \"action\": \"InsertEdge\", ";
    ostr << "\"parentID\": " << parentID << ", ";
    ostr << "\"childID\": " << childID << ", ";    
    ostr << "\"tag\": " << (int)tag << "\n";    
  }

  void ProgressRecorder::EraseNode::toJson(std::ostream& ostr) {
    ostr << "    \"action\": \"EraseNode\", ";
    ostr << "\"nodeID\": " << ID << "\n";
  }
}
