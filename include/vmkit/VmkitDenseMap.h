//===- VmkitDenseMap.h - Dense probed hash table ------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the VmkitDenseMap copied from llvm/ADT/DenseMap.h, but
// without storing pairs.
//
//===----------------------------------------------------------------------===//

#ifndef VMKIT_DENSEMAP_H
#define VMKIT_DENSEMAP_H

#include "llvm/Support/MathExtras.h"
#include "llvm/Support/PointerLikeTypeTraits.h"
#include "llvm/Support/type_traits.h"
#include <algorithm>
#include <iterator>
#include <new>
#include <utility>
#include <cassert>
#include <cstddef>
#include <cstring>

namespace vmkit {

template<typename T>
struct VmkitDenseMapInfo {
  //static inline T getEmptyKey();
  //static inline T getTombstoneKey();
  //static unsigned getHashValue(const T &Val);
  //static bool vmkIsEqual(const T &LHS, const T &RHS);
};

template<typename KeyT, typename ValueT,
         typename KeyInfoT = VmkitDenseMapInfo<KeyT>,
         bool IsConst = false>
class VmkitDenseMapIterator;

template<typename KeyT, typename ValueT>
struct VmkitPair {
  KeyT first;
  ValueT second;
};

template<typename KeyT, typename ValueT,
         typename KeyInfoT = VmkitDenseMapInfo<KeyT> >
class VmkitDenseMap {
public:
  typedef VmkitPair<KeyT, ValueT> BucketT;
  uint32_t NumBuckets;
  BucketT *Buckets;

  uint32_t NumEntries;
  uint32_t NumTombstones;
  bool IsPrecompiled;

  typedef KeyT key_type;
  typedef ValueT mapped_type;
  typedef BucketT value_type;

  explicit VmkitDenseMap(unsigned NumInitBuckets = 0) {
    IsPrecompiled = false;
    init(NumInitBuckets);
  }

  ~VmkitDenseMap() {
    const KeyT EmptyKey = getEmptyKey(), TombstoneKey = getTombstoneKey();
    for (BucketT *P = Buckets, *E = Buckets+NumBuckets; P != E; ++P) {
      if (!KeyInfoT::vmkIsEqual(P->first, EmptyKey) &&
          !KeyInfoT::vmkIsEqual(P->first, TombstoneKey))
        P->second.~ValueT();
      P->first.~KeyT();
    }
#ifndef NDEBUG
    if (NumBuckets)
      memset((void*)Buckets, 0x5a, sizeof(BucketT)*NumBuckets);
#endif
    if (!IsPrecompiled) {
      operator delete(Buckets);
    }
  }

  typedef VmkitDenseMapIterator<KeyT, ValueT, KeyInfoT> iterator;
  typedef VmkitDenseMapIterator<KeyT, ValueT, KeyInfoT, true> const_iterator;
  inline iterator begin() {
    // When the map is empty, avoid the overhead of AdvancePastEmptyBuckets().
    return empty() ? end() : iterator(Buckets, Buckets+NumBuckets);
  }
  inline iterator end() {
    return iterator(Buckets+NumBuckets, Buckets+NumBuckets);
  }
  inline const_iterator begin() const {
    return empty() ? end() : const_iterator(Buckets, Buckets+NumBuckets);
  }
  inline const_iterator end() const {
    return const_iterator(Buckets+NumBuckets, Buckets+NumBuckets);
  }

  bool empty() const { return NumEntries == 0; }
  unsigned size() const { return NumEntries; }

  /// Grow the densemap so that it has at least Size buckets. Does not shrink
  void resize(size_t Size) {
    if (Size > NumBuckets)
      grow(Size);
  }

  void clear() {
    if (NumEntries == 0 && NumTombstones == 0) return;
    
    // If the capacity of the array is huge, and the # elements used is small,
    // shrink the array.
    if (NumEntries * 4 < NumBuckets && NumBuckets > 64) {
      shrink_and_clear();
      return;
    }

    const KeyT EmptyKey = getEmptyKey(), TombstoneKey = getTombstoneKey();
    for (BucketT *P = Buckets, *E = Buckets+NumBuckets; P != E; ++P) {
      if (!KeyInfoT::vmkIsEqual(P->first, EmptyKey)) {
        if (!KeyInfoT::vmkIsEqual(P->first, TombstoneKey)) {
          P->second.~ValueT();
          --NumEntries;
        }
        P->first = EmptyKey;
      }
    }
    assert(NumEntries == 0 && "Node count imbalance!");
    NumTombstones = 0;
  }

  /// count - Return true if the specified key is in the map.
  bool count(const KeyT &Val) const {
    BucketT *TheBucket;
    return LookupBucketFor(Val, TheBucket);
  }

