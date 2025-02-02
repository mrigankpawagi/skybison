// Copyright (c) Facebook, Inc. and its affiliates. (http://www.facebook.com)
#include <cassert>
#include <climits>
#include <cstdarg>

#include "cpython-data.h"
#include "cpython-func.h"

#include "runtime.h"

namespace py {

#define FLAG_COMPAT 1
#define FLAG_SIZE_T 2

typedef int (*destr_t)(PyObject*, void*);

// Keep track of "objects" that have been allocated or initialized and
// which will need to be deallocated or cleaned up somehow if overall
// parsing fails.

typedef struct {
  void* item;
  destr_t destructor;
} freelistentry_t;

typedef struct {
  freelistentry_t* entries;
  int first_available;
  int entries_malloced;
} freelist_t;

#define STATIC_FREELIST_ENTRIES 8

static const int kMaxSmallArraySize = 16;

// Forward
static int vGetArgs1Impl(PyObject* args, PyObject* const* stack,
                         Py_ssize_t nargs, const char* format, va_list* p_va,
                         int flags);
static int vgetargs1(PyObject*, const char*, va_list*, int);
static void seterror(Py_ssize_t, const char*, int*, const char*, const char*);
static const char* convertitem(PyObject*, const char**, va_list*, int, int*,
                               char*, size_t, freelist_t*);
static const char* converttuple(PyObject*, const char**, va_list*, int, int*,
                                char*, size_t, int, freelist_t*);
static const char* convertsimple(PyObject*, const char**, va_list*, int, char*,
                                 size_t, freelist_t*);
static Py_ssize_t convertbuffer(PyObject*, void const** p, const char**);
static int getbuffer(PyObject*, Py_buffer*, const char**);

static int vgetargskeywords(PyObject*, PyObject*, const char*, char**, va_list*,
                            int);

static int vGetArgsKeywordsFast(PyObject*, PyObject*, struct _PyArg_Parser*,
                                va_list*, int);
static int vGetArgsKeywordsFastImpl(PyObject* const* args, Py_ssize_t nargs,
                                    PyObject* keywords, PyObject* kwnames,
                                    struct _PyArg_Parser* parser, va_list* p_va,
                                    int flags);
static bool parserInit(struct _PyArg_Parser* parser, int* keyword_count);
static PyObject* findKeyword(PyObject* kwnames, PyObject* const* kwstack,
                             const char* key);
static const char* skipitem(const char**, va_list*, int);

PY_EXPORT int PyArg_Parse(PyObject* args, const char* format, ...) {
  int retval;
  va_list va;

  va_start(va, format);
  retval = vgetargs1(args, format, &va, FLAG_COMPAT);
  va_end(va);
  return retval;
}

PY_EXPORT int _PyArg_Parse_SizeT(PyObject* args, const char* format, ...) {
  int retval;
  va_list va;

  va_start(va, format);
  retval = vgetargs1(args, format, &va, FLAG_COMPAT | FLAG_SIZE_T);
  va_end(va);
  return retval;
}

PY_EXPORT int PyArg_ParseTuple(PyObject* args, const char* format, ...) {
  int retval;
  va_list va;

  va_start(va, format);
  retval = vgetargs1(args, format, &va, 0);
  va_end(va);
  return retval;
}

PY_EXPORT int _PyArg_ParseTuple_SizeT(PyObject* args, const char* format, ...) {
  int retval;
  va_list va;

  va_start(va, format);
  retval = vgetargs1(args, format, &va, FLAG_SIZE_T);
  va_end(va);
  return retval;
}

PY_EXPORT int _PyArg_ParseStack(PyObject* const* args, Py_ssize_t nargs,
                                const char* format, ...) {
  va_list va;
  va_start(va, format);
  int retval = vGetArgs1Impl(nullptr, args, nargs, format, &va, 0);
  va_end(va);
  return retval;
}

PY_EXPORT int _PyArg_ParseStack_SizeT(PyObject* const* args, Py_ssize_t nargs,
                                      const char* format, ...) {
  va_list va;
  va_start(va, format);
  int retval = vGetArgs1Impl(nullptr, args, nargs, format, &va, FLAG_SIZE_T);
  va_end(va);
  return retval;
}

PY_EXPORT int PyArg_VaParse(PyObject* args, const char* format, va_list va) {
  va_list lva;
  int retval;

  va_copy(lva, va);

  retval = vgetargs1(args, format, &lva, 0);
  va_end(lva);
  return retval;
}

PY_EXPORT int _PyArg_VaParse_SizeT(PyObject* args, const char* format,
                                   va_list va) {
  va_list lva;
  int retval;

  va_copy(lva, va);

  retval = vgetargs1(args, format, &lva, FLAG_SIZE_T);
  va_end(lva);
  return retval;
}

// Handle cleanup of allocated memory in case of exception

static int cleanup_ptr(PyObject*, void* ptr) {
  if (ptr) {
    PyMem_FREE(ptr);
  }
  return 0;
}

static int cleanup_buffer(PyObject*, void* ptr) {
  Py_buffer* buf = static_cast<Py_buffer*>(ptr);
  if (buf) {
    PyBuffer_Release(buf);
  }
  return 0;
}

static int addcleanup(void* ptr, freelist_t* freelist, destr_t destructor) {
  int index;

  index = freelist->first_available;
  freelist->first_available += 1;

  freelist->entries[index].item = ptr;
  freelist->entries[index].destructor = destructor;

  return 0;
}

static int cleanreturn(int retval, freelist_t* freelist) {
  int index;

  if (retval == 0) {
    // A failure occurred, therefore execute all of the cleanup
    // functions.

    for (index = 0; index < freelist->first_available; ++index) {
      freelist->entries[index].destructor(nullptr,
                                          freelist->entries[index].item);
    }
  }
  if (freelist->entries_malloced) PyMem_FREE(freelist->entries);
  return retval;
}

static int vGetArgs1Impl(PyObject* compat_args, PyObject* const* stack,
                         Py_ssize_t nargs, const char* format, va_list* p_va,
                         int flags) {
  DCHECK(nargs == 0 || stack != nullptr,
         "if nargs == 0, stack must be nullptr");

  int compat = flags & FLAG_COMPAT;
  flags = flags & ~FLAG_COMPAT;

  int endfmt = 0;
  const char* formatsave = format;
  const char* fname = nullptr;
  int level = 0;
  int max = 0;
  const char* message = nullptr;
  int min = -1;
  while (endfmt == 0) {
    int c = *format++;
    switch (c) {
      case '(':
        if (level == 0) max++;
        level++;
        if (level >= 30) {
          Py_FatalError(
              "too many tuple nesting levels "
              "in argument format string");
        }
        break;
      case ')':
        if (level == 0) {
          Py_FatalError("excess ')' in getargs format");
        } else {
          level--;
        }
        break;
      case '\0':
        endfmt = 1;
        break;
      case ':':
        fname = format;
        endfmt = 1;
        break;
      case ';':
        message = format;
        endfmt = 1;
        break;
      case '|':
        if (level == 0) min = max;
        break;
      default:
        if (level == 0 && Py_ISALPHA(Py_CHARMASK(c)) && c != 'e') {
          /* skip encoded */
          max++;
        }
        break;
    }
  }

  if (level != 0) Py_FatalError(/* '(' */ "missing ')' in getargs format");

  if (min < 0) min = max;

  freelistentry_t static_entries[STATIC_FREELIST_ENTRIES];
  freelist_t freelist;
  freelist.entries = static_entries;
  freelist.first_available = 0;
  freelist.entries_malloced = 0;
  if (max > STATIC_FREELIST_ENTRIES) {
    freelist.entries = PyMem_NEW(freelistentry_t, max);
    if (freelist.entries == nullptr) {
      PyErr_NoMemory();
      return 0;
    }
    freelist.entries_malloced = 1;
  }

  format = formatsave;
  int levels[32];
  const char* msg;
  char msgbuf[256];
  if (compat) {
    if (max == 0) {
      if (compat_args == nullptr) return 1;
      PyErr_Format(PyExc_TypeError, "%.200s%s takes no arguments",
                   fname == nullptr ? "function" : fname,
                   fname == nullptr ? "" : "()");
      return cleanreturn(0, &freelist);
    }
    if (min == 1 && max == 1) {
      if (compat_args == nullptr) {
        PyErr_Format(PyExc_TypeError, "%.200s%s takes at least one argument",
                     fname == nullptr ? "function" : fname,
                     fname == nullptr ? "" : "()");
        return cleanreturn(0, &freelist);
      }
      msg = convertitem(compat_args, &format, p_va, flags, levels, msgbuf,
                        sizeof(msgbuf), &freelist);
      if (msg == nullptr) return cleanreturn(1, &freelist);
      seterror(levels[0], msg, levels + 1, fname, message);
      return cleanreturn(0, &freelist);
    }
    Thread::current()->raiseWithFmt(
        LayoutId::kSystemError, "old style getargs format uses new features");
    return cleanreturn(0, &freelist);
  }

  if (nargs < min || max < nargs) {
    if (message == nullptr) {
      PyErr_Format(
          PyExc_TypeError, "%.150s%s takes %s %d argument%s (%zd given)",
          fname == nullptr ? "function" : fname, fname == nullptr ? "" : "()",
          min == max ? "exactly" : nargs < min ? "at least" : "at most",
          nargs < min ? min : max, (nargs < min ? min : max) == 1 ? "" : "s",
          nargs);
    } else {
      Thread::current()->raiseWithFmt(LayoutId::kTypeError, message);
    }
    return cleanreturn(0, &freelist);
  }

  for (Py_ssize_t i = 0; i < nargs; i++) {
    if (*format == '|') format++;
    msg = convertitem(stack[i], &format, p_va, flags, levels, msgbuf,
                      sizeof(msgbuf), &freelist);
    if (msg) {
      seterror(i + 1, msg, levels, fname, message);
      return cleanreturn(0, &freelist);
    }
  }

  if (*format != '\0' && !Py_ISALPHA(Py_CHARMASK(*format)) && *format != '(' &&
      *format != '|' && *format != ':' && *format != ';') {
    PyErr_Format(PyExc_SystemError, "bad format string: %.200s", formatsave);
    return cleanreturn(0, &freelist);
  }

  return cleanreturn(1, &freelist);
}

static int vgetargs1(PyObject* args, const char* format, va_list* p_va,
                     int flags) {
  if (flags & FLAG_COMPAT) {
    return vGetArgs1Impl(args, nullptr, 0, format, p_va, flags);
  }
  DCHECK(args != nullptr, "args must be non-NULL");

  if (!PyTuple_Check(args)) {
    Thread::current()->raiseWithFmt(
        LayoutId::kSystemError,
        "new style getargs format but argument is not a tuple");
    return 0;
  }

  PyObject* small_array[kMaxSmallArraySize];
  Py_ssize_t nargs = PyTuple_Size(args);
  PyObject** array =
      nargs <= kMaxSmallArraySize ? small_array : new PyObject*[nargs];
  for (Py_ssize_t i = 0; i < nargs; i++) {
    array[i] = PyTuple_GET_ITEM(args, i);
  }
  int retval = vGetArgs1Impl(args, array, nargs, format, p_va, flags);
  if (array != small_array) {
    delete[] array;
  }
  return retval;
}

static void seterror(Py_ssize_t iarg, const char* msg, int* levels,
                     const char* fname, const char* message) {
  char buf[512];
  int i;
  char* p = buf;

  if (PyErr_Occurred()) return;
  if (message == nullptr) {
    if (fname != nullptr) {
      PyOS_snprintf(p, sizeof(buf), "%.200s() ", fname);
      p += std::strlen(p);
    }
    if (iarg != 0) {
      PyOS_snprintf(p, sizeof(buf) - (p - buf),
                    "argument %" PY_FORMAT_SIZE_T "d", iarg);
      i = 0;
      p += std::strlen(p);
      while (i < 32 && levels[i] > 0 && static_cast<int>(p - buf) < 220) {
        PyOS_snprintf(p, sizeof(buf) - (p - buf), ", item %d", levels[i] - 1);
        p += std::strlen(p);
        i++;
      }
    } else {
      PyOS_snprintf(p, sizeof(buf) - (p - buf), "argument");
      p += std::strlen(p);
    }
    PyOS_snprintf(p, sizeof(buf) - (p - buf), " %.256s", msg);
    message = buf;
  }
  if (msg[0] == '(') {
    Thread::current()->raiseWithFmt(LayoutId::kSystemError, message);
  } else {
    Thread::current()->raiseWithFmt(LayoutId::kTypeError, message);
  }
}

// Convert a tuple argument.
// On entry, *p_format points to the character _after_ the opening '('.
// On successful exit, *p_format points to the closing ')'.
// If successful:
//    *p_format and *p_va are updated,
//    *levels and *msgbuf are untouched,
//    and nullptr is returned.
// If the argument is invalid:
//    *p_format is unchanged,
//    *p_va is undefined,
//    *levels is a 0-terminated list of item numbers,
//    *msgbuf contains an error message, whose format is:
//   "must be <typename1>, not <typename2>", where:
//      <typename1> is the name of the expected type, and
//      <typename2> is the name of the actual type,
//    and msgbuf is returned.
static const char* converttuple(PyObject* arg, const char** p_format,
                                va_list* p_va, int flags, int* levels,
                                char* msgbuf, size_t bufsize, int toplevel,
                                freelist_t* freelist) {
  int level = 0;
  int n = 0;
  const char* format = *p_format;
  int i;
  Py_ssize_t len;

  for (;;) {
    int c = *format++;
    if (c == '(') {
      if (level == 0) n++;
      level++;
    } else if (c == ')') {
      if (level == 0) break;
      level--;
    } else if (c == ':' || c == ';' || c == '\0') {
      break;
    } else if (level == 0 && Py_ISALPHA(Py_CHARMASK(c))) {
      n++;
    }
  }

  if (!PySequence_Check(arg) || PyBytes_Check(arg)) {
    levels[0] = 0;
    PyOS_snprintf(msgbuf, bufsize,
                  toplevel ? "expected %d arguments, not %.50s"
                           : "must be %d-item sequence, not %.50s",
                  n, arg == Py_None ? "None" : _PyType_Name(Py_TYPE(arg)));
    return msgbuf;
  }

  len = PySequence_Size(arg);
  if (len != n) {
    levels[0] = 0;
    if (toplevel) {
      PyOS_snprintf(msgbuf, bufsize,
                    "expected %d arguments, not %" PY_FORMAT_SIZE_T "d", n,
                    len);
    } else {
      PyOS_snprintf(msgbuf, bufsize,
                    "must be sequence of length %d, "
                    "not %" PY_FORMAT_SIZE_T "d",
                    n, len);
    }
    return msgbuf;
  }

  format = *p_format;
  for (i = 0; i < n; i++) {
    const char* msg;
    PyObject* item;
    item = PySequence_GetItem(arg, i);
    if (item == nullptr) {
      PyErr_Clear();
      levels[0] = i + 1;
      levels[1] = 0;
      strncpy(msgbuf, "is not retrievable", bufsize);
      return msgbuf;
    }
    msg = convertitem(item, &format, p_va, flags, levels + 1, msgbuf, bufsize,
                      freelist);
    // PySequence_GetItem calls tp->sq_item, which INCREFs
    Py_XDECREF(item);
    if (msg != nullptr) {
      levels[0] = i + 1;
      return msg;
    }
  }

  *p_format = format;
  return nullptr;
}

// Convert a single item.
static const char* convertitem(PyObject* arg, const char** p_format,
                               va_list* p_va, int flags, int* levels,
                               char* msgbuf, size_t bufsize,
                               freelist_t* freelist) {
  const char* msg;
  const char* format = *p_format;

  if (*format == '(') {
    format++;
    msg = converttuple(arg, &format, p_va, flags, levels, msgbuf, bufsize, 0,
                       freelist);
    if (msg == nullptr) format++;
  } else {
    msg = convertsimple(arg, &format, p_va, flags, msgbuf, bufsize, freelist);
    if (msg != nullptr) levels[0] = 0;
  }
  if (msg == nullptr) *p_format = format;
  return msg;
}

// Format an error message generated by convertsimple().
static const char* converterr(const char* expected, PyObject* arg, char* msgbuf,
                              size_t bufsize) {
  assert(expected != nullptr);
  assert(arg != nullptr);
  if (expected[0] == '(') {
    PyOS_snprintf(msgbuf, bufsize, "%.100s", expected);
  } else {
    PyOS_snprintf(msgbuf, bufsize, "must be %.50s, not %.50s", expected,
                  arg == Py_None ? "None" : _PyType_Name(Py_TYPE(arg)));
  }
  return msgbuf;
}

#define CONV_UNICODE "(unicode conversion error)"

// Explicitly check for float arguments when integers are expected.
// Return 1 for error, 0 if ok.
static int float_argument_error(PyObject* arg) {
  if (PyFloat_Check(arg)) {
    Thread::current()->raiseWithFmt(LayoutId::kTypeError,
                                    "integer argument expected, got float");
    return 1;
  }
  return 0;
}

// Convert a non-tuple argument.  Return nullptr if conversion went OK,
// or a string with a message describing the failure.  The message is
// formatted as "must be <desired type>, not <actual type>".
// When failing, an exception may or may not have been raised.
// Don't call if a tuple is expected.
//
// When you add new format codes, please don't forget poor skipitem() below.
static const char* convertsimple(PyObject* arg, const char** p_format,
                                 va_list* p_va, int flags, char* msgbuf,
                                 size_t bufsize, freelist_t* freelist) {
  // For # codes
#define FETCH_SIZE                                                             \
  int* q = nullptr;                                                            \
  Py_ssize_t* q2 = nullptr;                                                    \
  if (flags & FLAG_SIZE_T)                                                     \
    q2 = va_arg(*p_va, Py_ssize_t*);                                           \
  else                                                                         \
    q = va_arg(*p_va, int*);
#define STORE_SIZE(s)                                                          \
  if (flags & FLAG_SIZE_T)                                                     \
    *q2 = s;                                                                   \
  else {                                                                       \
    if (INT_MAX < s) {                                                         \
      Thread::current()->raiseWithFmt(LayoutId::kOverflowError,                \
                                      "size does not fit in an int");          \
      return converterr("", arg, msgbuf, bufsize);                             \
    }                                                                          \
    *q = (int)s;                                                               \
  }
#define BUFFER_LEN ((flags & FLAG_SIZE_T) ? *q2 : *q)
#define RETURN_ERR_OCCURRED return msgbuf

  const char* format = *p_format;
  char c = *format++;

  switch (c) {
    case 'b': {  // unsigned byte -- very short int
      char* p = va_arg(*p_va, char*);
      long ival;
      if (float_argument_error(arg)) RETURN_ERR_OCCURRED;
      ival = PyLong_AsLong(arg);
      if (ival == -1 && PyErr_Occurred()) {
        RETURN_ERR_OCCURRED;
      }
      if (ival < 0) {
        Thread::current()->raiseWithFmt(
            LayoutId::kOverflowError,
            "unsigned byte integer is less than minimum");
        RETURN_ERR_OCCURRED;
      }
      if (ival > UCHAR_MAX) {
        Thread::current()->raiseWithFmt(
            LayoutId::kOverflowError,
            "unsigned byte integer is greater than maximum");
        RETURN_ERR_OCCURRED;
      }
      *p = static_cast<unsigned char>(ival);
      break;
    }

    case 'B': {  // byte sized bitfield - both signed and unsigned
                 // values allowed
      char* p = va_arg(*p_va, char*);
      long ival;
      if (float_argument_error(arg)) RETURN_ERR_OCCURRED;
      ival = PyLong_AsUnsignedLongMask(arg);
      if (ival == -1 && PyErr_Occurred()) {
        RETURN_ERR_OCCURRED;
      }
      *p = static_cast<unsigned char>(ival);
      break;
    }

    case 'h': {  // signed short int
      short* p = va_arg(*p_va, short*);
      long ival;
      if (float_argument_error(arg)) RETURN_ERR_OCCURRED;
      ival = PyLong_AsLong(arg);
      if (ival == -1 && PyErr_Occurred()) {
        RETURN_ERR_OCCURRED;
      }
      if (ival < SHRT_MIN) {
        Thread::current()->raiseWithFmt(
            LayoutId::kOverflowError,
            "signed short integer is less than minimum");
        RETURN_ERR_OCCURRED;
      }
      if (ival > SHRT_MAX) {
        Thread::current()->raiseWithFmt(
            LayoutId::kOverflowError,
            "signed short integer is greater than maximum");
        RETURN_ERR_OCCURRED;
      }
      *p = static_cast<short>(ival);
      break;
    }

    case 'H': {  // short int sized bitfield, both signed and
                 // unsigned allowed
      unsigned short* p = va_arg(*p_va, unsigned short*);
      long ival;
      if (float_argument_error(arg)) RETURN_ERR_OCCURRED;
      ival = PyLong_AsUnsignedLongMask(arg);
      if (ival == -1 && PyErr_Occurred()) {
        RETURN_ERR_OCCURRED;
      }
      *p = static_cast<unsigned short>(ival);
      break;
    }

    case 'i': {  // signed int
      int* p = va_arg(*p_va, int*);
      long ival;
      if (float_argument_error(arg)) RETURN_ERR_OCCURRED;
      ival = PyLong_AsLong(arg);
      if (ival == -1 && PyErr_Occurred()) {
        RETURN_ERR_OCCURRED;
      }
      if (ival > INT_MAX) {
        Thread::current()->raiseWithFmt(
            LayoutId::kOverflowError, "signed integer is greater than maximum");
        RETURN_ERR_OCCURRED;
      }
      if (ival < INT_MIN) {
        Thread::current()->raiseWithFmt(LayoutId::kOverflowError,
                                        "signed integer is less than minimum");
        RETURN_ERR_OCCURRED;
      }
      *p = ival;
      break;
    }

    case 'I': {  // int sized bitfield, both signed and
                 //   unsigned allowed
      unsigned int* p = va_arg(*p_va, unsigned int*);
      unsigned int ival;
      if (float_argument_error(arg)) RETURN_ERR_OCCURRED;
      ival = static_cast<unsigned int>(PyLong_AsUnsignedLongMask(arg));
      if (ival == -1U && PyErr_Occurred()) {
        RETURN_ERR_OCCURRED;
      }
      *p = ival;
      break;
    }

    case 'n':  // Py_ssize_t
    {
      PyObject* iobj;
      Py_ssize_t* p = va_arg(*p_va, Py_ssize_t*);
      Py_ssize_t ival = -1;
      if (float_argument_error(arg)) RETURN_ERR_OCCURRED;
      iobj = PyNumber_Index(arg);
      if (iobj != nullptr) {
        ival = PyLong_AsSsize_t(iobj);
        Py_DECREF(iobj);
      }
      if (ival == -1 && PyErr_Occurred()) RETURN_ERR_OCCURRED;
      *p = ival;
      break;
    }
    case 'l': {  // long int
      long* p = va_arg(*p_va, long*);
      long ival;
      if (float_argument_error(arg)) RETURN_ERR_OCCURRED;
      ival = PyLong_AsLong(arg);
      if (ival == -1 && PyErr_Occurred()) {
        RETURN_ERR_OCCURRED;
      }
      *p = ival;
      break;
    }

    case 'k': {  // long sized bitfield
      unsigned long* p = va_arg(*p_va, unsigned long*);
      unsigned long ival;
      if (!PyLong_Check(arg)) {
        return converterr("int", arg, msgbuf, bufsize);
      }
      ival = PyLong_AsUnsignedLongMask(arg);
      *p = ival;
      break;
    }

    case 'L': {  // long long
      long long* p = va_arg(*p_va, long long*);
      long long ival;
      if (float_argument_error(arg)) RETURN_ERR_OCCURRED;
      ival = PyLong_AsLongLong(arg);
      if (ival == -1LL && PyErr_Occurred()) {
        RETURN_ERR_OCCURRED;
      }
      *p = ival;
      break;
    }

    case 'K': {  // long long sized bitfield
      unsigned long long* p = va_arg(*p_va, unsigned long long*);
      unsigned long long ival;
      if (!PyLong_Check(arg)) {
        return converterr("int", arg, msgbuf, bufsize);
      }
      ival = PyLong_AsUnsignedLongLongMask(arg);
      *p = ival;
      break;
    }

    case 'f': {  // float
      float* p = va_arg(*p_va, float*);
      double dval = PyFloat_AsDouble(arg);
      if (PyErr_Occurred()) {
        RETURN_ERR_OCCURRED;
      }
      *p = static_cast<float>(dval);
      break;
    }

    case 'd': {  // double
      double* p = va_arg(*p_va, double*);
      double dval = PyFloat_AsDouble(arg);
      if (PyErr_Occurred()) {
        RETURN_ERR_OCCURRED;
      }
      *p = dval;
      break;
    }

    case 'D': {  // complex double
      Py_complex* p = va_arg(*p_va, Py_complex*);
      Py_complex cval;
      cval = PyComplex_AsCComplex(arg);
      if (PyErr_Occurred()) {
        RETURN_ERR_OCCURRED;
      }
      *p = cval;
      break;
    }

    case 'c': {  // char
      char* p = va_arg(*p_va, char*);
      if (PyBytes_Check(arg) && PyBytes_Size(arg) == 1) {
        *p = PyBytes_AS_STRING(arg)[0];
      } else if (PyByteArray_Check(arg) && PyByteArray_Size(arg) == 1) {
        *p = PyByteArray_AS_STRING(arg)[0];
      } else {
        return converterr("a byte string of length 1", arg, msgbuf, bufsize);
      }
      break;
    }

    case 'C': {  // unicode char
      int* p = va_arg(*p_va, int*);

      if (!PyUnicode_Check(arg)) {
        return converterr("a unicode character", arg, msgbuf, bufsize);
      }

      if (PyUnicode_READY(arg)) RETURN_ERR_OCCURRED;

      if (PyUnicode_GET_LENGTH(arg) != 1) {
        return converterr("a unicode character", arg, msgbuf, bufsize);
      }

      *p = PyUnicode_READ_CHAR(arg, 0);
      break;
    }

    case 'p': {  // boolean *p*redicate
      int* p = va_arg(*p_va, int*);
      int val = PyObject_IsTrue(arg);
      if (val > 0) {
        *p = 1;
      } else if (val == 0) {
        *p = 0;
      } else {
        RETURN_ERR_OCCURRED;
      }
      break;
    }

      // XXX WAAAAH!  's', 'y', 'z', 'u', 'Z', 'e', 'w' codes all
      //   need to be cleaned up!

    case 'y': {  // any bytes-like object
      void const** p =
          reinterpret_cast<void const**>(va_arg(*p_va, char const**));
      const char* buf;
      Py_ssize_t count;
      if (*format == '*') {
        if (getbuffer(arg, reinterpret_cast<Py_buffer*>(p), &buf) < 0) {
          return converterr(buf, arg, msgbuf, bufsize);
        }
        format++;
        if (addcleanup(p, freelist, cleanup_buffer)) {
          return converterr("(cleanup problem)", arg, msgbuf, bufsize);
        }
        break;
      }
      count = convertbuffer(arg, p, &buf);
      if (count < 0) {
        return converterr(buf, arg, msgbuf, bufsize);
      }
      if (*format == '#') {
        FETCH_SIZE;
        STORE_SIZE(count);
        format++;
      } else {
        if (std::strlen(static_cast<const char*>(*p)) !=
            static_cast<size_t>(count)) {
          Thread::current()->raiseWithFmt(LayoutId::kValueError,
                                          "embedded null byte");
          RETURN_ERR_OCCURRED;
        }
      }
      break;
    }

    case 's':  // text string or bytes-like object
    case 'z':  // text string, bytes-like object or None
    {
      if (*format == '*') {
        // "s*" or "z*"
        Py_buffer* p = va_arg(*p_va, Py_buffer*);

        if (c == 'z' && arg == Py_None) {
          PyBuffer_FillInfo(p, nullptr, nullptr, 0, 1, 0);
        } else if (PyUnicode_Check(arg)) {
          Py_ssize_t len;
          const char* sarg = PyUnicode_AsUTF8AndSize(arg, &len);
          if (sarg == nullptr) {
            return converterr(CONV_UNICODE, arg, msgbuf, bufsize);
          }
          // This const_cast is gross, but FillInfo should only ever read from
          // this arg.
          PyBuffer_FillInfo(p, arg, const_cast<char*>(sarg), len, 1, 0);
        } else {  // any bytes-like object
          const char* buf;
          if (getbuffer(arg, p, &buf) < 0) {
            return converterr(buf, arg, msgbuf, bufsize);
          }
        }
        if (addcleanup(p, freelist, cleanup_buffer)) {
          return converterr("(cleanup problem)", arg, msgbuf, bufsize);
        }
        format++;
      } else if (*format == '#') {  // a string or read-only bytes-like object
        // "s#" or "z#"
        void const** p =
            reinterpret_cast<void const**>(va_arg(*p_va, char const**));
        FETCH_SIZE;

        if (c == 'z' && arg == Py_None) {
          *p = nullptr;
          STORE_SIZE(0);
        } else if (PyUnicode_Check(arg)) {
          Py_ssize_t len;
          const char* sarg = PyUnicode_AsUTF8AndSize(arg, &len);
          if (sarg == nullptr) {
            return converterr(CONV_UNICODE, arg, msgbuf, bufsize);
          }
          *p = sarg;
          STORE_SIZE(len);
        } else {  // read-only bytes-like object
          // XXX Really?
          const char* buf;
          Py_ssize_t count = convertbuffer(arg, p, &buf);
          if (count < 0) return converterr(buf, arg, msgbuf, bufsize);
          STORE_SIZE(count);
        }
        format++;
      } else {
        // "s" or "z"
        char const** p = va_arg(*p_va, char const**);
        Py_ssize_t len;

        if (c == 'z' && arg == Py_None) {
          *p = nullptr;
        } else if (PyUnicode_Check(arg)) {
          const char* sarg = PyUnicode_AsUTF8AndSize(arg, &len);
          if (sarg == nullptr) {
            return converterr(CONV_UNICODE, arg, msgbuf, bufsize);
          }
          if (std::strlen(sarg) != static_cast<size_t>(len)) {
            Thread::current()->raiseWithFmt(LayoutId::kValueError,
                                            "embedded null character");
            RETURN_ERR_OCCURRED;
          }
          *p = sarg;
        } else {
          return converterr(c == 'z' ? "str or None" : "str", arg, msgbuf,
                            bufsize);
        }
      }
      break;
    }

    case 'u':  // raw unicode buffer (Py_UNICODE *)
    case 'Z':  // raw unicode buffer or None
    {
      Py_UNICODE** p = va_arg(*p_va, Py_UNICODE**);

      if (*format == '#') {
        // "u#" or "Z#"
        FETCH_SIZE;

        if (c == 'Z' && arg == Py_None) {
          *p = nullptr;
          STORE_SIZE(0);
        } else if (PyUnicode_Check(arg)) {
          Py_ssize_t len;
          *p = PyUnicode_AsUnicodeAndSize(arg, &len);
          if (*p == nullptr) RETURN_ERR_OCCURRED;
          STORE_SIZE(len);
        } else {
          return converterr(c == 'Z' ? "str or None" : "str", arg, msgbuf,
                            bufsize);
        }
        format++;
      } else {
        // "u" or "Z"
        if (c == 'Z' && arg == Py_None) {
          *p = nullptr;
        } else if (PyUnicode_Check(arg)) {
          Py_ssize_t len;
          *p = PyUnicode_AsUnicodeAndSize(arg, &len);
          if (*p == nullptr) RETURN_ERR_OCCURRED;
          if (Py_UNICODE_strlen(*p) != static_cast<size_t>(len)) {
            Thread::current()->raiseWithFmt(LayoutId::kValueError,
                                            "embedded null character");
            RETURN_ERR_OCCURRED;
          }
        } else {
          return converterr(c == 'Z' ? "str or None" : "str", arg, msgbuf,
                            bufsize);
        }
      }
      break;
    }

    case 'e': {  // encoded string
      char** buffer;
      const char* encoding;
      PyObject* s;
      int recode_strings;
      Py_ssize_t size;
      const char* ptr;

      // Get 'e' parameter: the encoding name
      encoding = va_arg(*p_va, const char*);
      if (encoding == nullptr) encoding = PyUnicode_GetDefaultEncoding();

      // Get output buffer parameter:
      // 's' (recode all objects via Unicode) or
      // 't' (only recode non-string objects)
      if (*format == 's') {
        recode_strings = 1;
      } else if (*format == 't') {
        recode_strings = 0;
      } else {
        return converterr("(unknown parser marker combination)", arg, msgbuf,
                          bufsize);
      }
      buffer = va_arg(*p_va, char**);
      format++;
      if (buffer == nullptr) {
        return converterr("(buffer is nullptr)", arg, msgbuf, bufsize);
      }
      // Encode object
      if (!recode_strings && (PyBytes_Check(arg) || PyByteArray_Check(arg))) {
        s = arg;
        Py_INCREF(s);
        if (PyObject_AsCharBuffer(s, &ptr, &size) < 0) {
          return converterr("(AsCharBuffer failed)", arg, msgbuf, bufsize);
        }
      } else if (PyUnicode_Check(arg)) {
        // Encode object; use default error handling
        s = PyUnicode_AsEncodedString(arg, encoding, nullptr);
        if (s == nullptr) {
          return converterr("(encoding failed)", arg, msgbuf, bufsize);
        }
        assert(PyBytes_Check(s));
        size = PyBytes_GET_SIZE(s);
        ptr = PyBytes_AS_STRING(s);
        if (ptr == nullptr) ptr = "";
      } else {
        return converterr(recode_strings ? "str" : "str, bytes or bytearray",
                          arg, msgbuf, bufsize);
      }

      // Write output; output is guaranteed to be 0-terminated
      if (*format == '#') {
        // Using buffer length parameter '#':
        //
        //   - if *buffer is nullptr, a new buffer of the
        //   needed size is allocated and the data
        //   copied into it; *buffer is updated to point
        //   to the new buffer; the caller is
        //   responsible for PyMem_Free()ing it after
        //   usage
        //
        //   - if *buffer is not nullptr, the data is
        //   copied to *buffer; *buffer_len has to be
        //   set to the size of the buffer on input;
        //   buffer overflow is signalled with an error;
        //   buffer has to provide enough room for the
        //   encoded string plus the trailing 0-byte
        //
        //   - in both cases, *buffer_len is updated to
        //   the size of the buffer /excluding/ the
        //   trailing 0-byte
        FETCH_SIZE;

        format++;
        if (q == nullptr && q2 == nullptr) {
          Py_DECREF(s);
          return converterr("(buffer_len is nullptr)", arg, msgbuf, bufsize);
        }
        if (*buffer == nullptr) {
          *buffer = PyMem_NEW(char, size + 1);
          if (*buffer == nullptr) {
            Py_DECREF(s);
            PyErr_NoMemory();
            RETURN_ERR_OCCURRED;
          }
          if (addcleanup(*buffer, freelist, cleanup_ptr)) {
            Py_DECREF(s);
            return converterr("(cleanup problem)", arg, msgbuf, bufsize);
          }
        } else {
          if (size + 1 > BUFFER_LEN) {
            Py_DECREF(s);
            PyErr_Format(PyExc_ValueError,
                         "encoded string too long "
                         "(%zd, maximum length %zd)",
                         size, static_cast<Py_ssize_t>(BUFFER_LEN - 1));
            RETURN_ERR_OCCURRED;
          }
        }
        memcpy(*buffer, ptr, size + 1);
        STORE_SIZE(size);
      } else {
        // Using a 0-terminated buffer:
        //
        //   - the encoded string has to be 0-terminated
        //   for this variant to work; if it is not, an
        //   error raised
        //
        //   - a new buffer of the needed size is
        //   allocated and the data copied into it;
        //   *buffer is updated to point to the new
        //   buffer; the caller is responsible for
        //   PyMem_Free()ing it after usage
        if (static_cast<Py_ssize_t>(std::strlen(ptr)) != size) {
          Py_DECREF(s);
          return converterr("encoded string without null bytes", arg, msgbuf,
                            bufsize);
        }
        *buffer = PyMem_NEW(char, size + 1);
        if (*buffer == nullptr) {
          Py_DECREF(s);
          PyErr_NoMemory();
          RETURN_ERR_OCCURRED;
        }
        if (addcleanup(*buffer, freelist, cleanup_ptr)) {
          Py_DECREF(s);
          return converterr("(cleanup problem)", arg, msgbuf, bufsize);
        }
        memcpy(*buffer, ptr, size + 1);
      }
      Py_DECREF(s);
      break;
    }

    case 'S': {  // PyBytes object
      PyObject** p = va_arg(*p_va, PyObject**);
      if (PyBytes_Check(arg)) {
        *p = arg;
      } else {
        return converterr("bytes", arg, msgbuf, bufsize);
      }
      break;
    }

    case 'Y': {  // PyByteArray object
      PyObject** p = va_arg(*p_va, PyObject**);
      if (PyByteArray_Check(arg)) {
        *p = arg;
      } else {
        return converterr("bytearray", arg, msgbuf, bufsize);
      }
      break;
    }

    case 'U': {  // PyUnicode object
      PyObject** p = va_arg(*p_va, PyObject**);
      if (PyUnicode_Check(arg)) {
        if (PyUnicode_READY(arg) == -1) RETURN_ERR_OCCURRED;
        *p = arg;
      } else {
        return converterr("str", arg, msgbuf, bufsize);
      }
      break;
    }

    case 'O': {  // object
      PyTypeObject* type;
      PyObject** p;
      if (*format == '!') {
        type = va_arg(*p_va, PyTypeObject*);
        p = va_arg(*p_va, PyObject**);
        format++;
        if (PyType_IsSubtype(Py_TYPE(arg), type)) {
          *p = arg;
        } else {
          return converterr(_PyType_Name(type), arg, msgbuf, bufsize);
        }

      } else if (*format == '&') {
        typedef int (*converter)(PyObject*, void*);
        converter convert = va_arg(*p_va, converter);
        void* addr = va_arg(*p_va, void*);
        int res;
        format++;
        if (!(res = (*convert)(arg, addr))) {
          return converterr("(unspecified)", arg, msgbuf, bufsize);
        }
        if (res == Py_CLEANUP_SUPPORTED &&
            addcleanup(addr, freelist, convert) == -1) {
          return converterr("(cleanup problem)", arg, msgbuf, bufsize);
        }
      } else {
        p = va_arg(*p_va, PyObject**);
        *p = arg;
      }
      break;
    }

    case 'w': {  // "w*": memory buffer, read-write access
      void** p = va_arg(*p_va, void**);

      if (*format != '*') {
        return converterr("(invalid use of 'w' format character)", arg, msgbuf,
                          bufsize);
      }
      format++;

      // Caller is interested in Py_buffer, and the object
      // supports it directly.
      if (PyObject_GetBuffer(arg, reinterpret_cast<Py_buffer*>(p),
                             PyBUF_WRITABLE) < 0) {
        PyErr_Clear();
        return converterr("read-write bytes-like object", arg, msgbuf, bufsize);
      }
      if (!PyBuffer_IsContiguous(reinterpret_cast<Py_buffer*>(p), 'C')) {
        PyBuffer_Release(reinterpret_cast<Py_buffer*>(p));
        return converterr("contiguous buffer", arg, msgbuf, bufsize);
      }
      if (addcleanup(p, freelist, cleanup_buffer)) {
        return converterr("(cleanup problem)", arg, msgbuf, bufsize);
      }
      break;
    }

    default:
      return converterr("(impossible<bad format char>)", arg, msgbuf, bufsize);
  }

  *p_format = format;
  return nullptr;

#undef FETCH_SIZE
#undef STORE_SIZE
#undef BUFFER_LEN
#undef RETURN_ERR_OCCURRED
}

static Py_ssize_t convertbuffer(PyObject* arg, void const** p,
                                const char** errmsg) {
  Py_ssize_t count;
  Py_buffer view;

  *errmsg = nullptr;
  *p = nullptr;
  // TODO(miro): check that the bytes-like object is read-only and fail with not
  if (getbuffer(arg, &view, errmsg) < 0) return -1;
  count = view.len;
  *p = view.buf;
  PyBuffer_Release(&view);
  return count;
}

static int getbuffer(PyObject* arg, Py_buffer* view, const char** errmsg) {
  if (PyObject_GetBuffer(arg, view, PyBUF_SIMPLE) != 0) {
    *errmsg = "bytes-like object";
    return -1;
  }
  if (!PyBuffer_IsContiguous(view, 'C')) {
    PyBuffer_Release(view);
    *errmsg = "contiguous buffer";
    return -1;
  }
  return 0;
}

// Support for keyword arguments donated by
//   Geoff Philbrick <philbric@delphi.hks.com>

// Return false (0) for error, else true.
PY_EXPORT int PyArg_ParseTupleAndKeywords(PyObject* args, PyObject* keywords,
                                          const char* format, char** kwlist,
                                          ...) {
  int retval;
  va_list va;

  if ((args == nullptr || !PyTuple_Check(args)) ||
      (keywords != nullptr && !PyDict_Check(keywords)) || format == nullptr ||
      kwlist == nullptr) {
    PyErr_BadInternalCall();
    return 0;
  }

  va_start(va, kwlist);
  retval = vgetargskeywords(args, keywords, format, kwlist, &va, 0);
  va_end(va);
  return retval;
}

PY_EXPORT int _PyArg_ParseTupleAndKeywords_SizeT(PyObject* args,
                                                 PyObject* keywords,
                                                 const char* format,
                                                 char** kwlist, ...) {
  int retval;
  va_list va;

  if ((args == nullptr || !PyTuple_Check(args)) ||
      (keywords != nullptr && !PyDict_Check(keywords)) || format == nullptr ||
      kwlist == nullptr) {
    PyErr_BadInternalCall();
    return 0;
  }

  va_start(va, kwlist);
  retval = vgetargskeywords(args, keywords, format, kwlist, &va, FLAG_SIZE_T);
  va_end(va);
  return retval;
}

PY_EXPORT int PyArg_VaParseTupleAndKeywords(PyObject* args, PyObject* keywords,
                                            const char* format, char** kwlist,
                                            va_list va) {
  int retval;
  va_list lva;

  if ((args == nullptr || !PyTuple_Check(args)) ||
      (keywords != nullptr && !PyDict_Check(keywords)) || format == nullptr ||
      kwlist == nullptr) {
    PyErr_BadInternalCall();
    return 0;
  }

  va_copy(lva, va);

  retval = vgetargskeywords(args, keywords, format, kwlist, &lva, 0);
  va_end(lva);
  return retval;
}

PY_EXPORT int _PyArg_VaParseTupleAndKeywords_SizeT(PyObject* args,
                                                   PyObject* keywords,
                                                   const char* format,
                                                   char** kwlist, va_list va) {
  int retval;
  va_list lva;

  if ((args == nullptr || !PyTuple_Check(args)) ||
      (keywords != nullptr && !PyDict_Check(keywords)) || format == nullptr ||
      kwlist == nullptr) {
    PyErr_BadInternalCall();
    return 0;
  }

  va_copy(lva, va);

  retval = vgetargskeywords(args, keywords, format, kwlist, &lva, FLAG_SIZE_T);
  va_end(lva);
  return retval;
}

PY_EXPORT int _PyArg_ParseTupleAndKeywordsFast(PyObject* args,
                                               PyObject* keywords,
                                               struct _PyArg_Parser* parser,
                                               ...) {
  if ((args == nullptr || !PyTuple_Check(args)) ||
      (keywords != nullptr && !PyDict_Check(keywords)) || parser == nullptr) {
    PyErr_BadInternalCall();
    return 0;
  }
  va_list va;
  va_start(va, parser);
  int retval = vGetArgsKeywordsFast(args, keywords, parser, &va, 0);
  va_end(va);
  return retval;
}

PY_EXPORT int _PyArg_ParseTupleAndKeywordsFast_SizeT(
    PyObject* args, PyObject* keywords, struct _PyArg_Parser* parser, ...) {
  if ((args == nullptr || !PyTuple_Check(args)) ||
      (keywords != nullptr && !PyDict_Check(keywords)) || parser == nullptr) {
    PyErr_BadInternalCall();
    return 0;
  }

  va_list va;
  va_start(va, parser);
  int retval = vGetArgsKeywordsFast(args, keywords, parser, &va, FLAG_SIZE_T);
  va_end(va);
  return retval;
}

PY_EXPORT int _PyArg_ParseStackAndKeywords(PyObject* const* args,
                                           Py_ssize_t nargs, PyObject* kwnames,
                                           struct _PyArg_Parser* parser, ...) {
  if ((kwnames != nullptr && !PyTuple_Check(kwnames)) || parser == nullptr) {
    PyErr_BadInternalCall();
    return 0;
  }

