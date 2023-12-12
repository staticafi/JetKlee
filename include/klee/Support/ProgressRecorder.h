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

namespace klee {

  class PTreeNode;
  class ExecutionState;
  class InstructionInfo;

  class ProgressRecorder {

    struct Action {
      virtual ~Action() {}
      virtual void toJson(std::ostream& ostr) = 0;
    };

    struct InsertNode : public Action {
      InsertNode(const PTreeNode *node_, int nodeID_, int stateID_, bool uniqueState_)
        : node{ node_ }, nodeID{ nodeID_ }, stateID{ stateID_ }, uniqueState{ uniqueState_ } {}
      void toJson(std::ostream& ostr) override;
      const PTreeNode *node;
      int nodeID;
      int stateID;
      bool uniqueState;
    };

    struct InsertEdge : public Action {
      InsertEdge(const int parentID_, const int childID_, uint8_t tag_)
        : parentID{ parentID_ }, childID{ childID_ }, tag{ tag_ } {}
      void toJson(std::ostream& ostr) override;
      const int parentID;
      const int childID;
      uint8_t tag;
    };

    struct EraseNode : public Action {
      explicit EraseNode(const int ID_) : ID{ ID_ } {}
      void toJson(std::ostream& ostr) override;
      const int ID;
    };

    std::string rootOutputDir;

    int roundCounter;

    int nodeCounter;
    std::unordered_map<const PTreeNode *, int> nodeIDs;

    int stateCounter;
    std::unordered_map<const ExecutionState *, int> stateIDs;

    std::vector<std::unique_ptr<Action> > roundActions;

    ProgressRecorder();
    ProgressRecorder(ProgressRecorder const&) = default;
    ProgressRecorder& operator=(ProgressRecorder const&) = default;

  public:
    static ProgressRecorder& instance();
    static const std::string rootDirName;

    bool start(const std::string &underDir, std::string fileName);
    void stop();
    bool started() const;

    void onRoundBegin();
    void onRoundEnd();

    void onInsertNode(const PTreeNode *node);
    void onInsertEdge(const PTreeNode *parent, const PTreeNode *child, uint8_t tag);
    void onEraseNode(const PTreeNode *node);
  };

  inline ProgressRecorder& recorder() { return ProgressRecorder::instance(); }

}

#endif