  iterator find(const KeyT &Val) {
    BucketT *TheBucket;
    if (LookupBucketFor(Val, TheBucket))
      return iterator(TheBucket, Buckets+NumBuckets);
    return end();
  }
  const_iterator find(const KeyT &Val) const {
    BucketT *TheBucket;
    if (LookupBucketFor(Val, TheBucket))
      return const_iterator(TheBucket, Buckets+NumBuckets);
    return end();
  }

  /// lookup - Return the entry for the specified key, or a default
  /// constructed value if no such entry exists.
  ValueT lookup(const KeyT &Val) const {
    BucketT *TheBucket;
    if (LookupBucketFor(Val, TheBucket))
      return TheBucket->second;
    return ValueT();
  }

  // Inserts key,value pair into the map if the key isn't already in the map.
  // If the key is already in the map, it returns false and doesn't update the
  // value.
  std::pair<iterator, bool> insert(const std::pair<KeyT, ValueT> &KV) {
    BucketT *TheBucket;
    if (LookupBucketFor(KV.first, TheBucket))
      return std::make_pair(iterator(TheBucket, Buckets+NumBuckets),
                            false); // Already in map.

    // Otherwise, insert the new element.
    TheBucket = InsertIntoBucket(KV.first, KV.second, TheBucket);
    return std::make_pair(iterator(TheBucket, Buckets+NumBuckets),
                          true);
  }

  /// insert - Range insertion of pairs.
  template<typename InputIt>
  void insert(InputIt I, InputIt E) {
    for (; I != E; ++I)
      insert(*I);
  }


  bool erase(const KeyT &Val) {
    BucketT *TheBucket;
    if (!LookupBucketFor(Val, TheBucket))
      return false; // not in map.

    TheBucket->second.~ValueT();
    TheBucket->first = getTombstoneKey();
    --NumEntries;
    ++NumTombstones;
    return true;
  }
  void erase(iterator I) {
    BucketT *TheBucket = &*I;
    TheBucket->second.~ValueT();
    TheBucket->first = getTombstoneKey();
    --NumEntries;
    ++NumTombstones;
  }

  void swap(VmkitDenseMap& RHS) {
    std::swap(NumBuckets, RHS.NumBuckets);
    std::swap(Buckets, RHS.Buckets);
    std::swap(NumEntries, RHS.NumEntries);
    std::swap(NumTombstones, RHS.NumTombstones);
  }

  value_type& FindAndConstruct(const KeyT &Key) {
    BucketT *TheBucket;
    if (LookupBucketFor(Key, TheBucket))
      return *TheBucket;

    return *InsertIntoBucket(Key, ValueT(), TheBucket);
  }

  ValueT &operator[](const KeyT &Key) {
    return FindAndConstruct(Key).second;
  }

  /// isPointerIntoBucketsArray - Return true if the specified pointer points
  /// somewhere into the VmkitDenseMap's array of buckets (i.e. either to a key or
  /// value in the VmkitDenseMap).
  bool isPointerIntoBucketsArray(const void *Ptr) const {
    return Ptr >= Buckets && Ptr < Buckets+NumBuckets;
  }

  /// getPointerIntoBucketsArray() - Return an opaque pointer into the buckets
  /// array.  In conjunction with the previous method, this can be used to
  /// determine whether an insertion caused the VmkitDenseMap to reallocate.
  const void *getPointerIntoBucketsArray() const { return Buckets; }

private:
  BucketT *InsertIntoBucket(const KeyT &Key, const ValueT &Value,
                            BucketT *TheBucket) {
    // If the load of the hash table is more than 3/4, or if fewer than 1/8 of
    // the buckets are empty (meaning that many are filled with tombstones),
    // grow the table.
    //
    // The later case is tricky.  For example, if we had one empty bucket with
    // tons of tombstones, failing lookups (e.g. for insertion) would have to
    // probe almost the entire table until it found the empty bucket.  If the
    // table completely filled with tombstones, no lookup would ever succeed,
    // causing infinite loops in lookup.
    ++NumEntries;
    if (NumEntries*4 >= NumBuckets*3) {
      this->grow(NumBuckets * 2);
      LookupBucketFor(Key, TheBucket);
    }
    if (NumBuckets-(NumEntries+NumTombstones) < NumBuckets/8) {
      this->grow(NumBuckets);
      LookupBucketFor(Key, TheBucket);
    }

    // If we are writing over a tombstone, remember this.
    if (!KeyInfoT::vmkIsEqual(TheBucket->first, getEmptyKey()))
      --NumTombstones;

    TheBucket->first = Key;
    new (&TheBucket->second) ValueT(Value);
    return TheBucket;
  }

