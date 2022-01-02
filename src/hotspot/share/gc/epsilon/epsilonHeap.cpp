/*
 * Copyright (c) 2017, 2020, Red Hat, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "gc/epsilon/epsilonHeap.hpp"
#include "gc/epsilon/epsilonInitLogger.hpp"
#include "gc/epsilon/epsilonMemoryPool.hpp"
#include "gc/epsilon/epsilonThreadLocalData.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/locationPrinter.inline.hpp"
#include "memory/allocation.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/metaspaceUtils.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "runtime/atomic.hpp"
#include "runtime/globals.hpp"

// added includes, we're pulling most of them from the shared gc library, which implements like everything already
// these classfiles seem to be something specific to the java implementation
// it's like they're walking the java classes
#include "classfile/classLoaderDataGraph.hpp"
// also seems to be specific to java
#include "code/codeCache.hpp"
// just a debug file used for profiling
#include "gc/shared/gcTraceTime.inline.hpp"
// **IMPORTANT** we use this to keep track of which objects are marked during mark and sweep, by attaching a bit (0 or 1) to each object to determine if they are alive or not
#include "gc/shared/markBitMap.inline.hpp"
// seems to be a mandatory part of the openjdk interface for multiple threads, but since we're using 1 thread it doesn't really matter
#include "gc/shared/strongRootsScope.hpp"
// the worklist of marked objects we use when we're traversing the list
#include "gc/shared/preservedMarks.inline.hpp"
//
#include "gc/shared/oopStorageSet.inline.hpp"
// **IMPORTANT** 1.2.2 object walkers, used for traversing nested objects in java, which we need for mark -sweep and basically any tracing garbage collection algorithm
#include "memory/iterator.inline.hpp"
// compressed oops, responsible for compressing java objects
#include "oops/compressedOops.inline.hpp"
// **IMPORTANT** locks a thread so that GC can do stuff with the marked objects
#include "runtime/biasedLocking.hpp"
// debug information for objects at runtime
#include "runtime/objectMonitor.inline.hpp"
// class responsible for starting multiple threads in java
#include "runtime/thread.hpp"
// "hodgepodge of useful vm functions"
#include "runtime/vmOperations.hpp"
// primary thread for vm, that's responsible for the infinite garbage collection loop and other methods
#include "runtime/vmThread.hpp"
// a debug class to measure vm performance
#include "services/management.hpp"
//
#include "services/memTracker.hpp"
// a simple stack data structure
#include "utilities/stack.inline.hpp"
// **IMPORTANT** the stack, responsible for holding segments and root objects that the gc needs access to in order to traverse the graph
// #include "utilities/stack.inline.hpp"

// #include "classfile/stringTable.hpp"
// #include "classfile/systemDictionary.hpp"
// // the set of barriers, which should be empty for epsilon, because we don't have any write barriers if we're not using generational collectors and not doing parallel
// #include "gc/shared/barrierSet.inline.hpp"
// // scans for root nodes and weak references
// #include "gc/shared/weakProcessor.hpp"
// // **IMPORTANT** marks object `word` for deletion or preservation during GC
// #include "oops/markWord.inline.hpp"

// provides the derived pointer clear
#include "compiler/oopMap.hpp"

jint EpsilonHeap::initialize() {
  size_t align = HeapAlignment;
  size_t init_byte_size = align_up(InitialHeapSize, align);
  size_t max_byte_size  = align_up(MaxHeapSize, align);

  // Initialize backing storage
  ReservedHeapSpace heap_rs = Universe::reserve_heap(max_byte_size, align);
  _virtual_space.initialize(heap_rs, init_byte_size);

  MemRegion committed_region((HeapWord*)_virtual_space.low(),          (HeapWord*)_virtual_space.high());

  initialize_reserved_region(heap_rs);

  _space = new ContiguousSpace();
  _space->initialize(committed_region, /* clear_space = */ true, /* mangle_space = */ true);

  // Precompute hot fields
  _max_tlab_size = MIN2(CollectedHeap::max_tlab_size(), align_object_size(EpsilonMaxTLABSize / HeapWordSize));
  _step_counter_update = MIN2<size_t>(max_byte_size / 16, EpsilonUpdateCountersStep);
  _step_heap_print = (EpsilonPrintHeapSteps == 0) ? SIZE_MAX : (max_byte_size / EpsilonPrintHeapSteps);
  _decay_time_ns = (int64_t) EpsilonTLABDecayTime * NANOSECS_PER_MILLISEC;

  // Enable monitoring
  _monitoring_support = new EpsilonMonitoringSupport(this);
  _last_counter_update = 0;
  _last_heap_print = 0;

  // Install barrier set
  BarrierSet::set_barrier_set(new EpsilonBarrierSet());

  // commit memory, which means that the bitmap is a reserved portion of memory
  // the header file mentioned that it shouldn't be allocated on heap but whatever
   size_t bitmap_page_size = UseLargePages ? (size_t)os::large_page_size() : (size_t)os::vm_page_size();
  // size of the bitmap? how is this not dynamic?
   size_t _bitmap_size = MarkBitMap::compute_size(heap_rs.size());
   _bitmap_size = align_up(_bitmap_size, bitmap_page_size);

  //  Initialize marking bitmap, but not commit it yet
   if (EpsilonMarkCompactGC) {
    //  reserves space the size of the bitmap
     ReservedSpace bitmap(_bitmap_size, bitmap_page_size);
    //  remember that the first bit represents marked bit
     MemTracker::record_virtual_memory_type(bitmap.base(), mtGC);
    // the bitmap region should be the beginning word of each heap
     _bitmap_region = MemRegion((HeapWord *) bitmap.base(), bitmap.size() / HeapWordSize);
    // let the heap region cover the heap word
     MemRegion heap_region = MemRegion((HeapWord *) heap_rs.base(), heap_rs.size() / HeapWordSize);
    // initialize it
     _bitmap.initialize(heap_region, _bitmap_region);
   }

  // All done, print out the configuration
  EpsilonInitLogger::print();

  return JNI_OK;
}

