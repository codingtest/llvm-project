//===-------- LoopOptTutorial.cpp - Loop Opt Tutorial Pass ------*- C++ -*-===//
//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// This file contains a small loop pass to be used to illustrate several
/// aspects about writing a loop optimization. It was developed as part of the
/// "Writing a Loop Optimization" tutorial, presented at LLVM Devepeloper's
/// Conference, 2019.
//===----------------------------------------------------------------------===

#include "llvm/Transforms/Scalar/LoopOptTutorial.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/IVDescriptors.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/IR/Function.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

using namespace llvm;

#define DEBUG_TYPE "loop-opt-tutorial"
static const char *VerboseDebug = DEBUG_TYPE "-verbose";

static cl::opt<bool>
    Verify(DEBUG_TYPE "-verify", cl::Hidden,
           cl::desc("Turn on DominatorTree and LoopInfo verification"),
           cl::init(false));

STATISTIC(LoopSplitted, "Loop has been splitted");
STATISTIC(LoopNotSplitted, "Failed to split the loop");
STATISTIC(NotInSimplifiedForm, "Loop not in simplified form");
STATISTIC(UnsafeToClone, "Loop cannot be safely cloned");
STATISTIC(NotUniqueExitingBlock, "Loop doesn't have a unique exiting block");
STATISTIC(NotUniqueExitBlock, "Loop doesn't have a unique exit block");
STATISTIC(NotInnerMostLoop, "Loop is not an innermost loop");

/// Clones a loop \p OrigLoop.  Returns the loop and the blocks in \p
/// Blocks.
/// Updates LoopInfo assuming the loop is dominated by block \p LoopDomBB.
/// Insert the new blocks before block specified in \p Before.
static Loop *myCloneLoopWithPreheader(BasicBlock *Before, BasicBlock *LoopDomBB,
                                      Loop *OrigLoop, ValueToValueMapTy &VMap,
                                      const Twine &NameSuffix, LoopInfo *LI,
                                      SmallVectorImpl<BasicBlock *> &Blocks);

//===----------------------------------------------------------------------===//
// LoopSplit implementation
//

bool LoopSplit::run(Loop &L) const {
  // First analyze the loop and prune invalid candidates.
  if (!isCandidate(L))
    return false;

  // Attempt to split the loop and report the result.
  if (!splitLoop(L))
    reportFailure(L, LoopNotSplitted);

  reportSuccess(L, LoopSplitted);
  return true;
}

bool LoopSplit::isCandidate(const Loop &L) const {
  // Require loops with preheaders and dedicated exits.
  if (!L.isLoopSimplifyForm())
    return reportInvalidCandidate(L, NotInSimplifiedForm);

  // Since we use cloning to split the loop, it has to be safe to clone.
  if (!L.isSafeToClone())
    return reportInvalidCandidate(L, UnsafeToClone);

  // If the loop has multiple exiting blocks, do not split
  if (!L.getExitingBlock())
    return reportInvalidCandidate(L, NotUniqueExitingBlock);

  // If loop has multiple exit blocks, do not split.
  if (!L.getExitBlock())
    return reportInvalidCandidate(L, NotUniqueExitBlock);

  // Only split innermost loops. Thus, if the loop has any children, it cannot
  // be split.
  if (!L.getSubLoops().empty())
    return reportInvalidCandidate(L, NotInnerMostLoop);

  return true;
}

bool LoopSplit::splitLoop(Loop &L) const {
  assert(L.isLoopSimplifyForm() && "Expecting a loop in simplify form");
  assert(L.isSafeToClone() && "Loop is not safe to be cloned");
  assert(L.getSubLoops().empty() && "Expecting a innermost loop");

  const Function &F = *L.getHeader()->getParent();
  LLVM_DEBUG(dbgs() << "Splitting loop " << L.getName() << "\n");

  // Generate the code that computes the split point.
  Instruction *Split =
    computeSplitPoint(L, L.getLoopPreheader()->getTerminator());

  // Split the loop preheader to create an insertion point for the cloned loop.
  BasicBlock *Preheader = L.getLoopPreheader();
  BasicBlock *Pred = Preheader;
  BasicBlock *InsertBefore =
      SplitBlock(Preheader, Preheader->getTerminator(), &DT, &LI);
  DEBUG_WITH_TYPE(VerboseDebug,
                  dumpFunction("After splitting preheader:\n", F););

  // Clone the original loop, and insert the clone before the original loop.
  Loop *ClonedLoop = cloneLoop(L, *InsertBefore, *Pred);

  // Modify the upper bound of the cloned loop.
  ICmpInst *LatchCmpInst = getLatchCmpInst(*ClonedLoop);
  assert(LatchCmpInst && "Unable to find the latch comparison instruction");
  LatchCmpInst->setOperand(1, Split);

  // Modify the lower bound of the original loop.
  PHINode *IndVar = L.getInductionVariable(SE);
  assert(IndVar && "Unable to find the induction variable PHI node");
  IndVar->setIncomingValueForBlock(L.getLoopPreheader(), Split);

  DEBUG_WITH_TYPE(VerboseDebug,
                  dumpFunction("After splitting the loop:\n", F););

  return true;
}

