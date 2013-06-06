// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// This file is NOT intended for use by clients, except in generated code.
//
// This file defines low-level, non-type-safe classes for traversing the Cap'n Proto memory layout
// (which is also its wire format).  Code generated by the Cap'n Proto compiler uses these classes,
// as does other parts of the Cap'n proto library which provide a higher-level interface for
// dynamic introspection.

#ifndef CAPNP_LAYOUT_H_
#define CAPNP_LAYOUT_H_

#include <kj/common.h>
#include "common.h"
#include "blob.h"

namespace capnp {
namespace internal {

class StructBuilder;
class StructReader;
class ListBuilder;
class ListReader;
struct ObjectBuilder;
struct ObjectReader;
struct WirePointer;
struct WireHelpers;
class SegmentReader;
class SegmentBuilder;

// =============================================================================

enum class FieldSize: uint8_t {
  // TODO(cleanup):  Rename to FieldLayout or maybe ValueLayout.

  // Notice that each member of this enum, when representing a list element size, represents a
  // size that is greater than or equal to the previous members, since INLINE_COMPOSITE is used
  // only for multi-word structs.  This is important because it allows us to compare FieldSize
  // values for the purpose of deciding when we need to upgrade a list.

  VOID = 0,
  BIT = 1,
  BYTE = 2,
  TWO_BYTES = 3,
  FOUR_BYTES = 4,
  EIGHT_BYTES = 5,

  POINTER = 6,  // Indicates that the field lives in the pointer section, not the data section.

  INLINE_COMPOSITE = 7
  // A composite type of fixed width.  This serves two purposes:
  // 1) For lists of composite types where all the elements would have the exact same width,
  //    allocating a list of pointers which in turn point at the elements would waste space.  We
  //    can avoid a layer of indirection by placing all the elements in a flat sequence, and only
  //    indicating the element properties (e.g. field count for structs) once.
  //
  //    Specifically, a list pointer indicating INLINE_COMPOSITE element size actually points to
  //    a "tag" describing one element.  This tag is formatted like a wire pointer, but the
  //    "offset" instead stores the element count of the list.  The flat list of elements appears
  //    immediately after the tag.  In the list pointer itself, the element count is replaced with
  //    a word count for the whole list (excluding tag).  This allows the tag and elements to be
  //    precached in a single step rather than two sequential steps.
  //
  //    It is NOT intended to be possible to substitute an INLINE_COMPOSITE list for a POINTER
  //    list or vice-versa without breaking recipients.  Recipients expect one or the other
  //    depending on the message definition.
  //
  //    However, it IS allowed to substitute an INLINE_COMPOSITE list -- specifically, of structs --
  //    when a list was expected, or vice versa, with the assumption that the first field of the
  //    struct (field number zero) correspond to the element type.  This allows a list of
  //    primitives to be upgraded to a list of structs, avoiding the need to use parallel arrays
  //    when you realize that you need to attach some extra information to each element of some
  //    primitive list.
  //
  // 2) At one point there was a notion of "inline" struct fields, but it was deemed too much of
  //    an implementation burden for too little gain, and so was deleted.
};

typedef decltype(BITS / ELEMENTS) BitsPerElement;
typedef decltype(POINTERS / ELEMENTS) PointersPerElement;

static constexpr BitsPerElement BITS_PER_ELEMENT_TABLE[8] = {
    0 * BITS / ELEMENTS,
    1 * BITS / ELEMENTS,
    8 * BITS / ELEMENTS,
    16 * BITS / ELEMENTS,
    32 * BITS / ELEMENTS,
    64 * BITS / ELEMENTS,
    0 * BITS / ELEMENTS,
    0 * BITS / ELEMENTS
};

inline constexpr BitsPerElement dataBitsPerElement(FieldSize size) {
  return internal::BITS_PER_ELEMENT_TABLE[static_cast<int>(size)];
}

inline constexpr PointersPerElement pointersPerElement(FieldSize size) {
  return size == FieldSize::POINTER ? 1 * POINTERS / ELEMENTS : 0 * POINTERS / ELEMENTS;
}

}  // namespace internal

enum class Kind: uint8_t {
  PRIMITIVE,
  BLOB,
  ENUM,
  STRUCT,
  UNION,
  INTERFACE,
  LIST,
  UNKNOWN
};

namespace internal {

template <typename T> struct Kind_ { static constexpr Kind kind = Kind::UNKNOWN; };

template <> struct Kind_<Void> { static constexpr Kind kind = Kind::PRIMITIVE; };
template <> struct Kind_<bool> { static constexpr Kind kind = Kind::PRIMITIVE; };
template <> struct Kind_<int8_t> { static constexpr Kind kind = Kind::PRIMITIVE; };
template <> struct Kind_<int16_t> { static constexpr Kind kind = Kind::PRIMITIVE; };
template <> struct Kind_<int32_t> { static constexpr Kind kind = Kind::PRIMITIVE; };
template <> struct Kind_<int64_t> { static constexpr Kind kind = Kind::PRIMITIVE; };
template <> struct Kind_<uint8_t> { static constexpr Kind kind = Kind::PRIMITIVE; };
template <> struct Kind_<uint16_t> { static constexpr Kind kind = Kind::PRIMITIVE; };
template <> struct Kind_<uint32_t> { static constexpr Kind kind = Kind::PRIMITIVE; };
template <> struct Kind_<uint64_t> { static constexpr Kind kind = Kind::PRIMITIVE; };
template <> struct Kind_<float> { static constexpr Kind kind = Kind::PRIMITIVE; };
template <> struct Kind_<double> { static constexpr Kind kind = Kind::PRIMITIVE; };
template <> struct Kind_<Text> { static constexpr Kind kind = Kind::BLOB; };
template <> struct Kind_<Data> { static constexpr Kind kind = Kind::BLOB; };

}  // namespace internal

template <typename T>
inline constexpr Kind kind() {
  return internal::Kind_<T>::kind;
}

// =============================================================================

namespace internal {

template <int wordCount>
union AlignedData {
  // Useful for declaring static constant data blobs as an array of bytes, but forcing those
  // bytes to be word-aligned.