void EpsilonHeap::post_initialize() {
  CollectedHeap::post_initialize();
}

void EpsilonHeap::initialize_serviceability() {
  _pool = new EpsilonMemoryPool(this);
  _memory_manager.add_pool(_pool);
}

GrowableArray<GCMemoryManager*> EpsilonHeap::memory_managers() {
  GrowableArray<GCMemoryManager*> memory_managers(1);
  memory_managers.append(&_memory_manager);
  return memory_managers;
}

GrowableArray<MemoryPool*> EpsilonHeap::memory_pools() {
  GrowableArray<MemoryPool*> memory_pools(1);
  memory_pools.append(_pool);
  return memory_pools;
}

size_t EpsilonHeap::unsafe_max_tlab_alloc(Thread* thr) const {
  // Return max allocatable TLAB size, and let allocation path figure out
  // the actual allocation size. Note: result should be in bytes.
  return _max_tlab_size * HeapWordSize;
}

EpsilonHeap* EpsilonHeap::heap() {
  return named_heap<EpsilonHeap>(CollectedHeap::Epsilon);
}

HeapWord* EpsilonHeap::allocate_work(size_t size) {
  assert(is_object_aligned(size), "Allocation size should be aligned: " SIZE_FORMAT, size);

  HeapWord* res = NULL;
  while (true) {
    // Try to allocate, assume space is available
    res = _space->par_allocate(size);
    if (res != NULL) {
      break;
    }

    // Allocation failed, attempt expansion, and retry:
    {
      MutexLocker ml(Heap_lock);

      // Try to allocate under the lock, assume another thread was able to expand
      res = _space->par_allocate(size);
      if (res != NULL) {
        break;
      }

      // Expand and loop back if space is available
      size_t space_left = max_capacity() - capacity();
      size_t want_space = MAX2(size, EpsilonMinHeapExpand);

      if (want_space < space_left) {
        // Enough space to expand in bulk:
        bool expand = _virtual_space.expand_by(want_space);
        assert(expand, "Should be able to expand");
      } else if (size < space_left) {
        // No space to expand in bulk, and this allocation is still possible,
        // take all the remaining space:
        bool expand = _virtual_space.expand_by(space_left);
        assert(expand, "Should be able to expand");
      } else {
        // No space left:
        return NULL;
      }

      _space->set_end((HeapWord *) _virtual_space.high());
    }
  }

  size_t used = _space->used();

  // Allocation successful, update counters
  {
    size_t last = _last_counter_update;
    if ((used - last >= _step_counter_update) && Atomic::cmpxchg(&_last_counter_update, last, used) == last) {
      _monitoring_support->update_counters();
    }
  }

  // ...and print the occupancy line, if needed
  {
    size_t last = _last_heap_print;
    if ((used - last >= _step_heap_print) && Atomic::cmpxchg(&_last_heap_print, last, used) == last) {
      print_heap_info(used);
      print_metaspace_info();
    }
  }

  assert(is_object_aligned(res), "Object should be aligned: " PTR_FORMAT, p2i(res));
  return res;
}

