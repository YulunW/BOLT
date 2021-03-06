//===--- BinarySection.h - Interface for object file section --------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_BINARY_SECTION_H
#define LLVM_TOOLS_LLVM_BOLT_BINARY_SECTION_H

#include "Relocation.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Triple.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
#include <map>

namespace llvm {

using namespace object;

namespace bolt {

class BinaryContext;
class BinaryData;

/// A class to manage binary sections that also manages related relocations.
class BinarySection {
  friend class BinaryContext;

  BinaryContext &BC;          // Owning BinaryContext
  const std::string Name;     // Section name
  const SectionRef Section;   // SectionRef (may be null)
  StringRef Contents;         // input section contents
  const uint64_t Address;     // address of section in input binary (may be 0)
  const uint64_t Size;        // input section size
  unsigned Alignment;         // alignment in bytes (must be > 0)
  unsigned ELFType;           // ELF section type
  unsigned ELFFlags;          // ELF section flags

  // Relocations associated with this section.  Relocation offsets are
  // wrt. to the original section address and size.
  using RelocationSetType = std::set<Relocation>;
  RelocationSetType Relocations;

  // Pending relocations for this section.  For the moment, just used by
  // the .debug_info section.
  RelocationSetType PendingRelocations;

  // Output info
  bool IsFinalized{false};         // Has this section had output information
                                   // finalized?
  std::string OutputName;          // Output section name (if the section has
                                   // been renamed)
  uint64_t OutputAddress{0};       // Section address for the rewritten binary.
  uint64_t OutputSize{0};          // Section size in the rewritten binary.
  uint64_t FileOffset{0};          // File offset in the rewritten binary file.
  StringRef OutputContents;        // Rewritten section contents.
  unsigned SectionID{-1u};         // Unique ID used for address mapping.
                                   // Set by ExecutableFileMemoryManager.
  uint32_t Index{0};               // Section index in the output file.
  mutable bool IsReordered{false}; // Have the contents been reordered?
  bool IsAnonymous{false};         // True if the name should not be included
                                   // in the output file.

  uint64_t hash(const BinaryData &BD,
                std::map<const BinaryData *, uint64_t> &Cache) const;

  // non-copyable
  BinarySection(const BinarySection &) = delete;
  BinarySection(BinarySection &&) = delete;
  BinarySection &operator=(const BinarySection &) = delete;
  BinarySection &operator=(BinarySection &&) = delete;

  static StringRef getName(SectionRef Section) {
    StringRef Name;
    Section.getName(Name);
    return Name;
  }
  static StringRef getContents(SectionRef Section) {
    StringRef Contents;
    if (Section.getObject()->isELF() && ELFSectionRef(Section).getType() == ELF::SHT_NOBITS)
      return Contents;

    if (auto EC = Section.getContents(Contents)) {
      errs() << "BOLT-ERROR: cannot get section contents for "
             << getName(Section) << ": " << EC.message() << ".\n";
      exit(1);
    }
    return Contents;
  }

  /// Get the set of relocations refering to data in this section that
  /// has been reordered.  The relocation offsets will be modified to
  /// reflect the new data locations.
  std::set<Relocation> reorderRelocations(bool Inplace) const;

  /// Set output info for this section.
  void update(uint8_t *NewData,
              uint64_t NewSize,
              unsigned NewAlignment,
              unsigned NewELFType,
              unsigned NewELFFlags) {
    assert(NewAlignment > 0 && "section alignment must be > 0");
    Alignment = NewAlignment;
    ELFType = NewELFType;
    ELFFlags = NewELFFlags;
    OutputSize = NewSize;
    OutputContents = StringRef(reinterpret_cast<const char*>(NewData),
                               NewData ? NewSize : 0);
    IsFinalized = true;
  }
public:
  /// Copy a section.
  explicit BinarySection(BinaryContext &BC,
                         StringRef Name,
                         const BinarySection &Section)
    : BC(BC),
      Name(Name),
      Section(Section.getSectionRef()),
      Contents(Section.getContents()),
      Address(Section.getAddress()),
      Size(Section.getSize()),
      Alignment(Section.getAlignment()),
      ELFType(Section.getELFType()),
      ELFFlags(Section.getELFFlags()),
      Relocations(Section.Relocations),
      PendingRelocations(Section.PendingRelocations),
      OutputName(Name) {
  }

