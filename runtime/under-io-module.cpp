#include "under-io-module.h"

#include "bytes-builtins.h"
#include "frame.h"
#include "frozen-modules.h"
#include "globals.h"
#include "int-builtins.h"
#include "modules.h"
#include "object-builtins.h"
#include "objects.h"
#include "os.h"
#include "runtime.h"
#include "str-builtins.h"
#include "thread.h"

namespace py {

const BuiltinFunction UnderIoModule::kBuiltinFunctions[] = {
    {SymbolId::kUnderBufferedReaderClearBuffer, underBufferedReaderClearBuffer},
    {SymbolId::kUnderBufferedReaderInit, underBufferedReaderInit},
    {SymbolId::kUnderBufferedReaderPeek, underBufferedReaderPeek},
    {SymbolId::kUnderBufferedReaderRead, underBufferedReaderRead},
    {SymbolId::kUnderBufferedReaderReadline, underBufferedReaderReadline},
    {SymbolId::kUnderStringIOClosedGuard, underStringIOClosedGuard},
    {SymbolId::kSentinelId, nullptr},
};

const BuiltinType UnderIoModule::kBuiltinTypes[] = {
    {SymbolId::kBufferedRandom, LayoutId::kBufferedRandom},
    {SymbolId::kBufferedReader, LayoutId::kBufferedReader},
    {SymbolId::kBufferedWriter, LayoutId::kBufferedWriter},
    {SymbolId::kBytesIO, LayoutId::kBytesIO},
    {SymbolId::kFileIO, LayoutId::kFileIO},
    {SymbolId::kStringIO, LayoutId::kStringIO},
    {SymbolId::kIncrementalNewlineDecoder,
     LayoutId::kIncrementalNewlineDecoder},
    {SymbolId::kTextIOWrapper, LayoutId::kTextIOWrapper},
    {SymbolId::kUnderBufferedIOBase, LayoutId::kUnderBufferedIOBase},
    {SymbolId::kUnderBufferedIOMixin, LayoutId::kUnderBufferedIOMixin},
    {SymbolId::kUnderIOBase, LayoutId::kUnderIOBase},
    {SymbolId::kUnderRawIOBase, LayoutId::kUnderRawIOBase},
    {SymbolId::kUnderTextIOBase, LayoutId::kUnderTextIOBase},
    {SymbolId::kSentinelId, LayoutId::kSentinelId},
};

void UnderIoModule::initialize(Thread* thread, const Module& module) {
  moduleAddBuiltinFunctions(thread, module, kBuiltinFunctions);
  moduleAddBuiltinTypes(thread, module, kBuiltinTypes);
  executeFrozenModule(thread, kUnderIoModuleData, module);
}

static RawObject initReadBuf(Thread* thread,
                             const BufferedReader& buffered_reader) {
  HandleScope scope(thread);
  word buffer_size = buffered_reader.bufferSize();
  MutableBytes read_buf(
      &scope, thread->runtime()->newMutableBytesUninitialized(buffer_size));
  buffered_reader.setReadBuf(*read_buf);
  buffered_reader.setReadPos(0);
  buffered_reader.setBufferNumBytes(0);
  return *read_buf;
}

// If there is no buffer allocated yet, allocate one. If there are remaining
// bytes in the buffer, move them to position 0; Set buffer read position to 0.
static RawObject rewindOrInitReadBuf(Thread* thread,
                                     const BufferedReader& buffered_reader) {
  HandleScope scope(thread);
  Object read_buf_obj(&scope, buffered_reader.readBuf());
  word read_pos = buffered_reader.readPos();
  if (read_pos > 0) {
    MutableBytes read_buf(&scope, *read_buf_obj);
    word buffer_num_bytes = buffered_reader.bufferNumBytes();
    read_buf.replaceFromWithStartAt(0, Bytes::cast(*read_buf),
                                    buffer_num_bytes - read_pos, read_pos);
    buffered_reader.setBufferNumBytes(buffer_num_bytes - read_pos);
    buffered_reader.setReadPos(0);
    return *read_buf;
  }
  if (read_buf_obj.isNoneType()) {
    return initReadBuf(thread, buffered_reader);
  }
  return *read_buf_obj;
}

// Perform one read operation to re-fill the buffer.
static RawObject fillBuffer(Thread* thread, const Object& raw_file,
                            const MutableBytes& buffer,
                            word* buffer_num_bytes) {
  HandleScope scope(thread);
  word buffer_size = buffer.length();
  word wanted = buffer_size - *buffer_num_bytes;
  Object wanted_int(&scope, SmallInt::fromWord(wanted));
  Object result_obj(
      &scope, thread->invokeMethod2(raw_file, SymbolId::kRead, wanted_int));
  if (result_obj.isError()) {
    if (result_obj.isErrorException()) return *result_obj;
    if (result_obj.isErrorNotFound()) {
      if (raw_file.isNoneType()) {
        return thread->raiseWithFmt(LayoutId::kValueError,
                                    "raw stream has been detached");
      }
      Object name(&scope, thread->runtime()->symbols()->at(SymbolId::kRead));
      return objectRaiseAttributeError(thread, raw_file, name);
    }
  }
  if (result_obj.isNoneType()) return NoneType::object();

  Runtime* runtime = thread->runtime();
  Bytes bytes(&scope, Bytes::empty());
  word length;
  if (runtime->isInstanceOfBytes(*result_obj)) {
    bytes = bytesUnderlying(*result_obj);
    length = bytes.length();
  } else if (runtime->isInstanceOfByteArray(*result_obj)) {
    ByteArray byte_array(&scope, *result_obj);
    bytes = byte_array.bytes();
    length = byte_array.numItems();
  } else if (runtime->isByteslike(*result_obj)) {
    UNIMPLEMENTED("byteslike");
  } else {
    return thread->raiseWithFmt(LayoutId::kTypeError,
                                "read() should return bytes");
  }
  if (length == 0) return Bytes::empty();
  if (length > wanted && wanted != -1) {
    UNIMPLEMENTED("read() returned too many bytes");
  }
  buffer.replaceFromWith(*buffer_num_bytes, *bytes, length);
  *buffer_num_bytes += length;
  return Unbound::object();
}

// Helper function for read requests that are bigger (or close to) than the size
// of the buffer.
static RawObject readBig(Thread* thread, const BufferedReader& buffered_reader,
                         word num_bytes) {
  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  word available = buffered_reader.bufferNumBytes() - buffered_reader.readPos();
  DCHECK(num_bytes == kMaxWord || num_bytes > available,
         "num_bytes should be big");

  // TODO(T59000373): We could specialize this to avoid the intermediate
  // allocations when the size of the result is known and `readinto` is
  // available.

  word length = available;
  Object chunks(&scope, NoneType::object());
  Object chunk(&scope, NoneType::object());
  Object raw_file(&scope, buffered_reader.underlying());
  Bytes bytes(&scope, Bytes::empty());
  for (;;) {
    word wanted = (num_bytes == kMaxWord) ? 32 * kKiB : num_bytes - available;
    Object wanted_int(&scope, SmallInt::fromWord(wanted));
    Object result_obj(
        &scope, thread->invokeMethod2(raw_file, SymbolId::kRead, wanted_int));
    if (result_obj.isError()) {
      if (result_obj.isErrorException()) return *result_obj;
      if (result_obj.isErrorNotFound()) {
        if (raw_file.isNoneType()) {
          return thread->raiseWithFmt(LayoutId::kValueError,
                                      "raw stream has been detached");
        }
        Object name(&scope, thread->runtime()->symbols()->at(SymbolId::kRead));
        return objectRaiseAttributeError(thread, raw_file, name);
      }
    }
    if (result_obj.isNoneType()) {
      if (length == 0) return NoneType::object();
      break;
    }

    word chunk_length;
    if (runtime->isInstanceOfBytes(*result_obj)) {
      bytes = bytesUnderlying(*result_obj);
      chunk = *bytes;
      chunk_length = bytes.length();
    } else if (runtime->isInstanceOfByteArray(*result_obj)) {
      ByteArray byte_array(&scope, *result_obj);
      bytes = byte_array.bytes();
      chunk = *byte_array;
      chunk_length = byte_array.numItems();
    } else if (runtime->isByteslike(*result_obj)) {
      UNIMPLEMENTED("byteslike");
    } else {
      return thread->raiseWithFmt(LayoutId::kTypeError,
                                  "read() should return bytes");
    }

    if (chunk_length == 0) {
      if (length == 0) return *chunk;
      break;
    }
    if (chunk_length > wanted) {
      UNIMPLEMENTED("read() returned too many bytes");
    }

    if (chunks.isNoneType()) {
      chunks = runtime->newList();
    }
    List list(&scope, *chunks);
    runtime->listAdd(thread, list, chunk);

    length += chunk_length;
    if (num_bytes != kMaxWord) {
      num_bytes -= chunk_length;
      if (num_bytes <= 0) break;
    }
  }

  MutableBytes result(&scope, runtime->newMutableBytesUninitialized(length));
  word idx = 0;
  if (available > 0) {
    result.replaceFromWithStartAt(idx, Bytes::cast(buffered_reader.readBuf()),
                                  available, buffered_reader.readPos());
    idx += available;
    buffered_reader.setReadPos(0);
    buffered_reader.setBufferNumBytes(0);
  }
  if (!chunks.isNoneType()) {
    List list(&scope, *chunks);
    for (word i = 0, num_items = list.numItems(); i < num_items; i++) {
      chunk = list.at(i);
      word chunk_length;
      if (chunk.isBytes()) {
        bytes = *chunk;
        chunk_length = bytes.length();
      } else {
        ByteArray byte_array(&scope, *chunk);
        bytes = byte_array.bytes();
        chunk_length = byte_array.numItems();
      }
      result.replaceFromWith(idx, *bytes, chunk_length);
      idx += chunk_length;
    }
  }
  DCHECK(idx == length, "mismatched length");
  return result.becomeImmutable();
}

RawObject UnderIoModule::underBufferedReaderClearBuffer(Thread* thread,
                                                        Frame* frame,
                                                        word nargs) {
  HandleScope scope(thread);
  Arguments args(frame, nargs);
  Runtime* runtime = thread->runtime();
  Object self_obj(&scope, args.get(0));
  if (!runtime->isInstanceOfBufferedReader(*self_obj)) {
    return thread->raiseRequiresType(self_obj, SymbolId::kBufferedReader);
  }
  BufferedReader self(&scope, *self_obj);
  self.setReadPos(0);
  self.setBufferNumBytes(0);
  return NoneType::object();
}

RawObject UnderIoModule::underBufferedReaderInit(Thread* thread, Frame* frame,
                                                 word nargs) {
  HandleScope scope(thread);
  Arguments args(frame, nargs);
  Runtime* runtime = thread->runtime();
  Object self_obj(&scope, args.get(0));
  if (!runtime->isInstanceOfBufferedReader(*self_obj)) {
    return thread->raiseRequiresType(self_obj, SymbolId::kBufferedReader);
  }
  BufferedReader self(&scope, *self_obj);

  Int buffer_size_obj(&scope, intUnderlying(args.get(1)));
  if (!buffer_size_obj.isSmallInt() && !buffer_size_obj.isBool()) {
    return thread->raiseWithFmt(LayoutId::kOverflowError,
                                "cannot fit value into an index-sized integer");
  }
  word buffer_size = buffer_size_obj.asWord();
  DCHECK(buffer_size > 0, "invalid buffer size");

  self.setBufferSize(buffer_size);
  self.setReadPos(0);
  self.setBufferNumBytes(0);
  // readBuf() starts out as `None` and is initialized lazily so patterns like
  // just doing a single `read()` on the whole buffered reader will not even
  // bother allocating the read buffer. There may however be already a
  // `_read_buf` allocated previously when `_init` is used to clear the buffer
  // as part of `seek`.
  if (!self.readBuf().isNoneType() &&
      MutableBytes::cast(self.readBuf()).length() != buffer_size) {
    return thread->raiseWithFmt(LayoutId::kValueError, "length mismatch");
  }
  return NoneType::object();
}

RawObject UnderIoModule::underBufferedReaderPeek(Thread* thread, Frame* frame,
                                                 word nargs) {
  // TODO(T58490915): Investigate what thread safety guarantees python has,
  // and add locking code as necessary.

  Arguments args(frame, nargs);
  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  Object self_obj(&scope, args.get(0));
  if (!runtime->isInstanceOfBufferedReader(*self_obj)) {
    return thread->raiseRequiresType(self_obj, SymbolId::kBufferedReader);
  }
  BufferedReader self(&scope, *self_obj);

  Object num_bytes_obj(&scope, args.get(1));
  // TODO(T59004416) Is there a way to push intFromIndex() towards managed?
  Object num_bytes_int_obj(&scope, intFromIndex(thread, num_bytes_obj));
  if (num_bytes_int_obj.isErrorException()) return *num_bytes_int_obj;
  Int num_bytes_int(&scope, intUnderlying(*num_bytes_int_obj));
  if (!num_bytes_int.isSmallInt() && !num_bytes_int.isBool()) {
    return thread->raiseWithFmt(LayoutId::kOverflowError,
                                "cannot fit value into an index-sized integer");
  }
  word num_bytes = num_bytes_int.asWord();

  word buffer_num_bytes = self.bufferNumBytes();
  word read_pos = self.readPos();
  Object read_buf_obj(&scope, self.readBuf());
  word available = buffer_num_bytes - read_pos;
  if (num_bytes <= 0 || num_bytes > available) {
    // Perform a lightweight "reset" of the read buffer that does not move data
    // around.
    if (read_buf_obj.isNoneType()) {
      read_buf_obj = initReadBuf(thread, self);
    } else if (available == 0) {
      buffer_num_bytes = 0;
      read_pos = 0;
      self.setReadPos(0);
      self.setBufferNumBytes(0);
    }
    // Attempt a single read to fill the buffer.
    MutableBytes read_buf(&scope, *read_buf_obj);
    Object raw_file(&scope, self.underlying());
    Object fill_result(
        &scope, fillBuffer(thread, raw_file, read_buf, &buffer_num_bytes));
    if (fill_result.isErrorException()) return *fill_result;
    self.setBufferNumBytes(buffer_num_bytes);
    available = buffer_num_bytes - read_pos;
  }

  Bytes read_buf(&scope, *read_buf_obj);
  return runtime->bytesSubseq(thread, read_buf, read_pos, available);
}

RawObject UnderIoModule::underBufferedReaderRead(Thread* thread, Frame* frame,
                                                 word nargs) {
  // TODO(T58490915): Investigate what thread safety guarantees python has,
  // and add locking code as necessary.

  Arguments args(frame, nargs);
  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  Object self_obj(&scope, args.get(0));
  if (!runtime->isInstanceOfBufferedReader(*self_obj)) {
    return thread->raiseRequiresType(self_obj, SymbolId::kBufferedReader);
  }
  BufferedReader self(&scope, *self_obj);

  Object num_bytes_obj(&scope, args.get(1));
  word num_bytes;
  if (num_bytes_obj.isNoneType()) {
    num_bytes = kMaxWord;
  } else {
    // TODO(T59004416) Is there a way to push intFromIndex() towards managed?
    Object num_bytes_int_obj(&scope, intFromIndex(thread, num_bytes_obj));
    if (num_bytes_int_obj.isErrorException()) return *num_bytes_int_obj;
    Int num_bytes_int(&scope, intUnderlying(*num_bytes_int_obj));
    if (!num_bytes_int.isSmallInt() && !num_bytes_int.isBool()) {
      return thread->raiseWithFmt(
          LayoutId::kOverflowError,
          "cannot fit value into an index-sized integer");
    }
    num_bytes = num_bytes_int.asWord();
    if (num_bytes == -1) {
      num_bytes = kMaxWord;
    } else if (num_bytes < 0) {
      return thread->raiseWithFmt(LayoutId::kValueError,
                                  "read length must be positive or -1");
    }
  }

  word buffer_num_bytes = self.bufferNumBytes();
  word read_pos = self.readPos();

  word available = buffer_num_bytes - read_pos;
  DCHECK(available >= 0, "invalid state");
  if (num_bytes <= available) {
    word new_read_pos = read_pos + num_bytes;
    self.setReadPos(new_read_pos);
    Bytes read_buf(&scope, self.readBuf());
    return runtime->bytesSubseq(thread, read_buf, read_pos, num_bytes);
  }

  Object raw_file(&scope, self.underlying());
  if (num_bytes == kMaxWord) {
    Object readall_result(&scope,
                          thread->invokeMethod1(raw_file, SymbolId::kReadall));
    if (readall_result.isErrorException()) return *readall_result;
    if (!readall_result.isErrorNotFound()) {
      Bytes bytes(&scope, Bytes::empty());
      word bytes_length;
      if (readall_result.isNoneType()) {
        if (available == 0) return NoneType::object();
        bytes_length = 0;
      } else if (runtime->isInstanceOfBytes(*readall_result)) {
        bytes = bytesUnderlying(*readall_result);
        bytes_length = bytes.length();
      } else if (runtime->isInstanceOfByteArray(*readall_result)) {
        ByteArray byte_array(&scope, *readall_result);
        bytes = byte_array.bytes();
        bytes_length = byte_array.numItems();
      } else if (runtime->isByteslike(*readall_result)) {
        UNIMPLEMENTED("byteslike");
      } else {
        return thread->raiseWithFmt(LayoutId::kTypeError,
                                    "readall() should return bytes");
      }
      word length = bytes_length + available;
      if (length == 0) return Bytes::empty();
      MutableBytes result(&scope,
                          runtime->newMutableBytesUninitialized(length));
      word idx = 0;
      if (available > 0) {
        Bytes read_buf(&scope, self.readBuf());
        result.replaceFromWithStartAt(idx, *read_buf, available, read_pos);
        idx += available;
        self.setReadPos(0);
        self.setBufferNumBytes(0);
      }
      if (bytes_length > 0) {
        result.replaceFromWith(idx, *bytes, bytes_length);
        idx += bytes_length;
      }
      DCHECK(idx == length, "length mismatch");
      return result.becomeImmutable();
    }
  }

  // Use alternate reading code for big requests where buffering would not help.
  // (This is also used for the num_bytes==kMaxWord (aka "readall") case when
  // the file object does not provide a "readall" method.
  word buffer_size = self.bufferSize();
  if (num_bytes > (buffer_size / 2)) {
    return readBig(thread, self, num_bytes);
  }

  // Fill buffer until we have enough bytes available.
  MutableBytes read_buf(&scope, rewindOrInitReadBuf(thread, self));
  buffer_num_bytes = self.bufferNumBytes();
  Object fill_result(&scope, NoneType::object());
  do {
    fill_result = fillBuffer(thread, raw_file, read_buf, &buffer_num_bytes);
    if (fill_result.isErrorException()) return *fill_result;
    if (!fill_result.isUnbound()) {
      if (buffer_num_bytes == 0) return *fill_result;
      break;
    }
  } while (buffer_num_bytes < num_bytes);

  word length = Utils::minimum(buffer_num_bytes, num_bytes);
  self.setBufferNumBytes(buffer_num_bytes);
  self.setReadPos(length);
  Bytes read_buf_bytes(&scope, *read_buf);
  return runtime->bytesSubseq(thread, read_buf_bytes, 0, length);
}

RawObject UnderIoModule::underBufferedReaderReadline(Thread* thread,
                                                     Frame* frame, word nargs) {
  // TODO(T58490915): Investigate what thread safety guarantees Python has,
  // and add locking code as necessary.

  Arguments args(frame, nargs);
  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  Object self_obj(&scope, args.get(0));
  if (!runtime->isInstanceOfBufferedReader(*self_obj)) {
    return thread->raiseRequiresType(self_obj, SymbolId::kBufferedReader);
  }
  BufferedReader self(&scope, *self_obj);

  Object max_line_bytes_obj(&scope, args.get(1));
  word max_line_bytes = kMaxWord;
  if (!max_line_bytes_obj.isNoneType()) {
    // TODO(T59004416) Is there a way to push intFromIndex() towards managed?
    Object max_line_bytes_int_obj(&scope,
                                  intFromIndex(thread, max_line_bytes_obj));
    if (max_line_bytes_int_obj.isErrorException()) {
      return *max_line_bytes_int_obj;
    }
    Int max_line_bytes_int(&scope, intUnderlying(*max_line_bytes_int_obj));
    if (!max_line_bytes_int.isSmallInt() && !max_line_bytes_int.isBool()) {
      return thread->raiseWithFmt(
          LayoutId::kOverflowError,
          "cannot fit value into an index-sized integer");
    }
    max_line_bytes = max_line_bytes_int.asWord();
    if (max_line_bytes == -1) {
      max_line_bytes = kMaxWord;
    } else if (max_line_bytes < 0) {
      return thread->raiseWithFmt(LayoutId::kValueError,
                                  "read length must be positive or -1");
    }
  }

  word buffer_num_bytes = self.bufferNumBytes();
  word read_pos = self.readPos();
  word available = buffer_num_bytes - read_pos;
  if (available > 0) {
    MutableBytes read_buf(&scope, self.readBuf());
    word line_end = -1;
    word scan_length = available;
    if (available >= max_line_bytes) {
      scan_length = max_line_bytes;
      line_end = read_pos + max_line_bytes;
    } else {
      max_line_bytes -= available;
    }
    word newline_index = read_buf.findByte('\n', read_pos, scan_length);
    if (newline_index >= 0) {
      line_end = newline_index + 1;
    }
    if (line_end >= 0) {
      self.setReadPos(line_end);
      Bytes read_buf_bytes(&scope, *read_buf);
      return runtime->bytesSubseq(thread, read_buf_bytes, read_pos,
                                  line_end - read_pos);
    }
  }

  MutableBytes read_buf(&scope, rewindOrInitReadBuf(thread, self));
  buffer_num_bytes = self.bufferNumBytes();
  word buffer_size = self.bufferSize();

  Object raw_file(&scope, self.underlying());
  Object fill_result(&scope, NoneType::object());
  Object chunks(&scope, NoneType::object());
  word line_end = -1;
  // Outer loop in case for case where a line is longer than a single buffer.
  // In that case we will collect the pieces in the `chunks` list.
  for (;;) {
    // Fill buffer until we find a newline character or filled up the whole
    // buffer.
    do {
      word old_buffer_num_bytes = buffer_num_bytes;
      fill_result = fillBuffer(thread, raw_file, read_buf, &buffer_num_bytes);
      if (fill_result.isErrorException()) return *fill_result;
      if (!fill_result.isUnbound()) {
        if (buffer_num_bytes == 0 && chunks.isNoneType()) return *fill_result;
        line_end = buffer_num_bytes;
        break;
      }

      word scan_start = old_buffer_num_bytes;
      word scan_length = buffer_num_bytes - old_buffer_num_bytes;
      if (scan_length >= max_line_bytes) {
        scan_length = max_line_bytes;
        line_end = scan_start + max_line_bytes;
      } else {
        max_line_bytes -= buffer_num_bytes - old_buffer_num_bytes;
      }
      word newline_index = read_buf.findByte('\n', scan_start, scan_length);
      if (newline_index >= 0) {
        line_end = newline_index + 1;
        break;
      }
    } while (line_end < 0 && buffer_num_bytes < buffer_size);

    if (line_end < 0) {
      // The line is longer than the buffer: Add the current buffer to the
      // chunks list, create a fresh one and repeat scan loop.
      if (chunks.isNoneType()) {
        chunks = runtime->newList();
      }
      List list(&scope, *chunks);
      runtime->listAdd(thread, list, read_buf);

      // Create a fresh buffer and retry.
      read_buf = initReadBuf(thread, self);
      buffer_num_bytes = 0;
      continue;
    }
    break;
  }

  word length = line_end;
  if (!chunks.isNoneType()) {
    List list(&scope, *chunks);
    for (word i = 0, num_items = list.numItems(); i < num_items; i++) {
      length += MutableBytes::cast(list.at(i)).length();
    }
  }
  MutableBytes result(&scope, runtime->newMutableBytesUninitialized(length));
  word idx = 0;
  if (!chunks.isNoneType()) {
    List list(&scope, *chunks);
    Bytes chunk(&scope, Bytes::empty());
    for (word i = 0, num_items = list.numItems(); i < num_items; i++) {
      chunk = list.at(i);
      word chunk_length = chunk.length();
      result.replaceFromWith(idx, *chunk, chunk_length);
      idx += chunk_length;
    }
  }
  result.replaceFromWith(idx, Bytes::cast(*read_buf), line_end);
  DCHECK(idx + line_end == length, "length mismatch");
  self.setReadPos(line_end);
  self.setBufferNumBytes(buffer_num_bytes);
  return result.becomeImmutable();
}

RawObject UnderIoModule::underStringIOClosedGuard(Thread* thread, Frame* frame,
                                                  word nargs) {
  Arguments args(frame, nargs);
  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  Object self_obj(&scope, args.get(0));
  if (!runtime->isInstanceOfStringIO(*self_obj)) {
    return thread->raiseRequiresType(self_obj, SymbolId::kStringIO);
  }
  StringIO self(&scope, *self_obj);
  if (self.closed()) {
    return thread->raiseWithFmt(LayoutId::kValueError,
                                "I/O operation on closed file.");
  }
  return NoneType::object();
}

const BuiltinAttribute UnderIOBaseBuiltins::kAttributes[] = {
    {SymbolId::kUnderClosed, UnderIOBase::kClosedOffset},
    {SymbolId::kSentinelId, 0},
};

const BuiltinAttribute IncrementalNewlineDecoderBuiltins::kAttributes[] = {
    {SymbolId::kUnderErrors, IncrementalNewlineDecoder::kErrorsOffset},
    {SymbolId::kUnderTranslate, IncrementalNewlineDecoder::kTranslateOffset},
    {SymbolId::kUnderDecoder, IncrementalNewlineDecoder::kDecoderOffset},
    {SymbolId::kUnderSeennl, IncrementalNewlineDecoder::kSeennlOffset},
    {SymbolId::kUnderPendingcr, IncrementalNewlineDecoder::kPendingcrOffset},
    {SymbolId::kSentinelId, 0},
};

void UnderRawIOBaseBuiltins::postInitialize(Runtime*, const Type& new_type) {
  new_type.setBuiltinBase(kSuperType);
}

void UnderBufferedIOBaseBuiltins::postInitialize(Runtime*,
                                                 const Type& new_type) {
  new_type.setBuiltinBase(kSuperType);
}

const BuiltinAttribute UnderBufferedIOMixinBuiltins::kAttributes[] = {
    {SymbolId::kUnderRaw, UnderBufferedIOMixin::kUnderlyingOffset},
    {SymbolId::kSentinelId, 0},
};

const BuiltinAttribute BufferedRandomBuiltins::kAttributes[] = {
    {SymbolId::kUnderRaw, BufferedRandom::kUnderlyingOffset},
    {SymbolId::kUnderReader, BufferedRandom::kReaderOffset},
    {SymbolId::kUnderWriteBuf, BufferedRandom::kWriteBufOffset},
    {SymbolId::kUnderWriteLock, BufferedRandom::kWriteLockOffset},
    {SymbolId::kBufferSize, BufferedRandom::kBufferSizeOffset},
    {SymbolId::kSentinelId, 0},
};

const BuiltinAttribute BufferedReaderBuiltins::kAttributes[] = {
    {SymbolId::kUnderRaw, BufferedReader::kUnderlyingOffset},
    {SymbolId::kUnderBufferSize, BufferedReader::kBufferSizeOffset,
     AttributeFlags::kReadOnly},
    {SymbolId::kInvalid, BufferedReader::kReadBufOffset},
    {SymbolId::kUnderReadPos, BufferedReader::kReadPosOffset,
     AttributeFlags::kReadOnly},
    {SymbolId::kUnderBufferNumBytes, BufferedReader::kBufferNumBytesOffset,
     AttributeFlags::kReadOnly},
    {SymbolId::kSentinelId, 0},
};

const BuiltinAttribute BufferedWriterBuiltins::kAttributes[] = {
    {SymbolId::kUnderRaw, BufferedWriter::kUnderlyingOffset},
    {SymbolId::kUnderWriteBuf, BufferedWriter::kWriteBufOffset},
    {SymbolId::kUnderWriteLock, BufferedWriter::kWriteLockOffset},
    {SymbolId::kBufferSize, BufferedWriter::kBufferSizeOffset},
    {SymbolId::kSentinelId, 0},
};

const BuiltinAttribute BytesIOBuiltins::kAttributes[] = {
    {SymbolId::kDunderDict, BytesIO::kDictOffset},
    {SymbolId::kUnderBuffer, BytesIO::kBufferOffset},
    {SymbolId::kUnderPos, BytesIO::kPosOffset},
    {SymbolId::kSentinelId, 0},
};

void BytesIOBuiltins::postInitialize(Runtime*, const Type& new_type) {
  new_type.setBuiltinBase(kSuperType);
}

const BuiltinAttribute FileIOBuiltins::kAttributes[] = {
    {SymbolId::kUnderFd, FileIO::kFdOffset},
    {SymbolId::kName, FileIO::kNameOffset},
    {SymbolId::kUnderCreated, FileIO::kCreatedOffset},
    {SymbolId::kUnderReadable, FileIO::kReadableOffset},
    {SymbolId::kUnderWritable, FileIO::kWritableOffset},
    {SymbolId::kUnderAppending, FileIO::kAppendingOffset},
    {SymbolId::kUnderSeekable, FileIO::kSeekableOffset},
    {SymbolId::kUnderCloseFd, FileIO::kCloseFdOffset},
    {SymbolId::kSentinelId, 0},
};

const BuiltinAttribute StringIOBuiltins::kAttributes[] = {
    {SymbolId::kUnderBuffer, StringIO::kBufferOffset},
    {SymbolId::kUnderPos, StringIO::kPosOffset},
    {SymbolId::kUnderReadnl, StringIO::kReadnlOffset},
    {SymbolId::kUnderReadtranslate, StringIO::kReadtranslateOffset},
    {SymbolId::kUnderReaduniversal, StringIO::kReaduniversalOffset},
    {SymbolId::kUnderSeennl, StringIO::kSeennlOffset},
    {SymbolId::kUnderWritenl, StringIO::kWritenlOffset},
    {SymbolId::kUnderWritetranslate, StringIO::kWritetranslateOffset},
    {SymbolId::kInvalid, RawFunction::kDictOffset},
    {SymbolId::kSentinelId, 0},
};

enum NewlineFound { kLF = 0x1, kCR = 0x2, kCRLF = 0x4 };

static RawObject stringIOWrite(Thread* thread, const StringIO& string_io,
                               const Str& value) {
  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  if (*value == Str::empty()) {
    return runtime->newInt(0);
  }

  Str writenl(&scope, string_io.writenl());
  bool long_writenl = writenl.charLength() == 2;
  byte first_writenl_char = writenl.charAt(0);
  bool has_write_translate =
      string_io.hasWritetranslate() && first_writenl_char != '\n';
  word original_val_len = value.charLength();
  word val_len = original_val_len;

  // TODO(T59696801): use a more efficient counting method.
  // If write_translate is true, read_translate is false
  // Contrapositively, if read_translate is true, write_translate is false
  // Therefore we don't have to worry about their interactions with each other
  if (has_write_translate && long_writenl) {
    for (word i = 0; i < original_val_len; i++) {
      byte current_char = value.charAt(i);
      if (current_char == '\n') {
        val_len++;
      }
    }
  }

  // TODO(T59696801): use a more efficient counting method.
  word start = string_io.pos();
  word new_len = start + val_len;
  bool has_read_translate = string_io.hasReadtranslate();
  if (has_read_translate) {
    for (word i = 0; i < val_len - 1; i++) {
      if (value.charAt(i) == '\r' && value.charAt(i + 1) == '\n') {
        new_len--;
        i++;
      }
    }
  }

  // TODO(T59697431): use a more efficient growing operation.
  MutableBytes buffer(&scope, string_io.buffer());
  if (buffer.length() < new_len) {
    MutableBytes new_buffer(&scope,
                            runtime->newMutableBytesUninitialized(new_len));
    new_buffer.replaceFromWith(0, RawBytes::cast(buffer.becomeImmutable()),
                               buffer.length());
    if (buffer.length() < start) {
      for (word i = buffer.length(); i < start; i++) {
        new_buffer.byteAtPut(i, 0);
      }
    }
    string_io.setBuffer(*new_buffer);
    buffer = *new_buffer;
  }

  if (has_read_translate) {
    word new_seen_nl = Int::cast(string_io.seennl()).asWord();
    for (word str_i = 0, byte_i = start; str_i < val_len; ++str_i, ++byte_i) {
      byte ch = value.charAt(str_i);
      if (ch == '\r') {
        if (val_len > str_i + 1 && value.charAt(str_i + 1) == '\n') {
          new_seen_nl |= NewlineFound::kCRLF;
          buffer.byteAtPut(byte_i, '\n');
          str_i++;
          continue;
        }
        new_seen_nl |= NewlineFound::kCR;
        buffer.byteAtPut(byte_i, '\n');
        continue;
      }
      if (ch == '\n') {
        new_seen_nl |= NewlineFound::kLF;
      }
      buffer.byteAtPut(byte_i, ch);
    }
    string_io.setSeennl(SmallInt::fromWord(new_seen_nl));
  } else if (has_write_translate) {
    for (word str_i = 0, byte_i = start; str_i < original_val_len;
         ++str_i, ++byte_i) {
      byte ch = value.charAt(str_i);
      if (ch == '\n') {
        buffer.byteAtPut(byte_i, first_writenl_char);
        if (long_writenl) {
          buffer.byteAtPut(++byte_i, writenl.charAt(1));
        }
        continue;
      }
      buffer.byteAtPut(byte_i, ch);
    }
  } else {
    buffer.replaceFromWithStr(start, *value, val_len);
  }
  string_io.setPos(new_len);
  return runtime->newInt(original_val_len);
}

static bool isValidStringIONewline(const Object& newline) {
  if (newline == SmallStr::empty()) return true;
  if (newline == SmallStr::fromCodePoint('\n')) return true;
  if (newline == SmallStr::fromCodePoint('\r')) return true;
  return newline == SmallStr::fromCStr("\r\n");
}

RawObject StringIOBuiltins::dunderInit(Thread* thread, Frame* frame,
                                       word nargs) {
  Arguments args(frame, nargs);
  HandleScope scope(thread);
  Object self(&scope, args.get(0));
  if (!thread->runtime()->isInstanceOfStringIO(*self)) {
    return thread->raiseRequiresType(self, SymbolId::kStringIO);
  }
  Object newline(&scope, args.get(2));
  Runtime* runtime = thread->runtime();
  if (newline != NoneType::object()) {
    if (!runtime->isInstanceOfStr(*newline)) {
      return thread->raiseWithFmt(LayoutId::kTypeError,
                                  "newline must be str or None, not %T",
                                  &newline);
    }
    newline = strUnderlying(*newline);
    if (!isValidStringIONewline(newline)) {
      return thread->raiseWithFmt(LayoutId::kValueError,
                                  "illegal newline value: %S", &newline);
    }
  }
  StringIO string_io(&scope, *self);
  string_io.setBuffer(runtime->emptyMutableBytes());
  string_io.setClosed(false);
  string_io.setPos(0);
  string_io.setReadnl(*newline);
  string_io.setSeennl(runtime->newInt(0));
  if (newline == NoneType::object()) {
    string_io.setReadtranslate(true);
    string_io.setReaduniversal(true);
    string_io.setWritetranslate(false);
    string_io.setWritenl(SmallStr::fromCodePoint('\n'));
  } else if (newline == Str::empty()) {
    string_io.setReadtranslate(false);
    string_io.setReaduniversal(true);
    string_io.setWritetranslate(false);
    string_io.setWritenl(SmallStr::fromCodePoint('\n'));
  } else {
    string_io.setReadtranslate(false);
    string_io.setReaduniversal(false);
    string_io.setWritetranslate(true);
    string_io.setWritenl(*newline);
  }

  Object initial_value_obj(&scope, args.get(1));
  if (initial_value_obj != NoneType::object()) {
    if (!runtime->isInstanceOfStr(*initial_value_obj)) {
      return thread->raiseWithFmt(LayoutId::kTypeError,
                                  "initial_value must be str or None, not %T",
                                  &initial_value_obj);
    }
    Str initial_value(&scope, strUnderlying(*initial_value_obj));
    stringIOWrite(thread, string_io, initial_value);
    string_io.setPos(0);
  }
  return NoneType::object();
}

static word stringIOReadline(Thread* thread, const StringIO& string_io,
                             word size) {
  HandleScope scope(thread);
  MutableBytes buffer(&scope, string_io.buffer());
  word buf_len = buffer.length();
  word start = string_io.pos();
  if (start >= buf_len) {
    return -1;
  }
  bool has_read_universal = string_io.hasReaduniversal();
  bool has_read_translate = string_io.hasReadtranslate();
  Object newline_obj(&scope, string_io.readnl());
  if (has_read_translate) {
    newline_obj = SmallStr::fromCodePoint('\n');
  }
  Str newline(&scope, *newline_obj);
  if (size < 0 || (size + start) > buf_len) {
    size = buf_len - start;
  }
  word i = start;

  // TODO(T59800533): use a more efficient character scanning method similar to
  // strchr, strcspn, or strstr.
  if (has_read_universal) {
    while (i < start + size) {
      byte ch = buffer.byteAt(i++);
      if (ch == '\n') break;
      if (ch == '\r') {
        if (buf_len > i && buffer.byteAt(i) == '\n') {
          i++;
        }
        break;
      }
    }
  } else {
    byte first_nl_byte = newline.charAt(0);
    while (i < start + size) {
      word index = buffer.findByte(first_nl_byte, i, (size + start - i));
      if (index == -1) {
        i += (size + start - i);
        break;
      }
      i = index + 1;
      if (buf_len >= (i + newline.charLength() - 1)) {
        bool match = true;
        for (int j = 1; j < newline.charLength(); j++) {
          if (buffer.byteAt(i + j - 1) != newline.charAt(j)) {
            match = false;
          }
        }
        if (match) {
          i += (newline.charLength() - 1);
          break;
        }
      }
    }
  }
  string_io.setPos(i);
  return i;
}

RawObject StringIOBuiltins::dunderNext(Thread* thread, Frame* frame,
                                       word nargs) {
  Arguments args(frame, nargs);
  HandleScope scope(thread);
  Object self(&scope, args.get(0));
  if (!thread->runtime()->isInstanceOfStringIO(*self)) {
    return thread->raiseRequiresType(self, SymbolId::kStringIO);
  }
  StringIO string_io(&scope, *self);
  if (string_io.closed()) {
    return thread->raiseWithFmt(LayoutId::kValueError,
                                "I/O operation on closed file.");
  }
  word start = string_io.pos();
  word end = stringIOReadline(thread, string_io, -1);
  if (end == -1) {
    return thread->raise(LayoutId::kStopIteration, NoneType::object());
  }
  Bytes result(&scope, string_io.buffer());
  result = thread->runtime()->bytesSubseq(thread, result, start, end - start);
  return result.becomeStr();
}

RawObject StringIOBuiltins::getvalue(Thread* thread, Frame* frame, word nargs) {
  Arguments args(frame, nargs);
  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  Object self(&scope, args.get(0));
  if (!runtime->isInstanceOfStringIO(*self)) {
    return thread->raiseRequiresType(self, SymbolId::kStringIO);
  }
  StringIO string_io(&scope, *self);
  if (string_io.closed()) {
    return thread->raiseWithFmt(LayoutId::kValueError,
                                "I/O operation on closed file.");
  }
  Bytes buffer(&scope, string_io.buffer());
  buffer = runtime->bytesCopy(thread, buffer);
  return buffer.becomeStr();
}

RawObject StringIOBuiltins::read(Thread* thread, Frame* frame, word nargs) {
  Arguments args(frame, nargs);
  HandleScope scope(thread);
  Object self(&scope, args.get(0));
  if (!thread->runtime()->isInstanceOfStringIO(*self)) {
    return thread->raiseRequiresType(self, SymbolId::kStringIO);
  }
  StringIO string_io(&scope, *self);
  if (string_io.closed()) {
    return thread->raiseWithFmt(LayoutId::kValueError,
                                "I/O operation on closed file.");
  }
  Object size_obj(&scope, args.get(1));
  Runtime* runtime = thread->runtime();
  word size;
  if (size_obj.isNoneType()) {
    size = -1;
  } else {
    size_obj = intFromIndex(thread, size_obj);
    if (size_obj.isError()) return *size_obj;
    // TODO(T55084422): have a better abstraction for int to word conversion
    if (!size_obj.isSmallInt() && !size_obj.isBool()) {
      return thread->raiseWithFmt(
          LayoutId::kOverflowError,
          "cannot fit value into an index-sized integer");
    }
    size = Int::cast(*size_obj).asWord();
  }
  Bytes result(&scope, string_io.buffer());
  word start = string_io.pos();
  word end = result.length();
  if (start > end) {
    return Str::empty();
  }
  if (size < 0) {
    string_io.setPos(end);
    result = runtime->bytesSubseq(thread, result, start, end - start);
    return result.becomeStr();
  }
  word new_pos = Utils::minimum(end, start + size);
  string_io.setPos(new_pos);
  result = runtime->bytesSubseq(thread, result, start, new_pos - start);
  return result.becomeStr();
}

RawObject StringIOBuiltins::readline(Thread* thread, Frame* frame, word nargs) {
  Arguments args(frame, nargs);
  HandleScope scope(thread);
  Object self(&scope, args.get(0));
  if (!thread->runtime()->isInstanceOfStringIO(*self)) {
    return thread->raiseRequiresType(self, SymbolId::kStringIO);
  }
  StringIO string_io(&scope, *self);
  if (string_io.closed()) {
    return thread->raiseWithFmt(LayoutId::kValueError,
                                "I/O operation on closed file.");
  }
  Object size_obj(&scope, args.get(1));
  Runtime* runtime = thread->runtime();
  word size;
  if (size_obj.isNoneType()) {
    size = -1;
  } else {
    size_obj = intFromIndex(thread, size_obj);
    if (size_obj.isError()) return *size_obj;
    // TODO(T55084422): have a better abstraction for int to word conversion
    if (!size_obj.isSmallInt() && !size_obj.isBool()) {
      return thread->raiseWithFmt(
          LayoutId::kOverflowError,
          "cannot fit value into an index-sized integer");
    }
    size = Int::cast(*size_obj).asWord();
  }
  word start = string_io.pos();
  word end = stringIOReadline(thread, string_io, size);
  if (end == -1) {
    return Str::empty();
  }
  Bytes result(&scope, string_io.buffer());
  result = runtime->bytesSubseq(thread, result, start, end - start);
  return result.becomeStr();
}

RawObject StringIOBuiltins::truncate(Thread* thread, Frame* frame, word nargs) {
  Arguments args(frame, nargs);
  HandleScope scope(thread);
  Object self(&scope, args.get(0));
  if (!thread->runtime()->isInstanceOfStringIO(*self)) {
    return thread->raiseRequiresType(self, SymbolId::kStringIO);
  }
  StringIO string_io(&scope, *self);
  if (string_io.closed()) {
    return thread->raiseWithFmt(LayoutId::kValueError,
                                "I/O operation on closed file.");
  }
  Object size_obj(&scope, args.get(1));
  Runtime* runtime = thread->runtime();
  word size;
  if (size_obj.isNoneType()) {
    size = string_io.pos();
  } else {
    size_obj = intFromIndex(thread, size_obj);
    if (size_obj.isError()) return *size_obj;
    // TODO(T55084422): have a better abstraction for int to word conversion
    if (!size_obj.isSmallInt() && !size_obj.isBool()) {
      return thread->raiseWithFmt(
          LayoutId::kOverflowError,
          "cannot fit value into an index-sized integer");
    }
    size = Int::cast(*size_obj).asWord();
    if (size < 0) {
      return thread->raiseWithFmt(LayoutId::kValueError,
                                  "Negative size value %d", size);
    }
  }
  Bytes buffer(&scope, string_io.buffer());
  if (size < buffer.length()) {
    MutableBytes new_buffer(
        &scope, thread->runtime()->newMutableBytesUninitialized(size));
    new_buffer.replaceFromWith(0, *buffer, size);
    string_io.setBuffer(*new_buffer);
  }
  return runtime->newInt(size);
}

RawObject StringIOBuiltins::write(Thread* thread, Frame* frame, word nargs) {
  Arguments args(frame, nargs);
  HandleScope scope(thread);
  Object self(&scope, args.get(0));
  if (!thread->runtime()->isInstanceOfStringIO(*self)) {
    return thread->raiseRequiresType(self, SymbolId::kStringIO);
  }
  StringIO string_io(&scope, *self);
  if (string_io.closed()) {
    return thread->raiseWithFmt(LayoutId::kValueError,
                                "I/O operation on closed file.");
  }
  Object value(&scope, args.get(1));
  if (!thread->runtime()->isInstanceOfStr(*value)) {
    return thread->raiseRequiresType(value, SymbolId::kStr);
  }
  Str str(&scope, strUnderlying(*value));
  return stringIOWrite(thread, string_io, str);
}

const BuiltinMethod StringIOBuiltins::kBuiltinMethods[] = {
    {SymbolId::kGetvalue, StringIOBuiltins::getvalue},
    {SymbolId::kDunderInit, StringIOBuiltins::dunderInit},
    {SymbolId::kDunderNext, StringIOBuiltins::dunderNext},
    {SymbolId::kRead, StringIOBuiltins::read},
    {SymbolId::kReadline, StringIOBuiltins::readline},
    {SymbolId::kTruncate, StringIOBuiltins::truncate},
    {SymbolId::kWrite, StringIOBuiltins::write},
    {SymbolId::kSentinelId, nullptr},
};

const BuiltinAttribute TextIOWrapperBuiltins::kAttributes[] = {
    {SymbolId::kUnderB2cratio, TextIOWrapper::kB2cratioOffset},
    {SymbolId::kUnderBuffer, TextIOWrapper::kBufferOffset},
    {SymbolId::kUnderDecodedChars, TextIOWrapper::kDecodedCharsOffset},
    {SymbolId::kUnderDecodedCharsUsed, TextIOWrapper::kDecodedCharsUsedOffset},
    {SymbolId::kUnderDecoder, TextIOWrapper::kDecoderOffset},
    {SymbolId::kUnderEncoder, TextIOWrapper::kEncoderOffset},
    {SymbolId::kUnderEncoding, TextIOWrapper::kEncodingOffset},
    {SymbolId::kUnderErrors, TextIOWrapper::kErrorsOffset},
    {SymbolId::kUnderHasRead1, TextIOWrapper::kHasRead1Offset},
    {SymbolId::kUnderLineBuffering, TextIOWrapper::kLineBufferingOffset},
    {SymbolId::kUnderReadnl, TextIOWrapper::kReadnlOffset},
    {SymbolId::kUnderReadtranslate, TextIOWrapper::kReadtranslateOffset},
    {SymbolId::kUnderReaduniversal, TextIOWrapper::kReaduniversalOffset},
    {SymbolId::kUnderSeekable, TextIOWrapper::kSeekableOffset},
    {SymbolId::kUnderSnapshot, TextIOWrapper::kSnapshotOffset},
    {SymbolId::kUnderTelling, TextIOWrapper::kTellingOffset},
    {SymbolId::kUnderWritenl, TextIOWrapper::kWritenlOffset},
    {SymbolId::kUnderWritetranslate, TextIOWrapper::kWritetranslateOffset},
    {SymbolId::kMode, TextIOWrapper::kModeOffset},  // TODO(T54575279): remove
    {SymbolId::kSentinelId, 0},
};

}  // namespace py