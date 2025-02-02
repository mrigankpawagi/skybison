// Copyright (c) Facebook, Inc. and its affiliates. (http://www.facebook.com)
#include <cstdarg>

#include "../Python/modsupport-internal.h"
#include "cpython-data.h"
#include "cpython-func.h"

#include "api-handle.h"
#include "array-module.h"
#include "attributedict.h"
#include "bytearrayobject-utils.h"
#include "bytesobject-utils.h"
#include "capi-typeslots.h"
#include "capi.h"
#include "exception-builtins.h"
#include "formatter.h"
#include "frame.h"
#include "int-builtins.h"
#include "list-builtins.h"
#include "object-builtins.h"
#include "runtime.h"
#include "type-builtins.h"

namespace py {

static PyObject* nullError(Thread* thread) {
  if (!thread->hasPendingException()) {
    thread->raiseWithFmt(LayoutId::kSystemError,
                         "null argument to internal routine");
  }
  return nullptr;
}

static PyObject* doUnaryOp(SymbolId op, PyObject* obj) {
  Thread* thread = Thread::current();
  if (obj == nullptr) {
    return nullError(thread);
  }

  HandleScope scope(thread);
  Object object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
  Object result(&scope, thread->invokeFunction1(ID(operator), op, object));
  return result.isError() ? nullptr
                          : ApiHandle::newReference(thread->runtime(), *result);
}

static PyObject* doBinaryOp(SymbolId op, PyObject* left, PyObject* right) {
  Thread* thread = Thread::current();
  DCHECK(left != nullptr && right != nullptr, "null argument to binary op %s",
         Symbols::predefinedSymbolAt(op));
  HandleScope scope(thread);
  Object left_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(left)));
  Object right_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(right)));
  Object result(&scope,
                thread->invokeFunction2(ID(operator), op, left_obj, right_obj));
  return result.isError() ? nullptr
                          : ApiHandle::newReference(thread->runtime(), *result);
}

static Py_ssize_t objectLength(PyObject* pyobj) {
  Thread* thread = Thread::current();
  if (pyobj == nullptr) {
    nullError(thread);
    return -1;
  }

  HandleScope scope(thread);
  Object obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(pyobj)));
  Object len_index(&scope, thread->invokeMethod1(obj, ID(__len__)));
  if (len_index.isError()) {
    if (len_index.isErrorNotFound()) {
      thread->raiseWithFmt(LayoutId::kTypeError, "object has no len()");
    }
    return -1;
  }
  Object len(&scope, intFromIndex(thread, len_index));
  if (len.isError()) {
    return -1;
  }
  Int index(&scope, intUnderlying(*len));
  if (index.isNegative()) {
    thread->raiseWithFmt(LayoutId::kValueError, "__len__() should return >= 0");
    return -1;
  }
  if (index.numDigits() > 1) {
    thread->raiseWithFmt(LayoutId::kOverflowError,
                         "cannot fit '%T' into an index-sized integer",
                         &len_index);
    return -1;
  }
  return index.asWord();
}

// Buffer Protocol

static RawObject raiseBufferError(Thread* thread, const Object& obj) {
  return thread->raiseWithFmt(
      LayoutId::kTypeError, "a bytes-like object is required, not '%T'", &obj);
}

RawObject newBytesFromBuffer(Thread* thread, const Object& obj) {
  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  Type type(&scope, runtime->typeOf(*obj));
  if (!typeHasSlots(type)) {
    return raiseBufferError(thread, obj);
  }
  void* get_slot = typeSlotAt(type, Py_bf_getbuffer);
  if (get_slot == nullptr) {
    return raiseBufferError(thread, obj);
  }
  Py_buffer view;
  int flags = PyBUF_SIMPLE;
  ApiHandle* handle = ApiHandle::borrowedReference(runtime, *obj);
  int get_result =
      reinterpret_cast<getbufferproc>(get_slot)(handle, &view, flags);
  if (get_result != 0) {
    return Error::exception();
  }
  DCHECK(view.readonly, "writable buffers not supported");
  DCHECK(view.ndim == 1, "multi-dimensional buffers not supported");
  Bytes result(&scope, runtime->newBytesWithAll(
                           {reinterpret_cast<byte*>(view.buf), view.len}));
  void* release_slot = typeSlotAt(type, Py_bf_releasebuffer);
  // The release slot may not be defined. That's allowed.
  if (release_slot != nullptr) {
    reinterpret_cast<releasebufferproc>(release_slot)(handle, &view);
  }
  return *result;
}

PY_EXPORT int PyBuffer_FillInfo(Py_buffer* view, PyObject* exporter, void* buf,
                                Py_ssize_t len, int readonly, int flags) {
  if (view == nullptr) {
    Thread::current()->raiseWithFmt(
        LayoutId::kBufferError,
        "PyBuffer_FillInfo: view==NULL argument is obsolete");
    return -1;
  }
  if ((flags & PyBUF_WRITABLE) == PyBUF_WRITABLE && readonly == 1) {
    Thread::current()->raiseWithFmt(LayoutId::kBufferError,
                                    "Object is not writable.");
    return -1;
  }

  if (exporter != nullptr) {
    Py_INCREF(exporter);
  }
  view->obj = exporter;
  view->buf = buf;
  view->len = len;
  view->readonly = readonly;
  view->itemsize = 1;
  view->format = nullptr;
  if ((flags & PyBUF_FORMAT) == PyBUF_FORMAT) {
    view->format = const_cast<char*>("B");
  }
  view->ndim = 1;
  view->shape = nullptr;
  if ((flags & PyBUF_ND) == PyBUF_ND) {
    view->shape = &(view->len);
  }
  view->strides = nullptr;
  if ((flags & PyBUF_STRIDES) == PyBUF_STRIDES) {
    view->strides = &(view->itemsize);
  }
  view->suboffsets = nullptr;
  view->internal = nullptr;
  return 0;
}

static bool isContiguousWithRowMajorOrder(const Py_buffer* view) {
  if (view->suboffsets != nullptr) return false;
  if (view->strides == nullptr) return true;
  if (view->len == 0) return true;

  Py_ssize_t dim_stride = view->itemsize;
  for (int d = view->ndim - 1; d >= 0; d--) {
    Py_ssize_t dim_size = view->shape[d];
    if (dim_size > 1 && view->strides[d] != dim_stride) {
      return false;
    }
    dim_stride *= dim_size;
  }
  return true;
}

static bool isContiguousWithColumnMajorOrder(const Py_buffer* view) {
  if (view->suboffsets != nullptr) return false;
  if (view->len == 0) return true;
  if (view->strides == nullptr) {
    if (view->ndim <= 1) return true;
    // Non-contiguous if there is more than 1 dimension with size > 0.
    bool had_nonempty_dim = false;
    for (int d = 0; d < view->ndim; d++) {
      if (view->shape[d] > 1) {
        if (had_nonempty_dim) return false;
        had_nonempty_dim = true;
      }
    }
    return true;
  }

  Py_ssize_t dim_stride = view->itemsize;
  for (int d = 0; d < view->ndim; d++) {
    Py_ssize_t dim_size = view->shape[d];
    if (dim_size > 1 && view->strides[d] != dim_stride) {
      return false;
    }
    dim_stride *= dim_size;
  }
  return true;
}

PY_EXPORT int PyBuffer_IsContiguous(const Py_buffer* view, char order) {
  if (order == 'C') {
    return isContiguousWithRowMajorOrder(view);
  }
  if (order == 'F') {
    return isContiguousWithColumnMajorOrder(view);
  }
  if (order == 'A') {
    return isContiguousWithRowMajorOrder(view) ||
           isContiguousWithColumnMajorOrder(view);
  }
  return false;
}