  BinarySection(BinaryContext &BC,
                SectionRef Section)
    : BC(BC),
      Name(getName(Section)),
      Section(Section),
      Contents(getContents(Section)),
      Address(Section.getAddress()),
      Size(Section.getSize()),
      Alignment(Section.getAlignment()),
      OutputName(Name) {
    if (Section.getObject()->isELF()) {
      ELFType = ELFSectionRef(Section).getType();
      ELFFlags = ELFSectionRef(Section).getFlags();
    }
  }

  // TODO: pass Data as StringRef/ArrayRef? use StringRef::copy method.
  BinarySection(BinaryContext &BC,
                StringRef Name,
                uint8_t *Data,
                uint64_t Size,
                unsigned Alignment,
                unsigned ELFType,
                unsigned ELFFlags)
    : BC(BC),
      Name(Name),
      Contents(reinterpret_cast<const char*>(Data), Data ? Size : 0),
      Address(0),
      Size(Size),
      Alignment(Alignment),
      ELFType(ELFType),
      ELFFlags(ELFFlags),
      IsFinalized(true),
      OutputName(Name),
      OutputSize(Size),
      OutputContents(Contents) {
    assert(Alignment > 0 && "section alignment must be > 0");
  }

  ~BinarySection();

  /// Helper function to generate the proper ELF flags from section properties.
  static unsigned getFlags(bool IsReadOnly = true,
                           bool IsText = false,
                           bool IsAllocatable = false) {
    unsigned Flags = 0;
    if (IsAllocatable)
      Flags |= ELF::SHF_ALLOC;
    if (!IsReadOnly)
      Flags |= ELF::SHF_WRITE;
    if (IsText)
      Flags |= ELF::SHF_EXECINSTR;
    return Flags;
  }

  operator bool() const {
    return ELFType != ELF::SHT_NULL;
  }

  bool operator==(const BinarySection &Other) const {
    return (Name == Other.Name &&
            Address == Other.Address &&
            Size == Other.Size &&
            getData() == Other.getData() &&
            Alignment == Other.Alignment &&
            ELFType == Other.ELFType &&
            ELFFlags == Other.ELFFlags);
  }

  bool operator!=(const BinarySection &Other) const {
    return !operator==(Other);
  }

  // Order sections by their immutable properties.
  bool operator<(const BinarySection &Other) const {
    return (getAddress() < Other.getAddress() ||
            (getAddress() == Other.getAddress() &&
             (getSize() < Other.getSize() ||
              (getSize() == Other.getSize() &&
               getName() < Other.getName()))));
  }

  ///
  /// Basic proprety access.
  ///
  bool isELF() const;
  StringRef getName() const { return Name; }
  uint64_t getAddress() const { return Address; }
  uint64_t getEndAddress() const { return Address + Size; }
  uint64_t getSize() const { return Size; }
  uint64_t getAlignment() const { return Alignment; }
  bool isText() const {
    if (isELF())
      return (ELFFlags & ELF::SHF_EXECINSTR);
    return getSectionRef().isText();
  }
  bool isData() const {
    if (isELF())
      return (ELFType == ELF::SHT_PROGBITS &&
              (ELFFlags & (ELF::SHF_ALLOC | ELF::SHF_WRITE)));
    return getSectionRef().isData();
  }
  bool isBSS() const {
    return (ELFType == ELF::SHT_NOBITS &&
            (ELFFlags & (ELF::SHF_ALLOC | ELF::SHF_WRITE)));
  }
  bool isTLS() const {
    return (ELFFlags & ELF::SHF_TLS);
  }
  bool isTBSS() const {
    return isBSS() && isTLS();
  }
  bool isVirtual() const { return ELFType == ELF::SHT_NOBITS; }
  bool isRela() const { return ELFType == ELF::SHT_RELA; }
  bool isReadOnly() const {
    return ((ELFFlags & ELF::SHF_ALLOC) &&
            !(ELFFlags & ELF::SHF_WRITE) &&
            ELFType == ELF::SHT_PROGBITS);
  }
  bool isAllocatable() const {
    return (ELFFlags & ELF::SHF_ALLOC) && !isTBSS();
  }
  bool isReordered() const { return IsReordered; }
  bool isAnonymous() const { return IsAnonymous; }
  unsigned getELFType() const { return ELFType; }
  unsigned getELFFlags() const { return ELFFlags; }