  uint8_t bytes[wordCount * sizeof(word)];
  word words[wordCount];
};

struct StructSize {
  WordCount16 data;
  WirePointerCount16 pointers;

  FieldSize preferredListEncoding;
  // Preferred size to use when encoding a list of this struct.  This is INLINE_COMPOSITE if and
  // only if the struct is larger than one word; otherwise the struct list can be encoded more
  // efficiently by encoding it as if it were some primitive type.

  inline constexpr WordCount total() const { return data + pointers * WORDS_PER_POINTER; }

  StructSize() = default;
  inline constexpr StructSize(WordCount data, WirePointerCount pointers,
                              FieldSize preferredListEncoding)
      : data(data), pointers(pointers), preferredListEncoding(preferredListEncoding) {}
};

template <typename T> struct StructSize_;
// Specialized for every struct type with member:  static constexpr StructSize value"

template <typename T>
inline constexpr StructSize structSize() {
  return StructSize_<T>::value;
}

// -------------------------------------------------------------------
// Masking of default values

template <typename T, Kind kind = kind<T>()> struct Mask_;
template <typename T> struct Mask_<T, Kind::PRIMITIVE> { typedef T Type; };
template <typename T> struct Mask_<T, Kind::ENUM> { typedef uint16_t Type; };
template <> struct Mask_<float, Kind::PRIMITIVE> { typedef uint32_t Type; };
template <> struct Mask_<double, Kind::PRIMITIVE> { typedef uint64_t Type; };

template <typename T> struct Mask_<T, Kind::UNKNOWN> {
  // Union discriminants end up here.
  static_assert(sizeof(T) == 2, "Don't know how to mask this type.");
  typedef uint16_t Type;
};

template <typename T>
using Mask = typename Mask_<T>::Type;

template <typename T>
KJ_ALWAYS_INLINE(Mask<T> mask(T value, Mask<T> mask));
template <typename T>
KJ_ALWAYS_INLINE(T unmask(Mask<T> value, Mask<T> mask));

template <typename T>
inline Mask<T> mask(T value, Mask<T> mask) {
  return static_cast<Mask<T> >(value) ^ mask;
}

template <>
inline uint32_t mask<float>(float value, uint32_t mask) {
  uint32_t i;
  static_assert(sizeof(i) == sizeof(value), "float is not 32 bits?");
  memcpy(&i, &value, sizeof(value));
  return i ^ mask;
}

template <>
inline uint64_t mask<double>(double value, uint64_t mask) {
  uint64_t i;
  static_assert(sizeof(i) == sizeof(value), "double is not 64 bits?");
  memcpy(&i, &value, sizeof(value));
  return i ^ mask;
}

template <typename T>
inline T unmask(Mask<T> value, Mask<T> mask) {
  return static_cast<T>(value ^ mask);
}

template <>
inline float unmask<float>(uint32_t value, uint32_t mask) {
  value ^= mask;
  float result;
  static_assert(sizeof(result) == sizeof(value), "float is not 32 bits?");
  memcpy(&result, &value, sizeof(value));
  return result;
}

template <>
inline double unmask<double>(uint64_t value, uint64_t mask) {
  value ^= mask;
  double result;
  static_assert(sizeof(result) == sizeof(value), "double is not 64 bits?");
  memcpy(&result, &value, sizeof(value));
  return result;
}

// -------------------------------------------------------------------

template <typename T>
class WireValue {
  // Wraps a primitive value as it appears on the wire.  Namely, values are little-endian on the
  // wire, because little-endian is the most common endianness in modern CPUs.
  //
  // TODO(soon):  On big-endian systems, inject byte-swapping here.  Most big-endian CPUs implement
  //   dedicated instructions for this, so use those rather than writing a bunch of shifts and
  //   masks.  Note that GCC has e.g. __builtin__bswap32() for this.
  //
  // Note:  In general, code that depends cares about byte ordering is bad.  See:
  //     http://commandcenter.blogspot.com/2012/04/byte-order-fallacy.html
  //   Cap'n Proto is special because it is essentially doing compiler-like things, fussing over
  //   allocation and layout of memory, in order to squeeze out every last drop of performance.

public:
  WireValue() = default;
  KJ_ALWAYS_INLINE(WireValue(T value)): value(value) {}