  static unsigned getHashValue(const KeyT &Val) {
    return KeyInfoT::getHashValue(Val);
  }
  static const KeyT getEmptyKey() {
    return KeyInfoT::getEmptyKey();
  }
  static const KeyT getTombstoneKey() {
    return KeyInfoT::getTombstoneKey();
  }

  /// LookupBucketFor - Lookup the appropriate bucket for Val, returning it in
  /// FoundBucket.  If the bucket contains the key and a value, this returns
  /// true, otherwise it returns a bucket with an empty marker or tombstone and
  /// returns false.
  bool LookupBucketFor(const KeyT &Val, BucketT *&FoundBucket) const {
    unsigned BucketNo = getHashValue(Val);
    unsigned ProbeAmt = 1;
    BucketT *BucketsPtr = Buckets;

    if (NumBuckets == 0) {
      FoundBucket = 0;
      return false;
    }

    // FoundTombstone - Keep track of whether we find a tombstone while probing.
    BucketT *FoundTombstone = 0;
    const KeyT EmptyKey = getEmptyKey();
    const KeyT TombstoneKey = getTombstoneKey();
    assert(!KeyInfoT::vmkIsEqual(Val, EmptyKey) &&
           !KeyInfoT::vmkIsEqual(Val, TombstoneKey) &&
           "Empty/Tombstone value shouldn't be inserted into map!");

    while (1) {
      BucketT *ThisBucket = BucketsPtr + (BucketNo & (NumBuckets-1));
      // Found Val's bucket?  If so, return it.
      if (KeyInfoT::vmkIsEqual(ThisBucket->first, Val)) {
        FoundBucket = ThisBucket;
        return true;
      }

      // If we found an empty bucket, the key doesn't exist in the set.
      // Insert it and return the default value.
      if (KeyInfoT::vmkIsEqual(ThisBucket->first, EmptyKey)) {
        // If we've already seen a tombstone while probing, fill it in instead
        // of the empty bucket we eventually probed to.
        if (FoundTombstone) ThisBucket = FoundTombstone;
        FoundBucket = FoundTombstone ? FoundTombstone : ThisBucket;
        return false;
      }

      // If this is a tombstone, remember it.  If Val ends up not in the map, we
      // prefer to return it than something that would require more probing.
      if (KeyInfoT::vmkIsEqual(ThisBucket->first, TombstoneKey) && !FoundTombstone)
        FoundTombstone = ThisBucket;  // Remember the first tombstone found.

      // Otherwise, it's a hash collision or a tombstone, continue quadratic
      // probing.
      BucketNo += ProbeAmt++;
    }
  }

  void init(unsigned InitBuckets) {
    NumEntries = 0;
    NumTombstones = 0;
    NumBuckets = InitBuckets;

    if (InitBuckets == 0) {
      Buckets = 0;
      return;
    }

    assert(InitBuckets && (InitBuckets & (InitBuckets-1)) == 0 &&
           "# initial buckets must be a power of two!");
    Buckets = static_cast<BucketT*>(operator new(sizeof(BucketT)*InitBuckets));
    // Initialize all the keys to EmptyKey.
    const KeyT EmptyKey = getEmptyKey();
    for (unsigned i = 0; i != InitBuckets; ++i)
      new (&Buckets[i].first) KeyT(EmptyKey);
  }

  void grow(unsigned AtLeast) {
    unsigned OldNumBuckets = NumBuckets;
    BucketT *OldBuckets = Buckets;

    if (NumBuckets < 64)
      NumBuckets = 64;

    // Double the number of buckets.
    while (NumBuckets < AtLeast)
      NumBuckets <<= 1;
    NumTombstones = 0;
    Buckets = static_cast<BucketT*>(operator new(sizeof(BucketT)*NumBuckets));

    // Initialize all the keys to EmptyKey.
    const KeyT EmptyKey = getEmptyKey();
    for (unsigned i = 0, e = NumBuckets; i != e; ++i)
      new (&Buckets[i].first) KeyT(EmptyKey);

    // Insert all the old elements.
    const KeyT TombstoneKey = getTombstoneKey();
    for (BucketT *B = OldBuckets, *E = OldBuckets+OldNumBuckets; B != E; ++B) {
      if (!KeyInfoT::vmkIsEqual(B->first, EmptyKey) &&
          !KeyInfoT::vmkIsEqual(B->first, TombstoneKey)) {
        // Insert the key/value into the new table.
        BucketT *DestBucket;
        bool FoundVal = LookupBucketFor(B->first, DestBucket);
        (void)FoundVal; // silence warning.
        assert(!FoundVal && "Key already in new map?");
        DestBucket->first = B->first;
        new (&DestBucket->second) ValueT(B->second);

        // Free the value.
        B->second.~ValueT();
      }
      B->first.~KeyT();
    }

#ifndef NDEBUG
    if (OldNumBuckets)
      memset((void*)OldBuckets, 0x5a, sizeof(BucketT)*OldNumBuckets);
#endif
    // Free the old table.
    if (!IsPrecompiled) {
      operator delete(OldBuckets);
    } else {
      IsPrecompiled = false;
    }
  }

