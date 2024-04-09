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

  static void objectInfoToJson(std::ostream& ostr, const ObjectInfo& obj) {
    ostr << "        { ";
    ostr << "\"objID\": " << obj.objID << ", ";
    ostr << "\"segment\": " << obj.segment << ", ";
    ostr << "\"name\": \"" << obj.name << "\", ";
    ostr << "\"size\": \"" << expr2str(obj.size) << "\", ";
    ostr << "\"isLocal\": " << obj.isLocal << ", ";
    ostr << "\"isGlobal\": " << obj.isGlobal << ", ";
    ostr << "\"isFixed\": " << obj.isFixed << ", ";
    ostr << "\"isUserSpecified\": " << obj.isUserSpecified << ", ";
    ostr << "\"isLazyInitialized\": " << obj.isLazyInitialized << ", ";
    ostr << "\"symbolicAddress\": \"" << (obj.symbolicAddress ? expr2str(*obj.symbolicAddress) : "null") << "\" ";
    ostr << "}";
  }

  static void byteInfoToJson(std::ostream& ostr, const ByteInfo& byte) {
      ostr << "            {";
      ostr <<  "\"byteID\": " << byte.byteID << ", ";
      ostr << "\"concrete\": " << byte.isConcrete << ", ";
      ostr << "\"knownSym\": " << byte.isKnownSym << ", ";
      ostr << "\"unflushed\": " << byte.isUnflushed << ", ";
      ostr << "\"value\": " << "\"" << expr2str(byte.value) << "\"";
      ostr << "}";
  }

  static void objectInfoDiffToJson(std::ostream& ostr, const std::vector<ObjectInfo>& parent, const std::vector<ObjectInfo>& child) {
    ostr << "    \"objects\": {\n";
    
    ostr << "      \"add\": [\n";
    size_t i = 0, j = 0;
    while (i < child.size()) {
      if (j >= parent.size() || child[i] < parent[j]) {
        objectInfoToJson(ostr, child[i]);
        i++;
        ostr << (i < child.size() ? ",\n" : "\n");
      }
      else if (child[i] > parent[j]) {
        j++;
      }
      else { // child[i] == parent[j])
          // no change
          i++;
          j++;
      }
    }
    ostr << "      ],\n";

    ostr << "      \"del\": [\n";
    i = 0, j = 0;
    while (j < parent.size()) {
      if (i >= child.size() || parent[j] < child[i]) {
        objectInfoToJson(ostr, parent[j]);
        j++;
        ostr << (j < parent.size() ? ",\n" : "\n");
      }
      else if (parent[j] > child[i]) {
        i++;
      }
      else { // child[i] == parent[j])
          // no change
          i++;
          j++;
      }
    }
    ostr << "      ]\n";

    ostr << "    },\n";
  }

  static void byteInfoDiffToJson(std::ostream& ostr, const std::vector<ByteInfo>& parent, const std::vector<ByteInfo>& child) {
    ostr << "        \"bytes\": {\n";
    
    ostr << "          \"add\": [\n";
    size_t i = 0, j = 0;
    while (i < child.size()) {
      if (j >= parent.size() || child[i] < parent[j]) {
        byteInfoToJson(ostr, child[i]);
        i++;
        ostr << (i < child.size() ? ",\n" : "\n");
      }
      else if (child[i] > parent[j]) {
        j++;
      }
      else { // child[i] == parent[j])
          // no change
          i++;
          j++;
      }
    }
    ostr << "          ],\n";

    ostr << "          \"del\": [\n";
    i = 0, j = 0;
    while (j < parent.size()) {
      if (i >= child.size() || parent[j] < child[i]) {
        byteInfoToJson(ostr, parent[j]);
        j++;
        ostr << (j < parent.size() ? ",\n" : "\n");
      }
      else if (parent[j] > child[i]) {
        i++;
      }
      else { // child[i] == parent[j])
          // no change
          i++;
          j++;
      }
    }
    ostr << "          ]\n";

    ostr << "        },\n";
  }

  static void plane2json(std::ostream& ostr, const ObjectStatePlane *const plane, int id, std::vector<ByteInfo> parentBytes, std::vector<ByteInfo> bytes) {
    if (plane == nullptr) return;

    ostr << "\"memoryObjectID\": " << id << ", ";
    
    std::string name = plane->getUpdateList().root ? plane->getUpdateList().root->getName() : "";
    ostr << "\"rootObject\": " << "\"" << name << "\", ";
    ostr << "\"sizeBound\": " << plane->sizeBound << ", ";
    ostr << "\"initialized\": " << plane->initialized << ", ";
    ostr << "\"symbolic\": " << plane->symbolic << ", ";
    ostr << "\"initialValue\": " << (int)plane->initialValue << ",\n";

    byteInfoDiffToJson(ostr, parentBytes, bytes);

    ostr << "      \"updates\": [\n";
    for (const auto *un = plane->getUpdateList().head.get(); un; ) {
      ostr << "        {";
      ostr << "\"" << expr2str(un->index) << "\"" << " : " << "\"" << expr2str(un->value) << "\"";
      un = un->next.get();
      ostr << "}" << (un != nullptr ? ",\n" : "\n");
    }
    ostr << "      ]\n";
  }

  ProgressRecorder& ProgressRecorder::instance() {
    static ProgressRecorder rec;
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

  std::vector<ObjectInfo> getObjectInfo(const MemoryMap& objects) {
    std::vector<ObjectInfo> objectsInfo;

    for (auto it = objects.begin(); it != objects.end(); ) {
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
      ++it;
      objectsInfo.push_back(info);
    }
    return objectsInfo;
  }

  std::vector<ByteInfo> getByteInfo(const ObjectStatePlane *const plane) {
    std::vector<ByteInfo> byteInfo;
    
    for (unsigned i = 0; i < plane->sizeBound; ++i) {
      ByteInfo byte;
      byte.byteID = i;
      byte.isConcrete = plane->isByteConcrete(i);
      byte.isKnownSym = plane->isByteKnownSymbolic(i);
      byte.isUnflushed = plane->isByteUnflushed(i);
      byte.value = plane->read8(i);
      byteInfo.push_back(byte);
    }
    return byteInfo;
  }


  bool ProgressRecorder::start(const std::string &underDir,
                               std::string fileName) {
    if (!createDir(underDir))
        return false;
    rootOutputDir = underDir;

    char bcFilePath[PATH_MAX];
    if (realpath(fileName.c_str(), bcFilePath)){
      std::string bcFilePathStr(bcFilePath);
      std::string command = "llvm-dis " + bcFilePathStr + " -o " + rootOutputDir + "/source.ll";
      std::system(command.c_str());
    }

    const std::string cFile = replaceFileExtension(fileName, ".c");
    const std::string iFile = replaceFileExtension(fileName, ".i");
    copyFile(cFile, rootOutputDir + "/source.c");
    copyFile(iFile, rootOutputDir + "/source.i");

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
    int nodeID = ++nodeCounter;
    auto nr = nodeIDs.insert({ node, nodeID });
    assert(nr.second);
    bool uniqueState{ false };
    auto sit = stateIDs.find(node->state);
    if (sit == stateIDs.end()) {
        sit = stateIDs.insert({ node->state, ++stateCounter }).first;
        uniqueState = true;
    }
    nodeJSONs.insert({ nodeID, std::max(1, roundCounter)});
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
    int parentID = instance().nodeIDs.at(node->parent);

    ostr << "    \"action\": \"InsertNode\", ";
    ostr << "\"nodeID\": " << nodeID << ", ";
    ostr << "\"stateID\": " << stateID << ", ";
    ostr << "\"uniqueState\": " << uniqueState << ", ";
    ostr << "\"parentID\": " << parentID << ", ";
    ostr << "\"parentJSON\": " << (parentID == 0 ? 0 : instance().nodeJSONs.at(parentID)) << ", ";

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

    // memoryObjs2json(ostr, node->state->addressSpace.objects);

    instance().accessCount[parentID] += 1;
    std::vector<ObjectInfo> nodeInfo = getObjectInfo(node->state->addressSpace.objects);
    instance().objectStates.insert({ nodeID, nodeInfo });
    std::vector<ObjectInfo> parentInfo;

    if (parentID != 0){
      parentInfo = instance().objectStates.at(parentID);
    }
    objectInfoDiffToJson(ostr, parentInfo, nodeInfo);

    ostr << "    \"objectStates\": [\n";
    for (auto it = node->state->addressSpace.objects.begin(); it != node->state->addressSpace.objects.end(); ) {
      ostr << "      {\n";
      ostr << "      \"objID\": " << it->first->id << ", ";
      ostr << "\"copyOnWriteOwner\": " << it->second->copyOnWriteOwner << ", ";
      ostr << "\"readOnly\": " << it->second->readOnly << ",\n";

      ostr << "      \"segmentPlane\": { ";
      if (it->second->segmentPlane != nullptr) {
        int id = it->second->segmentPlane->getParent() ? it->second->segmentPlane->getParent()->getObject()->id : -1;
        std::vector<ByteInfo> bytes = getByteInfo(it->second->segmentPlane);
        std::vector<ByteInfo> parentBytes;

        instance().segmentBytes.insert({ {nodeID, id}, bytes });
        auto itBytes = instance().segmentBytes.find({ parentID, id });
        if (itBytes != instance().segmentBytes.end()) {
          parentBytes = itBytes->second;
        }
        plane2json(ostr, it->second->segmentPlane, id, parentBytes, bytes);
      }
      ostr << "\n      },\n";

      ostr << "      \"offsetPlane\": { ";
      if (it->second->offsetPlane != nullptr) {
        int id = it->second->offsetPlane->getParent() ? it->second->offsetPlane->getParent()->getObject()->id : -1;
        std::vector<ByteInfo> bytes = getByteInfo(it->second->offsetPlane);
        std::vector<ByteInfo> parentBytes;

        instance().offsetBytes.insert({ {nodeID, id }, bytes });
        auto itBytes = instance().offsetBytes.find({ parentID, id });
        if (itBytes != instance().offsetBytes.end()) {
          parentBytes = itBytes->second;
        }
        plane2json(ostr, it->second->offsetPlane, id, parentBytes, bytes);
      }
      ostr << "\n      }";
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

    if (instance().accessCount[parentID] == 2) {
      instance().DeleteParentInfo(parentID);
    }
  }

  void ProgressRecorder::DeleteParentInfo(const int parentID) {
    nodeJSONs.erase(parentID);
    accessCount.erase(parentID);
    objectStates.erase(parentID);
    
    for (auto it = segmentBytes.begin(); it != segmentBytes.end(); ) {
      if (it->first.first == parentID) {
          it = instance().segmentBytes.erase(it);
      } else {
          ++it;
      }
    }

    for (auto it = offsetBytes.begin(); it != offsetBytes.end(); ) {
      if (it->first.first == parentID) {
          it = instance().offsetBytes.erase(it);
      } else {
          ++it;
      }
    }
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