HeapWord* EpsilonHeap::allocate_new_tlab(size_t min_size,
                                         size_t requested_size,
                                         size_t* actual_size) {
  Thread* thread = Thread::current();

  // Defaults in case elastic paths are not taken
  bool fits = true;
  size_t size = requested_size;
  size_t ergo_tlab = requested_size;
  int64_t time = 0;

  if (EpsilonElasticTLAB) {
    ergo_tlab = EpsilonThreadLocalData::ergo_tlab_size(thread);

    if (EpsilonElasticTLABDecay) {
      int64_t last_time = EpsilonThreadLocalData::last_tlab_time(thread);
      time = (int64_t) os::javaTimeNanos();

      assert(last_time <= time, "time should be monotonic");

      // If the thread had not allocated recently, retract the ergonomic size.
      // This conserves memory when the thread had initial burst of allocations,
      // and then started allocating only sporadically.
      if (last_time != 0 && (time - last_time > _decay_time_ns)) {
        ergo_tlab = 0;
        EpsilonThreadLocalData::set_ergo_tlab_size(thread, 0);
      }
    }

    // If we can fit the allocation under current TLAB size, do so.
    // Otherwise, we want to elastically increase the TLAB size.
    fits = (requested_size <= ergo_tlab);
    if (!fits) {
      size = (size_t) (ergo_tlab * EpsilonTLABElasticity);
    }
  }

  // Always honor boundaries
  size = clamp(size, min_size, _max_tlab_size);

  // Always honor alignment
  size = align_up(size, MinObjAlignment);

  // Check that adjustments did not break local and global invariants
  assert(is_object_aligned(size),
         "Size honors object alignment: " SIZE_FORMAT, size);
  assert(min_size <= size,
         "Size honors min size: "  SIZE_FORMAT " <= " SIZE_FORMAT, min_size, size);
  assert(size <= _max_tlab_size,
         "Size honors max size: "  SIZE_FORMAT " <= " SIZE_FORMAT, size, _max_tlab_size);
  assert(size <= CollectedHeap::max_tlab_size(),
         "Size honors global max size: "  SIZE_FORMAT " <= " SIZE_FORMAT, size, CollectedHeap::max_tlab_size());

  if (log_is_enabled(Trace, gc)) {
    ResourceMark rm;
    log_trace(gc)("TLAB size for \"%s\" (Requested: " SIZE_FORMAT "K, Min: " SIZE_FORMAT
                          "K, Max: " SIZE_FORMAT "K, Ergo: " SIZE_FORMAT "K) -> " SIZE_FORMAT "K",
                  thread->name(),
                  requested_size * HeapWordSize / K,
                  min_size * HeapWordSize / K,
                  _max_tlab_size * HeapWordSize / K,
                  ergo_tlab * HeapWordSize / K,
                  size * HeapWordSize / K);
  }

  // All prepared, let's do it!
  // Remember how gcs work, allocate memory, and if the heap is filled, we have to run a collect on it
  HeapWord* res = allocate_or_collect_work(size);

  if (res != NULL) {
    // Allocation successful
    *actual_size = size;
    if (EpsilonElasticTLABDecay) {
      EpsilonThreadLocalData::set_last_tlab_time(thread, time);
    }
    if (EpsilonElasticTLAB && !fits) {
      // If we requested expansion, this is our new ergonomic TLAB size
      EpsilonThreadLocalData::set_ergo_tlab_size(thread, size);
    }
  } else {
    // Allocation failed, reset ergonomics to try and fit smaller TLABs
    if (EpsilonElasticTLAB) {
      EpsilonThreadLocalData::set_ergo_tlab_size(thread, 0);
    }
  }

  return res;
}

HeapWord* EpsilonHeap::mem_allocate(size_t size, bool *gc_overhead_limit_was_exceeded) {
  *gc_overhead_limit_was_exceeded = false;
  return allocate_work(size);
}

// `cause` is the reason that we're running gc.
// usually it's because we're out of memory on the heap, but it can also be due to other reasons, such as debugging
void EpsilonHeap::collect(GCCause::Cause cause) {
  switch (cause) {
    case GCCause::_metadata_GC_threshold:
    case GCCause::_metadata_GC_clear_soft_refs:
      // Receiving these causes means the VM itself entered the safepoint for metadata collection.
      // While Epsilon does not do GC, it has to perform sizing adjustments, otherwise we would
      // re-enter the safepoint again very soon.

      assert(SafepointSynchronize::is_at_safepoint(), "Expected at safepoint");
      log_info(gc)("GC request for \"%s\" is handled", GCCause::to_string(cause));
      MetaspaceGC::compute_new_size();
      print_metaspace_info();
      break;
    default:
    if(EpsilonMarkCompactGC) {
      if (SafepointSynchronize::is_at_safepoint()) {
        entry_collect(cause);
      } else {
        vmentry_collect(cause);
      }
    } else {
      log_info(gc)("GC request for \"%s\" is ignored", GCCause::to_string(cause));
    }
  }
  _monitoring_support->update_counters();
}