  uint8_t *getData() {
    return reinterpret_cast<uint8_t *>(const_cast<char *>(getContents().data()));
  }
  const uint8_t *getData() const {
    return reinterpret_cast<const uint8_t *>(getContents().data());
  }
  StringRef getContents() const { return Contents; }
  void clearContents() { Contents = {}; }
  bool hasSectionRef() const { return Section != SectionRef(); }
  SectionRef getSectionRef() const { return Section; }

  /// Does this section contain the given \p Address?
  /// Note: this is in terms of the original mapped binary addresses.
  bool containsAddress(uint64_t Address) const {
    return (getAddress() <= Address && Address < getEndAddress()) ||
           (getSize() == 0 && getAddress() == Address);
  }

  /// Does this section contain the range [\p Address, \p Address + \p Size)?
  /// Note: this is in terms of the original mapped binary addresses.
  bool containsRange(uint64_t Address, uint64_t Size) const {
    return containsAddress(Address) && Address + Size <= getEndAddress();
  }

  /// Iterate over all non-pending relocations for this section.
  iterator_range<RelocationSetType::iterator> relocations() {
    return make_range(Relocations.begin(), Relocations.end());
  }

  /// Iterate over all non-pending relocations for this section.
  iterator_range<RelocationSetType::const_iterator> relocations() const {
    return make_range(Relocations.begin(), Relocations.end());
  }

  /// Does this section have any non-pending relocations?
  bool hasRelocations() const {
    return !Relocations.empty();
  }

  /// Iterate over all pending relocations in this section.
  iterator_range<RelocationSetType::const_iterator> pendingRelocations() const {
    return make_range(PendingRelocations.begin(), PendingRelocations.end());
  }

  /// Does this section have any pending relocations?
  bool hasPendingRelocations() const {
    return !PendingRelocations.empty();
  }

  /// Remove non-pending relocation with the given /p Offset.
  bool removeRelocationAt(uint64_t Offset) {
    Relocation Key{Offset, 0, 0, 0, 0};
    auto Itr = Relocations.find(Key);
    if (Itr != Relocations.end()) {
      Relocations.erase(Itr);
      return true;
    }
    return false;
  }

  /// Add a new relocation at the given /p Offset.  Note: pending relocations
  /// are only used by .debug_info and should eventually go away.
  void addRelocation(uint64_t Offset,
                     MCSymbol *Symbol,
                     uint64_t Type,
                     uint64_t Addend,
                     uint64_t Value = 0,
                     bool Pending = false) {
    assert(Offset < getSize() && "offset not within section bounds");
    if (!Pending) {
      Relocations.emplace(Relocation{Offset, Symbol, Type, Addend, Value});
    } else {
      PendingRelocations.emplace(
          Relocation{Offset, Symbol, Type, Addend, Value});
    }
  }

  /// Lookup the relocation (if any) at the given /p Offset.
  const Relocation *getRelocationAt(uint64_t Offset) const {
    Relocation Key{Offset, 0, 0, 0, 0};
    auto Itr = Relocations.find(Key);
    return Itr != Relocations.end() ? &*Itr : nullptr;
  }

