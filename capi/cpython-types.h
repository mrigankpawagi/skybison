/* Copyright (c) Facebook, Inc. and its affiliates. (http://www.facebook.com) */
#ifndef CPYTHON_TYPES_H
#define CPYTHON_TYPES_H

#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h> /* for struct stat and pid_t. */
#include <time.h>
#include <wchar.h>

#include "pyconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PY_UINT32_T uint32_t
#define PY_UINT64_T uint64_t
#define PY_INT32_T int32_t
#define PY_INT64_T int64_t

typedef ssize_t Py_ssize_t;
typedef uintptr_t Py_uintptr_t;
typedef intptr_t Py_intptr_t;
typedef Py_ssize_t Py_hash_t;
typedef size_t Py_uhash_t;

#ifdef PY_SSIZE_T_CLEAN
typedef Py_ssize_t Py_ssize_clean_t;
#else
typedef int Py_ssize_clean_t;
#endif

#define PY_LONG_LONG long long
#define PY_LLONG_MIN LLONG_MIN
#define PY_LLONG_MAX LLONG_MAX
#define PY_ULLONG_MAX ULLONG_MAX
#define PY_SIZE_MAX SIZE_MAX
#define PY_SSIZE_T_MAX ((Py_ssize_t)(((size_t)-1) >> 1))
#define PY_SSIZE_T_MIN (-PY_SSIZE_T_MAX - 1)

#define PY_FORMAT_SIZE_T "z"

#define Py_LL(x) x##LL
#define Py_ULL(x) Py_LL(x##U)

#define Py_HUGE_VAL HUGE_VAL
#define Py_NAN __builtin_nan("")

#define PY_BIG_ENDIAN (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define PY_LITTLE_ENDIAN (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)

struct stat;
#define _Py_stat_struct stat

#define _PyObject_HEAD_EXTRA uintptr_t reference_;

#define _PyObject_EXTRA_INIT 0,

#define PyObject_HEAD PyObject ob_base;

#define PyObject_VAR_HEAD PyVarObject ob_base;

#define Py_MEMBER_SIZE(type, member) sizeof(((type*)0)->member)
#define Py_ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))

/* clang-format off */
#define PyObject_HEAD_INIT(type) \
    { _PyObject_EXTRA_INIT 1},

/* The modification of PyVarObject_HEAD_INIT is described in detail here:
 * https://mail.python.org/pipermail/python-dev/2018-August/154946.html */
#define PyVarObject_HEAD_INIT(type, size) \
    { PyObject_HEAD_INIT(NULL) 0 },

/* clang-format on */

typedef struct _longobject PyLongObject;
typedef struct _typeobject PyTypeObject;
typedef struct _PyWeakReference PyWeakReference;
typedef struct _structsequence PyStructSequence;

struct PyMemberDef;

#if defined(__GNUC__) && __GNUC__ < 10 && !defined(__clang__) &&               \
    !defined(__cplusplus)
/* GCC has a bug where applying `&*` to a pointer to an incomplete type is
 * rejected incorrectly. See also:
 * https://gcc.gnu.org/bugzilla/show_bug.cgi?id=88827 */

/* Our PyXXX_Type macros rely on this working. As a workaround for gcc we add
 * an (incomplete) definition here so the type is no longer incomplete. */
struct _typeobject {
  _PyObject_HEAD_EXTRA
};

/* This definition is needed to compile the `&*` in _PyRuntime */
struct _rs {};
#endif

typedef struct _object {
  _PyObject_HEAD_EXTRA
  Py_ssize_t ob_refcnt;
} PyObject;

typedef struct {
  PyObject_HEAD
  // no additional fields necessary, all data is in managed heap!
} PyBaseExceptionObject;

typedef struct {
  PyObject_HEAD
  // no additional fields necessary, all data is in managed heap!
} PyHeapTypeObject;

typedef struct {
  PyObject ob_base;
  Py_ssize_t ob_size; /* Number of items in variable part */
} PyVarObject;

typedef struct bufferinfo {
  void* buf;
  PyObject* obj; /* owned reference */
  Py_ssize_t len;
  Py_ssize_t itemsize; /* This is Py_ssize_t so it can be
                          pointed to by strides in simple case.*/
  int readonly;
  int ndim;
  char* format;
  Py_ssize_t* shape;
  Py_ssize_t* strides;
  Py_ssize_t* suboffsets;
  void* internal;
} Py_buffer;