void EpsilonHeap::do_full_collection(bool clear_all_soft_refs) {
  collect(gc_cause());
}

void EpsilonHeap::object_iterate(ObjectClosure *cl) {
  _space->object_iterate(cl);
}

void EpsilonHeap::print_on(outputStream *st) const {
  st->print_cr("Epsilon Heap");

  // Cast away constness:
  ((VirtualSpace)_virtual_space).print_on(st);

  if (_space != NULL) {
    st->print_cr("Allocation space:");
    _space->print_on(st);
  }

  MetaspaceUtils::print_on(st);
}

bool EpsilonHeap::print_location(outputStream* st, void* addr) const {
  return BlockLocationPrinter<EpsilonHeap>::print_location(st, addr);
}

void EpsilonHeap::print_tracing_info() const {
  print_heap_info(used());
  print_metaspace_info();
}

void EpsilonHeap::print_heap_info(size_t used) const {
  size_t reserved  = max_capacity();
  size_t committed = capacity();

  if (reserved != 0) {
    log_info(gc)("Heap: " SIZE_FORMAT "%s reserved, " SIZE_FORMAT "%s (%.2f%%) committed, "
                 SIZE_FORMAT "%s (%.2f%%) used",
            byte_size_in_proper_unit(reserved),  proper_unit_for_byte_size(reserved),
            byte_size_in_proper_unit(committed), proper_unit_for_byte_size(committed),
            committed * 100.0 / reserved,
            byte_size_in_proper_unit(used),      proper_unit_for_byte_size(used),
            used * 100.0 / reserved);
  } else {
    log_info(gc)("Heap: no reliable data");
  }
}

void EpsilonHeap::print_metaspace_info() const {
  MetaspaceCombinedStats stats = MetaspaceUtils::get_combined_statistics();
  size_t reserved  = stats.reserved();
  size_t committed = stats.committed();
  size_t used      = stats.used();

  if (reserved != 0) {
    log_info(gc, metaspace)("Metaspace: " SIZE_FORMAT "%s reserved, " SIZE_FORMAT "%s (%.2f%%) committed, "
                            SIZE_FORMAT "%s (%.2f%%) used",
            byte_size_in_proper_unit(reserved),  proper_unit_for_byte_size(reserved),
            byte_size_in_proper_unit(committed), proper_unit_for_byte_size(committed),
            committed * 100.0 / reserved,
            byte_size_in_proper_unit(used),      proper_unit_for_byte_size(used),
            used * 100.0 / reserved);
  } else {
    log_info(gc, metaspace)("Metaspace: no reliable data");
  }
}




// EPSILON MARK SWEEP
// START OF MODIFIED CODE
// CODE FROM Shiplev @ https://shipilev.net/jvm/diy-gc/, I modified it to add comments and make it
// compatible with the latest version

// ------------------ EXPERIMENTAL MARK-COMPACT -------------------------------
//
// This implements a trivial Lisp2-style sliding collector:
//     https://en.wikipedia.org/wiki/Mark-compact_algorithm#LISP2_algorithm
//
// The goal for this implementation is to be as simple as possible, ignoring
// non-trivial performance optimizations. This collector does not implement
// reference processing: no soft/weak/phantom/finalizeable references are ever
// cleared. It also does not implement class unloading and other runtime
// cleanups.
//

// VM operation that executes collection cycle under safepoint
class VM_EpsilonCollect: public VM_Operation {
private:
  const GCCause::Cause _cause;
  EpsilonHeap* const _heap;
  static size_t _last_used;
public:
  VM_EpsilonCollect(GCCause::Cause cause) : VM_Operation(),
                                            _cause(cause),
                                            _heap(EpsilonHeap::heap()) {};

  VM_Operation::VMOp_Type type() const { return VMOp_EpsilonCollect; }
  const char* name()             const { return "Epsilon Collection"; }