  uint64_t hash(const BinaryData &BD) const {
    std::map<const BinaryData *, uint64_t> Cache;
    return hash(BD, Cache);
  }

  ///
  /// Property accessors related to output data.
  ///

  bool isFinalized() const { return IsFinalized; }
  void setIsFinalized() { IsFinalized = true; }
  StringRef getOutputName() const { return OutputName; }
  uint64_t getOutputSize() const { return OutputSize; }
  uint8_t *getOutputData() {
    return reinterpret_cast<uint8_t *>(const_cast<char *>(getOutputContents().data()));
  }
  const uint8_t *getOutputData() const {
    return reinterpret_cast<const uint8_t *>(getOutputContents().data());
  }
  StringRef getOutputContents() const { return OutputContents; }
  uint64_t getAllocAddress() const {
    return reinterpret_cast<uint64_t>(getOutputData());
  }
  uint64_t getOutputAddress() const { return OutputAddress; }
  uint64_t getFileOffset() const { return FileOffset; }
  unsigned getSectionID() const {
    assert(hasValidSectionID() && "trying to use uninitialized section id");
    return SectionID;
  }
  bool hasValidSectionID() const {
    return SectionID != -1u;
  }
  uint32_t getIndex() const {
    return Index;
  }

  // mutation
  void setOutputAddress(uint64_t Address) {
    OutputAddress = Address;
  }
  void setFileOffset(uint64_t Offset) {
    FileOffset = Offset;
  }
  void setSectionID(unsigned ID) {
    assert(!hasValidSectionID() && "trying to set section id twice");
    SectionID = ID;
  }
  void setIndex(uint32_t I) {
    Index = I;
  }
  void setOutputName(StringRef Name) {
    OutputName = Name;
  }
  void setAnonymous(bool Flag) {
    IsAnonymous = Flag;
  }

  /// Emit the section as data, possibly with relocations. Use name \p NewName
  //  for the section during emission if non-empty.
  void emitAsData(MCStreamer &Streamer, StringRef NewName = StringRef()) const;

  /// Flush all pending relocations to the emitted section.
  void flushPendingRelocations(raw_pwrite_stream &OS);

  /// Reorder the contents of this section according to /p Order.  If
  /// /p Inplace is true, the entire contents of the section is reordered,
  /// otherwise the new contents contain only the reordered data.
  void reorderContents(const std::vector<BinaryData *> &Order, bool Inplace);

  void print(raw_ostream &OS) const;

  /// Write the contents of an ELF note section given the name of the producer,
  /// a number identifying the type of note and the contents of the note in
  /// \p DescStr.
  static std::string encodeELFNote(StringRef NameStr, StringRef DescStr,
                                   uint32_t Type);

  /// Code for ELF notes written by producer 'BOLT'
  enum {
    NT_BOLT_BAT = 1,
    NT_BOLT_INSTRUMENTATION_TABLES = 2
  };
};

inline uint8_t *copyByteArray(const uint8_t *Data, uint64_t Size) {
  auto Array = new uint8_t[Size];
  memcpy(Array, Data, Size);
  return Array;
}

inline uint8_t *copyByteArray(StringRef Buffer) {
  return copyByteArray(reinterpret_cast<const uint8_t*>(Buffer.data()),
                       Buffer.size());
}

inline uint8_t *copyByteArray(ArrayRef<char> Buffer) {
  return copyByteArray(reinterpret_cast<const uint8_t*>(Buffer.data()),
                       Buffer.size());
}

inline raw_ostream &operator<<(raw_ostream &OS, const BinarySection &Section) {
  Section.print(OS);
  return OS;
}

struct SDTMarkerInfo {
  uint64_t PC;
  uint64_t Base;
  uint64_t Semaphore;
  StringRef Provider;
  StringRef Name;
  StringRef Args;

  /// The offset of PC within the note section
  unsigned PCOffset;
};

} // namespace bolt
} // namespace llvm

#endif