  KJ_ALWAYS_INLINE(T get() const) { return value; }
  KJ_ALWAYS_INLINE(void set(T newValue)) { value = newValue; }

private:
  T value;
};

class StructBuilder: public kj::DisallowConstCopy {
public:
  inline StructBuilder(): segment(nullptr), data(nullptr), pointers(nullptr), bit0Offset(0) {}

  static StructBuilder initRoot(SegmentBuilder* segment, word* location, StructSize size);
  static void setRoot(SegmentBuilder* segment, word* location, StructReader value);
  static StructBuilder getRoot(SegmentBuilder* segment, word* location, StructSize size);

  inline BitCount getDataSectionSize() const { return dataSize; }
  inline WirePointerCount getPointerSectionSize() const { return pointerCount; }
  inline Data::Builder getDataSectionAsBlob();

  template <typename T>
  KJ_ALWAYS_INLINE(T getDataField(ElementCount offset));
  // Gets the data field value of the given type at the given offset.  The offset is measured in
  // multiples of the field size, determined by the type.

  template <typename T>
  KJ_ALWAYS_INLINE(T getDataField(ElementCount offset, Mask<T> mask));
  // Like getDataField() but applies the given XOR mask to the data on load.  Used for reading
  // fields with non-zero default values.

  template <typename T>
  KJ_ALWAYS_INLINE(void setDataField(
      ElementCount offset, kj::NoInfer<T> value));
  // Sets the data field value at the given offset.

  template <typename T>
  KJ_ALWAYS_INLINE(void setDataField(
      ElementCount offset, kj::NoInfer<T> value, Mask<T> mask));
  // Like setDataField() but applies the given XOR mask before storing.  Used for writing fields
  // with non-zero default values.

  StructBuilder initStructField(WirePointerCount ptrIndex, StructSize size);
  // Initializes the struct field at the given index in the pointer section.  If it is already
  // initialized, the previous value is discarded or overwritten.  The struct is initialized to
  // the type's default state (all-zero).  Use getStructField() if you want the struct to be
  // initialized as a copy of the field's default value (which may have non-null pointers).

  StructBuilder getStructField(WirePointerCount ptrIndex, StructSize size,
                               const word* defaultValue);
  // Gets the struct field at the given index in the pointer section.  If the field is not already
  // initialized, it is initialized as a deep copy of the given default value (a flat message),
  // or to the empty state if defaultValue is nullptr.

  ListBuilder initListField(WirePointerCount ptrIndex, FieldSize elementSize,
                            ElementCount elementCount);
  // Allocates a new list of the given size for the field at the given index in the pointer
  // segment, and return a pointer to it.  All elements are initialized to zero.

  ListBuilder initStructListField(WirePointerCount ptrIndex, ElementCount elementCount,
                                  StructSize size);
  // Allocates a new list of the given size for the field at the given index in the pointer
  // segment, and return a pointer to it.  Each element is initialized to its empty state.

  ListBuilder getListField(WirePointerCount ptrIndex, FieldSize elementSize,
                           const word* defaultValue);
  // Gets the already-allocated list field for the given pointer index, ensuring that the list is
  // suitable for storing non-struct elements of the given size.  If the list is not already
  // allocated, it is allocated as a deep copy of the given default value (a flat message).  If
  // the default value is null, an empty list is used.