  virtual bool doit_prologue() {
    // Need to take the Heap lock before managing backing storage.
    // This also naturally serializes GC requests, and allows us to coalesce
    // back-to-back allocation failure requests from many threads. There is no
    // need to handle allocation failure that comes without allocations since
    // last complete GC. Waiting for 1% of heap allocated before starting next
    // GC seems to resolve most races.
    // obtain the lock for the heap
    Heap_lock->lock();
    // calculate the amount of memory that is taken up
    size_t used = _heap->used();
    // calcualte the amount of memory total
    size_t capacity = _heap->capacity();
    // if we've used more, then record that amount as allocated
    size_t allocated = used > _last_used ? used - _last_used : 0;
    // if there was an allocation failure, or if we've used at least 1% more of heap we can gc
    if (_cause != GCCause::_allocation_failure || allocated > capacity / 100) {
      return true;
    } else {
      Heap_lock->unlock();
      return false;
    }
  }

  virtual void doit() {
    _heap->entry_collect(_cause);
  }

  virtual void doit_epilogue() {
    _last_used = _heap->used();
    Heap_lock->unlock();
  }
};

size_t VM_EpsilonCollect::_last_used = 0;

void EpsilonHeap::vmentry_collect(GCCause::Cause cause) {
  VM_EpsilonCollect vmop(cause);
  VMThread::execute(&vmop);
}

HeapWord* EpsilonHeap::allocate_or_collect_work(size_t size) {
  HeapWord* res = allocate_work(size);
  if (res == NULL && EpsilonMarkCompactGC) {
    vmentry_collect(GCCause::_allocation_failure);
    res = allocate_work(size);
  }
  return res;
}

typedef Stack<oop, mtGC> EpsilonMarkStack;

void EpsilonHeap::do_roots(OopClosure* cl, bool everything) {
  // Need to tell runtime we are about to walk the roots with 1 thread
  StrongRootsScope scope(1);

  // Need to adapt oop closure for some special root types.
  CLDToOopClosure clds(cl, ClassLoaderData::_claim_none);
  MarkingCodeBlobClosure blobs(cl, CodeBlobToOopClosure::FixRelocations);

  // Strong roots: always reachable roots

  // General strong roots that are registered in OopStorages
  // for (OopStorageSet::<Iterator> it = OopStorageSet::strong_iterator(); !it.is_end(); ++it) {
  //   (*it)->oops_do(cl);
  // }
  // checkout `roots_do` functions for shenandoah. This is the new api for iterating over strong roots
  for (auto id : EnumRange<OopStorageSet::StrongId>()) {
     OopStorageSet::storage(id)->oops_do(cl);
   }

  // Subsystems that still have their own root handling
  ClassLoaderDataGraph::cld_do(&clds);
  Threads::possibly_parallel_oops_do(false, cl, &blobs);

  {
    MutexLocker lock(CodeCache_lock, Mutex::_no_safepoint_check_flag);
    CodeCache::blobs_do(&blobs);
  }

  // Weak roots: in an advanced GC these roots would be skipped during
  // the initial scan, and walked again after the marking is complete.
  // Then, we could discover which roots are not actually pointing
  // to surviving Java objects, and either clean the roots, or mark them.
  // Current simple implementation does not handle weak roots specially,
  // and therefore, we mark through them as if they are strong roots.
  // for (OopStorageSet::Iterator it = OopStorageSet::weak_iterator(); !it.is_end(); ++it) {
  //   (*it)->oops_do(cl);
  // }
  for (auto id : EnumRange<OopStorageSet::WeakId>()) {
     OopStorageSet::storage(id)->oops_do(cl);
   }
}

// Walk the marking bitmap and call object closure on every marked object.
// This is much faster that walking a (very sparse) parsable heap, but it
// takes up to 1/64-th of heap size for the bitmap.
void EpsilonHeap::walk_bitmap(ObjectClosure* cl) {
   // `top` is the highest address of the bitmap
   HeapWord* limit = _space->top();
   // `bottom` is the lowest address of the bitmap
   HeapWord* addr = _bitmap.get_next_marked_addr(_space->bottom(), limit);
   //  while we haven't reached the end of bitmap
   while (addr < limit) {
     //  shenandoah casts the addr to oop, so we'll do that too
     oop obj = cast_to_oop(addr);
     // the objects we get from bitmap should be marked, so we'll assert to make sure
     assert(_bitmap.is_marked(obj), "sanity");
     // call whatever function we need to on the object itself
     cl->do_object(obj);
     // add one to the address
     // we can simply add one because remember bitmap is continous block of memory
     // ~~also recall that bitmaps are made of bits [0,1,1,0,1, etc.] so we can increment by bit no problem?~~
     // not true, we're storing full on addresses in the bitwords
     addr += 1;
     if (addr < limit) {
       // increment address until we get to the next markedbit
       addr = _bitmap.get_next_marked_addr(addr, limit);
     }
   }
}

