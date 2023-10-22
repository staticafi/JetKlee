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

  static void plane2json(std::ostream& ostr, const ObjectStatePlane *const plane) {
    if (plane == nullptr) return;
    ostr << "\"sizeBound\": " << plane->sizeBound << ", ";
    ostr << "\"initialized\": " << plane->initialized << ", ";
    ostr << "\"symbolic\": " << plane->symbolic << ", ";
    ostr << "\"initialValue\": " << (int)plane->initialValue << " ";
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
 
  bool ProgressRecorder::start(const std::string &underDir) {
    if (!createDir(underDir))
        return false;
    rootOutputDir = underDir;
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
    ostr << "\"stack\": [";
    for (std::size_t i = 0U; i != node->state->stack.size(); ++i)
      if (node->state->stack.at(i).callPathNode->callSite != nullptr) {
        const InstructionInfo *const callerInfo{ node->state->stack.at(i).caller->info };
        ostr << "[\"" << callerInfo->file << "\","
             << callerInfo->line << ","
             << callerInfo->column << ","
             << callerInfo->assemblyLine << "]"
             << (i + 1U < node->state->stack.size() ? "," : "");
      }
    ostr << "],\n";
    ostr << "    \"constraints\": [\n";
    for (auto it = node->state->constraints.begin(); it != node->state->constraints.end(); ++it)
      ostr << "      \"" << expr2str(*it) << "\"" << (std::next(it) != node->state->constraints.end() ? ",\n" : "\n");
    ostr << "    ]";
    if (!uniqueState) {
      ostr << "\n";
      return;
    }
    ostr << ",\n";
    ostr << "    \"objects\": [\n";
    for (auto it = node->state->addressSpace.objects.begin(); it != node->state->addressSpace.objects.end(); ) {
      ostr << "      { ";
      ostr << "\"objID\": " << it->first->id << ", ";
      ostr << "\"segment\": " << it->first->segment << ", ";
      ostr << "\"name\": \"" << it->first->name << "\", ";
      ostr << "\"size\": \"" << expr2str(it->first->size) << "\", ";
      ostr << "\"isLocal\": " << it->first->isLocal << ", ";
      ostr << "\"isGlobal\": " << it->first->isGlobal << ", ";
      ostr << "\"isFixed\": " << it->first->isFixed << ", ";
      ostr << "\"isLazy\": " << it->first->isLazyInitialized << ", ";
      ostr << "\"symAddress\": \"" << (it->first->symbolicAddress ? expr2str(*it->first->symbolicAddress) : "") << "\" ";
      ostr << "}";
      ++it;
      ostr << (it != node->state->addressSpace.objects.end() ? ",\n" : "\n");
    }
    ostr << "    ],\n";
    ostr << "    \"objectStates\": [\n";
    for (auto it = node->state->addressSpace.objects.begin(); it != node->state->addressSpace.objects.end(); ) {
      ostr << "      { ";
      ostr << "\"objID\": " << it->first->id << ", ";
      ostr << "\"copyOnWriteOwner\": " << it->second->copyOnWriteOwner << ", ";
      ostr << "\"readOnly\": " << it->second->readOnly << ", ";
      ostr << "\"segmentPlane\": { ";
      plane2json(ostr, it->second->segmentPlane);
      ostr << "}, ";
      ostr << "\"offsetPlane\": { ";
      plane2json(ostr, it->second->offsetPlane);
      ostr << "}";
      ostr << " }";
      ++it;
      ostr << (it != node->state->addressSpace.objects.end() ? ",\n" : "\n");
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