  va_list va;
  va_start(va, parser);
  int retval =
      vGetArgsKeywordsFastImpl(args, nargs, nullptr, kwnames, parser, &va, 0);
  va_end(va);
  return retval;
}

PY_EXPORT int _PyArg_ParseStackAndKeywords_SizeT(PyObject* const* args,
                                                 Py_ssize_t nargs,
                                                 PyObject* kwnames,
                                                 struct _PyArg_Parser* parser,
                                                 ...) {
  if ((kwnames != nullptr && !PyTuple_Check(kwnames)) || parser == nullptr) {
    PyErr_BadInternalCall();
    return 0;
  }

  va_list va;
  va_start(va, parser);
  int retval = vGetArgsKeywordsFastImpl(args, nargs, nullptr, kwnames, parser,
                                        &va, FLAG_SIZE_T);
  va_end(va);
  return retval;
}

#define IS_END_OF_FORMAT(c) (c == '\0' || c == ';' || c == ':')

static bool isValidKeyword(struct _PyArg_Parser* parser,
                           Py_ssize_t num_keywords, PyObject* key) {
  word start = parser->pos;
  for (word i = 0; i < num_keywords; i++) {
    if (_PyUnicode_EqualToASCIIString(key, parser->keywords[i + start])) {
      return true;
    }
  }
  return false;
}

static int vGetArgsKeywordsFast(PyObject* args, PyObject* keywords,
                                struct _PyArg_Parser* parser, va_list* p_va,
                                int flags) {
  DCHECK(args != nullptr && PyTuple_Check(args),
         "args must be a non-null tuple");
  PyObject* small_array[kMaxSmallArraySize];
  Py_ssize_t nargs = PyTuple_GET_SIZE(args);
  PyObject** stack =
      nargs <= kMaxSmallArraySize ? small_array : new PyObject*[nargs];
  for (Py_ssize_t i = 0; i < nargs; i++) {
    stack[i] = PyTuple_GET_ITEM(args, i);
  }
  int result = vGetArgsKeywordsFastImpl(stack, nargs, keywords, nullptr, parser,
                                        p_va, flags);
  if (stack != small_array) {
    delete[] stack;
  }
  return result;
}

static int vGetArgsKeywordsFastImpl(PyObject* const* args, Py_ssize_t nargs,
                                    PyObject* keywords, PyObject* kwnames,
                                    struct _PyArg_Parser* parser, va_list* p_va,
                                    int flags) {
  freelistentry_t static_entries[STATIC_FREELIST_ENTRIES];
  freelist_t freelist;
  freelist.entries = static_entries;
  freelist.first_available = 0;
  freelist.entries_malloced = 0;

  DCHECK(keywords == nullptr || PyDict_Check(keywords),
         "keywords must be null or a dict");
  DCHECK(kwnames == nullptr || PyTuple_Check(kwnames),
         "kwnames must be null or a tuple");
  DCHECK(keywords == nullptr || kwnames == nullptr,
         "both keywords and kwnames cannot be non-null");
  DCHECK(parser != nullptr, "parser must not be null");
  DCHECK(p_va != nullptr, "p_va must not be null");

  int keyword_count = 0;
  if (!parserInit(parser, &keyword_count)) {
    return false;
  }

  int pos = parser->pos;
  int len = pos + keyword_count;

  if (len > STATIC_FREELIST_ENTRIES) {
    freelist.entries = PyMem_NEW(freelistentry_t, len);
    if (freelist.entries == nullptr) {
      PyErr_NoMemory();
      return 0;
    }
    freelist.entries_malloced = 1;
  }

  Py_ssize_t num_keywords = 0;
  PyObject* const* kwstack = nullptr;
  if (keywords != nullptr) {
    num_keywords = PyDict_Size(keywords);
  } else if (kwnames != nullptr) {
    num_keywords = PyTuple_GET_SIZE(kwnames);
    kwstack = args + nargs;
  }

  if (nargs + num_keywords > len) {
    PyErr_Format(PyExc_TypeError,
                 "%s%s takes at most %d argument%s (%zd given)",
                 (parser->fname == nullptr) ? "function" : parser->fname,
                 (parser->fname == nullptr) ? "" : "()", len,
                 (len == 1) ? "" : "s", nargs + num_keywords);
    return cleanreturn(0, &freelist);
  }
  if (parser->max < nargs) {
    PyErr_Format(
        PyExc_TypeError, "Function takes %s %d positional arguments (%d given)",
        (parser->min != INT_MAX) ? "at most" : "exactly", parser->max, nargs);
    return cleanreturn(0, &freelist);
  }

  // convert tuple args and keyword args in same loop, using kwtuple to drive
  // process
  const char* format = parser->format;
  for (int i = 0; i < len; i++) {
    const char* keyword = i >= pos ? parser->keywords[i] : nullptr;
    if (*format == '|') {
      format++;
    }
    if (*format == '$') {
      format++;
    }
    DCHECK(!IS_END_OF_FORMAT(*format), "end of format was reached");

    PyObject* current_arg = nullptr;
    if (num_keywords && i >= pos) {
      if (keywords != nullptr) {
        current_arg = PyDict_GetItemString(keywords, keyword);
        if (current_arg == nullptr && PyErr_Occurred()) {
          return cleanreturn(0, &freelist);
        }
      } else {
        current_arg = findKeyword(kwnames, kwstack, keyword);
      }
    }
    if (current_arg != nullptr) {
      --num_keywords;
      if (i < nargs) {
        // arg present in tuple and in dict
        PyErr_Format(PyExc_TypeError,
                     "Argument given by name ('%s') "
                     "and position (%d)",
                     keyword, i + 1);
        return cleanreturn(0, &freelist);
      }
    } else if (i < nargs) {
      current_arg = args[i];
    }

    if (current_arg != nullptr) {
      char msgbuf[512];
      int levels[32];
      const char* msg = convertitem(current_arg, &format, p_va, flags, levels,
                                    msgbuf, sizeof(msgbuf), &freelist);
      if (msg) {
        seterror(i + 1, msg, levels, parser->fname, parser->custom_msg);
        return cleanreturn(0, &freelist);
      }
      continue;
    }

    if (i < parser->min) {
      // Less arguments than required
      if (i < pos) {
        PyErr_Format(
            PyExc_TypeError,
            "Function takes %s %d positional arguments"
            " (%d given)",
            (Py_MIN(pos, parser->min) < parser->max) ? "at least" : "exactly",
            Py_MIN(pos, parser->min), nargs);
      } else {
        PyErr_Format(PyExc_TypeError,
                     "Required argument "
                     "'%s' (pos %d) not found",
                     keyword, i + 1);
      }
      return cleanreturn(0, &freelist);
    }
    // current code reports success when all required args fulfilled and no
    // keyword args left, with no further validation.
    if (!num_keywords) {
      return cleanreturn(1, &freelist);
    }

    // We are into optional args, skip through to any remaining keyword args
    const char* message = skipitem(&format, p_va, flags);
    DCHECK(message == nullptr, "message was not null");
  }

  DCHECK(IS_END_OF_FORMAT(*format) || (*format == '|') || (*format == '$'),
         "expected end of format, '|', '$'");

  // make sure there are no extraneous keyword arguments
  if (num_keywords > 0) {
    if (keywords != nullptr) {
      PyObject *key, *value;
      Py_ssize_t pos1 = 0;
      while (PyDict_Next(keywords, &pos1, &key, &value)) {
        if (!PyUnicode_Check(key)) {
          Thread::current()->raiseWithFmt(LayoutId::kTypeError,
                                          "keywords must be strings");
          return cleanreturn(0, &freelist);
        }
        if (!isValidKeyword(parser, keyword_count, key)) {
          PyErr_Format(PyExc_TypeError,
                       "'%s' is an invalid keyword "
                       "argument for this function",
                       key);
          return cleanreturn(0, &freelist);
        }
      }
    } else {
      Py_ssize_t num_kwargs = PyTuple_GET_SIZE(kwnames);
      for (Py_ssize_t j = 0; j < num_kwargs; j++) {
        PyObject* key = PyTuple_GET_ITEM(kwnames, j);
        if (!PyUnicode_Check(key)) {
          Thread::current()->raiseWithFmt(LayoutId::kTypeError,
                                          "keywords must be strings");
          return cleanreturn(0, &freelist);
        }
        if (!isValidKeyword(parser, keyword_count, key)) {
          PyErr_Format(PyExc_TypeError,
                       "'%U' is an invalid keyword "
                       "argument for this function",
                       key);
          return cleanreturn(0, &freelist);
        }
      }
    }
  }
  return cleanreturn(1, &freelist);
}

static int vgetargskeywords(PyObject* args, PyObject* keywords,
                            const char* format, char** kwlist, va_list* p_va,
                            int flags) {
  char msgbuf[512];
  int levels[32];
  const char *fname, *msg, *custom_msg, *keyword;
  int min = INT_MAX;
  int max = INT_MAX;
  int i, pos, len;
  int skip = 0;
  Py_ssize_t nargs, nkeywords;
  PyObject* current_arg;
  freelistentry_t static_entries[STATIC_FREELIST_ENTRIES];
  freelist_t freelist;

  freelist.entries = static_entries;
  freelist.first_available = 0;
  freelist.entries_malloced = 0;

  assert(args != nullptr && PyTuple_Check(args));
  assert(keywords == nullptr || PyDict_Check(keywords));
  assert(format != nullptr);
  assert(kwlist != nullptr);
  assert(p_va != nullptr);

  // grab the function name or custom error msg first (mutually exclusive)
  fname = strchr(format, ':');
  if (fname) {
    fname++;
    custom_msg = nullptr;
  } else {
    custom_msg = strchr(format, ';');
    if (custom_msg) custom_msg++;
  }

  // scan kwlist and count the number of positional-only parameters
  for (pos = 0; kwlist[pos] && !*kwlist[pos]; pos++) {
  }
  // scan kwlist and get greatest possible nbr of args
  for (len = pos; kwlist[len]; len++) {
    if (!*kwlist[len]) {
      Thread::current()->raiseWithFmt(LayoutId::kSystemError,
                                      "Empty keyword parameter name");
      return cleanreturn(0, &freelist);
    }
  }

  if (len > STATIC_FREELIST_ENTRIES) {
    freelist.entries = PyMem_NEW(freelistentry_t, len);
    if (freelist.entries == nullptr) {
      PyErr_NoMemory();
      return 0;
    }
    freelist.entries_malloced = 1;
  }

  nargs = PyTuple_GET_SIZE(args);
  nkeywords = (keywords == nullptr) ? 0 : PyDict_Size(keywords);
  if (nargs + nkeywords > len) {
    PyErr_Format(
        PyExc_TypeError, "%s%s takes at most %d argument%s (%zd given)",
        (fname == nullptr) ? "function" : fname, (fname == nullptr) ? "" : "()",
        len, (len == 1) ? "" : "s", nargs + nkeywords);
    return cleanreturn(0, &freelist);
  }

  // convert tuple args and keyword args in same loop, using kwlist to drive
  // process
  for (i = 0; i < len; i++) {
    keyword = kwlist[i];
    if (*format == '|') {
      if (min != INT_MAX) {
        Thread::current()->raiseWithFmt(
            LayoutId::kSystemError,
            "Invalid format string (| specified twice)");
        return cleanreturn(0, &freelist);
      }

      min = i;
      format++;

      if (max != INT_MAX) {
        Thread::current()->raiseWithFmt(LayoutId::kSystemError,
                                        "Invalid format string ($ before |)");
        return cleanreturn(0, &freelist);
      }
    }
    if (*format == '$') {
      if (max != INT_MAX) {
        Thread::current()->raiseWithFmt(
            LayoutId::kSystemError,
            "Invalid format string ($ specified twice)");
        return cleanreturn(0, &freelist);
      }

      max = i;
      format++;

      if (max < pos) {
        Thread::current()->raiseWithFmt(LayoutId::kSystemError,
                                        "Empty parameter name after $");
        return cleanreturn(0, &freelist);
      }
      if (skip) {
        // Now we know the minimal and the maximal numbers of
        // positional arguments and can raise an exception with
        // informative message (see below).
        break;
      }
      if (max < nargs) {
        PyErr_Format(PyExc_TypeError,
                     "Function takes %s %d positional arguments"
                     " (%d given)",
                     (min != INT_MAX) ? "at most" : "exactly", max, nargs);
        return cleanreturn(0, &freelist);
      }
    }
    if (IS_END_OF_FORMAT(*format)) {
      PyErr_Format(PyExc_SystemError,
                   "More keyword list entries (%d) than "
                   "format specifiers (%d)",
                   len, i);
      return cleanreturn(0, &freelist);
    }
    if (!skip) {
      current_arg = nullptr;
      if (nkeywords && i >= pos) {
        current_arg = PyDict_GetItemString(keywords, keyword);
        if (!current_arg && PyErr_Occurred()) {
          return cleanreturn(0, &freelist);
        }
      }
      if (current_arg) {
        --nkeywords;
        if (i < nargs) {
          // arg present in tuple and in dict
          PyErr_Format(PyExc_TypeError,
                       "Argument given by name ('%s') "
                       "and position (%d)",
                       keyword, i + 1);
          return cleanreturn(0, &freelist);
        }
      } else if (i < nargs) {
        // Facebook: Use PyTuple_GetItem instead of &PyTuple_GET_ITEM
        // (D12953145)
        current_arg = PyTuple_GetItem(args, i);
      }

      if (current_arg) {
        msg = convertitem(current_arg, &format, p_va, flags, levels, msgbuf,
                          sizeof(msgbuf), &freelist);
        if (msg) {
          seterror(i + 1, msg, levels, fname, custom_msg);
          return cleanreturn(0, &freelist);
        }
        continue;
      }

      if (i < min) {
        if (i < pos) {
          assert(min == INT_MAX);
          assert(max == INT_MAX);
          skip = 1;
          // At that moment we still don't know the minimal and
          // the maximal numbers of positional arguments.  Raising
          // an exception is deferred until we encounter | and $
          // or the end of the format.
        } else {
          PyErr_Format(PyExc_TypeError,
                       "Required argument "
                       "'%s' (pos %d) not found",
                       keyword, i + 1);
          return cleanreturn(0, &freelist);
        }
      }
      // current code reports success when all required args
      // fulfilled and no keyword args left, with no further
      // validation. XXX Maybe skip this in debug build ?

      if (!nkeywords && !skip) {
        return cleanreturn(1, &freelist);
      }
    }

    // We are into optional args, skip thru to any remaining
    // keyword args
    msg = skipitem(&format, p_va, flags);
    if (msg) {
      PyErr_Format(PyExc_SystemError, "%s: '%s'", msg, format);
      return cleanreturn(0, &freelist);
    }
  }

  if (skip) {
    PyErr_Format(PyExc_TypeError,
                 "Function takes %s %d positional arguments"
                 " (%d given)",
                 (Py_MIN(pos, min) < i) ? "at least" : "exactly",
                 Py_MIN(pos, min), nargs);
    return cleanreturn(0, &freelist);
  }

  if (!IS_END_OF_FORMAT(*format) && (*format != '|') && (*format != '$')) {
    PyErr_Format(PyExc_SystemError,
                 "more argument specifiers than keyword list entries "
                 "(remaining format:'%s')",
                 format);
    return cleanreturn(0, &freelist);
  }

  // make sure there are no extraneous keyword arguments
  if (nkeywords > 0) {
    PyObject *key, *value;
    Py_ssize_t iter_pos = 0;
    while (PyDict_Next(keywords, &iter_pos, &key, &value)) {
      int match = 0;
      if (!PyUnicode_Check(key)) {
        Thread::current()->raiseWithFmt(LayoutId::kTypeError,
                                        "keywords must be strings");
        return cleanreturn(0, &freelist);
      }
      for (i = 0; i < len; i++) {
        if (*kwlist[i] && _PyUnicode_EqualToASCIIString(key, kwlist[i])) {
          match = 1;
          break;
        }
      }
      if (!match) {
        PyErr_Format(PyExc_TypeError,
                     "'%U' is an invalid keyword "
                     "argument for this function",
                     key);
        return cleanreturn(0, &freelist);
      }
    }
  }

  return cleanreturn(1, &freelist);
}

static bool parserInit(struct _PyArg_Parser* parser, int* keyword_count) {
  DCHECK(parser->keywords != nullptr, "parser->keywords must not be null");

  // grab the function name or custom error msg first (mutually exclusive)
  const char* format = parser->format;
  if (format != nullptr) {
    parser->fname = std::strchr(format, ':');
    if (parser->fname != nullptr) {
      parser->fname++;
      parser->custom_msg = nullptr;
    } else {
      parser->custom_msg = std::strchr(format, ';');
      if (parser->custom_msg) parser->custom_msg++;
    }
  }

  const char* const* keywords = parser->keywords;
  // scan keywords and count the number of positional-only parameters
  parser->pos = 0;
  for (int i = 0; keywords[i] != nullptr && !*keywords[i]; i++) {
    parser->pos++;
  }

  // scan keywords and get greatest possible nbr of args
  int len = parser->pos;
  for (; keywords[len] != nullptr; len++) {
    if (*keywords[len] == '\0') {
      Thread::current()->raiseWithFmt(LayoutId::kSystemError,
                                      "Empty keyword parameter name");
      return false;
    }
  }

  if (format != nullptr) {
    int min, max;
    min = max = INT_MAX;
    for (int i = 0; i < len; i++) {
      if (*format == '|') {
        if (min != INT_MAX) {
          Thread::current()->raiseWithFmt(
              LayoutId::kSystemError,
              "Invalid format string (| specified twice)");
          return false;
        }
        if (max != INT_MAX) {
          Thread::current()->raiseWithFmt(LayoutId::kSystemError,
                                          "Invalid format string ($ before |)");
          return false;
        }
        min = i;
        format++;
      }
      if (*format == '$') {
        if (max != INT_MAX) {
          Thread::current()->raiseWithFmt(
              LayoutId::kSystemError,
              "Invalid format string ($ specified twice)");
          return false;
        }
        if (i < parser->pos) {
          Thread::current()->raiseWithFmt(LayoutId::kSystemError,
                                          "Empty parameter name after $");
          return false;
        }
        max = i;
        format++;
      }
      if (IS_END_OF_FORMAT(*format)) {
        PyErr_Format(PyExc_SystemError,
                     "More keyword list entries (%d) than "
                     "format specifiers (%d)",
                     len, i);
        return false;
      }

      const char* msg;
      msg = skipitem(&format, nullptr, 0);
      if (msg) {
        PyErr_Format(PyExc_SystemError, "%s: '%s'", msg, format);
        return false;
      }
    }
    parser->min = Py_MIN(min, len);
    parser->max = Py_MIN(max, len);

    if (!IS_END_OF_FORMAT(*format) && (*format != '|') && (*format != '$')) {
      PyErr_Format(PyExc_SystemError,
                   "more argument specifiers than keyword list entries "
                   "(remaining format:'%s')",
                   format);
      return false;
    }
  }

  *keyword_count = len - parser->pos;
  return true;
}

static PyObject* findKeyword(PyObject* kwnames, PyObject* const* kwstack,
                             const char* key) {
  Py_ssize_t num_kwargs = PyTuple_GET_SIZE(kwnames);
  for (Py_ssize_t i = 0; i < num_kwargs; i++) {
    PyObject* kwname = PyTuple_GET_ITEM(kwnames, i);

    if (!PyUnicode_Check(kwname)) {
      // ignore non-string keyword keys: an error will be raised above
      continue;
    }
    if (_PyUnicode_EqualToASCIIString(kwname, key)) {
      return kwstack[i];
    }
  }
  return nullptr;
}

static const char* skipitem(const char** p_format, va_list* p_va, int flags) {
  const char* format = *p_format;
  char c = *format++;

  switch (c) {
    // codes that take a single data pointer as an argument
    // (the type of the pointer is irrelevant)
    case 'b':  // byte -- very short int
    case 'B':  // byte as bitfield
    case 'h':  // short int
    case 'H':  // short int as bitfield
    case 'i':  // int
    case 'I':  // int sized bitfield
    case 'l':  // long int
    case 'k':  // long int sized bitfield
    case 'L':  // long long
    case 'K':  // long long sized bitfield
    case 'n':  // Py_ssize_t
    case 'f':  // float
    case 'd':  // double
    case 'D':  // complex double
    case 'c':  // char
    case 'C':  // unicode char
    case 'p':  // boolean predicate
    case 'S':  // string object
    case 'Y':  // string object
    case 'U':  // unicode string object
    {
      if (p_va != nullptr) {
        (void)va_arg(*p_va, void*);
      }
      break;
    }

    // string codes
    case 'e':  // string with encoding
    {
      if (p_va != nullptr) {
        va_arg(*p_va, const char*);
      }
      if (!(*format == 's' ||
            *format == 't')) {  // after 'e', only 's' and 't' is allowed
        goto err;
      }
      format++;
    }
      // fall through

    case 's':  // string
    case 'z':  // string or None
    case 'y':  // bytes
    case 'u':  // unicode string
    case 'Z':  // unicode string or None
    case 'w':  // buffer, read-write
    {
      if (p_va != nullptr) {
        (void)va_arg(*p_va, char**);
      }
      if (*format == '#') {
        if (p_va != nullptr) {
          if (flags & FLAG_SIZE_T) {
            va_arg(*p_va, Py_ssize_t*);
          } else {
            va_arg(*p_va, int*);
          }
        }
        format++;
      } else if ((c == 's' || c == 'z' || c == 'y') && *format == '*') {
        format++;
      }
      break;
    }

    case 'O':  // object
    {
      if (*format == '!') {
        format++;
        if (p_va != nullptr) {
          va_arg(*p_va, PyTypeObject*);
          va_arg(*p_va, PyObject**);
        }
      } else if (*format == '&') {
        typedef int (*converter)(PyObject*, void*);
        if (p_va != nullptr) {
          va_arg(*p_va, converter);
          va_arg(*p_va, void*);
        }
        format++;
      } else {
        if (p_va != nullptr) {
          va_arg(*p_va, PyObject**);
        }
      }
      break;
    }

    case '(':  // bypass tuple, not handled at all previously
    {
      const char* msg;
      for (;;) {
        if (*format == ')') break;
        if (IS_END_OF_FORMAT(*format)) {
          return "Unmatched left paren in format "
                 "string";
        }
        msg = skipitem(&format, p_va, flags);
        if (msg) return msg;
      }
      format++;
      break;
    }

    case ')':
      return "Unmatched right paren in format string";

    default:
    err:
      return "impossible<bad format char>";
  }

  *p_format = format;
  return nullptr;
}

PY_EXPORT int PyArg_UnpackTuple(PyObject* args, const char* name,
                                Py_ssize_t min, Py_ssize_t max, ...) {
  Py_ssize_t i, l;
  PyObject** o;
  va_list vargs;

  assert(min >= 0);
  assert(min <= max);
  if (!PyTuple_Check(args)) {
    Thread::current()->raiseWithFmt(
        LayoutId::kSystemError,
        "PyArg_UnpackTuple() argument list is not a tuple");
    return 0;
  }
  l = PyTuple_GET_SIZE(args);
  if (l < min) {
    if (name != nullptr) {
      PyErr_Format(PyExc_TypeError, "%s expected %s%zd arguments, got %zd",
                   name, (min == max ? "" : "at least "), min, l);
    } else {
      PyErr_Format(PyExc_TypeError,
                   "unpacked tuple should have %s%zd elements,"
                   " but has %zd",
                   (min == max ? "" : "at least "), min, l);
    }
    return 0;
  }
  if (l == 0) return 1;
  if (l > max) {
    if (name != nullptr) {
      PyErr_Format(PyExc_TypeError, "%s expected %s%zd arguments, got %zd",
                   name, (min == max ? "" : "at most "), max, l);
    } else {
      PyErr_Format(PyExc_TypeError,
                   "unpacked tuple should have %s%zd elements,"
                   " but has %zd",
                   (min == max ? "" : "at most "), max, l);
    }
    return 0;
  }

  va_start(vargs, max);
  for (i = 0; i < l; i++) {
    o = va_arg(vargs, PyObject**);
    // Facebook: Use PyTuple_GetItem instead of &PyTuple_GET_ITEM (D12953145)
    *o = PyTuple_GetItem(args, i);
  }
  va_end(vargs);
  return 1;
}

PY_EXPORT void _PyArg_BadArgument(const char* fname, const char* displayname,
                                  const char* expected, PyObject* arg) {
  PyErr_Format(PyExc_TypeError, "%.200s() %.200s must be %.50s, not %.50s",
               fname, displayname, expected,
               arg == Py_None ? "None" : _PyType_Name(Py_TYPE(arg)));
}

PY_EXPORT int _PyArg_CheckPositional(const char* name, Py_ssize_t nargs,
                                     Py_ssize_t min, Py_ssize_t max) {
  DCHECK_BOUND(min, max);

  if (nargs < min) {
    if (name != nullptr) {
      PyErr_Format(PyExc_TypeError, "%.200s expected %s%zd argument%s, got %zd",
                   name, (min == max ? "" : "at least "), min,
                   min == 1 ? "" : "s", nargs);
    } else {
      PyErr_Format(PyExc_TypeError,
                   "unpacked tuple should have %s%zd element%s,"
                   " but has %zd",
                   (min == max ? "" : "at least "), min, min == 1 ? "" : "s",
                   nargs);
    }
    return 0;
  }

  if (nargs > max) {
    if (name != nullptr) {
      PyErr_Format(PyExc_TypeError, "%.200s expected %s%zd argument%s, got %zd",
                   name, (min == max ? "" : "at most "), max,
                   max == 1 ? "" : "s", nargs);
    } else {
      PyErr_Format(PyExc_TypeError,
                   "unpacked tuple should have %s%zd element%s,"
                   " but has %zd",
                   (min == max ? "" : "at most "), max, max == 1 ? "" : "s",
                   nargs);
    }
    return 0;
  }

  return 1;
}

static int unpackStack(PyObject* const* args, Py_ssize_t nargs,
                       const char* name, Py_ssize_t min, Py_ssize_t max,
                       va_list vargs) {
  DCHECK(min >= 0, "min must be positive");
  DCHECK(min <= max, "min must be <= max");

  if (nargs < min) {
    if (name != nullptr) {
      PyErr_Format(PyExc_TypeError, "%.200s expected %s%zd arguments, got %zd",
                   name, (min == max ? "" : "at least "), min, nargs);
    } else {
      PyErr_Format(PyExc_TypeError,
                   "unpacked tuple should have %s%zd elements,"
                   " but has %zd",
                   (min == max ? "" : "at least "), min, nargs);
    }
    return 0;
  }

  if (nargs == 0) {
    return 1;
  }

  if (nargs > max) {
    if (name != nullptr) {
      PyErr_Format(PyExc_TypeError, "%.200s expected %s%zd arguments, got %zd",
                   name, (min == max ? "" : "at most "), max, nargs);
    } else {
      PyErr_Format(PyExc_TypeError,
                   "unpacked tuple should have %s%zd elements,"
                   " but has %zd",
                   (min == max ? "" : "at most "), max, nargs);
    }
    return 0;
  }

  for (Py_ssize_t i = 0; i < nargs; i++) {
    PyObject** o = va_arg(vargs, PyObject**);
    *o = args[i];
  }
  return 1;
}

PY_EXPORT int _PyArg_UnpackStack(PyObject* const* args, Py_ssize_t nargs,
                                 const char* name, Py_ssize_t min,
                                 Py_ssize_t max, ...) {
  va_list vargs;
  va_start(vargs, max);
  int retval = unpackStack(args, nargs, name, min, max, vargs);
  va_end(vargs);
  return retval;
}

PY_EXPORT int _PyArg_NoKeywords(const char* funcname, PyObject* kwargs) {
  if (kwargs == nullptr) {
    return 1;
  }
  if (!PyDict_CheckExact(kwargs)) {
    PyErr_BadInternalCall();
    return 0;
  }
  if (PyDict_Size(kwargs) == 0) {
    return 1;
  }
  PyErr_Format(PyExc_TypeError, "%.200s() takes no keyword arguments",
               funcname);
  return 0;
}

PY_EXPORT int _PyArg_NoPositional(const char* funcname, PyObject* args) {
  if (args == nullptr) {
    return 1;
  }
  if (!PyTuple_CheckExact(args)) {
    PyErr_BadInternalCall();
    return 0;
  }
  if (PyTuple_Size(args) == 0) {
    return 1;
  }
  PyErr_Format(PyExc_TypeError, "%.200s() takes no positional arguments",
               funcname);
  return 0;
}

PY_EXPORT PyObject* const* _PyArg_UnpackKeywords(
    PyObject* const* args, Py_ssize_t nargs, PyObject* kwargs,
    PyObject* kwnames, struct _PyArg_Parser* parser, int minpos, int maxpos,
    int minkw, PyObject** buf) {
  DCHECK(kwargs == nullptr || PyDict_Check(kwargs),
         "kwargs must be dict or NULL");
  DCHECK(kwargs == nullptr || kwnames == nullptr,
         "cannot have both kwargs and kwnames");

  if (parser == nullptr) {
    PyErr_BadInternalCall();
    return nullptr;
  }

  if (kwnames != nullptr && !PyTuple_Check(kwnames)) {
    PyErr_BadInternalCall();
    return nullptr;
  }

  if (args == nullptr && nargs == 0) {
    args = buf;
  }

  int keyword_count = 0;
  if (!parserInit(parser, &keyword_count)) {
    return nullptr;
  }

  PyObject* kwtuple = parser->kwtuple;
  int posonly = parser->pos;
  int minposonly = Py_MIN(posonly, minpos);
  int maxargs = posonly + keyword_count;

  Py_ssize_t nkwargs;
  PyObject* const* kwstack = nullptr;
  if (kwargs != nullptr) {
    nkwargs = PyDict_GET_SIZE(kwargs);
  } else if (kwnames != nullptr) {
    nkwargs = PyTuple_GET_SIZE(kwnames);
    kwstack = args + nargs;
  } else {
    nkwargs = 0;
  }

  if (nkwargs == 0 && minkw == 0 && minpos <= nargs && nargs <= maxpos) {
    /* Fast path. */
    return args;
  }

  if (nargs + nkwargs > maxargs) {
    /* Adding "keyword" (when nargs == 0) prevents producing wrong error
       messages in some special cases (see bpo-31229). */
    PyErr_Format(PyExc_TypeError,
                 "%.200s%s takes at most %d %sargument%s (%zd given)",
                 (parser->fname == nullptr) ? "function" : parser->fname,
                 (parser->fname == nullptr) ? "" : "()", maxargs,
                 (nargs == 0) ? "keyword " : "", (maxargs == 1) ? "" : "s",
                 nargs + nkwargs);
    return nullptr;
  }

  if (nargs > maxpos) {
    if (maxpos == 0) {
      PyErr_Format(PyExc_TypeError, "%.200s%s takes no positional arguments",
                   (parser->fname == nullptr) ? "function" : parser->fname,
                   (parser->fname == nullptr) ? "" : "()");
    } else {
      PyErr_Format(PyExc_TypeError,
                   "%.200s%s takes %s %d positional argument%s (%zd given)",
                   (parser->fname == nullptr) ? "function" : parser->fname,
                   (parser->fname == nullptr) ? "" : "()",
                   (minpos < maxpos) ? "at most" : "exactly", maxpos,
                   (maxpos == 1) ? "" : "s", nargs);
    }
    return nullptr;
  }

  if (nargs < minposonly) {
    PyErr_Format(PyExc_TypeError,
                 "%.200s%s takes %s %d positional argument%s"
                 " (%zd given)",
                 (parser->fname == nullptr) ? "function" : parser->fname,
                 (parser->fname == nullptr) ? "" : "()",
                 minposonly < maxpos ? "at least" : "exactly", minposonly,
                 minposonly == 1 ? "" : "s", nargs);
    return nullptr;
  }

  /* copy tuple args */
  for (Py_ssize_t i = 0; i < nargs; i++) {
    buf[i] = args[i];
  }

  /* copy keyword args using kwtuple to drive process */
  int reqlimit = minkw ? maxpos + minkw : minpos;
  for (int i = Py_MAX(nargs, posonly); i < maxargs; i++) {
    PyObject* current_arg;
    if (nkwargs) {
      const char* keyword = i >= posonly ? parser->keywords[i] : nullptr;
      if (kwargs != nullptr) {
        current_arg = PyDict_GetItemString(kwargs, keyword);
        if (!current_arg && PyErr_Occurred()) {
          return nullptr;
        }
      } else {
        current_arg = findKeyword(kwnames, kwstack, keyword);
      }
    } else if (i >= reqlimit) {
      break;
    } else {
      current_arg = nullptr;
    }

    buf[i] = current_arg;

    if (current_arg != nullptr) {
      --nkwargs;
    } else if (i < minpos || (maxpos <= i && i < reqlimit)) {
      /* Less arguments than required */
      const char* keyword = i >= posonly ? parser->keywords[i] : nullptr;
      PyErr_Format(PyExc_TypeError,
                   "%.200s%s missing required "
                   "argument '%s' (pos %d)",
                   (parser->fname == nullptr) ? "function" : parser->fname,
                   (parser->fname == nullptr) ? "" : "()", keyword, i + 1);
      return nullptr;
    }
  }

  if (nkwargs > 0) {
    // make sure there are no arguments given by name and position
    for (int i = posonly; i < nargs; i++) {
      PyObject* current_arg;
      const char* keyword = i >= posonly ? parser->keywords[i] : nullptr;
      if (kwargs != nullptr) {
        current_arg = PyDict_GetItemString(kwargs, keyword);
        if (!current_arg && PyErr_Occurred()) {
          return nullptr;
        }
      } else {
        current_arg = findKeyword(kwnames, kwstack, keyword);
      }

      if (current_arg != nullptr) {
        // arg present in tuple and in dict
        PyErr_Format(PyExc_TypeError,
                     "argument for %.200s%s given by name ('%s') "
                     "and position (%d)",
                     (parser->fname == nullptr) ? "function" : parser->fname,
                     (parser->fname == nullptr) ? "" : "()", keyword, i + 1);
        return nullptr;
      }
    }

    // make sure there are no extraneous keyword arguments
    Py_ssize_t j = 0;
    for (;;) {
      int match;
      PyObject* kw;
      if (kwargs != nullptr) {
        if (!PyDict_Next(kwargs, &j, &kw, nullptr)) break;
      } else {
        if (j >= PyTuple_GET_SIZE(kwnames)) break;
        kw = PyTuple_GET_ITEM(kwnames, j);
        j++;
      }

      if (!PyUnicode_Check(kw)) {
        PyErr_SetString(PyExc_TypeError, "keywords must be strings");
        return nullptr;
      }

      match = PySequence_Contains(kwtuple, kw);
      if (match <= 0) {
        if (!match) {
          PyErr_Format(
              PyExc_TypeError,
              "'%U' is an invalid keyword "
              "argument for %.200s%s",
              kw, (parser->fname == nullptr) ? "this function" : parser->fname,
              (parser->fname == nullptr) ? "" : "()");
        }
        return nullptr;
      }
    }
  }

  return buf;
}

PY_EXPORT int PyArg_ValidateKeywordArguments(PyObject*) {
  UNIMPLEMENTED("PyArg_ValidateKeywordArguments");
}

// Adding empty function to be compatible with cpython
PY_EXPORT void _PyArg_Fini(void) { return; }

}  // namespace py
