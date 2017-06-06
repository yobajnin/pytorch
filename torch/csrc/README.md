## csrc

The csrc directory contains all of the code concerned with integration
with Python.  This is in contrast to lib, which contains the Torch
libraries that are Python agnostic.  csrc depends on lib, but not vice
versa.

There are a number of utilities for easing integration with Python which
are worth knowing about, which we briefly describe here.  But the most
important gotchas:

* DO NOT forget to take out the GIL with `AutoGil` before calling Python
  API or bringing a `THPObjectPtr` into scope.

### `Exceptions.h`

Frequently when working with the Python API, you may call a function
which returns an error.  In this case, we want to return directly to the
Python interpreter, so that this exception can be propagated
accordingly; however, because the Python API is C-based, what actually
will happen is it will return control to whatever C++ code called it.
Similarly, if we raise a C++ exception, prior to returning to the Python
interpreter, we must set the Python error flags, so it turns into a C++
exception.

Exceptions defines some useful helpers: `HANDLE_TH_ERRORS`, `END_HANDLE_TH_ERRORS`
and an exception class `python_error`.  You call them like this:

```
// Entry point from Python interpreter
PyObject* run() {
  HANDLE_TH_ERRORS
  ...
  if (!x) throw python_error();
  ...
  END_HANDLE_TH_ERRORS
}
```

The `HANDLE_TH_ERRORS` macro will catch all exceptions and convert them
into an appropriate Python signal.  `python_error` is a special
exception which doesn't contain any info, instead it says, "An error
occurred in the Python API; if you return to the interpreter, Python
will raise that exception, nothing else needs to be done."

### `utils/auto_gil.h`

Whenever you make any calls to the Python API, you must have taken out
the Python GIL, as none of these calls are thread safe.  `AutoGIL` is
a RAII struct which handles taking and releasing the GIL.  Use it like
this:

```
void iWantToUsePython() {
  AutoGil gil;
  ...
}
```

In general, the compiler will NOT warn you if you use Python
functionality without taking out the GIL, so DO NOT FORGET this call.

### `utils/object_ptr.h`

`THPPointer` is a smart pointer class analogous to `std::shared_ptr`,
but which is overloaded to handle reference counting scheme of various
objects which are not based on `shared_ptr`.  The most important overloads are:

* `PyObject` (so important we've aliased it as `THPObjectPtr`), which
  hooks into Python reference counting.  (By the way, that means you
  MUST take out the GIL before bringing one of these into scope!)

* The various TH tensor and storage types (e.g., `THTensor`), which
  hook into TH's reference counting.  (TH's reference counting
  IS thread safe, no locks necessary.)
