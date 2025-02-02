// Copyright (c) Facebook, Inc. and its affiliates. (http://www.facebook.com)
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <clocale>
#include <cstdio>
#include <cstdlib>

#include "cpython-data.h"
#include "cpython-func.h"

#include "api-handle.h"
#include "capi.h"
#include "exception-builtins.h"
#include "file.h"
#include "modules.h"
#include "os.h"
#include "runtime.h"
#include "str-builtins.h"
#include "sys-module.h"
#include "vector.h"

extern "C" int _PyCapsule_Init(void);
extern "C" int _PySTEntry_Init(void);

int Py_BytesWarningFlag = 0;
int Py_DebugFlag = 0;
int Py_DontWriteBytecodeFlag = 0;
int Py_FrozenFlag = 0;
int Py_HashRandomizationFlag = 0;
int Py_IgnoreEnvironmentFlag = 0;
int Py_InspectFlag = 0;
int Py_InteractiveFlag = 0;
int Py_IsolatedFlag = 0;
int Py_NoSiteFlag = 0;
int Py_NoUserSiteDirectory = 0;
int Py_OptimizeFlag = 0;
int Py_QuietFlag = 0;
int Py_UTF8Mode = 1;
int Py_UnbufferedStdioFlag = 0;
int Py_VerboseFlag = 0;