PY_EXPORT void PyBuffer_Release(Py_buffer* view) {
  DCHECK(view != nullptr, "view must not be nullptr");
  PyObject* pyobj = view->obj;
  if (pyobj == nullptr) return;

  Thread* thread = Thread::current();
  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  Object object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(pyobj)));
  Type type(&scope, runtime->typeOf(*object));
  if (typeHasSlots(type)) {
    // Call Py_bf_releasebuffer slot if defined
    void* releasebuffer_fn =
        PyType_GetSlot(Py_TYPE(pyobj), Py_bf_releasebuffer);
    if (releasebuffer_fn != nullptr) {
      reinterpret_cast<releasebufferproc>(releasebuffer_fn)(pyobj, view);
    }
  }
  view->obj = nullptr;
  Py_DECREF(pyobj);
}

// PyIndex_Check

PY_EXPORT int PyIndex_Check_Func(PyObject* obj) {
  DCHECK(obj != nullptr, "Got null argument");
  Thread* thread = Thread::current();
  HandleScope scope(thread);
  Object num(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
  Type type(&scope, thread->runtime()->typeOf(*num));
  return !typeLookupInMroById(thread, *type, ID(__index__)).isErrorNotFound();
}

// PyIter_Next

PY_EXPORT PyObject* PyIter_Next(PyObject* iter) {
  Thread* thread = Thread::current();
  HandleScope scope(thread);
  Object iter_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(iter)));
  Object next(&scope, thread->invokeMethod1(iter_obj, ID(__next__)));
  if (thread->clearPendingStopIteration()) {
    // End of iterable
    return nullptr;
  }
  if (next.isError()) {
    // Method lookup or call failed
    if (next.isErrorNotFound()) {
      thread->raiseWithFmt(LayoutId::kTypeError,
                           "failed to call __next__ on iterable");
    }
    return nullptr;
  }
  return ApiHandle::newReference(thread->runtime(), *next);
}

// Mapping Protocol

PY_EXPORT int PyMapping_Check(PyObject* py_obj) {
  Thread* thread = Thread::current();
  HandleScope scope(thread);
  Object obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(py_obj)));
  return thread->runtime()->isMapping(thread, obj);
}

PY_EXPORT int PyMapping_DelItemString(PyObject* obj, const char* attr_name) {
  return PyObject_DelItemString(obj, attr_name);
}

PY_EXPORT int PyMapping_DelItem(PyObject* obj, PyObject* attr_name) {
  return PyObject_DelItem(obj, attr_name);
}

PY_EXPORT PyObject* PyMapping_GetItemString(PyObject* obj, const char* key) {
  Thread* thread = Thread::current();
  if (obj == nullptr || key == nullptr) {
    return nullError(thread);
  }
  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  Object object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
  Object key_obj(&scope, runtime->newStrFromCStr(key));
  Object result(&scope, objectGetItem(thread, object, key_obj));
  if (result.isErrorException()) return nullptr;
  return ApiHandle::newReference(runtime, *result);
}

PY_EXPORT int PyMapping_HasKey(PyObject* obj, PyObject* key) {
  PyObject* v = PyObject_GetItem(obj, key);
  if (v != nullptr) {
    Py_DECREF(v);
    return 1;
  }
  PyErr_Clear();
  return 0;
}

PY_EXPORT int PyMapping_HasKeyString(PyObject* obj, const char* key) {
  PyObject* v = PyMapping_GetItemString(obj, key);
  if (v != nullptr) {
    Py_DECREF(v);
    return 1;
  }
  PyErr_Clear();
  return 0;
}

PY_EXPORT PyObject* PyMapping_Items(PyObject* mapping) {
  if (PyDict_CheckExact(mapping)) {
    return PyDict_Items(mapping);
  }
  PyObject* items = PyObject_CallMethod(mapping, "items", nullptr);
  if (items == nullptr) {
    return nullptr;
  }
  PyObject* fast = PySequence_Fast(items, "mapping.items() are not iterable");
  Py_DECREF(items);
  return fast;
}

PY_EXPORT PyObject* PyMapping_Keys(PyObject* mapping) {
  DCHECK(mapping != nullptr, "mapping was null");
  if (PyDict_CheckExact(mapping)) {
    return PyDict_Keys(mapping);
  }
  PyObject* keys = PyObject_CallMethod(mapping, "keys", nullptr);
  if (keys == nullptr) {
    return nullptr;
  }
  PyObject* fast = PySequence_Fast(keys, "mapping.keys() are not iterable");
  Py_DECREF(keys);
  return fast;
}

PY_EXPORT Py_ssize_t PyMapping_Length(PyObject* pyobj) {
  return objectLength(pyobj);
}

PY_EXPORT int PyMapping_SetItemString(PyObject* obj, const char* key,
                                      PyObject* value) {
  if (key == nullptr) {
    nullError(Thread::current());
    return -1;
  }
  PyObject* key_obj = PyUnicode_FromString(key);
  if (key_obj == nullptr) {
    return -1;
  }
  int r = PyObject_SetItem(obj, key_obj, value);
  Py_DECREF(key_obj);
  return r;
}

PY_EXPORT Py_ssize_t PyMapping_Size(PyObject* pyobj) {
  return objectLength(pyobj);
}

PY_EXPORT PyObject* PyMapping_Values(PyObject* mapping) {
  if (PyDict_CheckExact(mapping)) {
    return PyDict_Values(mapping);
  }
  PyObject* values = PyObject_CallMethod(mapping, "values", nullptr);
  if (values == nullptr) {
    return nullptr;
  }
  PyObject* fast = PySequence_Fast(values, "mapping.values() are not iterable");
  Py_DECREF(values);
  return fast;
}

// Number Protocol

PY_EXPORT PyObject* PyNumber_Absolute(PyObject* obj) {
  return doUnaryOp(ID(abs), obj);
}

static PyObject* smallIntAdd(PyObject* left, PyObject* right) {
  RawObject left_obj = ApiHandle::asObject(ApiHandle::fromPyObject(left));
  RawObject right_obj = ApiHandle::asObject(ApiHandle::fromPyObject(right));
  if (left_obj.isSmallInt() && right_obj.isSmallInt()) {
    Runtime* runtime = Thread::current()->runtime();
    return ApiHandle::newReference(
        runtime, runtime->newInt(SmallInt::cast(left_obj).value() +
                                 SmallInt::cast(right_obj).value()));
  }
  return nullptr;
}

PY_EXPORT PyObject* PyNumber_Add(PyObject* left, PyObject* right) {
  PyObject* result = smallIntAdd(left, right);
  if (result != nullptr) {
    // Fast path: smallint + smallint.
    return result;
  }
  return doBinaryOp(ID(add), left, right);
}

PY_EXPORT PyObject* PyNumber_And(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(and_), left, right);
}