typedef struct _PyArg_Parser {
  const char* format;
  const char* const* keywords;
  const char* fname;
  const char* custom_msg;
  int pos;            // number of positional-only arguments
  int min;            // minimal number of arguments
  int max;            // maximal number of positional arguments
  PyObject* kwtuple;  // tuple of keyword parameter names
  struct _PyArg_Parser* next;
} _PyArg_Parser;

typedef void (*freefunc)(void*);
typedef void (*destructor)(PyObject*);
typedef int (*printfunc)(PyObject*, FILE*, int);
typedef PyObject* (*getattrfunc)(PyObject*, char*);
typedef PyObject* (*getattrofunc)(PyObject*, PyObject*);
typedef int (*setattrfunc)(PyObject*, char*, PyObject*);
typedef int (*setattrofunc)(PyObject*, PyObject*, PyObject*);
typedef PyObject* (*reprfunc)(PyObject*);
typedef Py_hash_t (*hashfunc)(PyObject*);
typedef PyObject* (*richcmpfunc)(PyObject*, PyObject*, int);
typedef PyObject* (*getiterfunc)(PyObject*);
typedef PyObject* (*iternextfunc)(PyObject*);
typedef PyObject* (*descrgetfunc)(PyObject*, PyObject*, PyObject*);
typedef int (*descrsetfunc)(PyObject*, PyObject*, PyObject*);
typedef int (*initproc)(PyObject*, PyObject*, PyObject*);
typedef PyObject* (*newfunc)(struct _typeobject*, PyObject*, PyObject*);
typedef PyObject* (*allocfunc)(struct _typeobject*, Py_ssize_t);

typedef PyObject* (*unaryfunc)(PyObject*);
typedef PyObject* (*binaryfunc)(PyObject*, PyObject*);
typedef PyObject* (*ternaryfunc)(PyObject*, PyObject*, PyObject*);
typedef int (*inquiry)(PyObject*);
typedef Py_ssize_t (*lenfunc)(PyObject*);
typedef PyObject* (*ssizeargfunc)(PyObject*, Py_ssize_t);
typedef PyObject* (*ssizessizeargfunc)(PyObject*, Py_ssize_t, Py_ssize_t);
typedef int (*ssizeobjargproc)(PyObject*, Py_ssize_t, PyObject*);
typedef int (*ssizessizeobjargproc)(PyObject*, Py_ssize_t, Py_ssize_t,
                                    PyObject*);
typedef int (*objobjargproc)(PyObject*, PyObject*, PyObject*);

typedef int (*objobjproc)(PyObject*, PyObject*);
typedef int (*visitproc)(PyObject*, void*);
typedef int (*traverseproc)(PyObject*, visitproc, void*);

typedef int (*getbufferproc)(PyObject*, Py_buffer*, int);
typedef void (*releasebufferproc)(PyObject*, Py_buffer*);

typedef PyObject* (*getter)(PyObject*, void*);
typedef int (*setter)(PyObject*, PyObject*, void*);

typedef PyObject* (*PyCFunction)(PyObject*, PyObject*);
typedef PyObject* (*_PyCFunctionFast)(PyObject*, PyObject* const*, Py_ssize_t);
typedef PyObject* (*_PyCFunctionFastWithKeywords)(PyObject*, PyObject* const*,
                                                  Py_ssize_t, PyObject*);
typedef PyObject* (*PyCFunctionWithKeywords)(PyObject*, PyObject*, PyObject*);
typedef PyObject* (*PyNoArgsFunction)(PyObject*);

typedef struct {
  /* Number implementations must check *both*
     arguments for proper type and implement the necessary conversions
     in the slot functions themselves. */

  binaryfunc nb_add;
  binaryfunc nb_subtract;
  binaryfunc nb_multiply;
  binaryfunc nb_remainder;
  binaryfunc nb_divmod;
  ternaryfunc nb_power;
  unaryfunc nb_negative;
  unaryfunc nb_positive;
  unaryfunc nb_absolute;
  inquiry nb_bool;
  unaryfunc nb_invert;
  binaryfunc nb_lshift;
  binaryfunc nb_rshift;
  binaryfunc nb_and;
  binaryfunc nb_xor;
  binaryfunc nb_or;
  unaryfunc nb_int;
  void* nb_reserved; /* the slot formerly known as nb_long */
  unaryfunc nb_float;

  binaryfunc nb_inplace_add;
  binaryfunc nb_inplace_subtract;
  binaryfunc nb_inplace_multiply;
  binaryfunc nb_inplace_remainder;
  ternaryfunc nb_inplace_power;
  binaryfunc nb_inplace_lshift;
  binaryfunc nb_inplace_rshift;
  binaryfunc nb_inplace_and;
  binaryfunc nb_inplace_xor;
  binaryfunc nb_inplace_or;

  binaryfunc nb_floor_divide;
  binaryfunc nb_true_divide;
  binaryfunc nb_inplace_floor_divide;
  binaryfunc nb_inplace_true_divide;

  unaryfunc nb_index;

  binaryfunc nb_matrix_multiply;
  binaryfunc nb_inplace_matrix_multiply;
} PyNumberMethods;

