#define DEBUG_TYPE "gc"
#include "hermes/VM/YoungGenNC.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"

#include "hermes/Support/OSCompat.h"
#include "hermes/Support/PerfSection.h"
#include "hermes/VM/AllocResult.h"
#include "hermes/VM/CompactionResult-inline.h"
#include "hermes/VM/CompactionResult.h"
#include "hermes/VM/CompleteMarkState-inline.h"
#include "hermes/VM/FillerCell.h"
#include "hermes/VM/GC.h"
#include "hermes/VM/GCBase-inline.h"
#include "hermes/VM/GCCell-inline.h"
#include "hermes/VM/GCPointer-inline.h"
#include "hermes/VM/HermesValue-inline.h"
#include "hermes/VM/HiddenClass.h"
#include "hermes/VM/YoungGenNC-inline.h"

#include <chrono>

using llvm::dbgs;
using std::chrono::steady_clock;

namespace hermes {
namespace vm {

YoungGen::YoungGen(GenGC *gc, size_t minSize, size_t maxSize, OldGen *nextGen)
    : GCGeneration(gc),
      // The minimum young generation size is 2 pages.
      // Round up the minSize as needed.
      minSize_(adjustSizeWithBounds(
          minSize,
          2 * hermes::oscompat::page_size(),
          AlignedHeapSegment::maxSize())),
      // Round up the maxSize as needed.
      maxSize_(adjustSizeWithBounds(
          maxSize,
          2 * hermes::oscompat::page_size(),
          AlignedHeapSegment::maxSize())),
      nextGen_(nextGen) {
  exchangeActiveSegment(
      {AlignedStorage{&gc_->storageProvider_, "hermes-younggen-segment"},
       this});
  if (!activeSegment())
    gc_->oom();
  resetTrueAllocContext();

  lowLim_ = activeSegment().lowLim();
  hiLim_ = activeSegment().hiLim();

  // Record the initial level, as if we had done a GC before starting.
  didFinishGC();
}

void YoungGen::sweepAndInstallForwardingPointers(
    GC *gc,
    SweepResult *sweepResult) {
  activeSegment().sweepAndInstallForwardingPointers(gc, sweepResult);
}

void YoungGen::updateReferences(
    GC *gc,
    SweepResult::VTablesRemaining &vTables) {
  auto acceptor = getFullMSCUpdateAcceptor(*gc);

  // Update reachable cells with finalizers to their after-compaction location.
  updateFinalizableCellListReferences();

  activeSegment().updateReferences(gc, acceptor.get(), vTables);
}

void YoungGen::compactFinalizableObjectList() {
  size_t numCells = cellsWithFinalizers().size();
  unsigned retainedIndex = 0;
  uint64_t movedExternalMemory = 0;
  for (unsigned i = 0; i < numCells; i++) {
    const auto cell = cellsWithFinalizers().at(i);
    if (!contains(cell)) {
      nextGen_->addToFinalizerList(cell);
      movedExternalMemory += cell->externalMemorySize();
    } else {
      // This may overwrite some with the same value, if retainedIndex
      // is the same as the current implicit index of the for loop,
      // but that's OK.
      cellsWithFinalizers()[retainedIndex] = cell;
      retainedIndex++;
    }
  }
  cellsWithFinalizers().resize(retainedIndex);
  cellsWithFinalizers().shrink_to_fit();

  assert(externalMemory() >= movedExternalMemory);
  debitExternalMemory(movedExternalMemory);
  nextGen_->creditExternalMemory(movedExternalMemory);
}

void YoungGen::recordLevelAfterCompaction(
    CompactionResult::ChunksRemaining &chunks) {
  if (!chunks.hasNext()) {
    activeSegment().resetLevel();
    return;
  }

  auto &chunk = chunks.next();
  assert(
      this == chunk.generation() &&
      "Chunk does not correspond to YoungGen's segment.");

  chunk.recordLevel(&activeSegment());
}

#ifdef HERMES_SLOW_DEBUG
void YoungGen::checkWellFormed(const GC *gc) const {
  uint64_t extSize = 0;
  activeSegment().checkWellFormed(gc, &extSize);
  assert(extSize == externalMemory());
  checkFinalizableObjectsListWellFormed();
}
#endif // HERMES_SLOW_DEBUG

void YoungGen::forAllObjs(const std::function<void(GCCell *)> &callback) {
  trueActiveSegment().forAllObjs(callback);
}

void YoungGen::printStats(llvm::raw_ostream &os, bool trailingComma) const {
  double youngGenSurvivalPct = 0.0;
  if (cumPreBytes_ > 0) {
    youngGenSurvivalPct = 100.0 * static_cast<double>(cumPromotedBytes_) /
        static_cast<double>(cumPreBytes_);
  }

  os << "\t\t\t\"ygMarkOldToYoungTime\": " << markOldToYoungSecs_ << ",\n"
     << "\t\t\t\"ygMarkRootsTime\": " << markRootsSecs_ << ",\n"
     << "\t\t\t\"ygScanTransitiveTime\": " << scanTransitiveSecs_ << ",\n"
     << "\t\t\t\"ygUpdateWeakRefsTime\": " << updateWeakRefsSecs_ << ",\n"
     << "\t\t\t\"ygFinalizersTime\": " << finalizersSecs_ << ",\n"
     << "\t\t\t\"ygSurvivalPct\": " << youngGenSurvivalPct;
  if (trailingComma) {
    os << ",";
  }
  os << "\n";
}

#ifndef NDEBUG
AllocResult
YoungGen::alloc(uint32_t sz, HasFinalizer hasFinalizer, bool fixedSizeAlloc) {
  assert(ownsAllocContext());
  AllocResult res = allocRaw(sz, hasFinalizer);
  if (res.success) {
    return res;
  }
  return allocSlow(sz, hasFinalizer, fixedSizeAlloc);
}
#endif

AllocResult YoungGen::allocSlow(
    uint32_t allocSize,
    HasFinalizer hasFinalizer,
    bool fixedSizeAlloc) {
  // Can we do a worst-case collection of the YoungGen?  That is, does the
  // next generation have sufficient space to let the collection complete
  // if everything survives?
  if (LLVM_LIKELY(nextGen_->ensureFits(usedDirect()))) {
    // There is enough space; do the young-gen collection.
    collect();
    AllocResult res = allocRaw(allocSize, hasFinalizer);
    if (res.success) {
      return res;
    } else if (!fixedSizeAlloc) {
      // Since the collection above evacuated the young generation,
      // the object being allocated must be too large to fit in the
      // young generation.  Try allocating it directly in the old
      // generation (if that was allowed.
      res = nextGen_->allocRaw(allocSize, hasFinalizer);
      if (res.success) {
        return res;
      }
    }
  }
  // Otherwise: either there wasn't enough space in the old generation
  // to allow worst-case evacuation, or the individual allocation was
  // too large to fit in the young-gen, and it also didn't fit in the
  // old gen, or old-gen allocation was not allowed.  Try collecting
  // the old gen, and see if that allows the collection or allocation.
  return fullCollectThenAlloc(allocSize, hasFinalizer, fixedSizeAlloc);
}

AllocResult YoungGen::fullCollectThenAlloc(
    uint32_t allocSize,
    HasFinalizer hasFinalizer,
    bool fixedSizeAlloc) {
  gc_->collect(/* canEffectiveOOM */ true);
  {
    AllocResult res = allocRaw(allocSize, hasFinalizer);
    if (LLVM_LIKELY(res.success)) {
      return res;
    }
  }

  // Try to grow the next gen to allow young-gen collection, if the allocation
  // can fit into the young generation.
  if (allocSize <= sizeDirect() && nextGen_->growToFit(usedDirect())) {
    collect();
    AllocResult res = allocRaw(allocSize, hasFinalizer);
    assert(res.success && "preceding test should guarantee success.");
    return res;
  }

  // The allocation is not going to fit into the young generation, if it is not
  // a fixed size allocation, try and fit it into the old generation.
  if (!fixedSizeAlloc) {
    if (nextGen_->growToFit(allocSize)) {
      AllocResult res = nextGen_->allocRaw(allocSize, hasFinalizer);
      assert(res.success && "preceding test should guarantee success.");
      return res;
    }
  }

  // We did everything we could, bail.
  gc_->oom();
}

void YoungGen::collect() {
  GenGC::CollectionSection ygCollection(gc_, "YoungGen collection");

  // Reset the number of consecutive full GCs, because we're about to do a young
  // gen collection.
  gc_->consecFullGCs_ = 0;

// Reset the number of reachable and finalized objects for the young gen.
#ifndef NDEBUG
  resetNumReachableObjects();
  resetNumAllHiddenClasses();

  // Remember the number of allocated objects, to compute the number collected.
  unsigned numAllocatedObjectsBefore = gc_->computeNumAllocatedObjects();
#endif
  // Track the sum of the total pre-collection sizes of the young gens.
  size_t youngGenUsedBefore = usedDirect();
  ygCollection.addArg("ygUsedBefore", youngGenUsedBefore);
  ygCollection.addArg("ogUsedBefore", nextGen_->used());
  ygCollection.addArg("ogSize", nextGen_->size());

  size_t oldGenUsedBefore = nextGen_->used();
  cumPreBytes_ += youngGenUsedBefore;

  DEBUG(
      dbgs() << "\nStarting (young-gen, " << formatSize(sizeDirect())
             << ") garbage collection; collection # " << gc_->numGCs() << "\n");

  // Remember the point in the older generation into which we started
  // promoting objects.
  OldGen::Location toScan = nextGen_->levelDirect();

  // We do this first, before marking from the roots, so that we can take
  // a "snapshot" of the level of the old gen, and only iterate over pointers
  // in old-gen objects allocated at the start of the collection.
  auto markOldToYoungStart = steady_clock::now();
  {
    PerfSection ygMarkOldToYoungSystraceRegion("ygMarkOldToYoung");
    nextGen_->markYoungGenPointers(toScan);
  }

  auto markRootsStart = steady_clock::now();
  EvacAcceptor acceptor(*gc_, *this);
  DroppingAcceptor<EvacAcceptor> nameAcceptor{acceptor};
  {
    PerfSection ygMarkRootsSystraceRegion("ygMarkRoots");
    gc_->markRoots(nameAcceptor, /*markLongLived*/ false);
  }

  auto scanTransitiveStart = steady_clock::now();
  {
    PerfSection ygScanTransitiveSystraceRegion("ygScanTransitive");
    nextGen_->youngGenTransitiveClosure(toScan, acceptor);
  }

  // We've now determined reachability; find weak refs to young-gen
  // pointers that have become unreachable.
  auto updateWeakRefsStart = steady_clock::now();
  {
    PerfSection ygUpdateWeakRefsSystraceRegion("ygUpdateWeakRefs");
    gc_->updateWeakReferences(/*fullGC*/ false);
  }

  // Call the finalizers of unreachable objects. Assumes all cells that survived
  // the young gen collection are moved to the old gen collection.
  auto finalizersStart = steady_clock::now();
  {
    PerfSection ygFinalizeSystraceRegion("ygFinalize");
    finalizeUnreachableAndTransferReachableObjects();
  }
  auto finalizersEnd = steady_clock::now();

  // Restart allocation at the bottom of the space.
  activeSegment().resetLevel();

#ifndef NDEBUG
  // Update statistics:

  // Update the "last-gc" stats for the heap as a whole.

  // At this point, all young-gen objects that were reachable have
  // been moved to the old generation, and we're considering the
  // objects already in the old generation to be reachable, so the
  // total number of reachable objects is just the old-gen allocated
  // objects.
  gc_->recordNumReachableObjects(nextGen_->numAllocatedObjects());

  // The hidden classes found reachable in the young gen were moved to
  // the old gen; move the stat, and record it in the GCBase variable.
  nextGen_->incNumHiddenClasses(numHiddenClasses_);
  nextGen_->incNumLeafHiddenClasses(numLeafHiddenClasses_);
  gc_->recordNumHiddenClasses(
      nextGen_->numHiddenClasses(), nextGen_->numLeafHiddenClasses());

  // Record the number of collected objects.
  gc_->recordNumCollectedObjects(
      numAllocatedObjectsBefore - gc_->numReachableObjects_);
  // Only objects in the young generation were finalized, so we set
  // the heap number to the young-gen's number.
  gc_->recordNumFinalizedObjects(numFinalizedObjects_);

  // Space is free; reset num allocated, reachable.
  resetNumAllocatedObjects();
  resetNumReachableObjects();
  resetNumAllHiddenClasses();
#endif // !NDEBUG

  ygCollection.recordGCStats(sizeDirect(), &gc_->youngGenCollectionCumStats_);

  markOldToYoungSecs_ +=
      GCBase::clockDiffSeconds(markOldToYoungStart, markRootsStart);
  markRootsSecs_ +=
      GCBase::clockDiffSeconds(markRootsStart, scanTransitiveStart);
  scanTransitiveSecs_ +=
      GCBase::clockDiffSeconds(scanTransitiveStart, updateWeakRefsStart);
  updateWeakRefsSecs_ +=
      GCBase::clockDiffSeconds(updateWeakRefsStart, finalizersStart);
  finalizersSecs_ += GCBase::clockDiffSeconds(finalizersStart, finalizersEnd);
  // Track the bytes of promoted objects.
  size_t promotedBytes = (nextGen_->used() - oldGenUsedBefore);
  cumPromotedBytes_ += promotedBytes;
  ygCollection.addArg("ygPromoted", promotedBytes);
  ygCollection.addArg("ogUsedAfter", nextGen_->used());
  ygCollection.addArg(
      "ygGCNum", gc_->youngGenCollectionCumStats_.numCollections);

#ifdef HERMES_SLOW_DEBUG
  GCBase::DebugHeapInfo info;
  gc_->getDebugHeapInfo(info);
  info.assertInvariants();
  // Assert an additional invariant involving the number of allocated
  // objects before collection.
  assert(
      numAllocatedObjectsBefore - info.numReachableObjects ==
          info.numCollectedObjects &&
      "collected objects computed incorrectly");
#endif
}

void YoungGen::creditExternalMemory(uint32_t size) {
  GCGeneration::creditExternalMemory(size);
  trueActiveSegment().creditExternalMemory(size);
}

void YoungGen::debitExternalMemory(uint32_t size) {
  GCGeneration::debitExternalMemory(size);
  trueActiveSegment().debitExternalMemory(size);
}

void YoungGen::updateEffectiveEndForExternalMemory() {
  assert(ownsAllocContext());
  if (sizeDirect() >= externalMemory()) {
    activeSegment().setEffectiveEnd(activeSegment().end() - externalMemory());
  } else {
    activeSegment().setEffectiveEnd(activeSegment().start());
  }
}

void YoungGen::ensureReferentCopied(HermesValue *hv) {
  assert(hv->isPointer() && "Should only call on pointer HermesValues");
  GCCell *cell = static_cast<GCCell *>(hv->getPointer());
  if (contains(cell)) {
    hv->setInGC(hv->updatePointer(forwardPointer(cell)), gc_);
  }
}

void YoungGen::ensureReferentCopied(GCCell **ptrLoc) {
  GCCell *ptr = *ptrLoc;
  if (contains(ptr)) {
    *ptrLoc = forwardPointer(ptr);
  }
}

GCCell *YoungGen::forwardPointer(GCCell *ptr) {
  assert(contains(ptr));
  GCCell *cell = ptr;

  // If the object has already been forwarded, we return the new location.
  if (cell->hasMarkedForwardingPointer()) {
    return cell->getMarkedForwardingPointer();
  }

  uint32_t size = cell->getAllocatedSize();

  /// The finalizer parameter is always set to no under the assumption that the
  /// reachable cells with finalizers in the finalizer list of Young Gen are
  /// moved to the finalizer list of Old Gen.
  AllocResult res = nextGen_->allocRaw(size, HasFinalizer::No);
  // This assertion is justified because we take pains not to start a
  // young-gen collection unless we can ensure that the worst-case of all
  // data being live can be accommodated in the old generation.
  assert(res.success);
  memcpy(res.ptr, cell, size);
  // We can now consider res.ptr to be a GCCell.
  GCCell *newCell = reinterpret_cast<GCCell *>(res.ptr);
#ifndef NDEBUG
  numReachableObjects_++;
  if (auto *hiddenClass = dyn_vmcast<HiddenClass>(cell)) {
    ++numHiddenClasses_;
    numLeafHiddenClasses_ += hiddenClass->isKnownLeaf();
  }
#endif

  // Store the forwarding pointer in the original object.
  cell->setMarkedForwardingPointer(newCell);

  // Update the source pointer.
  return newCell;
}

void YoungGen::finalizeUnreachableAndTransferReachableObjects() {
  numFinalizedObjects_ = 0;
  for (const auto &cell : cellsWithFinalizers()) {
    if (cell->hasMarkedForwardingPointer()) {
      nextGen_->addToFinalizerList(cell->getMarkedForwardingPointer());
      continue;
    }
    cell->getVT()->finalize(cell, gc_);
    numFinalizedObjects_++;
  }
  cellsWithFinalizers().clear();
  // At this point, we've considered YG objects with external memory (and thus
  // finalizers.) If they were unreachable, their finalizers were run, and the
  // YG's external memory credit was debited.  If they were reachable, their
  // charge remains, but the objects have been promoted to the old gen. so
  // their external memory credit must be transferred.
  nextGen_->creditExternalMemory(externalMemory_);
  externalMemory_ = 0;
  /// Since there is no longer any charged external memory, effectiveEnd_ can
  /// return to end_.
  updateEffectiveEndForExternalMemory();
}

void YoungGen::didFinishGC() {
  assert(ownsAllocContext());
  levelAtEndOfLastGC_ = activeSegment().level();
}

gcheapsize_t YoungGen::bytesAllocatedSinceLastGC() const {
  return trueActiveSegment().level() - levelAtEndOfLastGC_;
}

#ifndef NDEBUG
void YoungGen::forObjsAllocatedSinceGC(
    const std::function<void(GCCell *)> &callback) {
  trueActiveSegment().forObjsInRange(
      callback, levelAtEndOfLastGC_, trueAllocContext_->activeSegment.level());
}
#endif

void YoungGen::moveHeap(GC *gc, ptrdiff_t moveHeapDelta) {
#if 0 // TODO (T25686322) Non-contiguous heap does not support moving the heap.
  ContigAllocGCSpace::moveHeap(gc, moveHeapDelta);
#endif
}

} // namespace vm
} // namespace hermes