PY_EXPORT Py_ssize_t PyNumber_AsSsize_t(PyObject* obj, PyObject* overflow_err) {
  Thread* thread = Thread::current();
  if (obj == nullptr) {
    nullError(thread);
    return -1;
  }
  HandleScope scope(thread);
  Object index(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
  Object num(&scope, intFromIndex(thread, index));
  if (num.isError()) return -1;
  Int number(&scope, intUnderlying(*num));
  if (overflow_err == nullptr || number.numDigits() == 1) {
    // Overflows should be clipped, or value is already in range.
    return number.asWordSaturated();
  }
  // Value overflows, raise an exception.
  thread->setPendingExceptionType(
      ApiHandle::asObject(ApiHandle::fromPyObject(overflow_err)));
  thread->setPendingExceptionValue(thread->runtime()->newStrFromFmt(
      "cannot fit '%T' into an index-sized integer", &index));
  return -1;
}

PY_EXPORT int PyNumber_Check(PyObject* obj) {
  if (obj == nullptr) {
    return false;
  }

  Thread* thread = Thread::current();
  HandleScope scope(thread);
  Object num(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
  Type type(&scope, thread->runtime()->typeOf(*num));
  if (!typeLookupInMroById(thread, *type, ID(__int__)).isErrorNotFound()) {
    return true;
  }
  if (!typeLookupInMroById(thread, *type, ID(__float__)).isErrorNotFound()) {
    return true;
  }
  return false;
}

PY_EXPORT PyObject* PyNumber_Divmod(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(divmod), left, right);
}

PY_EXPORT PyObject* PyNumber_Float(PyObject* obj) {
  Thread* thread = Thread::current();
  if (obj == nullptr) {
    return nullError(thread);
  }
  HandleScope scope(thread);
  Object object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
  Object flt(&scope, thread->invokeFunction1(ID(builtins), ID(float), object));
  return flt.isError() ? nullptr
                       : ApiHandle::newReference(thread->runtime(), *flt);
}

PY_EXPORT PyObject* PyNumber_FloorDivide(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(floordiv), left, right);
}

PY_EXPORT PyObject* PyNumber_Index(PyObject* item) {
  Thread* thread = Thread::current();
  if (item == nullptr) {
    return nullError(thread);
  }

  HandleScope scope(thread);
  Object obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(item)));
  Object index(&scope, intFromIndex(thread, obj));
  return index.isError() ? nullptr
                         : ApiHandle::newReference(thread->runtime(), *index);
}

PY_EXPORT PyObject* PyNumber_InPlaceAdd(PyObject* left, PyObject* right) {
  PyObject* result = smallIntAdd(left, right);
  if (result != nullptr) {
    // Fast path: smallint + smallint.
    // In case operands are SmallInts, InPlaceAdd doesn't mutate them.
    return result;
  }
  return doBinaryOp(ID(iadd), left, right);
}

PY_EXPORT PyObject* PyNumber_InPlaceAnd(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(iand), left, right);
}

PY_EXPORT PyObject* PyNumber_InPlaceFloorDivide(PyObject* left,
                                                PyObject* right) {
  return doBinaryOp(ID(ifloordiv), left, right);
}

PY_EXPORT PyObject* PyNumber_InPlaceLshift(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(ilshift), left, right);
}

PY_EXPORT PyObject* PyNumber_InPlaceMatrixMultiply(PyObject* left,
                                                   PyObject* right) {
  return doBinaryOp(ID(imatmul), left, right);
}

PY_EXPORT PyObject* PyNumber_InPlaceMultiply(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(imul), left, right);
}

PY_EXPORT PyObject* PyNumber_InPlaceOr(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(ior), left, right);
}

PY_EXPORT PyObject* PyNumber_InPlacePower(PyObject* base, PyObject* exponent,
                                          PyObject* divisor) {
  if (divisor == Py_None) {
    return doBinaryOp(ID(ipow), base, exponent);
  }
  UNIMPLEMENTED("ipow(base, exponent, divisor)");
}

PY_EXPORT PyObject* PyNumber_InPlaceRemainder(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(imod), left, right);
}

PY_EXPORT PyObject* PyNumber_InPlaceRshift(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(irshift), left, right);
}

PY_EXPORT PyObject* PyNumber_InPlaceSubtract(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(isub), left, right);
}

PY_EXPORT PyObject* PyNumber_InPlaceTrueDivide(PyObject* left,
                                               PyObject* right) {
  return doBinaryOp(ID(itruediv), left, right);
}

PY_EXPORT PyObject* PyNumber_InPlaceXor(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(ixor), left, right);
}

PY_EXPORT PyObject* PyNumber_Invert(PyObject* pyobj) {
  return doUnaryOp(ID(invert), pyobj);
}

PY_EXPORT PyObject* PyNumber_Long(PyObject* obj) {
  Thread* thread = Thread::current();
  if (obj == nullptr) {
    return nullError(thread);
  }
  HandleScope scope(thread);
  Object object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
  Object result(&scope, thread->invokeFunction1(ID(builtins), ID(int), object));
  if (result.isError()) {
    return nullptr;
  }
  return ApiHandle::newReference(thread->runtime(), *result);
}

PY_EXPORT PyObject* PyNumber_Lshift(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(lshift), left, right);
}

PY_EXPORT PyObject* PyNumber_MatrixMultiply(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(matmul), left, right);
}

PY_EXPORT PyObject* PyNumber_Multiply(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(mul), left, right);
}

PY_EXPORT PyObject* PyNumber_Negative(PyObject* pyobj) {
  return doUnaryOp(ID(neg), pyobj);
}

PY_EXPORT PyObject* PyNumber_Or(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(or_), left, right);
}

PY_EXPORT PyObject* PyNumber_Positive(PyObject* pyobj) {
  return doUnaryOp(ID(pos), pyobj);
}

PY_EXPORT PyObject* PyNumber_Power(PyObject* base, PyObject* exponent,
                                   PyObject* divisor) {
  if (divisor == Py_None) {
    return doBinaryOp(ID(pow), base, exponent);
  }
  UNIMPLEMENTED("pow(base, exponent, divisor)");
}

PY_EXPORT PyObject* PyNumber_Remainder(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(mod), left, right);
}

PY_EXPORT PyObject* PyNumber_Rshift(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(rshift), left, right);
}

PY_EXPORT PyObject* PyNumber_Subtract(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(sub), left, right);
}

PY_EXPORT PyObject* PyNumber_ToBase(PyObject* n, int base) {
  Thread* thread = Thread::current();
  HandleScope scope(thread);
  Object n_object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(n)));
  n_object = intFromIndex(thread, n_object);
  if (n_object.isError()) {
    return nullptr;
  }
  Int number(&scope, intUnderlying(*n_object));
  Object formatted(&scope, NoneType::object());
  switch (base) {
    case 2:
      formatted = formatIntBinarySimple(thread, number);
      break;
    case 8:
      formatted = formatIntOctalSimple(thread, number);
      break;
    case 10:
      formatted = formatIntDecimalSimple(thread, number);
      break;
    case 16:
      formatted = formatIntHexadecimalSimple(thread, number);
      break;
    default:
      thread->raiseWithFmt(LayoutId::kSystemError,
                           "PyNumber_ToBase: base must be 2, 8, 10 or 16");
      return nullptr;
  }
  return ApiHandle::newReference(thread->runtime(), *formatted);
}

PY_EXPORT PyObject* PyNumber_TrueDivide(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(truediv), left, right);
}

PY_EXPORT PyObject* PyNumber_Xor(PyObject* left, PyObject* right) {
  return doBinaryOp(ID(xor), left, right);
}

// Object Protocol

PY_EXPORT int PyObject_AsCharBuffer(PyObject* /* j */,
                                    const char** /* buffer */,
                                    Py_ssize_t* /* n */) {
  UNIMPLEMENTED("PyObject_AsCharBuffer");
}

PY_EXPORT int PyObject_AsReadBuffer(PyObject* /* j */,
                                    const void** /* buffer */,
                                    Py_ssize_t* /* n */) {
  UNIMPLEMENTED("PyObject_AsReadBuffer");
}

PY_EXPORT int PyObject_AsWriteBuffer(PyObject* /* j */, void** /* buffer */,
                                     Py_ssize_t* /* n */) {
  UNIMPLEMENTED("PyObject_AsWriteBuffer");
}