typedef struct {
  lenfunc sq_length;
  binaryfunc sq_concat;
  ssizeargfunc sq_repeat;
  ssizeargfunc sq_item;
  void* was_sq_slice;
  ssizeobjargproc sq_ass_item;
  void* was_sq_ass_slice;
  objobjproc sq_contains;

  binaryfunc sq_inplace_concat;
  ssizeargfunc sq_inplace_repeat;
} PySequenceMethods;

typedef struct {
  lenfunc mp_length;
  binaryfunc mp_subscript;
  objobjargproc mp_ass_subscript;
} PyMappingMethods;

typedef struct {
  getbufferproc bf_getbuffer;
  releasebufferproc bf_releasebuffer;
} PyBufferProcs;

struct PyMethodDef {
  const char* ml_name; /* The name of the built-in function/method */
  PyCFunction ml_meth; /* The C function that implements it */
  int ml_flags;        /* Combination of METH_xxx flags, which mostly
                          describe the args expected by the C func */
  const char* ml_doc;  /* The __doc__ attribute, or NULL */
};
typedef struct PyMethodDef PyMethodDef;

typedef struct PyModuleDef_Base {
  PyObject_HEAD
  PyObject* (*m_init)(void);
  Py_ssize_t m_index;
  PyObject* m_copy;
} PyModuleDef_Base;

#define PyModuleDef_HEAD_INIT                                                  \
  {                                                                            \
    PyObject_HEAD_INIT(NULL) NULL, /* m_init */                                \
        0,                         /* m_index */                               \
        NULL,                      /* m_copy */                                \
  }

typedef struct PyModuleDef_Slot {
  int slot;
  void* value;
} PyModuleDef_Slot;

#define Py_mod_create 1
#define Py_mod_exec 2

typedef struct PyModuleDef {
  PyModuleDef_Base m_base;
  const char* m_name;
  const char* m_doc;
  Py_ssize_t m_size;
  PyMethodDef* m_methods;
  struct PyModuleDef_Slot* m_slots;
  traverseproc m_traverse;
  inquiry m_clear;
  freefunc m_free;
} PyModuleDef;

typedef struct PyGetSetDef {
  const char* name;
  getter get;
  setter set;
  const char* doc;
  void* closure;
} PyGetSetDef;

typedef struct {
  unaryfunc am_await;
  unaryfunc am_aiter;
  unaryfunc am_anext;
} PyAsyncMethods;

typedef struct {
  int cf_flags;           /* bitmask of CO_xxx flags relevant to future */
  int cf_feature_version; /* minor Python version (PyCF_ONLY_AST) */
} PyCompilerFlags;

#define _PyCompilerFlags_INIT                                                  \
  { 0, PY_MINOR_VERSION, }

struct _inittab {
  const char* name;
  PyObject* (*initfunc)(void);
};

typedef struct {
  double real;
  double imag;
} Py_complex;

#define Py_UNICODE_SIZE __SIZEOF_WCHAR_T__
#define PY_UNICODE_TYPE wchar_t

typedef uint32_t Py_UCS4;
typedef uint16_t Py_UCS2;
typedef uint8_t Py_UCS1;
typedef wchar_t Py_UNICODE;

typedef struct {
  int slot;    /* slot id, see below */
  void* pfunc; /* function pointer */
} PyType_Slot;

typedef struct {
  const char* name;
  int basicsize;
  int itemsize;
  unsigned int flags;
  PyType_Slot* slots; /* terminated by slot==0. */
} PyType_Spec;

typedef struct PyStructSequence_Field {
  const char* name;
  const char* doc;
} PyStructSequence_Field;

