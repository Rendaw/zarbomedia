#ifndef hash_h
#define hash_h

#include "type.h"
#include "filesystem.h"
using PathT = Filesystem::PathT;

#include <array>

typedef std::array<uint8_t, 16> HashT;

std::string FormatHash(HashT const &Hash);

OptionalT<HashT> UnformatHash(char const *String);

OptionalT<std::pair<HashT, size_t>> HashFile(PathT const &Path);

#endif