  ListBuilder getStructListField(WirePointerCount ptrIndex, StructSize elementSize,
                                 const word* defaultValue);
  // Gets the already-allocated list field for the given pointer index, ensuring that the list
  // is suitable for storing struct elements of the given size.  If the list is not
  // already allocated, it is allocated as a deep copy of the given default value (a flat
  // message).  If the default value is null, an empty list is used.

  template <typename T>
  typename T::Builder initBlobField(WirePointerCount ptrIndex, ByteCount size);
  // Initialize a Text or Data field to the given size in bytes (not including NUL terminator for
  // Text) and return a Text::Builder which can be used to fill in the content.

  template <typename T>
  void setBlobField(WirePointerCount ptrIndex, typename T::Reader value);
  // Set the blob field to a copy of the given blob.

  template <typename T>
  typename T::Builder getBlobField(WirePointerCount ptrIndex,
                                   const void* defaultValue, ByteCount defaultSize);
  // Get the blob field.  If it is not initialized, initialize it to a copy of the given default.

  ObjectBuilder getObjectField(WirePointerCount ptrIndex, const word* defaultValue);
  // Read a pointer of arbitrary type.

  void setStructField(WirePointerCount ptrIndex, StructReader value);
  void setListField(WirePointerCount ptrIndex, ListReader value);
  void setObjectField(WirePointerCount ptrIndex, ObjectReader value);
  // Sets a pointer field to a deep copy of the given value.

  bool isPointerFieldNull(WirePointerCount ptrIndex);

  StructReader asReader() const;
  // Gets a StructReader pointing at the same memory.

private:
  SegmentBuilder* segment;     // Memory segment in which the struct resides.
  void* data;                  // Pointer to the encoded data.
  WirePointer* pointers;   // Pointer to the encoded pointers.

  BitCount32 dataSize;
  // Size of data section.  We use a bit count rather than a word count to more easily handle the
  // case of struct lists encoded with less than a word per element.

  WirePointerCount16 pointerCount;  // Size of the pointer section.

  BitCount8 bit0Offset;
  // A special hack:  If dataSize == 1 bit, then bit0Offset is the offset of that bit within the
  // byte pointed to by `data`.  In all other cases, this is zero.  This is needed to implement
  // struct lists where each struct is one bit.

  inline StructBuilder(SegmentBuilder* segment, void* data, WirePointer* pointers,
                       BitCount dataSize, WirePointerCount pointerCount, BitCount8 bit0Offset)
      : segment(segment), data(data), pointers(pointers),
        dataSize(dataSize), pointerCount(pointerCount), bit0Offset(bit0Offset) {}

  friend class ListBuilder;
  friend struct WireHelpers;
};

class StructReader {
public:
  inline StructReader()
      : segment(nullptr), data(nullptr), pointers(nullptr), dataSize(0),
        pointerCount(0), bit0Offset(0), nestingLimit(0x7fffffff) {}

  static StructReader readRootUnchecked(const word* location);
  static StructReader readRoot(const word* location, SegmentReader* segment, int nestingLimit);

  inline BitCount getDataSectionSize() const { return dataSize; }
  inline WirePointerCount getPointerSectionSize() const { return pointerCount; }
  inline Data::Reader getDataSectionAsBlob();

  template <typename T>
  KJ_ALWAYS_INLINE(T getDataField(ElementCount offset) const);
  // Get the data field value of the given type at the given offset.  The offset is measured in
  // multiples of the field size, determined by the type.  Returns zero if the offset is past the
  // end of the struct's data section.

  template <typename T>
  KJ_ALWAYS_INLINE(
      T getDataField(ElementCount offset, Mask<T> mask) const);
  // Like getDataField(offset), but applies the given XOR mask to the result.  Used for reading
  // fields with non-zero default values.

  StructReader getStructField(WirePointerCount ptrIndex, const word* defaultValue) const;
  // Get the struct field at the given index in the pointer section, or the default value if not
  // initialized.  defaultValue will be interpreted as a flat message -- it must point at a
  // struct pointer, which in turn points at the struct value.  The default value is allowed to
  // be null, in which case an empty struct is used.

  ListReader getListField(WirePointerCount ptrIndex, FieldSize expectedElementSize,
                          const word* defaultValue) const;
  // Get the list field at the given index in the pointer section, or the default value if not
  // initialized.  The default value is allowed to be null, in which case an empty list is used.

  template <typename T>
  typename T::Reader getBlobField(WirePointerCount ptrIndex,
                                  const void* defaultValue, ByteCount defaultSize) const;
  // Gets the text or data field, or the given default value if not initialized.

  ObjectReader getObjectField(WirePointerCount ptrIndex, const word* defaultValue) const;
  // Read a pointer of arbitrary type.