typedef struct PyStructSequence_Desc {
  const char* name;
  const char* doc;
  struct PyStructSequence_Field* fields;
  int n_in_sequence;
} PyStructSequence_Desc;

enum PyUnicode_Kind {
  PyUnicode_WCHAR_KIND = 0,
  PyUnicode_1BYTE_KIND = 1,
  PyUnicode_2BYTE_KIND = 2,
  PyUnicode_4BYTE_KIND = 4
};

struct _Py_Identifier;
struct _mod;
struct _node;

typedef struct {
  unsigned char* heap_buffer;       // byte*
  unsigned char* ptr;               // byte*
  Py_ssize_t allocated;             // word
  Py_ssize_t min_size;              // word
  int overallocate;                 // bool
  int use_bytearray;                // bool
  int use_heap_buffer;              // bool
  unsigned char stack_buffer[128];  // byte[128]
} _PyBytesWriter;

typedef struct {
  PyObject* buffer;
  void* data;
  enum PyUnicode_Kind kind;
  Py_UCS4 maxchar;
  Py_ssize_t size;
  Py_ssize_t pos;
  Py_ssize_t min_length;
  Py_UCS4 min_char;
  unsigned char overallocate;
  unsigned char readonly;
} _PyUnicodeWriter;

typedef struct {
  int ff_features;
  int ff_lineno;
} PyFutureFeatures;

typedef uint16_t _Py_CODEUNIT;

// The following types are intentionally incomplete to make it impossible to
// dereference the objects
typedef struct _arena PyArena;
typedef struct _frame PyFrameObject;
typedef struct _code PyCodeObject;
typedef struct _is PyInterpreterState;
typedef struct _ts PyThreadState;
typedef struct _rs _PyRuntimeState;

typedef void (*PyOS_sighandler_t)(int);
typedef void (*PyCapsule_Destructor)(PyObject*);
typedef PyObject* (*Py_OpenCodeHookFunction)(PyObject*, void*);
typedef int (*Py_tracefunc)(PyObject*, PyFrameObject*, int, PyObject*);

struct timespec;
struct timeval;
typedef int64_t _PyTime_t;
#define _PyTime_MIN INT64_MIN
#define _PyTime_MAX INT64_MAX

typedef enum {
  _PyTime_ROUND_FLOOR = 0,
  _PyTime_ROUND_CEILING = 1,
  _PyTime_ROUND_HALF_EVEN = 2,
  _PyTime_ROUND_UP = 3,
  _PyTime_ROUND_TIMEOUT = _PyTime_ROUND_UP
} _PyTime_round_t;

typedef struct {
  const char* implementation;
  int monotonic;
  int adjustable;
  double resolution;
} _Py_clock_info_t;

typedef enum { PyGILState_LOCKED, PyGILState_UNLOCKED } PyGILState_STATE;

typedef void* PyThread_type_lock;
typedef void* PyThread_type_sema;

typedef enum PyLockStatus {
  PY_LOCK_FAILURE = 0,
  PY_LOCK_ACQUIRED = 1,
  PY_LOCK_INTR
} PyLockStatus;

#define PY_TIMEOUT_T long long
#define PY_TIMEOUT_MAX LLONG_MAX

typedef union {
  /* ensure 24 bytes */
  unsigned char uc[24];
  /* two Py_hash_t for FNV */
  struct {
    Py_hash_t prefix;
    Py_hash_t suffix;
  } fnv;
  /* two uint64 for SipHash24 */
  struct {
    uint64_t k0;
    uint64_t k1;
  } siphash;
  /* a different (!) Py_hash_t for small string optimization */
  struct {
    unsigned char padding[16];
    Py_hash_t suffix;
  } djbx33a;
  struct {
    unsigned char padding[16];
    Py_hash_t hashsalt;
  } expat;
} _Py_HashSecret_t;

typedef enum {
  _Py_ERROR_UNKNOWN = 0,
  _Py_ERROR_STRICT,
  _Py_ERROR_SURROGATEESCAPE,
  _Py_ERROR_REPLACE,
  _Py_ERROR_IGNORE,
  _Py_ERROR_BACKSLASHREPLACE,
  _Py_ERROR_SURROGATEPASS,
  _Py_ERROR_XMLCHARREFREPLACE,
  _Py_ERROR_OTHER,
} _Py_error_handler;

struct lconv;

#ifdef __cplusplus
}
#endif

#endif /* !CPYTHON_TYPES_H */