class EpsilonScanOopClosure : public BasicOopIterateClosure {
private:
  EpsilonMarkStack* const _stack;
  MarkBitMap* const _bitmap;

  template <class T>
  // for each reference that we encounter
  void do_oop_work(T* p) {
    // p is the pointer to memory location where oop is, load the value
    // from it, unpack the compressed reference, if needed:
    T o = RawAccess<>::oop_load(p);
    if (!CompressedOops::is_null(o)) {
      oop obj = CompressedOops::decode_not_null(o);

      // Object is discovered. See if it is marked already. If not,
      // mark and push it on mark stack for further traversal. Non-atomic
      // check and set would do, as this closure is called by single thread.
      if (!_bitmap->is_marked(obj)) {
        // TL;DR we mark the object on the bitmap if it isn't marked already
        _bitmap->mark(obj);
        // then we push it to the stack for further processing
        _stack->push(obj);
      }
    }
  }

public:
  EpsilonScanOopClosure(EpsilonMarkStack* stack, MarkBitMap* bitmap) :
                        _stack(stack), _bitmap(bitmap) {}
  virtual void do_oop(oop* p)       { do_oop_work(p); }
  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
};

class EpsilonCalcNewLocationObjectClosure : public ObjectClosure {
private:
  HeapWord* _compact_point;
  PreservedMarks* const _preserved_marks;

public:
  EpsilonCalcNewLocationObjectClosure(HeapWord* start, PreservedMarks* pm) :
                                      _compact_point(start),
                                      _preserved_marks(pm) {}

  void do_object(oop obj) {
    // Record the new location of the object: it is current compaction point.
    // If object stays at the same location (which is true for objects in
    // dense prefix, that we would normally get), do not bother recording the
    // move, letting downstream code ignore it.
    if (obj != cast_to_oop(_compact_point)) {
      // change from mark_raw to mark
      markWord mark = obj->mark();
      // change from mark.must_be_preserved(obj->klass()) to obj->mark_must_be_preserved()
      // checkout `g1FullGCCompactionPoint.cpp` for more context
      if (obj->mark_must_be_preserved()) {
        _preserved_marks->push(obj, mark);
      }
      obj->forward_to(cast_to_oop(_compact_point));
    }
    _compact_point += obj->size();
  }

  HeapWord* compact_point() {
    return _compact_point;
  }
};

class EpsilonAdjustPointersOopClosure : public BasicOopIterateClosure {
private:
  template <class T>
  void do_oop_work(T* p) {
    // p is the pointer to memory location where oop is, load the value
    // from it, unpack the compressed reference, if needed:
    T o = RawAccess<>::oop_load(p);
    if (!CompressedOops::is_null(o)) {
      oop obj = CompressedOops::decode_not_null(o);

      // Rewrite the current pointer to the object with its forwardee.
      // Skip the write if update is not needed.
      if (obj->is_forwarded()) {
        oop fwd = obj->forwardee();
        assert(fwd != NULL, "just checking");
        RawAccess<>::oop_store(p, fwd);
      }
    }
  }

public:
  virtual void do_oop(oop* p)       { do_oop_work(p); }
  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
};

class EpsilonAdjustPointersObjectClosure : public ObjectClosure {
private:
  EpsilonAdjustPointersOopClosure _cl;
public:
  void do_object(oop obj) {
    // Apply the updates to all references reachable from current object:
    obj->oop_iterate(&_cl);
  }
};

class EpsilonMoveObjectsObjectClosure : public ObjectClosure {
private:
  size_t _moved;
public:
  EpsilonMoveObjectsObjectClosure() : ObjectClosure(), _moved(0) {}

  void do_object(oop obj) {
    // Copy the object to its new location, if needed. This is final step,
    // so we have to re-initialize its new mark word, dropping the forwardee
    // data from it.
    if (obj->is_forwarded()) {
      oop fwd = obj->forwardee();
      assert(fwd != NULL, "just checking");
      Copy::aligned_conjoint_words(cast_from_oop<HeapWord*>(obj), cast_from_oop<HeapWord*>(fwd), obj->size());
      // change from init_mark_raw to init_mark
      // checkout g1fullgccompactionpoint.cpp for more detail
      fwd->init_mark();
      _moved++;
    }
  }

  size_t moved() {
    return _moved;
  }
};

class EpsilonVerifyOopClosure : public BasicOopIterateClosure {
private:
  EpsilonHeap* const _heap;
  EpsilonMarkStack* const _stack;
  MarkBitMap* const _bitmap;