  const word* getUncheckedPointer(WirePointerCount ptrIndex) const;
  // If this is an unchecked message, get a word* pointing at the location of the pointer.  This
  // word* can actually be passed to readUnchecked() to read the designated sub-object later.  If
  // this isn't an unchecked message, throws an exception.

  bool isPointerFieldNull(WirePointerCount ptrIndex) const;

  WordCount64 totalSize() const;
  // Return the total size of the struct and everything to which it points.  Does not count far
  // pointer overhead.  This is useful for deciding how much space is needed to copy the struct
  // into a flat array.  However, the caller is advised NOT to treat this value as secure.  Instead,
  // use the result as a hint for allocating the first segment, do the copy, and then throw an
  // exception if it overruns.

private:
  SegmentReader* segment;  // Memory segment in which the struct resides.

  const void* data;
  const WirePointer* pointers;

  BitCount32 dataSize;
  // Size of data section.  We use a bit count rather than a word count to more easily handle the
  // case of struct lists encoded with less than a word per element.

  WirePointerCount16 pointerCount;  // Size of the pointer section.

  BitCount8 bit0Offset;
  // A special hack:  If dataSize == 1 bit, then bit0Offset is the offset of that bit within the
  // byte pointed to by `data`.  In all other cases, this is zero.  This is needed to implement
  // struct lists where each struct is one bit.
  //
  // TODO(someday):  Consider packing this together with dataSize, since we have 10 extra bits
  //   there doing nothing -- or arguably 12 bits, if you consider that 2-bit and 4-bit sizes
  //   aren't allowed.  Consider that we could have a method like getDataSizeIn<T>() which is
  //   specialized to perform the correct shifts for each size.

  int nestingLimit;
  // Limits the depth of message structures to guard against stack-overflow-based DoS attacks.
  // Once this reaches zero, further pointers will be pruned.
  // TODO(perf):  Limit to 8 bits for better alignment?

  inline StructReader(SegmentReader* segment, const void* data, const WirePointer* pointers,
                      BitCount dataSize, WirePointerCount pointerCount, BitCount8 bit0Offset,
                      int nestingLimit)
      : segment(segment), data(data), pointers(pointers),
        dataSize(dataSize), pointerCount(pointerCount), bit0Offset(bit0Offset),
        nestingLimit(nestingLimit) {}

  friend class ListReader;
  friend class StructBuilder;
  friend struct WireHelpers;
};

// -------------------------------------------------------------------

class ListBuilder: public kj::DisallowConstCopy {
public:
  inline ListBuilder()
      : segment(nullptr), ptr(nullptr), elementCount(0 * ELEMENTS),
        step(0 * BITS / ELEMENTS) {}

  inline ElementCount size() const;
  // The number of elements in the list.

  Text::Builder asText();
  Data::Builder asData();
  // Reinterpret the list as a blob.  Throws an exception if the elements are not byte-sized.

  template <typename T>
  KJ_ALWAYS_INLINE(T getDataElement(ElementCount index));
  // Get the element of the given type at the given index.

  template <typename T>
  KJ_ALWAYS_INLINE(void setDataElement(
      ElementCount index, kj::NoInfer<T> value));
  // Set the element at the given index.

  StructBuilder getStructElement(ElementCount index);
  // Get the struct element at the given index.

  ListBuilder initListElement(
      ElementCount index, FieldSize elementSize, ElementCount elementCount);
  // Create a new list element of the given size at the given index.  All elements are initialized
  // to zero.

  ListBuilder initStructListElement(ElementCount index, ElementCount elementCount,
                                    StructSize size);
  // Allocates a new list of the given size for the field at the given index in the pointer
  // segment, and return a pointer to it.  Each element is initialized to its empty state.

  ListBuilder getListElement(ElementCount index, FieldSize elementSize);
  // Get the existing list element at the given index, making sure it is suitable for storing
  // non-struct elements of the given size.  Returns an empty list if the element is not
  // initialized.

  ListBuilder getStructListElement(ElementCount index, StructSize elementSize);
  // Get the existing list element at the given index, making sure it is suitable for storing
  // struct elements of the given size.  Returns an empty list if the element is not
  // initialized.

  template <typename T>
  typename T::Builder initBlobElement(ElementCount index, ByteCount size);
  // Initialize a Text or Data element to the given size in bytes (not including NUL terminator for
  // Text) and return a Text::Builder which can be used to fill in the content.

  template <typename T>
  void setBlobElement(ElementCount index, typename T::Reader value);
  // Set the blob element to a copy of the given blob.

  template <typename T>
  typename T::Builder getBlobElement(ElementCount index);
  // Get the blob element.  If it is not initialized, return an empty blob builder.