PY_EXPORT PyObject* PyObject_Call(PyObject* callable, PyObject* args,
                                  PyObject* kwargs) {
  Thread* thread = Thread::current();
  if (callable == nullptr) {
    return nullError(thread);
  }

  DCHECK(!thread->hasPendingException(),
         "may accidentally clear pending exception");

  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  Object callable_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(callable)));
  Type callable_type(&scope, runtime->typeOf(*callable_obj));
  if (typeHasSlots(callable_type)) {
    // Attempt to call tp_call directly for native types to avoid
    // recursive interpreter calls.
    void* tp_call_value = typeSlotAt(callable_type, Py_tp_call);
    if (tp_call_value != nullptr) {
      ternaryfunc call = reinterpret_cast<ternaryfunc>(tp_call_value);
      return call(callable, args, kwargs);
    }
  }
  thread->stackPush(*callable_obj);

  Object args_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(args)));
  DCHECK(runtime->isInstanceOfTuple(*args_obj), "args mut be a tuple");
  thread->stackPush(*args_obj);

  word flags = 0;
  if (kwargs != nullptr) {
    Object kwargs_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(kwargs)));
    DCHECK(thread->runtime()->isInstanceOfDict(*kwargs_obj),
           "kwargs must be a dict");
    thread->stackPush(*kwargs_obj);
    flags |= CallFunctionExFlag::VAR_KEYWORDS;
  }

  // TODO(T30925218): Protect against native stack overflow.

  Object result(&scope, Interpreter::callEx(thread, flags));
  if (result.isError()) return nullptr;
  return ApiHandle::newReference(runtime, *result);
}

static PyObject* makeInterpreterCall(Thread* thread, word nargs) {
  HandleScope scope(thread);
  Object result(&scope, Interpreter::call(thread, nargs));
  if (result.isError()) return nullptr;
  return ApiHandle::newReference(thread->runtime(), *result);
}

static PyObject* callWithVarArgs(Thread* thread, const Object& callable,
                                 const char* format, std::va_list* va,
                                 int build_value_flags) {
  thread->stackPush(*callable);

  if (format == nullptr) {
    return makeInterpreterCall(thread, /*nargs=*/0);
  }

  word nargs = countFormat(format, '\0');
  if (nargs == 1) {
    PyObject* value = makeValueFromFormat(&format, va, build_value_flags);
    if (!PyTuple_Check(value)) {
      thread->stackPush(ApiHandle::stealReference(value));
      return makeInterpreterCall(thread, nargs);
    }
    // If the only argument passed is a tuple, splat the tuple as positional
    // arguments
    nargs = PyTuple_Size(value);
    for (word i = 0; i < nargs; i++) {
      PyObject* arg = PyTuple_GetItem(value, i);
      thread->stackPush(ApiHandle::asObject(ApiHandle::fromPyObject(arg)));
    }
    return makeInterpreterCall(thread, nargs);
  }
  for (const char* f = format; *f != '\0';) {
    PyObject* value = makeValueFromFormat(&f, va, build_value_flags);
    if (value == nullptr) break;
    thread->stackPush(ApiHandle::stealReference(value));
  }

  return makeInterpreterCall(thread, nargs);
}

static PyObject* callFunction(PyObject* callable, const char* format,
                              std::va_list* va) {
  Thread* thread = Thread::current();
  if (callable == nullptr) {
    return nullError(thread);
  }

  HandleScope scope(thread);
  Object callable_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(callable)));
  PyObject* result = callWithVarArgs(thread, callable_obj, format, va, 0);
  return result;
}

PY_EXPORT PyObject* PyObject_CallFunction(PyObject* callable,
                                          const char* format, ...) {
  va_list va;
  va_start(va, format);
  PyObject* result = callFunction(callable, format, &va);
  va_end(va);
  return result;
}

PY_EXPORT PyObject* PyEval_CallFunction(PyObject* callable, const char* format,
                                        ...) {
  va_list va;
  va_start(va, format);
  PyObject* result = callFunction(callable, format, &va);
  va_end(va);
  return result;
}

static PyObject* callWithObjArgs(Thread* thread, const Object& callable,
                                 std::va_list va) {
  DCHECK(!thread->hasPendingException(),
         "may accidentally clear pending exception");

  thread->stackPush(*callable);
  word nargs = 0;
  for (PyObject* arg; (arg = va_arg(va, PyObject*)) != nullptr; nargs++) {
    thread->stackPush(ApiHandle::asObject(ApiHandle::fromPyObject(arg)));
  }

  // TODO(T30925218): CPython tracks recursive calls before calling the function
  // through Py_EnterRecursiveCall, and we should probably do the same
  HandleScope scope(thread);
  Object result(&scope, Interpreter::call(thread, nargs));
  if (result.isError()) return nullptr;
  return ApiHandle::newReference(thread->runtime(), *result);
}

PY_EXPORT PyObject* PyObject_CallFunctionObjArgs(PyObject* callable, ...) {
  Thread* thread = Thread::current();
  if (callable == nullptr) {
    return nullError(thread);
  }
  HandleScope scope(thread);
  Object callable_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(callable)));
  va_list va;
  va_start(va, callable);
  PyObject* result = callWithObjArgs(thread, callable_obj, va);
  va_end(va);
  return result;
}

PY_EXPORT PyObject* _PyObject_CallFunction_SizeT(PyObject* callable,
                                                 const char* format, ...) {
  Thread* thread = Thread::current();
  if (callable == nullptr) {
    return nullError(thread);
  }

  HandleScope scope(thread);
  Object callable_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(callable)));
  va_list va;
  va_start(va, format);
  PyObject* result =
      callWithVarArgs(thread, callable_obj, format, &va, kFlagSizeT);
  va_end(va);
  return result;
}

static PyObject* callMethod(PyObject* pyobj, const char* name,
                            const char* format, std::va_list* va) {
  Thread* thread = Thread::current();
  if (pyobj == nullptr) {
    return nullError(thread);
  }

  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  Object obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(pyobj)));
  Object callable(&scope, runtime->attributeAtByCStr(thread, obj, name));
  if (callable.isError()) return nullptr;

  PyObject* result = callWithVarArgs(thread, callable, format, va, 0);
  return result;
}

PY_EXPORT PyObject* PyObject_CallMethod(PyObject* pyobj, const char* name,
                                        const char* format, ...) {
  va_list va;
  va_start(va, format);
  PyObject* result = callMethod(pyobj, name, format, &va);
  va_end(va);
  return result;
}

PY_EXPORT PyObject* PyEval_CallMethod(PyObject* pyobj, const char* name,
                                      const char* format, ...) {
  va_list va;
  va_start(va, format);
  PyObject* result = callMethod(pyobj, name, format, &va);
  va_end(va);
  return result;
}

PY_EXPORT PyObject* PyObject_CallMethodObjArgs(PyObject* pyobj,
                                               PyObject* py_method_name, ...) {
  Thread* thread = Thread::current();
  if (pyobj == nullptr || py_method_name == nullptr) {
    return nullError(thread);
  }
  HandleScope scope(thread);
  Object obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(pyobj)));
  Object name(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(py_method_name)));
  name = attributeName(thread, name);
  if (name.isErrorException()) return nullptr;
  Object callable(&scope, thread->runtime()->attributeAt(thread, obj, name));
  if (callable.isError()) return nullptr;

  va_list va;
  va_start(va, py_method_name);
  PyObject* result = callWithObjArgs(thread, callable, va);
  va_end(va);
  return result;
}

PY_EXPORT PyObject* _PyObject_CallMethod_SizeT(PyObject* pyobj,
                                               const char* name,
                                               const char* format, ...) {
  Thread* thread = Thread::current();
  if (pyobj == nullptr) {
    return nullError(thread);
  }

  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  Object obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(pyobj)));
  Object callable(&scope, runtime->attributeAtByCStr(thread, obj, name));
  if (callable.isError()) return nullptr;

  va_list va;
  va_start(va, format);
  PyObject* result = callWithVarArgs(thread, callable, format, &va, kFlagSizeT);
  va_end(va);
  return result;
}