  template <class T>
  void do_oop_work(T* p) {
    T o = RawAccess<>::oop_load(p);
    if (!CompressedOops::is_null(o)) {
      oop obj = CompressedOops::decode_not_null(o);
      if (!_bitmap->is_marked(obj)) {
        _bitmap->mark(obj);

        guarantee(_heap->is_in(obj),        "Is in heap: "   PTR_FORMAT, p2i(obj));
        guarantee(oopDesc::is_oop(obj),     "Is an object: " PTR_FORMAT, p2i(obj));
        guarantee(!obj->mark().is_marked(), "Mark is gone: " PTR_FORMAT, p2i(obj));

        _stack->push(obj);
      }
    }
  }

public:
  EpsilonVerifyOopClosure(EpsilonMarkStack* stack, MarkBitMap* bitmap) :
    _heap(EpsilonHeap::heap()), _stack(stack), _bitmap(bitmap) {}
  virtual void do_oop(oop* p)       { do_oop_work(p); }
  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
};

void EpsilonHeap::entry_collect(GCCause::Cause cause) {
  GCIdMark mark;
  GCTraceTime(Info, gc) time("Lisp2-style Mark-Compact", NULL, cause, true);

  // Some statistics, for fun and profit:
  // number of reachable roots, which are stored on the heap
  size_t stat_reachable_roots = 0;
  // number of reachable objects on the heap
  size_t stat_reachable_heap = 0;
  // number of objects that we move in one compact
  size_t stat_moved = 0;
  // number of objects that are marked and retained, vs. the number of unreachables
  size_t stat_preserved_marks = 0;

  {
    GCTraceTime(Info, gc) time("Step 0: Prologue", NULL);

    // Commit marking bitmap memory. There are several upsides of doing this
    // before the cycle: no memory is taken if GC is not happening, the memory
    // is "cleared" on first touch, and untouched parts of bitmap are mapped
    // to zero page, boosting performance on sparse heaps.
    // remember, committing memory is like recieving a range of virtual addresses that directly map to physical addresses,
    // and other programs cannot use it anymore
    if (!os::commit_memory((char*)_bitmap_region.start(), _bitmap_region.byte_size(), false)) {
      log_warning(gc)("Could not commit native memory for marking bitmap, GC failed");
      return;
    }

    // We do not need parsable heap for this algorithm to work, but we want
    // threads to give up their TLABs.
    ensure_parsability(true);

    // Tell various parts of runtime we are doing GC.
    BiasedLocking::preserve_marks();

    // Derived pointers would be re-discovered during the mark.
    // Clear and activate the table for them.
    DerivedPointerTable::clear();
  }

  {
    GCTraceTime(Info, gc) time("Step 1: Mark", NULL);

    // Marking stack and the closure that does most of the work. The closure
    // would scan the outgoing references, mark them, and push newly-marked
    // objects to stack for further processing.
    // this is basically breadth-first traversal with a stack + iteration instead of with recursion
    // we use iteration since there could be potentially billions of references so we can't recurse that much
    EpsilonMarkStack stack;
    // this is a closure that marks the reference and pushes it to the stack. think of it like a function that you can pass around as a variable, like javascript
    EpsilonScanOopClosure cl(&stack, &_bitmap);

    // Seed the marking with roots.
    // in other words, pushes initial reachable roots onto the stack
    process_roots(&cl);
    stat_reachable_roots = stack.size();

    // Scan the rest of the heap until we run out of objects. Termination is
    // guaranteed, because all reachable objects would be marked eventually.
    while (!stack.is_empty()) {
      // we grab the top level object (breadth-first search)
      oop obj = stack.pop();
      // iterate its references, and apply those to the heap.
      // in other words, `oop_iterate` iterates over the fields of obj, not obj itself dummy
      // it's a visitor pattern btw, notice how obj calls the closure on itself
      obj->oop_iterate(&cl);
      stat_reachable_heap++;
    }

    // No more derived pointers discovered after marking is done.
    DerivedPointerTable::set_active(false);
  }

  // We are going to store forwarding information (where the new copy resides)
  // in mark words. Some of those mark words need to be carefully preserved.
  // This is an utility that maintains the list of those special mark words.
  PreservedMarks preserved_marks;

  // New top of the allocated space.
  HeapWord* new_top;

  {
    GCTraceTime(Info, gc) time("Step 2: Calculate new locations", NULL);

    // Walk all alive objects, compute their new addresses and store those
    // addresses in mark words. Optionally preserve some marks.
    EpsilonCalcNewLocationObjectClosure cl(_space->bottom(), &preserved_marks);
    walk_bitmap(&cl);

    // After addresses are calculated, we know the new top for the allocated
    // space. We cannot set it just yet, because some asserts check that objects
    // are "in heap" based on current "top".
    new_top = cl.compact_point();

    stat_preserved_marks = preserved_marks.size();
  }

  {
    GCTraceTime(Info, gc) time("Step 3: Adjust pointers", NULL);

    // Walk all alive objects _and their reference fields_, and put "new
    // addresses" there. We know the new addresses from the forwarding data
    // in mark words. Take care of the heap objects first.
    EpsilonAdjustPointersObjectClosure cl;
    walk_bitmap(&cl);

    // Now do the same, but for all VM roots, which reference the objects on
    // their own: their references should also be updated.
    EpsilonAdjustPointersOopClosure cli;
    process_roots(&cli);

    // Finally, make sure preserved marks know the objects are about to move.
    preserved_marks.adjust_during_full_gc();
  }

  {
    GCTraceTime(Info, gc) time("Step 4: Move objects", NULL);

    // Move all alive objects to their new locations. All the references are
    // already adjusted at previous step.
    EpsilonMoveObjectsObjectClosure cl;
    walk_bitmap(&cl);
    stat_moved = cl.moved();

    // Now we moved all objects to their relevant locations, we can retract
    // the "top" of the allocation space to the end of the compacted prefix.
    _space->set_top(new_top);
  }

  {
    GCTraceTime(Info, gc) time("Step 5: Epilogue", NULL);

    // Restore all special mark words.
    preserved_marks.restore();

    // Tell the rest of runtime we have finished the GC.
    DerivedPointerTable::update_pointers();
    BiasedLocking::restore_marks();

    // Verification code walks entire heap and verifies nothing is broken.
    if (EpsilonVerify) {
      // The basic implementation turns heap into entirely parsable one with
      // only alive objects, which mean we could just walked the heap object
      // by object and verify it. But, it would be inconvenient for verification
      // to assume heap has only alive objects. Any future change that leaves
      // at least one dead object with dead outgoing references would fail the
      // verification. Therefore, it makes more sense to mark through the heap
      // again, not assuming objects are all alive.
      EpsilonMarkStack stack;
      EpsilonVerifyOopClosure cl(&stack, &_bitmap);

      _bitmap.clear();

      // Verify all roots are correct, and that we have the same number of
      // object reachable from roots.
      process_all_roots(&cl);

      size_t verified_roots = stack.size();
      guarantee(verified_roots == stat_reachable_roots,
                "Verification discovered " SIZE_FORMAT " roots out of " SIZE_FORMAT,
                verified_roots, stat_reachable_roots);

      // Verify the rest of the heap is correct, and that we have the same
      // number of objects reachable from heap.
      size_t verified_heap = 0;
      while (!stack.is_empty()) {
        oop obj = stack.pop();
        obj->oop_iterate(&cl);
        verified_heap++;
      }

      guarantee(verified_heap == stat_reachable_heap,
                "Verification discovered " SIZE_FORMAT " heap objects out of " SIZE_FORMAT,
                verified_heap, stat_reachable_heap);

      // Ask parts of runtime to verify themselves too
      Universe::verify(VerifyOption_Default, "");
    }

    // Marking bitmap is not needed anymore
    if (!os::uncommit_memory((char*)_bitmap_region.start(), _bitmap_region.byte_size())) {
      log_warning(gc)("Could not uncommit native memory for marking bitmap");
    }

    // Return all memory back if so requested. On large heaps, this would
    // take a while.
    if (EpsilonUncommit) {
      _virtual_space.shrink_by((_space->end() - new_top) * HeapWordSize);
      _space->set_end((HeapWord*)_virtual_space.high());
    }
  }

  size_t stat_reachable = stat_reachable_roots + stat_reachable_heap;
  log_info(gc)("GC Stats: " SIZE_FORMAT " (%.2f%%) reachable from roots, " SIZE_FORMAT " (%.2f%%) reachable from heap, "
               SIZE_FORMAT " (%.2f%%) moved, " SIZE_FORMAT " (%.2f%%) markwords preserved",
               stat_reachable_roots, 100.0 * stat_reachable_roots / stat_reachable,
               stat_reachable_heap,  100.0 * stat_reachable_heap  / stat_reachable,
               stat_moved,           100.0 * stat_moved           / stat_reachable,
               stat_preserved_marks, 100.0 * stat_preserved_marks / stat_reachable);

  print_heap_info(used());
  print_metaspace_info();
}