  ObjectBuilder getObjectElement(ElementCount index);
  // Gets a pointer element of arbitrary type.

  void setListElement(ElementCount index, ListReader value);
  void setObjectElement(ElementCount index, ObjectReader value);
  // Sets a pointer element to a deep copy of the given value.

  ListReader asReader() const;
  // Get a ListReader pointing at the same memory.

private:
  SegmentBuilder* segment;  // Memory segment in which the list resides.

  byte* ptr;  // Pointer to list content.

  ElementCount elementCount;  // Number of elements in the list.

  decltype(BITS / ELEMENTS) step;
  // The distance between elements.

  BitCount32 structDataSize;
  WirePointerCount16 structPointerCount;
  // The struct properties to use when interpreting the elements as structs.  All lists can be
  // interpreted as struct lists, so these are always filled in.

  inline ListBuilder(SegmentBuilder* segment, void* ptr,
                     decltype(BITS / ELEMENTS) step, ElementCount size,
                     BitCount structDataSize, WirePointerCount structPointerCount)
      : segment(segment), ptr(reinterpret_cast<byte*>(ptr)),
        elementCount(size), step(step), structDataSize(structDataSize),
        structPointerCount(structPointerCount) {}

  friend class StructBuilder;
  friend struct WireHelpers;
};

class ListReader {
public:
  inline ListReader()
      : segment(nullptr), ptr(nullptr), elementCount(0), step(0 * BITS / ELEMENTS),
        structDataSize(0), structPointerCount(0), nestingLimit(0x7fffffff) {}

  inline ElementCount size() const;
  // The number of elements in the list.

  Text::Reader asText();
  Data::Reader asData();
  // Reinterpret the list as a blob.  Throws an exception if the elements are not byte-sized.

  template <typename T>
  KJ_ALWAYS_INLINE(T getDataElement(ElementCount index) const);
  // Get the element of the given type at the given index.

  StructReader getStructElement(ElementCount index) const;
  // Get the struct element at the given index.

  ListReader getListElement(ElementCount index, FieldSize expectedElementSize) const;
  // Get the list element at the given index.

  template <typename T>
  typename T::Reader getBlobElement(ElementCount index) const;
  // Gets the text or data field.  If it is not initialized, returns an empty blob reader.

  ObjectReader getObjectElement(ElementCount index) const;
  // Gets a pointer element of arbitrary type.

private:
  SegmentReader* segment;  // Memory segment in which the list resides.

  const byte* ptr;  // Pointer to list content.

  ElementCount elementCount;  // Number of elements in the list.

  decltype(BITS / ELEMENTS) step;
  // The distance between elements.

  BitCount32 structDataSize;
  WirePointerCount16 structPointerCount;
  // The struct properties to use when interpreting the elements as structs.  All lists can be
  // interpreted as struct lists, so these are always filled in.

  int nestingLimit;
  // Limits the depth of message structures to guard against stack-overflow-based DoS attacks.
  // Once this reaches zero, further pointers will be pruned.

  inline ListReader(SegmentReader* segment, const void* ptr,
                    ElementCount elementCount, decltype(BITS / ELEMENTS) step,
                    BitCount structDataSize, WirePointerCount structPointerCount,
                    int nestingLimit)
      : segment(segment), ptr(reinterpret_cast<const byte*>(ptr)), elementCount(elementCount),
        step(step), structDataSize(structDataSize),
        structPointerCount(structPointerCount), nestingLimit(nestingLimit) {}

  friend class StructReader;
  friend class ListBuilder;
  friend struct WireHelpers;
};

// -------------------------------------------------------------------

enum class ObjectKind {
  NULL_POINTER,   // Object was read from a null pointer.
  STRUCT,
  LIST
};

struct ObjectBuilder {
  // A reader for any kind of object.

  ObjectKind kind;

  union {
    StructBuilder structBuilder;
    ListBuilder listBuilder;
  };

  ObjectBuilder(): kind(ObjectKind::NULL_POINTER), structBuilder() {}
  ObjectBuilder(StructBuilder structBuilder)
      : kind(ObjectKind::STRUCT), structBuilder(structBuilder) {}
  ObjectBuilder(ListBuilder listBuilder)
      : kind(ObjectKind::LIST), listBuilder(listBuilder) {}
};

struct ObjectReader {
  // A reader for any kind of object.

  ObjectKind kind;

  union {
    StructReader structReader;
    ListReader listReader;
  };