PY_EXPORT PyObject* PyObject_CallObject(PyObject* callable, PyObject* args) {
  Thread* thread = Thread::current();
  if (callable == nullptr) {
    return nullError(thread);
  }
  DCHECK(!thread->hasPendingException(),
         "may accidentally clear pending exception");
  HandleScope scope(thread);
  thread->stackPush(ApiHandle::asObject(ApiHandle::fromPyObject(callable)));
  Object result(&scope, NoneType::object());
  Runtime* runtime = thread->runtime();
  if (args != nullptr) {
    Object args_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(args)));
    if (!runtime->isInstanceOfTuple(*args_obj)) {
      thread->raiseWithFmt(LayoutId::kTypeError,
                           "argument list must be a tuple");
      return nullptr;
    }
    thread->stackPush(*args_obj);
    // TODO(T30925218): Protect against native stack overflow.
    result = Interpreter::callEx(thread, 0);
  } else {
    result = Interpreter::call(thread, 0);
  }
  if (result.isError()) return nullptr;
  return ApiHandle::newReference(runtime, *result);
}

PY_EXPORT int PyObject_CheckBuffer_Func(PyObject* pyobj) {
  // TODO(T38246066): Collapse all the cases into Runtime::isByteslike and make
  // this function a small wrapper around that
  Thread* thread = Thread::current();
  HandleScope scope(thread);
  Object obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(pyobj)));
  Runtime* runtime = thread->runtime();
  if (runtime->isInstanceOfBytes(*obj) ||
      runtime->isInstanceOfBytearray(*obj)) {
    return true;
  }
  if (runtime->isByteslike(*obj)) {
    UNIMPLEMENTED("PyObject_CheckBuffer with builtin byteslike");
  }
  Type type(&scope, runtime->typeOf(*obj));
  if (type.isBuiltin()) return false;
  if (!typeHasSlots(type)) return false;
  return typeSlotAt(type, Py_bf_getbuffer) != nullptr;
}

PY_EXPORT int PyObject_CheckReadBuffer(PyObject* /* j */) {
  UNIMPLEMENTED("PyObject_CheckReadBuffer");
}

PY_EXPORT int PyObject_DelItem(PyObject* obj, PyObject* key) {
  Thread* thread = Thread::current();
  if (obj == nullptr || key == nullptr) {
    nullError(thread);
    return -1;
  }
  HandleScope scope(thread);
  Object object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
  Object key_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(key)));
  Object result(&scope, objectDelItem(thread, object, key_obj));
  if (result.isErrorException()) {
    return -1;
  }
  return 0;
}

PY_EXPORT int PyObject_DelItemString(PyObject* obj, const char* key) {
  Thread* thread = Thread::current();
  if (obj == nullptr || key == nullptr) {
    nullError(thread);
    return -1;
  }
  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  Object object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
  Object key_obj(&scope, runtime->newStrFromCStr(key));
  Object result(&scope, objectDelItem(thread, object, key_obj));
  if (result.isErrorException()) {
    return -1;
  }
  return 0;
}

PY_EXPORT PyObject* _PyObject_CallNoArg(PyObject* callable) {
  return _PyObject_FastCall(callable, nullptr, 0);
}

PY_EXPORT PyObject* _PyObject_FastCall(PyObject* callable, PyObject** pyargs,
                                       Py_ssize_t n_args) {
  return _PyObject_FastCallDict(callable, pyargs, n_args, nullptr);
}

PY_EXPORT PyObject* _PyObject_FastCallDict(PyObject* callable,
                                           PyObject** pyargs, Py_ssize_t n_args,
                                           PyObject* kwargs) {
  DCHECK(callable != nullptr, "callable must not be nullptr");
  Thread* thread = Thread::current();
  DCHECK(!thread->hasPendingException(),
         "may accidentally clear pending exception");
  DCHECK(n_args >= 0, "n_args must not be negative");

  HandleScope scope(thread);
  thread->stackPush(ApiHandle::asObject(ApiHandle::fromPyObject(callable)));
  DCHECK(n_args == 0 || pyargs != nullptr, "Args array must not be nullptr");
  Object result(&scope, NoneType::object());
  Runtime* runtime = thread->runtime();
  if (kwargs != nullptr) {
    Object args_obj(&scope, NoneType::object());
    if (n_args > 0) {
      MutableTuple args(&scope, runtime->newMutableTuple(n_args));
      for (Py_ssize_t i = 0; i < n_args; i++) {
        args.atPut(i, ApiHandle::asObject(ApiHandle::fromPyObject(pyargs[i])));
      }
      args_obj = args.becomeImmutable();
    } else {
      args_obj = runtime->emptyTuple();
    }
    thread->stackPush(*args_obj);
    Object kwargs_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(kwargs)));
    DCHECK(runtime->isInstanceOfDict(*kwargs_obj), "kwargs must be a dict");
    thread->stackPush(*kwargs_obj);
    // TODO(T30925218): Protect against native stack overflow.
    result = Interpreter::callEx(thread, CallFunctionExFlag::VAR_KEYWORDS);
  } else {
    for (Py_ssize_t i = 0; i < n_args; i++) {
      thread->stackPush(ApiHandle::asObject(ApiHandle::fromPyObject(pyargs[i])));
    }
    // TODO(T30925218): Protect against native stack overflow.
    result = Interpreter::call(thread, n_args);
  }
  if (result.isError()) return nullptr;
  return ApiHandle::newReference(runtime, *result);
}

PY_EXPORT PyObject* _PyObject_FastCallKeywords(PyObject* /* e */,
                                               PyObject** /* k */,
                                               Py_ssize_t /* s */,
                                               PyObject* /* s */) {
  UNIMPLEMENTED("_PyObject_FastCallKeywords");
}

PY_EXPORT PyObject* PyObject_Format(PyObject* obj, PyObject* format_spec) {
  DCHECK(obj != nullptr, "obj should not be null");
  Thread* thread = Thread::current();
  HandleScope scope(thread);
  Object object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
  Object format_spec_obj(
      &scope, format_spec == nullptr
                  ? Str::empty()
                  : ApiHandle::asObject(ApiHandle::fromPyObject(format_spec)));
  Object result(&scope, thread->invokeFunction2(ID(builtins), ID(format),
                                                object, format_spec_obj));
  if (result.isError()) {
    return nullptr;
  }
  return ApiHandle::newReference(thread->runtime(), *result);
}