Loop *LoopSplit::cloneLoop(Loop &L, BasicBlock &InsertBefore,
                           BasicBlock &Pred) const {
  // Clone the original loop, insert the clone before the "InsertBefore" BB.
  const Function &F = *L.getHeader()->getParent();
  SmallVector<BasicBlock *, 4> ClonedLoopBlocks;
  ValueToValueMapTy VMap;

  // Same as cloneLoopWithPreheader but does not update the dominator tree.
  // Use for education purposes only, use cloneLoopWithPreheader in production
  // code.
  Loop *NewLoop = myCloneLoopWithPreheader(&InsertBefore, &Pred, &L, VMap,
                                           "", &LI, ClonedLoopBlocks);

  assert(NewLoop && "Run ot of memory");
  DEBUG_WITH_TYPE(VerboseDebug,
                  dbgs() << "Create new loop: " << NewLoop->getName() << "\n";
                  dumpFunction("After cloning loop:\n", F););

  // Update instructions referencing the original loop basic blocks to
  // reference the corresponding block in the cloned loop.
  VMap[L.getExitBlock()] = &InsertBefore;
  remapInstructionsInBlocks(ClonedLoopBlocks, VMap);
  DEBUG_WITH_TYPE(VerboseDebug,
                  dumpFunction("After instruction remapping:\n", F););

  // Make the predecessor of original loop jump to the cloned loop.
  Pred.getTerminator()->replaceUsesOfWith(&InsertBefore,
                                          NewLoop->getLoopPreheader());


  // Now that we have cloned the loop we need to update the dominator tree.
  updateDominatorTree(L, *NewLoop, InsertBefore, Pred, VMap);

  // Verify that the dominator tree and the loops are correct.
  if (Verify) {
    assert(DT.verify(DominatorTree::VerificationLevel::Fast) &&
           "Dominator tree is invalid");

    L.verifyLoop();
    NewLoop->verifyLoop();
    if (L.getParentLoop())
      L.getParentLoop()->verifyLoop();

    LI.verify(DT);
  }

  return NewLoop;
}

Instruction *LoopSplit::computeSplitPoint(const Loop &L,
                                          Instruction *InsertBefore) const {
  Optional<Loop::LoopBounds> Bounds = L.getBounds(SE);
  assert(Bounds.hasValue() && "Unable to retrieve the loop bounds");

  Value &IVInitialVal = Bounds->getInitialIVValue();
  Value &IVFinalVal = Bounds->getFinalIVValue();
  auto *Sub =
    BinaryOperator::Create(Instruction::Sub, &IVFinalVal, &IVInitialVal, "", InsertBefore);

  return BinaryOperator::Create(Instruction::UDiv, Sub,
                                ConstantInt::get(IVFinalVal.getType(), 2),
                                "", InsertBefore);
}

ICmpInst *LoopSplit::getLatchCmpInst(const Loop &L) const {
  if (BasicBlock *Latch = L.getLoopLatch())
    if (BranchInst *BI = dyn_cast_or_null<BranchInst>(Latch->getTerminator()))
      if (BI->isConditional())
        return dyn_cast<ICmpInst>(BI->getCondition());

  return nullptr;
}

void LoopSplit::updateDominatorTree(const Loop &OrigLoop,
                                    const Loop &ClonedLoop,
                                    BasicBlock &InsertBefore,
                                    BasicBlock &Pred,
                                    ValueToValueMapTy &VMap) const {
  // Add the basic block that belongs to the cloned loop we have created to the
  // dominator tree.
  BasicBlock *NewPH = ClonedLoop.getLoopPreheader();
  assert(NewPH && "Expecting a valid preheader");

  DT.addNewBlock(NewPH, &Pred);
  for (BasicBlock *BB : ClonedLoop.getBlocks())
    DT.addNewBlock(BB, NewPH);

  // Now update the immediate dominator of the cloned loop blocks.
  for (BasicBlock *BB : OrigLoop.getBlocks()) {
    BasicBlock *IDomBB = DT.getNode(BB)->getIDom()->getBlock();
    DT.changeImmediateDominator(cast<BasicBlock>(VMap[BB]),
                                cast<BasicBlock>(VMap[IDomBB]));
  }

  // The cloned loop exiting block now dominates the original loop.
  DT.changeImmediateDominator(&InsertBefore, ClonedLoop.getExitingBlock());
}

bool LoopSplit::reportInvalidCandidate(const Loop &L, Statistic &Stat) const {
  assert(L.getLoopPreheader() && "Expecting loop with a preheader");
  ++Stat;
  ORE.emit(OptimizationRemarkAnalysis(DEBUG_TYPE, Stat.getName(),
                                      L.getStartLoc(), L.getLoopPreheader())
           << "[" << L.getLoopPreheader()->getParent()->getName() << "]: "
           << "Loop is not a candidate for splitting: " << Stat.getDesc());
  return false;
}