  ObjectReader(): kind(ObjectKind::NULL_POINTER), structReader() {}
  ObjectReader(StructReader structReader)
      : kind(ObjectKind::STRUCT), structReader(structReader) {}
  ObjectReader(ListReader listReader)
      : kind(ObjectKind::LIST), listReader(listReader) {}
};

// =======================================================================================
// Internal implementation details...

inline Data::Builder StructBuilder::getDataSectionAsBlob() {
  return Data::Builder(reinterpret_cast<byte*>(data), dataSize / BITS_PER_BYTE / BYTES);
}

template <typename T>
inline T StructBuilder::getDataField(ElementCount offset) {
  return reinterpret_cast<WireValue<T>*>(data)[offset / ELEMENTS].get();
}

template <>
inline bool StructBuilder::getDataField<bool>(ElementCount offset) {
  // This branch should be compiled out whenever this is inlined with a constant offset.
  BitCount boffset = (offset == 0 * ELEMENTS) ?
      BitCount(bit0Offset) : offset * (1 * BITS / ELEMENTS);
  byte* b = reinterpret_cast<byte*>(data) + boffset / BITS_PER_BYTE;
  return (*reinterpret_cast<uint8_t*>(b) & (1 << (boffset % BITS_PER_BYTE / BITS))) != 0;
}

template <>
inline Void StructBuilder::getDataField<Void>(ElementCount offset) {
  return Void::VOID;
}

template <typename T>
inline T StructBuilder::getDataField(ElementCount offset, Mask<T> mask) {
  return unmask<T>(getDataField<Mask<T> >(offset), mask);
}

template <typename T>
inline void StructBuilder::setDataField(ElementCount offset, kj::NoInfer<T> value) {
  reinterpret_cast<WireValue<T>*>(data)[offset / ELEMENTS].set(value);
}

template <>
inline void StructBuilder::setDataField<bool>(ElementCount offset, bool value) {
  // This branch should be compiled out whenever this is inlined with a constant offset.
  BitCount boffset = (offset == 0 * ELEMENTS) ?
      BitCount(bit0Offset) : offset * (1 * BITS / ELEMENTS);
  byte* b = reinterpret_cast<byte*>(data) + boffset / BITS_PER_BYTE;
  uint bitnum = boffset % BITS_PER_BYTE / BITS;
  *reinterpret_cast<uint8_t*>(b) = (*reinterpret_cast<uint8_t*>(b) & ~(1 << bitnum))
                                 | (static_cast<uint8_t>(value) << bitnum);
}

template <>
inline void StructBuilder::setDataField<Void>(ElementCount offset, Void value) {}

template <typename T>
inline void StructBuilder::setDataField(ElementCount offset, kj::NoInfer<T> value, Mask<T> m) {
  setDataField<Mask<T> >(offset, mask<T>(value, m));
}

// -------------------------------------------------------------------

inline Data::Reader StructReader::getDataSectionAsBlob() {
  return Data::Reader(reinterpret_cast<const byte*>(data), dataSize / BITS_PER_BYTE / BYTES);
}

template <typename T>
T StructReader::getDataField(ElementCount offset) const {
  if ((offset + 1 * ELEMENTS) * capnp::bitsPerElement<T>() <= dataSize) {
    return reinterpret_cast<const WireValue<T>*>(data)[offset / ELEMENTS].get();
  } else {
    return static_cast<T>(0);
  }
}

template <>
inline bool StructReader::getDataField<bool>(ElementCount offset) const {
  BitCount boffset = offset * (1 * BITS / ELEMENTS);
  if (boffset < dataSize) {
    // This branch should be compiled out whenever this is inlined with a constant offset.
    if (offset == 0 * ELEMENTS) {
      boffset = bit0Offset;
    }
    const byte* b = reinterpret_cast<const byte*>(data) + boffset / BITS_PER_BYTE;
    return (*reinterpret_cast<const uint8_t*>(b) & (1 << (boffset % BITS_PER_BYTE / BITS))) != 0;
  } else {
    return false;
  }
}

template <>
inline Void StructReader::getDataField<Void>(ElementCount offset) const {
  return Void::VOID;
}

template <typename T>
T StructReader::getDataField(ElementCount offset, Mask<T> mask) const {
  return unmask<T>(getDataField<Mask<T> >(offset), mask);
}

// -------------------------------------------------------------------

inline ElementCount ListBuilder::size() const { return elementCount; }

template <typename T>
inline T ListBuilder::getDataElement(ElementCount index) {
  return reinterpret_cast<WireValue<T>*>(ptr + index * step / BITS_PER_BYTE)->get();

  // TODO(soon):  Benchmark this alternate implementation, which I suspect may make better use of
  //   the x86 SIB byte.  Also use it for all the other getData/setData implementations below, and
  //   the various non-inline methods that look up pointers.
  //   Also if using this, consider changing ptr back to void* instead of byte*.
//  return reinterpret_cast<WireValue<T>*>(ptr)[
//      index / ELEMENTS * (step / capnp::bitsPerElement<T>())].get();
}

template <>
inline bool ListBuilder::getDataElement<bool>(ElementCount index) {
  // Ignore stepBytes for bit lists because bit lists cannot be upgraded to struct lists.
  BitCount bindex = index * step;
  byte* b = ptr + bindex / BITS_PER_BYTE;
  return (*reinterpret_cast<uint8_t*>(b) & (1 << (bindex % BITS_PER_BYTE / BITS))) != 0;
}

template <>
inline Void ListBuilder::getDataElement<Void>(ElementCount index) {
  return Void::VOID;
}

template <typename T>
inline void ListBuilder::setDataElement(ElementCount index, kj::NoInfer<T> value) {
  reinterpret_cast<WireValue<T>*>(ptr + index * step / BITS_PER_BYTE)->set(value);
}

template <>
inline void ListBuilder::setDataElement<bool>(ElementCount index, bool value) {
  // Ignore stepBytes for bit lists because bit lists cannot be upgraded to struct lists.
  BitCount bindex = index * (1 * BITS / ELEMENTS);
  byte* b = ptr + bindex / BITS_PER_BYTE;
  uint bitnum = bindex % BITS_PER_BYTE / BITS;
  *reinterpret_cast<uint8_t*>(b) = (*reinterpret_cast<uint8_t*>(b) & ~(1 << bitnum))
                                 | (static_cast<uint8_t>(value) << bitnum);
}

template <>
inline void ListBuilder::setDataElement<Void>(ElementCount index, Void value) {}

// -------------------------------------------------------------------

inline ElementCount ListReader::size() const { return elementCount; }

template <typename T>
inline T ListReader::getDataElement(ElementCount index) const {
  return reinterpret_cast<const WireValue<T>*>(ptr + index * step / BITS_PER_BYTE)->get();
}

template <>
inline bool ListReader::getDataElement<bool>(ElementCount index) const {
  // Ignore stepBytes for bit lists because bit lists cannot be upgraded to struct lists.
  BitCount bindex = index * step;
  const byte* b = ptr + bindex / BITS_PER_BYTE;
  return (*reinterpret_cast<const uint8_t*>(b) & (1 << (bindex % BITS_PER_BYTE / BITS))) != 0;
}

template <>
inline Void ListReader::getDataElement<Void>(ElementCount index) const {
  return Void::VOID;
}

// These are defined in the source file.
template <> typename Text::Builder StructBuilder::initBlobField<Text>(WirePointerCount ptrIndex, ByteCount size);
template <> void StructBuilder::setBlobField<Text>(WirePointerCount ptrIndex, typename Text::Reader value);
template <> typename Text::Builder StructBuilder::getBlobField<Text>(WirePointerCount ptrIndex, const void* defaultValue, ByteCount defaultSize);
template <> typename Text::Reader StructReader::getBlobField<Text>(WirePointerCount ptrIndex, const void* defaultValue, ByteCount defaultSize) const;
template <> typename Text::Builder ListBuilder::initBlobElement<Text>(ElementCount index, ByteCount size);
template <> void ListBuilder::setBlobElement<Text>(ElementCount index, typename Text::Reader value);
template <> typename Text::Builder ListBuilder::getBlobElement<Text>(ElementCount index);
template <> typename Text::Reader ListReader::getBlobElement<Text>(ElementCount index) const;

template <> typename Data::Builder StructBuilder::initBlobField<Data>(WirePointerCount ptrIndex, ByteCount size);
template <> void StructBuilder::setBlobField<Data>(WirePointerCount ptrIndex, typename Data::Reader value);
template <> typename Data::Builder StructBuilder::getBlobField<Data>(WirePointerCount ptrIndex, const void* defaultValue, ByteCount defaultSize);
template <> typename Data::Reader StructReader::getBlobField<Data>(WirePointerCount ptrIndex, const void* defaultValue, ByteCount defaultSize) const;
template <> typename Data::Builder ListBuilder::initBlobElement<Data>(ElementCount index, ByteCount size);
template <> void ListBuilder::setBlobElement<Data>(ElementCount index, typename Data::Reader value);
template <> typename Data::Builder ListBuilder::getBlobElement<Data>(ElementCount index);
template <> typename Data::Reader ListReader::getBlobElement<Data>(ElementCount index) const;

}  // namespace internal
}  // namespace capnp

#endif  // CAPNP_LAYOUT_H_