PY_EXPORT int PyObject_GetBuffer(PyObject* obj, Py_buffer* view, int flags) {
  DCHECK(obj != nullptr, "obj must not be nullptr");

  Thread* thread = Thread::current();
  ApiHandle* handle = ApiHandle::fromPyObject(obj);
  HandleScope scope(thread);
  Object obj_obj(&scope, ApiHandle::asObject(handle));
  Runtime* runtime = thread->runtime();
  if (runtime->isInstanceOfBytes(*obj_obj)) {
    Bytes bytes(&scope, bytesUnderlying(*obj_obj));
    char* buffer = bytesAsString(runtime, handle, bytes);
    if (buffer == nullptr) {
      return -1;
    }
    return PyBuffer_FillInfo(view, handle, buffer, bytes.length(),
                             /*readonly=*/1, flags);
  }
  if (runtime->isInstanceOfBytearray(*obj_obj)) {
    // TODO(T54579154): This creates a copy of the object which does not stay
    // in sync. We should have a way to pin the memory to allow direct access.
    Bytearray array(&scope, *obj_obj);
    char* buffer = bytearrayAsString(runtime, handle, array);
    if (buffer == nullptr) {
      return -1;
    }
    return PyBuffer_FillInfo(view, handle, buffer, array.numItems(),
                             /*readonly=*/1, flags);
  }
  if (obj_obj.isMemoryView()) {
    MemoryView memoryview(&scope, *obj_obj);
    Object buffer(&scope, memoryview.buffer());
    // A MemoryView's underlying buffer is either a bytes object or a raw
    // pointer.
    if (runtime->isInstanceOfBytes(*buffer)) {
      Bytes bytes(&scope, bytesUnderlying(*obj_obj));
      // We use the memoryview handle's cache directly to store the buffer.
      char* underlying_buffer = bytesAsString(runtime, handle, bytes);
      if (underlying_buffer == nullptr) {
        return -1;
      }
      return PyBuffer_FillInfo(view, handle, underlying_buffer,
                               memoryview.length(),
                               /*readonly=*/1, flags);
    }

    Pointer underlying_pointer(&scope, *buffer);
    char* underlying_buffer =
        reinterpret_cast<char*>(underlying_pointer.cptr());
    return PyBuffer_FillInfo(view, handle, underlying_buffer,
                             memoryview.length(),
                             /*readonly=*/1, flags);
  }
  if (runtime->isInstanceOfArray(*obj_obj)) {
    Array array(&scope, *obj_obj);
    word length = arrayByteLength(*array);
    // We create a copy of the array's buffer and place it in the API handle's
    // cache to ensure it gets reaped.
    if (void* cache = ApiHandle::cache(runtime, handle)) {
      std::free(cache);
    }
    byte* buffer = static_cast<byte*>(std::malloc(length + 1));
    if (buffer == nullptr) {
      return -1;
    }
    MutableBytes::cast(array.buffer()).copyTo(buffer, length);
    ApiHandle::setCache(runtime, handle, buffer);

    return PyBuffer_FillInfo(view, handle, reinterpret_cast<char*>(buffer),
                             length,
                             /*readonly=*/1, flags);
  }
  // We must be dealing with a buffer protocol or an incompatible type.
  Type type(&scope, runtime->typeOf(*obj_obj));
  if (type.isBuiltin()) {
    raiseBufferError(thread, obj_obj);
    return -1;
  }
  if (!typeHasSlots(type)) {
    raiseBufferError(thread, obj_obj);
    return -1;
  }
  void* slot = typeSlotAt(type, Py_bf_getbuffer);
  if (slot == nullptr) {
    raiseBufferError(thread, obj_obj);
    return -1;
  }
  return reinterpret_cast<getbufferproc>(slot)(handle, view, flags);
}

PY_EXPORT PyObject* PyObject_GetItem(PyObject* obj, PyObject* key) {
  Thread* thread = Thread::current();
  if (obj == nullptr || key == nullptr) {
    return nullError(thread);
  }
  HandleScope scope(thread);
  Object object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
  Object key_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(key)));
  Object result(&scope, objectGetItem(thread, object, key_obj));
  if (result.isErrorException()) return nullptr;
  return ApiHandle::newReference(thread->runtime(), *result);
}

PY_EXPORT PyObject* PyObject_GetIter(PyObject* pyobj) {
  Thread* thread = Thread::current();
  HandleScope scope(thread);
  Object obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(pyobj)));
  Object result(&scope, Interpreter::createIterator(thread, obj));
  if (result.isError()) {
    return nullptr;
  }
  return ApiHandle::newReference(thread->runtime(), *result);
}

PY_EXPORT int PyObject_IsInstance(PyObject* instance, PyObject* cls) {
  Thread* thread = Thread::current();
  HandleScope scope(thread);
  Object object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(instance)));
  Object classinfo(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(cls)));
  Object result(&scope, thread->invokeFunction2(ID(builtins), ID(isinstance),
                                                object, classinfo));
  return result.isError() ? -1 : Bool::cast(*result).value();
}

PY_EXPORT int PyObject_IsSubclass(PyObject* derived, PyObject* cls) {
  Thread* thread = Thread::current();
  HandleScope scope(thread);
  Object subclass(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(derived)));
  Object classinfo(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(cls)));
  Object result(&scope, thread->invokeFunction2(ID(builtins), ID(issubclass),
                                                subclass, classinfo));
  return result.isError() ? -1 : Bool::cast(*result).value();
}

PY_EXPORT Py_ssize_t PyObject_Length(PyObject* pyobj) {
  return objectLength(pyobj);
}

PY_EXPORT Py_ssize_t PyObject_LengthHint(PyObject* obj,
                                         Py_ssize_t default_value) {
  Py_ssize_t res = objectLength(obj);
  Thread* thread = Thread::current();
  Runtime* runtime = thread->runtime();
  HandleScope scope(thread);
  if (res < 0 && thread->hasPendingException()) {
    Object given_obj(&scope, thread->pendingExceptionType());
    Object exc_obj(&scope, runtime->typeAt(LayoutId::kTypeError));
    if (!givenExceptionMatches(thread, given_obj, exc_obj)) {
      return -1;
    }
    // Catch TypeError when obj does not have __len__.
    thread->clearPendingException();
  } else {
    return res;
  }

  Object object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
  Object length_hint(&scope,
                     thread->invokeMethod1(object, ID(__length_hint__)));
  if (length_hint.isErrorNotFound() || length_hint.isNotImplementedType()) {
    return default_value;
  }
  if (length_hint.isError()) {
    return -1;
  }
  if (!thread->runtime()->isInstanceOfInt(*length_hint)) {
    thread->raiseWithFmt(LayoutId::kTypeError,
                         "__length_hint__ must be an integer, not %T",
                         &length_hint);
    return -1;
  }
  Int index(&scope, intUnderlying(*length_hint));
  if (!index.isSmallInt()) {
    thread->raiseWithFmt(LayoutId::kOverflowError,
                         "cannot fit '%T' into an index-sized integer",
                         &length_hint);
    return -1;
  }
  if (index.isNegative()) {
    thread->raiseWithFmt(LayoutId::kValueError, "__len__() should return >= 0");
    return -1;
  }
  return index.asWord();
}

PY_EXPORT int PyObject_SetItem(PyObject* obj, PyObject* key, PyObject* value) {
  Thread* thread = Thread::current();
  if (obj == nullptr || key == nullptr || value == nullptr) {
    nullError(thread);
    return -1;
  }
  HandleScope scope(thread);
  Object object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
  Object key_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(key)));
  Object value_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(value)));
  Object result(&scope, objectSetItem(thread, object, key_obj, value_obj));
  return result.isErrorException() ? -1 : 0;
}

PY_EXPORT Py_ssize_t PyObject_Size(PyObject* pyobj) {
  return objectLength(pyobj);
}

PY_EXPORT PyTypeObject* Py_TYPE_Func(PyObject* pyobj) {
  Thread* thread = Thread::current();
  if (pyobj == nullptr) {
    nullError(thread);
    return nullptr;
  }

  Runtime* runtime = thread->runtime();
  return reinterpret_cast<PyTypeObject*>(ApiHandle::borrowedReference(
      runtime, runtime->typeOf(ApiHandle::asObject(ApiHandle::fromPyObject(pyobj)))));
}