void LoopSplit::reportSuccess(const Loop &L, Statistic &Stat) const {
  assert(L.getLoopPreheader() && "Expecting loop with a preheader");
  ++Stat;
  ORE.emit(OptimizationRemark(DEBUG_TYPE, Stat.getName(), L.getStartLoc(),
                              L.getLoopPreheader())
           << "[" << L.getLoopPreheader()->getParent()->getName()
           << "]: " << Stat.getDesc());
}

void LoopSplit::reportFailure(const Loop &L, Statistic &Stat) const {
  assert(L.getLoopPreheader() && "Expecting loop with a preheader");
  ++Stat;
  ORE.emit(OptimizationRemarkMissed(DEBUG_TYPE, Stat.getName(), L.getStartLoc(),
                                    L.getLoopPreheader())
           << "[" << L.getLoopPreheader()->getParent()->getName()
           << "]: " << Stat.getDesc());
}

void LoopSplit::dumpFunction(const StringRef Msg, const Function &F) const {
  dbgs() << Msg;
  F.dump();
}

/// Clones a loop \p OrigLoop.  Returns the loop and the blocks in \p
/// Blocks.
/// Updates LoopInfo assuming the loop is dominated by block \p LoopDomBB.
/// Insert the new blocks before block specified in \p Before.
static Loop *myCloneLoopWithPreheader(BasicBlock *Before, BasicBlock *LoopDomBB,
                                      Loop *OrigLoop, ValueToValueMapTy &VMap,
                                      const Twine &NameSuffix, LoopInfo *LI,
                                      SmallVectorImpl<BasicBlock *> &Blocks) {
  Function *F = OrigLoop->getHeader()->getParent();
  Loop *ParentLoop = OrigLoop->getParentLoop();
  DenseMap<Loop *, Loop *> LMap;

  Loop *NewLoop = LI->AllocateLoop();
  LMap[OrigLoop] = NewLoop;
  if (ParentLoop)
    ParentLoop->addChildLoop(NewLoop);
  else
    LI->addTopLevelLoop(NewLoop);

  BasicBlock *OrigPH = OrigLoop->getLoopPreheader();
  assert(OrigPH && "No preheader");
  BasicBlock *NewPH = CloneBasicBlock(OrigPH, VMap, NameSuffix, F);
  // To rename the loop PHIs.
  VMap[OrigPH] = NewPH;
  Blocks.push_back(NewPH);

  // Update LoopInfo.
  if (ParentLoop)
    ParentLoop->addBasicBlockToLoop(NewPH, *LI);

  for (Loop *CurLoop : OrigLoop->getLoopsInPreorder()) {
    Loop *&NewLoop = LMap[CurLoop];
    if (!NewLoop) {
      NewLoop = LI->AllocateLoop();

      // Establish the parent/child relationship.
      Loop *OrigParent = CurLoop->getParentLoop();
      assert(OrigParent && "Could not find the original parent loop");
      Loop *NewParentLoop = LMap[OrigParent];
      assert(NewParentLoop && "Could not find the new parent loop");

      NewParentLoop->addChildLoop(NewLoop);
    }
  }

  for (BasicBlock *BB : OrigLoop->getBlocks()) {
    Loop *CurLoop = LI->getLoopFor(BB);
    Loop *&NewLoop = LMap[CurLoop];
    assert(NewLoop && "Expecting new loop to be allocated");

    BasicBlock *NewBB = CloneBasicBlock(BB, VMap, NameSuffix, F);
    VMap[BB] = NewBB;

    // Update LoopInfo.
    NewLoop->addBasicBlockToLoop(NewBB, *LI);
    if (BB == CurLoop->getHeader())
      NewLoop->moveToHeader(NewBB);

    Blocks.push_back(NewBB);
  }

  // Move them physically from the end of the block list.
  F->getBasicBlockList().splice(Before->getIterator(), F->getBasicBlockList(),
                                NewPH);
  F->getBasicBlockList().splice(Before->getIterator(), F->getBasicBlockList(),
                                NewLoop->getHeader()->getIterator(), F->end());

  return NewLoop;
}

//===----------------------------------------------------------------------===//
// LoopOptTutorialPass implementation
//

PreservedAnalyses LoopOptTutorialPass::run(Loop &L, LoopAnalysisManager &LAM,
                                           LoopStandardAnalysisResults &AR,
                                           LPMUpdater &U) {
  // Retrieve a function analysis manager to get a cached OptimizationRemarkEmitter.
  const auto &FAM =
    LAM.getResult<FunctionAnalysisManagerLoopProxy>(L, AR).getManager();
  Function *F = L.getHeader()->getParent();
  auto *ORE = FAM.getCachedResult<OptimizationRemarkEmitterAnalysis>(*F);
  if (!ORE)
    report_fatal_error("OptimizationRemarkEmitterAnalysis was not cached");

  LLVM_DEBUG(dbgs() << "Entering LoopOptTutorialPass::run\n");
  LLVM_DEBUG(dbgs() << "Loop: "; L.dump(); dbgs() << "\n");

  bool Changed = LoopSplit(AR.LI, AR.SE, AR.DT, *ORE).run(L);

  if (!Changed)
    return PreservedAnalyses::all();

  return llvm::getLoopPassPreservedAnalyses();
}