  void shrink_and_clear() {
    unsigned OldNumBuckets = NumBuckets;
    BucketT *OldBuckets = Buckets;

    // Reduce the number of buckets.
    NumBuckets = NumEntries > 32 ? 1 << (llvm::Log2_32_Ceil(NumEntries) + 1)
                                 : 64;
    NumTombstones = 0;
    Buckets = static_cast<BucketT*>(operator new(sizeof(BucketT)*NumBuckets));

    // Initialize all the keys to EmptyKey.
    const KeyT EmptyKey = getEmptyKey();
    for (unsigned i = 0, e = NumBuckets; i != e; ++i)
      new (&Buckets[i].first) KeyT(EmptyKey);

    // Free the old buckets.
    const KeyT TombstoneKey = getTombstoneKey();
    for (BucketT *B = OldBuckets, *E = OldBuckets+OldNumBuckets; B != E; ++B) {
      if (!KeyInfoT::vmkIsEqual(B->first, EmptyKey) &&
          !KeyInfoT::vmkIsEqual(B->first, TombstoneKey)) {
        // Free the value.
        B->second.~ValueT();
      }
      B->first.~KeyT();
    }

#ifndef NDEBUG
    memset((void*)OldBuckets, 0x5a, sizeof(BucketT)*OldNumBuckets);
#endif
    if (!IsPrecompiled) {
      // Free the old table.
      operator delete(OldBuckets);
    } else {
      IsPrecompiled = false;
    }

    NumEntries = 0;
  }
  
public:
  /// Return the approximate size (in bytes) of the actual map.
  /// This is just the raw memory used by VmkitDenseMap.
  /// If entries are pointers to objects, the size of the referenced objects
  /// are not included.
  size_t getMemorySize() const {
    return NumBuckets * sizeof(BucketT);
  }
};

template<typename KeyT, typename ValueT,
         typename KeyInfoT, bool IsConst>
class VmkitDenseMapIterator {
  typedef VmkitPair<KeyT, ValueT> Bucket;
  typedef VmkitDenseMapIterator<KeyT, ValueT, KeyInfoT, true> ConstIterator;
  friend class VmkitDenseMapIterator<KeyT, ValueT, KeyInfoT, true>;
public:
  typedef ptrdiff_t difference_type;
  typedef typename llvm::conditional<IsConst, const Bucket, Bucket>::type value_type;
  typedef value_type *pointer;
  typedef value_type &reference;
  typedef std::forward_iterator_tag iterator_category;
private:
  pointer Ptr, End;
public:
  VmkitDenseMapIterator() : Ptr(0), End(0) {}

  VmkitDenseMapIterator(pointer Pos, pointer E) : Ptr(Pos), End(E) {
    AdvancePastEmptyBuckets();
  }

  // If IsConst is true this is a converting constructor from iterator to
  // const_iterator and the default copy constructor is used.
  // Otherwise this is a copy constructor for iterator.
  VmkitDenseMapIterator(const VmkitDenseMapIterator<KeyT, ValueT,
                                          KeyInfoT, false>& I)
    : Ptr(I.Ptr), End(I.End) {}

  reference operator*() const {
    return *Ptr;
  }
  pointer operator->() const {
    return Ptr;
  }

  bool operator==(const ConstIterator &RHS) const {
    return Ptr == RHS.operator->();
  }
  bool operator!=(const ConstIterator &RHS) const {
    return Ptr != RHS.operator->();
  }

  inline VmkitDenseMapIterator& operator++() {  // Preincrement
    ++Ptr;
    AdvancePastEmptyBuckets();
    return *this;
  }
  VmkitDenseMapIterator operator++(int) {  // Postincrement
    VmkitDenseMapIterator tmp = *this; ++*this; return tmp;
  }

private:
  void AdvancePastEmptyBuckets() {
    const KeyT Empty = KeyInfoT::getEmptyKey();
    const KeyT Tombstone = KeyInfoT::getTombstoneKey();

    while (Ptr != End &&
           (KeyInfoT::vmkIsEqual(Ptr->first, Empty) ||
            KeyInfoT::vmkIsEqual(Ptr->first, Tombstone)))
      ++Ptr;
  }
};

} // end namespace j3

#endif
