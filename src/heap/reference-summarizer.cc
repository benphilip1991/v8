// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/reference-summarizer.h"

#include "src/heap/mark-compact-inl.h"
#include "src/heap/marking-visitor-inl.h"
#include "src/objects/embedder-data-array-inl.h"
#include "src/objects/js-array-buffer-inl.h"

namespace v8 {
namespace internal {

namespace {

// A class which acts as a MarkingState but does not actually update any marking
// bits. It reports all objects as white and all transitions as successful. It
// also tracks which objects are retained by the primary object according to the
// marking visitor.
class ReferenceSummarizerMarkingState final {
 public:
  explicit ReferenceSummarizerMarkingState(HeapObject object)
      : primary_object_(object),
        local_marking_worklists_(&marking_worklists_),
        local_weak_objects_(&weak_objects_) {}

  ~ReferenceSummarizerMarkingState() {
    // Clean up temporary state.
    local_weak_objects_.Publish();
    weak_objects_.Clear();
    local_marking_worklists_.Publish();
    marking_worklists_.Clear();
  }

  // Retrieves the references that were collected by this marker. This operation
  // transfers ownership of the set, so calling it again would yield an empty
  // result.
  ReferenceSummary DestructivelyRetrieveReferences() {
    ReferenceSummary tmp = std::move(references_);
    references_.Clear();
    return tmp;
  }

  // Standard marking visitor functions:
  bool TryMark(HeapObject obj) { return true; }
  bool IsUnmarked(HeapObject obj) const { return true; }
  bool IsMarked(HeapObject obj) const { return false; }

  // Adds a retaining relationship found by the marking visitor.
  void AddStrongReferenceForReferenceSummarizer(HeapObject host,
                                                HeapObject obj) {
    AddReference(host, obj, references_.strong_references());
  }

  // Adds a non-retaining weak reference found by the marking visitor. The value
  // in an ephemeron hash table entry is also included here, since it is not
  // known to be strong without further information about the key.
  void AddWeakReferenceForReferenceSummarizer(HeapObject host, HeapObject obj) {
    AddReference(host, obj, references_.weak_references());
  }

  // Other member functions, not part of the marking visitor contract:

  MarkingWorklists::Local* local_marking_worklists() {
    return &local_marking_worklists_;
  }
  WeakObjects::Local* local_weak_objects() { return &local_weak_objects_; }

 private:
  void AddReference(HeapObject host, HeapObject obj,
                    ReferenceSummary::UnorderedHeapObjectSet& references) {
    // It's possible that the marking visitor handles multiple objects at once,
    // such as a Map and its DescriptorArray, but we're only interested in
    // references from the primary object.
    if (host == primary_object_) {
      references.insert(obj);
    }
  }

  ReferenceSummary references_;
  HeapObject primary_object_;
  MarkingWorklists marking_worklists_;
  MarkingWorklists::Local local_marking_worklists_;
  WeakObjects weak_objects_;
  WeakObjects::Local local_weak_objects_;
};

class ReferenceSummarizerMarkingVisitor
    : public MarkingVisitorBase<ReferenceSummarizerMarkingVisitor> {
 public:
  ReferenceSummarizerMarkingVisitor(
      Heap* heap, ReferenceSummarizerMarkingState* marking_state)
      : MarkingVisitorBase(
            marking_state->local_marking_worklists(),
            marking_state->local_weak_objects(), heap, 0 /*mark_compact_epoch*/,
            {} /*code_flush_mode*/, false /*embedder_tracing_enabled*/,
            true /*should_keep_ages_unchanged*/, 0 /*code_flushing_increase*/),
        marking_state_(marking_state) {}

  template <typename TSlot>
  void RecordSlot(HeapObject object, TSlot slot, HeapObject target) {}

  void RecordRelocSlot(InstructionStream host, RelocInfo* rinfo,
                       HeapObject target) {}

  V8_INLINE void AddStrongReferenceForReferenceSummarizer(HeapObject host,
                                                          HeapObject obj) {
    marking_state_->AddStrongReferenceForReferenceSummarizer(host, obj);
  }

  V8_INLINE void AddWeakReferenceForReferenceSummarizer(HeapObject host,
                                                        HeapObject obj) {
    marking_state_->AddWeakReferenceForReferenceSummarizer(host, obj);
  }

  TraceRetainingPathMode retaining_path_mode() {
    return TraceRetainingPathMode::kDisabled;
  }

  constexpr bool CanUpdateValuesInHeap() { return false; }

  // Standard marking visitor functions:
  bool TryMark(HeapObject obj) { return true; }
  bool IsMarked(HeapObject obj) const { return false; }

 private:
  ReferenceSummarizerMarkingState* marking_state_;
};

}  // namespace

ReferenceSummary ReferenceSummary::SummarizeReferencesFrom(Heap* heap,
                                                           HeapObject obj) {
  ReferenceSummarizerMarkingState marking_state(obj);

  ReferenceSummarizerMarkingVisitor visitor(heap, &marking_state);
  visitor.Visit(obj.map(heap->isolate()), obj);

  return marking_state.DestructivelyRetrieveReferences();
}

}  // namespace internal
}  // namespace v8