PY_EXPORT void Py_SET_TYPE_Func(PyObject* obj, PyTypeObject* type) {
  DCHECK(obj != nullptr, "obj must be non-null");
  DCHECK(type != nullptr, "type must be non-null");
  Thread* thread = Thread::current();
  HandleScope scope(thread);
  Object self(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
  Type new_type(&scope, ApiHandle::asObject(ApiHandle::fromPyTypeObject(type)));
  Object result(&scope, typeSetDunderClass(thread, self, new_type));
  if (result.isError()) {
    UNIMPLEMENTED("unhandled case in __class__ setter");
  }
}

PY_EXPORT PyObject* PyObject_Type(PyObject* pyobj) {
  Thread* thread = Thread::current();
  if (pyobj == nullptr) {
    return nullError(thread);
  }

  Runtime* runtime = thread->runtime();
  return ApiHandle::newReference(
      runtime, runtime->typeOf(ApiHandle::asObject(ApiHandle::fromPyObject(pyobj))));
}

PY_EXPORT const char* PyObject_TypeName(PyObject* /* obj */) {
  UNIMPLEMENTED("PyObject_TypeName");
}

// Sequence Protocol

PY_EXPORT void _Py_FreeCharPArray(char* const array[]) {
  for (Py_ssize_t i = 0; array[i] != nullptr; ++i) {
    PyMem_Free(array[i]);
  }
  PyMem_Free(const_cast<char**>(array));
}

PY_EXPORT char* const* _PySequence_BytesToCharpArray(PyObject* self) {
  Py_ssize_t argc = PySequence_Size(self);
  if (argc < 0) {
    DCHECK(argc == -1, "size cannot be negative (-1 denotes an error)");
    return nullptr;
  }

  if (argc > (kMaxWord / kPointerSize) - 1) {
    PyErr_NoMemory();
    return nullptr;
  }

  char** result = static_cast<char**>(PyMem_Malloc((argc + 1) * kPointerSize));
  if (result == nullptr) {
    PyErr_NoMemory();
    return nullptr;
  }

  for (Py_ssize_t i = 0; i < argc; ++i) {
    PyObject* item = PySequence_GetItem(self, i);
    if (item == nullptr) {
      // NULL terminate before freeing.
      result[i] = nullptr;
      _Py_FreeCharPArray(result);
      return nullptr;
    }
    char* data;
    if (PyBytes_AsStringAndSize(item, &data, nullptr) < 0) {
      // NULL terminate before freeing.
      result[i] = nullptr;
      Py_DECREF(item);
      _Py_FreeCharPArray(result);
      return nullptr;
    }
    Py_ssize_t size = PyBytes_GET_SIZE(item) + 1;
    result[i] = static_cast<char*>(PyMem_Malloc(size));
    if (result[i] == nullptr) {
      PyErr_NoMemory();
      Py_DECREF(item);
      _Py_FreeCharPArray(result);
      return nullptr;
    }
    std::memcpy(result[i], data, size);
    Py_DECREF(item);
  }

  result[argc] = nullptr;
  return result;
}

PY_EXPORT int PySequence_Check(PyObject* py_obj) {
  Thread* thread = Thread::current();
  HandleScope scope(thread);
  Object obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(py_obj)));
  return thread->runtime()->isSequence(thread, obj);
}

PY_EXPORT PyObject* PySequence_Concat(PyObject* left, PyObject* right) {
  Thread* thread = Thread::current();
  if (left == nullptr || right == nullptr) {
    return nullError(thread);
  }
  if (!PySequence_Check(left) || !PySequence_Check(right)) {
    thread->raiseWithFmt(LayoutId::kTypeError,
                         "objects cannot be concatenated");
    return nullptr;
  }
  return PyNumber_Add(left, right);
}

PY_EXPORT int PySequence_Contains(PyObject* seq, PyObject* obj) {
  Thread* thread = Thread::current();
  if (seq == nullptr || obj == nullptr) {
    nullError(thread);
    return -1;
  }
  HandleScope scope(thread);
  Object seq_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(seq)));
  Object object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
  Object result(&scope, thread->invokeFunction2(ID(operator), ID(contains),
                                                seq_obj, object));
  if (result.isError()) {
    return -1;
  }
  return Bool::cast(*result).value() ? 1 : 0;
}

PY_EXPORT Py_ssize_t PySequence_Count(PyObject* seq, PyObject* obj) {
  Thread* thread = Thread::current();
  if (seq == nullptr || obj == nullptr) {
    nullError(thread);
    return -1;
  }
  HandleScope scope(thread);
  Object seq_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(seq)));
  Object object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
  Object result(&scope, thread->invokeFunction2(ID(operator), ID(countOf),
                                                seq_obj, object));
  if (result.isError()) {
    return -1;
  }
  return SmallInt::cast(*result).value();
}

PY_EXPORT int PySequence_DelItem(PyObject* seq, Py_ssize_t idx) {
  Thread* thread = Thread::current();
  if (seq == nullptr) {
    return -1;
  }
  HandleScope scope(thread);
  Object seq_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(seq)));
  Object idx_obj(&scope, thread->runtime()->newInt(idx));
  Object result(&scope,
                thread->invokeMethod2(seq_obj, ID(__delitem__), idx_obj));
  if (result.isError()) {
    return -1;
  }
  return 0;
}

static RawObject makeSlice(Thread* thread, Py_ssize_t low, Py_ssize_t high) {
  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  Object start(&scope, runtime->newInt(low));
  Object stop(&scope, runtime->newInt(high));
  Object step(&scope, NoneType::object());
  return runtime->newSlice(start, stop, step);
}

PY_EXPORT int PySequence_DelSlice(PyObject* seq, Py_ssize_t low,
                                  Py_ssize_t high) {
  Thread* thread = Thread::current();
  if (seq == nullptr) {
    nullError(thread);
    return -1;
  }
  HandleScope scope(thread);
  Object slice(&scope, makeSlice(thread, low, high));
  Object seq_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(seq)));
  Object result(&scope, thread->invokeMethod2(seq_obj, ID(__delitem__), slice));
  if (result.isError()) {
    if (result.isErrorNotFound()) {
      thread->raiseWithFmt(LayoutId::kTypeError,
                           "object does not support slice deletion");
    }
    return -1;
  }
  return 0;
}

PY_EXPORT PyObject* PySequence_Fast(PyObject* seq, const char* msg) {
  Thread* thread = Thread::current();
  if (seq == nullptr) {
    return nullError(thread);
  }
  HandleScope scope(thread);
  Object seq_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(seq)));

  Runtime* runtime = thread->runtime();
  if (seq_obj.isList() || seq_obj.isTuple()) {
    return ApiHandle::newReference(runtime, *seq_obj);
  }
  Object iter(&scope, Interpreter::createIterator(thread, seq_obj));
  if (iter.isError()) {
    Object given(&scope, thread->pendingExceptionType());
    Object exc(&scope, runtime->typeAt(LayoutId::kTypeError));
    if (givenExceptionMatches(thread, given, exc)) {
      thread->setPendingExceptionValue(runtime->newStrFromCStr(msg));
    }
    return nullptr;
  }

  Object result(&scope,
                thread->invokeFunction1(ID(builtins), ID(list), seq_obj));
  if (result.isError()) {
    return nullptr;
  }
  return ApiHandle::newReference(runtime, *result);
}

PY_EXPORT Py_ssize_t PySequence_Fast_GET_SIZE_Func(PyObject* seq) {
  return PyList_Check(seq) ? PyList_GET_SIZE(seq) : PyTuple_GET_SIZE(seq);
}

PY_EXPORT PyObject* PySequence_Fast_GET_ITEM_Func(PyObject* seq,
                                                  Py_ssize_t idx) {
  return PyList_Check(seq) ? PyList_GET_ITEM(seq, idx)
                           : PyTuple_GET_ITEM(seq, idx);
}

