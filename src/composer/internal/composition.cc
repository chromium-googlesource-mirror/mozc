// Copyright 2010, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "composer/internal/composition.h"

#include "base/util.h"
#include "composer/internal/char_chunk.h"
#include "composer/internal/transliterators.h"
#include "composer/table.h"

namespace mozc {
namespace composer {

namespace {
const TransliteratorInterface *kNullT12r = NULL;
}  // namespace

Composition::Composition()
    : input_t12r_(NULL) {}

Composition::~Composition() {
  Erase();
}

void Composition::Erase() {
  CharChunkList::iterator it;
  for (it = chunks_.begin(); it != chunks_.end(); ++it) {
    delete *it;
  }
  chunks_.clear();
}

size_t Composition::InsertAt(size_t pos, const string &input) {
  if (input.empty()) {
    return pos;
  }

  CharChunkList::iterator it;
  MaybeSplitChunkAt(pos, &it);

  CharChunk *chunk = GetInsertionChunk(&it);
  string key = input;
  while (true) {
    chunk->AddInput(*table_, &key);
    if (key.empty()) {
      break;
    }
    chunk = InsertChunk(&it);
  }
  return GetPosition(kNullT12r, it);
}

size_t Composition::InsertKeyAndPreeditAt(const size_t pos,
                                          const string &key,
                                          const string &preedit) {
  if (key.empty() && preedit.empty()) {
    return pos;
  }

  CharChunkList::iterator it;
  MaybeSplitChunkAt(pos, &it);

  CharChunk *chunk = GetInsertionChunk(&it);
  string raw_char = key;
  string converted_char = preedit;
  while (true) {
    chunk->AddInputAndConvertedChar(*table_, &raw_char, &converted_char);
    if (raw_char.empty() && converted_char.empty()) {
      break;
    }
    chunk = InsertChunk(&it);
  }
  return GetPosition(kNullT12r, it);
}

// Deletes a right-hand character of the composition.
size_t Composition::DeleteAt(const size_t position) {
  CharChunkList::iterator chunk_it;
  MaybeSplitChunkAt(position, &chunk_it);
  const size_t new_position = GetPosition(kNullT12r, chunk_it);
  if (chunk_it == chunks_.end()) {
    return new_position;
  }

  if ((*chunk_it)->GetLength(kNullT12r) == 1) {
    delete *chunk_it;
    chunks_.erase(chunk_it);
    return new_position;
  }

  CharChunk left_deleted_chunk;
  (*chunk_it)->SplitChunk(kNullT12r, 1, &left_deleted_chunk);
  return new_position;
}

size_t Composition::ConvertPosition(
    const size_t position_from,
    const TransliteratorInterface *transliterator_from,
    const TransliteratorInterface *transliterator_to) {
  // TODO(komatsu) This is a hacky way.
  if (transliterator_from == transliterator_to) {
    return position_from;
  }

  CharChunkList::iterator chunk_it;
  size_t inner_position_from;
  GetChunkAt(position_from, transliterator_from,
             &chunk_it, &inner_position_from);

  // No chunk was found, return 0 as a fallback.
  if (chunk_it == chunks_.end()) {
    return 0;
  }

  const size_t chunk_length_from = (*chunk_it)->GetLength(transliterator_from);

  CHECK(inner_position_from <= chunk_length_from);

  const size_t position_to = GetPosition(transliterator_to, chunk_it);

  if (inner_position_from == 0) {
    return position_to;
  }

  const size_t chunk_length_to = (*chunk_it)->GetLength(transliterator_to);
  if (inner_position_from == chunk_length_from) {
    // If the inner_position_from is the end of the chunk (ex. "ka|"
    // vs "か"), the converterd position should be the end of the
    // chunk too (ie. "か|").
    return position_to + chunk_length_to;
  }


  if (inner_position_from > chunk_length_to) {
    // When inner_position_from is greater than chunk_length_to
    // (ex. "ts|u" vs "つ", inner_position_from is 2 and
    // chunk_length_to is 1), the converted position should be the end
    // of the chunk (ie. "つ|").
    return position_to + chunk_length_to;
  }

  DCHECK(inner_position_from <= chunk_length_to);
  // When inner_position_from is less than or equal to chunk_length_to
  // (ex. "っ|と" vs "tto", inner_position_from is 1 and
  // chunk_length_to is 2), the converted position is adjusted from
  // the beginning of the chunk (ie. "t|to").
  return position_to + inner_position_from;
}

size_t Composition::SetDisplayMode(
    const size_t position,
    const TransliteratorInterface *transliterator) {
  SetTransliterator(0, GetLength(), transliterator);
  SetInputMode(transliterator);
  return GetLength();
}

void Composition::SetTransliterator(
    const size_t position_from,
    const size_t position_to,
    const TransliteratorInterface *transliterator) {
  if (position_from > position_to) {
    LOG(ERROR) << "position_from should not be greater than position_to.";
    return;
  }

  CharChunkList::iterator chunk_it;
  size_t inner_position_from;
  GetChunkAt(position_from, kNullT12r, &chunk_it, &inner_position_from);

  CharChunkList::iterator end_it;
  size_t inner_position_to;
  GetChunkAt(position_to, kNullT12r, &end_it, &inner_position_to);

  // chunk_it and end_it can be the same iterator from the beginning.
  while (chunk_it != end_it) {
    (*chunk_it)->SetTransliterator(transliterator);
    ++chunk_it;
  }
  (*end_it)->SetTransliterator(transliterator);
}

const TransliteratorInterface *Composition::GetTransliterator(size_t position) {
  // Due to GetChunkAt is not a const funcion, this function cannot be
  // a const function.
  CharChunkList::iterator chunk_it;
  size_t inner_position;
  GetChunkAt(position, kNullT12r, &chunk_it, &inner_position);
  return (*chunk_it)->GetTransliterator(kNullT12r);
}

size_t Composition::GetLength() const {
  return GetPosition(kNullT12r, chunks_.end());
}

void Composition::GetStringWithModes(
    const TransliteratorInterface* transliterator,
    const TrimMode trim_mode,
    string* composition) const {
  composition->clear();
  if (chunks_.empty()) {
    LOG(WARNING) << "The composition size is zero.";
    return;
  }

  CharChunkList::const_iterator it;
  for (it = chunks_.begin(); (*it) != chunks_.back(); ++it) {
    (*it)->AppendResult(*table_, transliterator, composition);
  }
  switch (trim_mode) {
    case TRIM:
      (*it)->AppendTrimedResult(*table_, transliterator, composition);
      break;
    case ASIS:
      (*it)->AppendResult(*table_, transliterator, composition);
      break;
    case FIX:
      (*it)->AppendFixedResult(*table_, transliterator, composition);
      break;
    default:
      LOG(WARNING) << "Unexpected trim mode: " << trim_mode;
      break;
  }
}

void Composition::GetString(string *composition) const {
  composition->clear();
  if (chunks_.empty()) {
    LOG(WARNING) << "The composition size is zero.";
    return;
  }

  for (CharChunkList::const_iterator it = chunks_.begin();
       it != chunks_.end();
       ++it) {
    (*it)->AppendResult(*table_, kNullT12r, composition);
  }
}

void Composition::GetStringWithTransliterator(
    const TransliteratorInterface * transliterator,
    string* output) const {
  GetStringWithModes(transliterator, FIX, output);
}

void Composition::GetStringWithTrimMode(const TrimMode trim_mode,
                                        string* output) const {
  GetStringWithModes(kNullT12r, trim_mode, output);
}

void Composition::GetPreedit(
    size_t position, string *left, string *focused, string *right) const {
  string composition;
  GetString(&composition);
  left->assign(Util::SubString(composition, 0, position));
  focused->assign(Util::SubString(composition, position, 1));
  right->assign(Util::SubString(composition, position + 1, string::npos));
}

// This function is essentialy a const function, but chunks_.begin()
// violates the constness of this function.
void Composition::GetChunkAt(const size_t position,
                             const TransliteratorInterface *transliterator,
                             CharChunkList::iterator *chunk_it,
                             size_t *inner_position) {
  if (chunks_.empty()) {
    *inner_position = 0;
    *chunk_it = chunks_.begin();
    return;
  }

  size_t rest_pos = position;
  CharChunkList::iterator it;
  for (it = chunks_.begin(); it != chunks_.end(); ++it) {
    const size_t chunk_length = (*it)->GetLength(transliterator);
    if (rest_pos <= chunk_length) {
      *inner_position = rest_pos;
      *chunk_it = it;
      return;
    }
    rest_pos -= chunk_length;
  }
  *chunk_it = chunks_.end();
  --(*chunk_it);
  *inner_position = (**chunk_it)->GetLength(transliterator);
}

size_t Composition::GetPosition(
    const TransliteratorInterface *transliterator,
    const CharChunkList::const_iterator &cur_it) const {
  size_t position = 0;
  CharChunkList::const_iterator it;
  for (it = chunks_.begin(); it != cur_it; ++it) {
    position += (*it)->GetLength(transliterator);
  }
  return position;
}


// Return the left CharChunk and the right it.
CharChunk *Composition::MaybeSplitChunkAt(const size_t pos,
                                          CharChunkList::iterator *it) {
  // The position is the beginning of composition.
  if (pos <= 0) {
    *it = chunks_.begin();
    return NULL;
  }

  size_t inner_position;
  GetChunkAt(pos, kNullT12r, it, &inner_position);

  CharChunk *chunk = **it;
  if (inner_position == chunk->GetLength(kNullT12r)) {
    ++(*it);
    return chunk;
  }

  CharChunk *left_chunk = new CharChunk();  // Left hand of new chunks
  chunk->SplitChunk(kNullT12r, inner_position, left_chunk);
  chunks_.insert(*it, left_chunk);
  return left_chunk;
}


// Insert a chunk to the prev of it.
CharChunk *Composition::InsertChunk(CharChunkList::iterator *it) {
  CharChunk *new_chunk = new CharChunk();
  new_chunk->SetTransliterator(input_t12r_);
  chunks_.insert(*it, new_chunk);
  return new_chunk;
}

const CharChunkList &Composition::GetCharChunkList() const {
  return chunks_;
}

// Return charchunk to be inserted and iterator of the *next* char chunk.
CharChunk *Composition::GetInsertionChunk(CharChunkList::iterator *it) {
  if (*it == chunks_.begin()) {
    return InsertChunk(it);
  }

  CharChunkList::iterator left_it = *it;
  --left_it;
  if ((*left_it)->IsAppendable(input_t12r_)) {
    return *left_it;
  }
  return InsertChunk(it);
}

void Composition::SetTable(const Table *table) {
  table_ = table;
}

void Composition::SetInputMode(const TransliteratorInterface *transliterator) {
  input_t12r_ = transliterator;
}

}  // namespace composer
}  // namespace mozc