namespace py {

// Used by Py_BytesMain to store `-W` options. `Py_Initialize` will read
// them and clear the vector.
Vector<const char*> warn_options;

PY_EXPORT PyOS_sighandler_t PyOS_getsig(int signum) {
  return OS::signalHandler(signum);
}

PY_EXPORT PyOS_sighandler_t PyOS_setsig(int signum, PyOS_sighandler_t handler) {
  return OS::setSignalHandler(signum, handler);
}

PY_EXPORT int Py_AtExit(void (*/* func */)(void)) {
  UNIMPLEMENTED("Py_AtExit");
}

PY_EXPORT void Py_EndInterpreter(PyThreadState* /* e */) {
  UNIMPLEMENTED("Py_EndInterpreter");
}

PY_EXPORT void Py_Exit(int status_code) {
  if (Py_FinalizeEx() < 0) {
    status_code = 120;
  }

  std::exit(status_code);
}

PY_EXPORT void _Py_NO_RETURN Py_FatalError(const char* msg) {
  // TODO(T39151288): Correctly print exceptions when the current thread holds
  // the GIL
  std::fprintf(stderr, "Fatal Python error: %s\n", msg);
  Thread* thread = Thread::current();
  if (thread != nullptr) {
    if (thread->hasPendingException()) {
      printPendingException(thread);
    } else {
      thread->runtime()->printTraceback(thread, File::kStderr);
    }
  }
  std::abort();
}

// The file descriptor fd is considered ``interactive'' if either:
//   a) isatty(fd) is TRUE, or
//   b) the -i flag was given, and the filename associated with the descriptor
//      is NULL or "<stdin>" or "???".
PY_EXPORT int Py_FdIsInteractive(FILE* fp, const char* filename) {
  if (::isatty(fileno(fp))) {
    return 1;
  }
  if (!Py_InteractiveFlag) {
    return 0;
  }
  return filename == nullptr || std::strcmp(filename, "<stdin>") == 0 ||
         std::strcmp(filename, "???") == 0;
}

PY_EXPORT void Py_Finalize() { Py_FinalizeEx(); }

// TODO(T70098990): Implement and add PyEnum_Type

#define FOREACH_STATICTYPE(V)                                                  \
  V(PyAsyncGen_Type)                                                           \
  V(PyBaseObject_Type)                                                         \
  V(PyBool_Type)                                                               \
  V(PyByteArrayIter_Type)                                                      \
  V(PyByteArray_Type)                                                          \
  V(PyBytesIter_Type)                                                          \
  V(PyBytes_Type)                                                              \
  V(PyClassMethod_Type)                                                        \
  V(PyCode_Type)                                                               \
  V(PyComplex_Type)                                                            \
  V(PyCoro_Type)                                                               \
  V(PyDictItems_Type)                                                          \
  V(PyDictIterItem_Type)                                                       \
  V(PyDictIterKey_Type)                                                        \
  V(PyDictIterValue_Type)                                                      \
  V(PyDictKeys_Type)                                                           \
  V(PyDictProxy_Type)                                                          \
  V(PyDictValues_Type)                                                         \
  V(PyDict_Type)                                                               \
  V(PyEllipsis_Type)                                                           \
  V(PyEnum_Type)                                                               \
  V(PyFloat_Type)                                                              \
  V(PyFrozenSet_Type)                                                          \
  V(PyFunction_Type)                                                           \
  V(PyGen_Type)                                                                \
  V(PyListIter_Type)                                                           \
  V(PyList_Type)                                                               \
  V(PyLongRangeIter_Type)                                                      \
  V(PyLong_Type)                                                               \
  V(PyMemoryView_Type)                                                         \
  V(PyMethod_Type)                                                             \
  V(PyModule_Type)                                                             \
  V(PyProperty_Type)                                                           \
  V(PyRangeIter_Type)                                                          \
  V(PyRange_Type)                                                              \
  V(PySeqIter_Type)                                                            \
  V(PySetIter_Type)                                                            \
  V(PySet_Type)                                                                \
  V(PySlice_Type)                                                              \
  V(PyStaticMethod_Type)                                                       \
  V(PySuper_Type)                                                              \
  V(PyTupleIter_Type)                                                          \
  V(PyTuple_Type)                                                              \
  V(PyType_Type)                                                               \
  V(PyUnicodeIter_Type)                                                        \
  V(PyUnicode_Type)                                                            \
  V(_PyNone_Type)                                                              \
  V(_PyNotImplemented_Type)

#define FOREACH_POINTER(V)                                                     \
  V(PyExc_ArithmeticError)                                                     \
  V(PyExc_AssertionError)                                                      \
  V(PyExc_AttributeError)                                                      \
  V(PyExc_BaseException)                                                       \
  V(PyExc_BlockingIOError)                                                     \
  V(PyExc_BrokenPipeError)                                                     \
  V(PyExc_BufferError)                                                         \
  V(PyExc_BytesWarning)                                                        \
  V(PyExc_ChildProcessError)                                                   \
  V(PyExc_ConnectionAbortedError)                                              \
  V(PyExc_ConnectionError)                                                     \
  V(PyExc_ConnectionRefusedError)                                              \
  V(PyExc_ConnectionResetError)                                                \
  V(PyExc_DeprecationWarning)                                                  \
  V(PyExc_EOFError)                                                            \
  V(PyExc_EnvironmentError)                                                    \
  V(PyExc_Exception)                                                           \
  V(PyExc_FileExistsError)                                                     \
  V(PyExc_FileNotFoundError)                                                   \
  V(PyExc_FloatingPointError)                                                  \
  V(PyExc_FutureWarning)                                                       \
  V(PyExc_GeneratorExit)                                                       \
  V(PyExc_IOError)                                                             \
  V(PyExc_ImportError)                                                         \
  V(PyExc_ImportWarning)                                                       \
  V(PyExc_IndentationError)                                                    \
  V(PyExc_IndexError)                                                          \
  V(PyExc_InterruptedError)                                                    \
  V(PyExc_IsADirectoryError)                                                   \
  V(PyExc_KeyError)                                                            \
  V(PyExc_KeyboardInterrupt)                                                   \
  V(PyExc_LookupError)                                                         \
  V(PyExc_MemoryError)                                                         \
  V(PyExc_ModuleNotFoundError)                                                 \
  V(PyExc_NameError)                                                           \
  V(PyExc_NotADirectoryError)                                                  \
  V(PyExc_NotImplementedError)                                                 \
  V(PyExc_OSError)                                                             \
  V(PyExc_OverflowError)                                                       \
  V(PyExc_PendingDeprecationWarning)                                           \
  V(PyExc_PermissionError)                                                     \
  V(PyExc_ProcessLookupError)                                                  \
  V(PyExc_RecursionError)                                                      \
  V(PyExc_ReferenceError)                                                      \
  V(PyExc_ResourceWarning)                                                     \
  V(PyExc_RuntimeError)                                                        \
  V(PyExc_RuntimeWarning)                                                      \
  V(PyExc_StopAsyncIteration)                                                  \
  V(PyExc_StopIteration)                                                       \
  V(PyExc_SyntaxError)                                                         \
  V(PyExc_SyntaxWarning)                                                       \
  V(PyExc_SystemError)                                                         \
  V(PyExc_SystemExit)                                                          \
  V(PyExc_TabError)                                                            \
  V(PyExc_TimeoutError)                                                        \
  V(PyExc_TypeError)                                                           \
  V(PyExc_UnboundLocalError)                                                   \
  V(PyExc_UnicodeDecodeError)                                                  \
  V(PyExc_UnicodeEncodeError)                                                  \
  V(PyExc_UnicodeError)                                                        \
  V(PyExc_UnicodeTranslateError)                                               \
  V(PyExc_UnicodeWarning)                                                      \
  V(PyExc_UserWarning)                                                         \
  V(PyExc_ValueError)                                                          \
  V(PyExc_Warning)                                                             \
  V(PyExc_ZeroDivisionError)                                                   \
  V(Py_Ellipsis)                                                               \
  V(Py_False)                                                                  \
  V(Py_None)                                                                   \
  V(Py_NotImplemented)                                                         \
  V(Py_True)                                                                   \
  V(_PyLong_One)                                                               \
  V(_PyLong_Zero)

void finalizeCAPIModules() {
#define DECREF(ptr) Py_DECREF(&ptr);
  FOREACH_STATICTYPE(DECREF);
#undef DECREF
#define DECREF(ptr) Py_DECREF(ptr);
  FOREACH_POINTER(DECREF)
#undef DECREF
}

void initializeCAPIModules() {
  CHECK(_PyCapsule_Init() == 0, "Failed to initialize PyCapsule");
  CHECK(_PySTEntry_Init() == 0, "Failed to initialize PySTEntry");
  // Even though our runtime keeps objects like the `dict` type alive, the
  // handle (`PyDict_Type`) may not live as long. This is because we are using
  // a borrowedReference to simulate CPython's reference to a static type. To
  // mitigate this, incref each well-known handle name once in initialization
  // and decref it again in finalization.
#define INCREF(ptr) Py_INCREF(&ptr);
  FOREACH_STATICTYPE(INCREF);
#undef INCREF
#define INCREF(ptr) Py_INCREF(ptr);
  FOREACH_POINTER(INCREF)
#undef INCREF
}

PY_EXPORT int Py_FinalizeEx() {
  Thread* thread = Thread::current();
  Runtime* runtime = thread->runtime();
  delete runtime;
  return 0;
}

static bool boolFromEnv(const char* name, bool default_value) {
  if (Py_IgnoreEnvironmentFlag) return default_value;
  const char* value = std::getenv(name);
  if (value == nullptr) return default_value;
  if (std::strcmp(value, "0") == 0) return false;
  if (std::strcmp(value, "1") == 0) return true;
  fprintf(stderr, "Error: Environment variable '%s' must be '0' or '1'\n",
          name);
  return default_value;
}

PY_EXPORT void Py_Initialize() { Py_InitializeEx(1); }

static void initializeSysFromGlobals(Thread* thread) {
  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  unique_c_ptr<char> path(OS::executablePath());
  Str executable(&scope, runtime->newStrFromCStr(path.get()));

  Object python_path_obj(&scope, NoneType::object());
  const wchar_t* explicitly_provided_module_search_path =
      Runtime::moduleSearchPath();
  bool has_explicitly_provided_module_search_path =
      explicitly_provided_module_search_path[0] != L'\0';
  if (has_explicitly_provided_module_search_path) {
    // TODO(T88306794): Instead of passing in the python path to initializeSys,
    // we should indicate that binary_location/../lib should not be included in
    // the search path when an explicit module search path is provided.
    Str python_path_str(
        &scope,
        newStrFromWideChar(thread, explicitly_provided_module_search_path));
    Str sep(&scope, SmallStr::fromCStr(":"));
    python_path_obj = strSplit(thread, python_path_str, sep, kMaxWord);
    CHECK(!python_path_obj.isError(),
          "Failed to calculate path provided by `Py_SetPath`.");
  } else {
    const char* python_path_cstr =
        Py_IgnoreEnvironmentFlag ? nullptr : std::getenv("PYTHONPATH");
    if (python_path_cstr != nullptr) {
      Str python_path_str(&scope, runtime->newStrFromCStr(python_path_cstr));
      Str sep(&scope, SmallStr::fromCStr(":"));
      python_path_obj = strSplit(thread, python_path_str, sep, kMaxWord);
      CHECK(!python_path_obj.isError(), "Failed to calculate PYTHONPATH");
    } else {
      python_path_obj = runtime->newList();
    }
  }
  List python_path(&scope, *python_path_obj);
  const char* warnoptions_cstr =
      Py_IgnoreEnvironmentFlag ? nullptr : std::getenv("PYTHONWARNINGS");
  Object warnoptions_obj(&scope, NoneType::object());
  if (warnoptions_cstr != nullptr) {
    Str warnoptions_str(&scope, runtime->newStrFromCStr(warnoptions_cstr));
    Str sep(&scope, SmallStr::fromCStr(","));
    warnoptions_obj = strSplit(thread, warnoptions_str, sep, kMaxWord);
  } else {
    warnoptions_obj = runtime->newList();
  }
  List warnoptions(&scope, *warnoptions_obj);
  Object option(&scope, NoneType::object());
  for (word i = 0, size = warn_options.size(); i < size; i++) {
    option = runtime->newStrFromCStr(warn_options[i]);
    runtime->listAdd(thread, warnoptions, option);
  }
  warn_options.release();

  const char* pycache_prefix_cstr =
      Py_IgnoreEnvironmentFlag ? nullptr : std::getenv("PYTHONPYCACHEPREFIX");
  if (pycache_prefix_cstr != nullptr) {
    Str pycache_prefix_str(&scope,
                           runtime->newStrFromCStr(pycache_prefix_cstr));
    setPycachePrefix(thread, pycache_prefix_str);
  }

  MutableTuple data(
      &scope, runtime->newMutableTuple(static_cast<word>(SysFlag::kNumFlags)));
  data.atPut(static_cast<word>(SysFlag::kDebug),
             SmallInt::fromWord(Py_DebugFlag));
  data.atPut(static_cast<word>(SysFlag::kInspect),
             SmallInt::fromWord(Py_InspectFlag));
  data.atPut(static_cast<word>(SysFlag::kInteractive),
             SmallInt::fromWord(Py_InteractiveFlag));
  data.atPut(static_cast<word>(SysFlag::kOptimize),
             SmallInt::fromWord(Py_OptimizeFlag));
  data.atPut(static_cast<word>(SysFlag::kDontWriteBytecode),
             SmallInt::fromWord(Py_DontWriteBytecodeFlag));
  data.atPut(static_cast<word>(SysFlag::kNoUserSite),
             SmallInt::fromWord(Py_NoUserSiteDirectory));
  data.atPut(static_cast<word>(SysFlag::kNoSite),
             SmallInt::fromWord(Py_NoSiteFlag));
  data.atPut(static_cast<word>(SysFlag::kIgnoreEnvironment),
             SmallInt::fromWord(Py_IgnoreEnvironmentFlag));
  data.atPut(static_cast<word>(SysFlag::kVerbose),
             SmallInt::fromWord(Py_VerboseFlag));
  data.atPut(static_cast<word>(SysFlag::kBytesWarning),
             SmallInt::fromWord(Py_BytesWarningFlag));
  data.atPut(static_cast<word>(SysFlag::kQuiet),
             SmallInt::fromWord(Py_QuietFlag));
  data.atPut(static_cast<word>(SysFlag::kHashRandomization),
             SmallInt::fromWord(Py_HashRandomizationFlag));
  data.atPut(static_cast<word>(SysFlag::kIsolated),
             SmallInt::fromWord(Py_IsolatedFlag));
  data.atPut(static_cast<word>(SysFlag::kDevMode), Bool::falseObj());
  data.atPut(static_cast<word>(SysFlag::kUTF8Mode),
             SmallInt::fromWord(Py_UTF8Mode));
  static_assert(static_cast<word>(SysFlag::kNumFlags) == 15,
                "unexpected flag count");
  Tuple flags_data(&scope, data.becomeImmutable());
  CHECK(initializeSys(thread, executable, python_path, flags_data, warnoptions,
                      /*extend_python_path_with_stdlib=*/
                      !has_explicitly_provided_module_search_path)
            .isNoneType(),
        "initializeSys() failed");
}

PY_EXPORT void Py_InitializeEx(int initsigs) {
  CHECK(Py_BytesWarningFlag == 0, "Py_BytesWarningFlag != 0 not supported");
  CHECK(Py_DebugFlag == 0, "parser debug mode not supported");
  CHECK(Py_UTF8Mode == 1, "UTF8Mode != 1 not supported");
  CHECK(initsigs == 1, "Skipping signal handler registration unimplemented");
  // TODO(T63603973): Reduce initial heap size once we can auto-grow the heap
  word heap_size = word{2} * kGiB;
  RandomState random_seed;
  const char* hashseed =
      Py_IgnoreEnvironmentFlag ? nullptr : std::getenv("PYTHONHASHSEED");
  if (hashseed != nullptr && hashseed[0] != '\0' &&
      std::strcmp(hashseed, "random") != 0) {
    char* endptr;
    unsigned long seed = std::strtoul(hashseed, &endptr, 10);
    if (*endptr != '\0' || seed > 4294967295 ||
        (seed == ULONG_MAX && errno == ERANGE)) {
      Py_FatalError(
          "PYTHONHASHSEED must be \"random\" or an integer in range [0; "
          "4294967295]");
    }
    random_seed = randomStateFromSeed(static_cast<uint64_t>(seed));
    Py_HashRandomizationFlag = (seed != 0);
  } else {
    random_seed = randomState();
    Py_HashRandomizationFlag = 1;
  }
  StdioState stdio_state =
      Py_UnbufferedStdioFlag ? StdioState::kUnbuffered : StdioState::kBuffered;
  Interpreter* interpreter = boolFromEnv("PYRO_CPP_INTERPRETER", false)
                                 ? createCppInterpreter()
                                 : createAsmInterpreter();
  Runtime* runtime =
      new Runtime(heap_size, interpreter, random_seed, stdio_state);
  Thread* thread = Thread::current();
  initializeSysFromGlobals(thread);
  CHECK(runtime->initialize(thread).isNoneType(),
        "Failed to initialize runtime");
}

PY_EXPORT int Py_IsInitialized() {
  Thread* thread = Thread::current();
  if (thread == nullptr) {
    return 0;
  }
  Runtime* runtime = thread->runtime();
  CHECK(runtime != nullptr, "runtime is expected not to be null");
  return runtime->initialized();
}

PY_EXPORT PyThreadState* Py_NewInterpreter() {
  UNIMPLEMENTED("Py_NewInterpreter");
}

struct AtExitContext {
  void (*func)(PyObject*);
  PyObject* module;
};

static void callAtExitFunction(void* context) {
  DCHECK(context != nullptr, "context must not be null");
  AtExitContext* thunk = reinterpret_cast<AtExitContext*>(context);
  DCHECK(thunk->func != nullptr, "function must not be null");
  thunk->func(thunk->module);
  // CPython does not own the reference, but that's dangerous.
  Py_DECREF(thunk->module);
  PyErr_Clear();
  delete thunk;
}

PY_EXPORT void _Py_PyAtExit(void (*func)(PyObject*), PyObject* module) {
  AtExitContext* thunk = new AtExitContext;
  thunk->func = func;
  // CPython does not own the reference, but that's dangerous.
  Py_INCREF(module);
  thunk->module = module;
  Thread::current()->runtime()->setAtExit(callAtExitFunction, thunk);
}

PY_EXPORT void _Py_RestoreSignals() {
  PyOS_setsig(SIGPIPE, SIG_DFL);
  PyOS_setsig(SIGXFSZ, SIG_DFL);
}

// NOTE: this implementation does not work for Android
PY_EXPORT char* _Py_SetLocaleFromEnv(int category) {
  return std::setlocale(category, "");
}

PY_EXPORT int _Py_IsFinalizing(void) {
  if (Thread::current() == nullptr) return 0;
  Runtime* runtime = Thread::current()->runtime();
  DCHECK(runtime != nullptr, "unexpected");
  return runtime->isFinalizing();
}

}  // namespace py