PY_EXPORT PyObject* PySequence_GetItem(PyObject* seq, Py_ssize_t idx) {
  Thread* thread = Thread::current();
  if (seq == nullptr) {
    return nullError(thread);
  }
  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  Object seq_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(seq)));
  if (seq_obj.isTuple()) {
    // Fast path: return `tuple`'s element directly.
    RawTuple tuple = Tuple::cast(*seq_obj);
    if (0 <= idx && idx < tuple.length()) {
      return ApiHandle::newReference(runtime, tuple.at(idx));
    }
  } else if (seq_obj.isList()) {
    // Fast path: return `list`'s element directly.
    RawList list = List::cast(*seq_obj);
    if (0 <= idx && idx < list.numItems()) {
      return ApiHandle::newReference(runtime, list.at(idx));
    }
  }
  Object idx_obj(&scope, thread->runtime()->newInt(idx));
  Object result(&scope,
                thread->invokeMethod2(seq_obj, ID(__getitem__), idx_obj));
  if (result.isError()) {
    if (result.isErrorNotFound()) {
      thread->raiseWithFmt(LayoutId::kTypeError, "could not call __getitem__");
    }
    return nullptr;
  }
  return ApiHandle::newReference(runtime, *result);
}

PY_EXPORT PyObject* PySequence_ITEM_Func(PyObject* seq, Py_ssize_t i) {
  DCHECK(seq != nullptr, "sequence must not be nullptr");
  DCHECK(i >= 0, "index can't be negative");
  Thread* thread = Thread::current();
  HandleScope scope(thread);
  Object seq_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(seq)));
  Runtime* runtime = thread->runtime();
  DCHECK(runtime->isSequence(thread, seq_obj), "seq must be a sequence");
  Object idx(&scope, runtime->newInt(i));
  Object result(&scope, thread->invokeMethod2(seq_obj, ID(__getitem__), idx));
  if (result.isError()) return nullptr;
  return ApiHandle::newReference(runtime, *result);
}

PY_EXPORT PyObject* PySequence_GetSlice(PyObject* seq, Py_ssize_t low,
                                        Py_ssize_t high) {
  Thread* thread = Thread::current();
  if (seq == nullptr) {
    return nullError(thread);
  }
  HandleScope scope(thread);
  Object slice(&scope, makeSlice(thread, low, high));
  Object seq_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(seq)));
  Object result(&scope, thread->invokeMethod2(seq_obj, ID(__getitem__), slice));
  if (result.isError()) {
    if (result.isErrorNotFound()) {
      thread->raiseWithFmt(LayoutId::kTypeError, "could not call __getitem__");
    }
    return nullptr;
  }
  return ApiHandle::newReference(thread->runtime(), *result);
}

PY_EXPORT int PySequence_In(PyObject* pyseq, PyObject* pyobj) {
  return PySequence_Contains(pyseq, pyobj);
}

PY_EXPORT Py_ssize_t PySequence_Index(PyObject* seq, PyObject* obj) {
  Thread* thread = Thread::current();
  if (seq == nullptr || obj == nullptr) {
    nullError(thread);
    return -1;
  }
  HandleScope scope(thread);
  Object seq_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(seq)));
  Object object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
  Object result(&scope, thread->invokeFunction2(ID(operator), ID(indexOf),
                                                seq_obj, object));
  if (result.isError()) {
    return -1;
  }
  return SmallInt::cast(*result).value();
}

PY_EXPORT PyObject* PySequence_InPlaceConcat(PyObject* left, PyObject* right) {
  Thread* thread = Thread::current();
  if (left == nullptr || right == nullptr) {
    return nullError(thread);
  }
  HandleScope scope(thread);
  Object left_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(left)));
  Object right_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(right)));
  Object result(&scope, thread->invokeFunction2(ID(operator), ID(iconcat),
                                                left_obj, right_obj));
  return result.isError() ? nullptr
                          : ApiHandle::newReference(thread->runtime(), *result);
}

PY_EXPORT PyObject* PySequence_InPlaceRepeat(PyObject* seq, Py_ssize_t count) {
  Thread* thread = Thread::current();
  if (seq == nullptr) {
    return nullError(thread);
  }
  HandleScope scope(thread);
  Runtime* runtime = thread->runtime();
  Object sequence(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(seq)));
  Object count_obj(&scope, runtime->newInt(count));
  Object result(&scope, thread->invokeFunction2(ID(operator), ID(irepeat),
                                                sequence, count_obj));
  return result.isError() ? nullptr : ApiHandle::newReference(runtime, *result);
}

PY_EXPORT Py_ssize_t PySequence_Length(PyObject* pyobj) {
  return objectLength(pyobj);
}

PY_EXPORT PyObject* PySequence_List(PyObject* seq) {
  Thread* thread = Thread::current();
  if (seq == nullptr) {
    return nullError(thread);
  }
  HandleScope scope(thread);
  Object seq_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(seq)));
  RawObject result = thread->invokeFunction1(ID(builtins), ID(list), seq_obj);
  return result.isError() ? nullptr
                          : ApiHandle::newReference(thread->runtime(), result);
}

PY_EXPORT PyObject* PySequence_Repeat(PyObject* pyseq, Py_ssize_t count) {
  Thread* thread = Thread::current();
  if (pyseq == nullptr) {
    return nullError(thread);
  }
  if (!PySequence_Check(pyseq)) {
    thread->raiseWithFmt(LayoutId::kTypeError, "object cannot be repeated");
    return nullptr;
  }
  PyObject* count_obj(PyLong_FromSsize_t(count));
  PyObject* result = PyNumber_Multiply(pyseq, count_obj);
  Py_DECREF(count_obj);
  return result;
}

PY_EXPORT int PySequence_SetItem(PyObject* seq, Py_ssize_t idx, PyObject* obj) {
  Thread* thread = Thread::current();
  if (seq == nullptr) {
    nullError(thread);
    return -1;
  }
  HandleScope scope(thread);
  Object seq_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(seq)));
  Object idx_obj(&scope, thread->runtime()->newInt(idx));
  Object result(&scope, NoneType::object());
  if (obj == nullptr) {
    // Equivalent to PySequence_DelItem
    result = thread->invokeMethod2(seq_obj, ID(__delitem__), idx_obj);
  } else {
    Object object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
    result = thread->invokeMethod3(seq_obj, ID(__setitem__), idx_obj, object);
  }
  if (result.isError()) {
    if (result.isErrorNotFound()) {
      thread->raiseWithFmt(LayoutId::kTypeError, "object is not subscriptable");
    }
    return -1;
  }
  return 0;
}

PY_EXPORT int PySequence_SetSlice(PyObject* seq, Py_ssize_t low,
                                  Py_ssize_t high, PyObject* obj) {
  Thread* thread = Thread::current();
  if (seq == nullptr) {
    nullError(thread);
    return -1;
  }
  HandleScope scope(thread);
  Object slice(&scope, makeSlice(thread, low, high));
  Object seq_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(seq)));
  Object result(&scope, NoneType::object());
  if (obj == nullptr) {
    result = thread->invokeMethod2(seq_obj, ID(__delitem__), slice);
  } else {
    Object object(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(obj)));
    result = thread->invokeMethod3(seq_obj, ID(__setitem__), slice, object);
  }
  if (result.isError()) {
    if (result.isErrorNotFound()) {
      thread->raiseWithFmt(LayoutId::kTypeError,
                           "object does not support slice assignment");
    }
    return -1;
  }
  return 0;
}

PY_EXPORT Py_ssize_t PySequence_Size(PyObject* pyobj) {
  return objectLength(pyobj);
}

PY_EXPORT PyObject* PySequence_Tuple(PyObject* seq) {
  Thread* thread = Thread::current();
  if (seq == nullptr) {
    return nullError(thread);
  }
  HandleScope scope(thread);
  Object seq_obj(&scope, ApiHandle::asObject(ApiHandle::fromPyObject(seq)));
  Runtime* runtime = thread->runtime();
  if (seq_obj.isTuple()) {
    return ApiHandle::newReference(runtime, *seq_obj);
  }
  Object result(&scope,
                thread->invokeFunction1(ID(builtins), ID(tuple), seq_obj));
  if (result.isError()) {
    return nullptr;
  }
  return ApiHandle::newReference(runtime, *result);
}

}  // namespace py
