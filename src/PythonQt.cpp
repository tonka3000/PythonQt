/*
 *
 *  Copyright (C) 2006 MeVis Research GmbH All Rights Reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  Further, this software is distributed without any warranty that it is
 *  free of the rightful claim of any third person regarding infringement
 *  or the like.  Any license provided herein, whether implied or
 *  otherwise, applies only to this software file.  Patent licenses, if
 *  any, provided herein do not apply to combinations of this program with
 *  other software, or any other product whatsoever.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Contact information: MeVis Research GmbH, Universitaetsallee 29,
 *  28359 Bremen, Germany or:
 *
 *  http://www.mevis.de
 *
 */

//----------------------------------------------------------------------------------
/*!
// \file    PythonQt.cpp
// \author  Florian Link
// \author  Last changed by $Author: florian $
// \date    2006-05
*/
//----------------------------------------------------------------------------------

#include "PythonQt.h"
#include "PythonQtImporter.h"
#include "PythonQtClassInfo.h"
#include "PythonQtMethodInfo.h"
#include "PythonQtSignalReceiver.h"
#include "PythonQtConversion.h"
#include "PythonQtStdOut.h"
#include "PythonQtCppWrapperFactory.h"
#include "PythonQtVariants.h"
#include <pydebug.h>

PythonQt* PythonQt::_self = NULL;

void PythonQt::init(int flags)
{
  if (!_self) {
    _self = new PythonQt(flags);
  }
}

void PythonQt::cleanup()
{
  if (_self) {
    delete _self;
    _self = NULL;
  }
}

PythonQt::PythonQt(int flags)
{
  _p = new PythonQtPrivate;
  Py_SetProgramName("PythonQt");
  if (flags & IgnoreSiteModule) {
    // this prevents the automatic importing of Python site files
    Py_NoSiteFlag = 1;
  }
  Py_Initialize();

  // add our own python object types for qt object slots
  if (PyType_Ready(&PythonQtSlotFunction_Type) < 0) {
    std::cerr << "could not initialize PythonQtSlotFunction_Type" << std::endl;
  }
  Py_INCREF(&PythonQtSlotFunction_Type);

  // add our own python object types for qt objects
  if (PyType_Ready(&PythonQtWrapper_Type) < 0) {
    std::cerr << "could not initialize PythonQtWrapper_Type" << std::endl;
  }
  Py_INCREF(&PythonQtWrapper_Type);

  // add our own python object types for redirection of stdout
  if (PyType_Ready(&PythonQtStdOutRedirectType) < 0) {
    std::cerr << "could not initialize PythonQtStdOutRedirectType" << std::endl;
  }
  Py_INCREF(&PythonQtStdOutRedirectType);

  initPythonQtModule(flags & RedirectStdOut);

}

PythonQt::~PythonQt() {
  delete _p;
  _p = NULL;
}

PythonQtPrivate::~PythonQtPrivate() {
}

PythonQtImportFileInterface* PythonQt::importInterface()
{
  return _self->_p->_importInterface;
}

void PythonQt::registerClass(const QMetaObject* metaobject)
{
  _p->registerClass(metaobject);
}

void PythonQtPrivate::registerClass(const QMetaObject* metaobject)
{
  // we register all classes in the hierarchy
  const QMetaObject* m = metaobject;
  while (m) {
    PythonQtClassInfo* info = _knownQtClasses.value(m->className());
    if (!info) {
      _knownQtClasses.insert(m->className(), new PythonQtClassInfo(m, QByteArray()));
    }
    m = m->superClass();
  }
}

PyObject* PythonQtPrivate::wrapQObject(QObject* obj)
{
  if (!obj) {
    Py_INCREF(Py_None);
    return Py_None;
  }
  PythonQtWrapper* wrap = _wrappedObjects.value(obj);
  if (!wrap) {
    // smuggling it in...
    _pendingObject._obj = obj;
    _pendingObject._info = _knownQtClasses.value(obj->metaObject()->className());
    if (!_pendingObject._info) {
      registerClass(obj->metaObject());
      _pendingObject._info = _knownQtClasses.value(obj->metaObject()->className());
      _pendingObject._wrappedPtr = NULL;
    }
    wrap = (PythonQtWrapper *)PythonQtWrapper_Type.tp_new(&PythonQtWrapper_Type,
                   NULL, NULL);
    _wrappedObjects.insert(obj, wrap);
    // insert destroyed handler
    connect(obj, SIGNAL(destroyed(QObject*)), this, SLOT(wrappedObjectDestroyed(QObject*)));
//    mlabDebugConst("MLABPython","new qobject wrapper added " << " " << wrap->_obj->className() << " " << wrap->_info->wrappedClassName().latin1());
  } else {
    Py_INCREF(wrap);
//    mlabDebugConst("MLABPython","qobject wrapper reused " << wrap->_obj->className() << " " << wrap->_info->wrappedClassName().latin1());
  }
  return (PyObject*)wrap;
}

PyObject* PythonQtPrivate::wrapPtr(void* ptr, const QByteArray& name)
{
  if (!ptr) {
    Py_INCREF(Py_None);
    return Py_None;
  }
  PythonQtWrapper* wrap = _wrappedObjects.value(ptr);
  if (!wrap) {
    PythonQtClassInfo* info = _knownQtClasses.value(name);
    if (info) {
      QObject* qptr = (QObject*)ptr;
      // if the object is a derived object, we want to switch the class info to the one of the derived class:
      if (name!=(qptr->metaObject()->className())) {
        registerClass(qptr->metaObject());
        info = _knownQtClasses.value(qptr->metaObject()->className());
      }
      _pendingObject._obj = qptr;
      _pendingObject._info = info;
      _pendingObject._wrappedPtr = NULL;
      wrap = (PythonQtWrapper *)PythonQtWrapper_Type.tp_new(&PythonQtWrapper_Type,
        NULL, NULL);
      _wrappedObjects.insert(ptr, wrap);
    } else {
      // not a known QObject, so try our wrapper factory:
      QObject* wrapper = NULL;
      for (int i=0; i<_cppWrapperFactories.size(); i++) {
        wrapper = _cppWrapperFactories.at(i)->create(name, ptr);
        if (wrapper) {
          break;
        }
      }
      if (wrapper) {
        PythonQtClassInfo* info = _knownQtClasses.value(wrapper->metaObject()->className());
        if (!info) {
          info = new PythonQtClassInfo(wrapper->metaObject(), name);
          _knownQtClasses.insert(wrapper->metaObject()->className(), info);
        }
        _pendingObject._obj = wrapper;
        _pendingObject._info = info;
        _pendingObject._wrappedPtr = ptr;

        wrap = (PythonQtWrapper *)PythonQtWrapper_Type.tp_new(&PythonQtWrapper_Type, NULL, NULL);
        // the ptr is registered, not the qobject wrapper
        _wrappedObjects.insert(ptr, wrap);
//          mlabDebugConst("MLABPython","new c++ wrapper added " << wrap->_wrappedPtr << " " << wrap->_obj->className() << " " << wrap->_info->wrappedClassName().latin1());
      }
    }
  } else {
    Py_INCREF(wrap);
    //mlabDebugConst("MLABPython","c++ wrapper reused " << wrap->_wrappedPtr << " " << wrap->_obj->className() << " " << wrap->_info->wrappedClassName().latin1());
  }
  return (PyObject*)wrap;
}


PythonQtSignalReceiver* PythonQt::getSignalReceiver(QObject* obj)
{
  PythonQtSignalReceiver* r = _p->_signalReceivers[obj];
  if (!r) {
    r = new PythonQtSignalReceiver(obj);
    _p->_signalReceivers.insert(obj, r);
    // insert destroyed handler
    connect(obj, SIGNAL(destroyed(QObject*)), _p ,SLOT(destroyedSignalEmitter(QObject*)));
  }
  return r;
}

bool PythonQt::addSignalHandler(QObject* obj, const char* signal, PyObject* module, const QString& objectname)
{
  bool flag = false;
  PythonQtObjectPtr callable = lookupCallable(module, objectname);
  if (callable) {
    PythonQtSignalReceiver* r = getSignalReceiver(obj);
    flag = r->addSignalHandler(signal, callable);
    if (!flag) {
      // signal not found
    }
  } else {
    // callable not found
  }
  return flag;
}

bool PythonQt::removeSignalHandler(QObject* obj, const char* signal, PyObject* module, const QString& objectname)
{
  bool flag = false;
  PythonQtObjectPtr callable = lookupCallable(module, objectname);
  if (callable) {
    PythonQtSignalReceiver* r = _p->_signalReceivers[obj];
    if (r) {
      flag = r->removeSignalHandler(signal, callable);
    }
  } else {
    // callable not found
  }
  return flag;
}

PythonQtObjectPtr PythonQt::lookupCallable(PyObject* module, const QString& name)
{
  PythonQtObjectPtr p = lookupObject(module, name);
  if (p) {
    if (PyCallable_Check(p)) {
      return p;
    }
  }
  PyErr_Clear();
  return NULL;
}

PythonQtObjectPtr PythonQt::lookupObject(PyObject* module, const QString& name)
{
  QStringList l = name.split('.');
  PythonQtObjectPtr p = module;
  PythonQtObjectPtr prev;
  QString s;
  QByteArray b;
  for (QStringList::ConstIterator i = l.begin(); i!=l.end() && p; ++i) {
    prev = p;
    b = (*i).toLatin1();
    p.setNewRef(PyObject_GetAttrString(p, b.data()));
  }
  PyErr_Clear();
  return p;
}

PythonQtObjectPtr PythonQt::getMainModule() {
  //both borrowed
  PythonQtObjectPtr dict = PyImport_GetModuleDict();
  return PyDict_GetItemString(dict, "__main__");
}

QVariant PythonQt::evalCode(PyObject* module, PyObject* pycode) {
  QVariant result;
  if (pycode) {
    PyObject* r = PyEval_EvalCode((PyCodeObject*)pycode, PyModule_GetDict((PyObject*)module) , PyModule_GetDict((PyObject*)module));
    if (r) {
      result = PythonQtConv::PyObjToQVariant(r);
      Py_DECREF(r);
    } else {
      handleError();
    }
  } else {
    handleError();
  }
  return result;
}

QVariant PythonQt::evalScript(PyObject* module, const QString& script, int start)
{
  QVariant result;
  PythonQtObjectPtr p;
  p.setNewRef(PyRun_String(script.toLatin1().data(), start, PyModule_GetDict(module), PyModule_GetDict(module)));
  if (p) {
    result = PythonQtConv::PyObjToQVariant(p);
  } else {
    handleError();
  }
  return result;
}

PythonQtObjectPtr PythonQt::parseFile(const QString& filename)
{
  PythonQtObjectPtr p;
  p.setNewRef(PythonQtImport::getCodeFromPyc(filename));
  if (!p) {
    handleError();
  }
  return p;
}

void PythonQt::addObject(PyObject* module, const QString& name, QObject* object)
{
  PyModule_AddObject(module, name.toLatin1().data(), _p->wrapQObject(object));
}

void PythonQt::addVariable(PyObject* module, const QString& name, const QVariant& v)
{
  PyModule_AddObject(module, name.toLatin1().data(), PythonQtConv::QVariantToPyObject(v));
}

void PythonQt::removeVariable(PyObject* module, const QString& name)
{
  PyObject_DelAttrString(module, name.toLatin1().data());
}

QVariant PythonQt::getVariable(PyObject* module, const QString& objectname)
{
  QVariant result;
  PythonQtObjectPtr obj = lookupObject(module, objectname);
  if (obj) {
    result = PythonQtConv::PyObjToQVariant(obj);
  }
  return result;
}

QStringList PythonQt::introspection(PyObject* module, const QString& objectname, PythonQt::ObjectType type)
{
  QStringList results;

  PythonQtObjectPtr object;
  if (objectname.isEmpty()) {
    object = module;
  } else {
    object = lookupObject(module, objectname);
  }
  if (object) {
    PyObject* keys = PyObject_Dir(object);
    if (keys) {
      int count = PyList_Size(keys);
      PyObject* key;
      PyObject* value;
      QString keystr;
      for (int i = 0;i<count;i++) {
        key = PyList_GetItem(keys,i);
        value = PyObject_GetAttr(object, key);
        if (!value) continue;
        keystr = PyString_AsString(key);
        static const QString underscoreStr("__");
        if (!keystr.startsWith(underscoreStr)) {
          switch (type) {
          case Class:
            if (value->ob_type == &PyClass_Type) {
              results << keystr;
            }
            break;
          case Variable:
            if (value->ob_type != &PyClass_Type
              && value->ob_type != &PyCFunction_Type
              && value->ob_type != &PyFunction_Type
              && value->ob_type != &PyModule_Type
              ) {
              results << keystr;
            }
            break;
          case Function:
            if (value->ob_type == &PyFunction_Type ||
              value->ob_type == &PyMethod_Type
              ) {
              results << keystr;
            }
            break;
          case Module:
            if (value->ob_type == &PyModule_Type) {
              results << keystr;
            }
            break;
          default:
            std::cerr << "PythonQt: introspection: unknown case" << std::endl;
          }
        }
        Py_DECREF(value);
      }
      Py_DECREF(keys);
    }
  }
  return results;
}

QVariant PythonQt::call(PyObject* module, const QString& name, const QVariantList& args)
{
  QVariant r;

  PythonQtObjectPtr callable = lookupCallable(module, name);
  if (callable) {
    PythonQtObjectPtr pargs;
    int count = args.size();
    if (count>0) {
      pargs.setNewRef(PyTuple_New(count));
    }
    bool err = false;
    // transform QVariants to Python
    for (int i = 0; i < count; i++) {
      PyObject* arg = PythonQtConv::QVariantToPyObject(args.at(i));
      if (arg) {
        // steals reference, no unref
        PyTuple_SetItem(pargs, i,arg);
      } else {
        err = true;
        break;
      }
    }

    if (!err) {
      PyErr_Clear();
      PythonQtObjectPtr result;
      result.setNewRef(PyObject_CallObject(callable, pargs));
      if (result) {
        // ok
        r = PythonQtConv::PyObjToQVariant(result);
      } else {
        PythonQt::self()->handleError();
      }
    }
  }
  return r;
}

void PythonQt::setImporter(PythonQtImportFileInterface* importInterface)
{
  static bool first = true;
  if (first) {
    first = false;
    _p->_importInterface = importInterface;
    PythonQtImport::init();
  }
}

void PythonQt::addWrapperFactory(PythonQtCppWrapperFactory* factory)
{
  _p->_cppWrapperFactories.append(factory);
}

//---------------------------------------------------------------------------------------------------
PythonQtPrivate::PythonQtPrivate()
{
  _importInterface = NULL;
}

void PythonQtPrivate::wrappedObjectDestroyed(QObject* obj)
{
  // mlabDebugConst("MLABPython","PyWrapper QObject destroyed " << o << " " << o->name() << " " << o->className());
  PythonQtWrapper* wrap = _wrappedObjects[obj];
  if (wrap) {
    _wrappedObjects.remove(obj);
    // remove the pointer but keep the wrapper alive in python
    wrap->_obj = NULL;
  }
}

const PythonQtMethodInfo* PythonQtPrivate::getSignalInfo(const QMetaMethod& signal)
{
  PythonQtMethodInfo* result = _cachedSignalSignatures.value(signal.signature());
  if (!result) {
    result = new PythonQtMethodInfo(signal);
    _cachedSignalSignatures.insert(signal.signature(), result);
  }
  return result;
}

void PythonQtPrivate::destroyedSignalEmitter(QObject* obj)
{
  _signalReceivers.take(obj);
}

bool PythonQt::handleError()
{
  bool flag = false;
  if (PyErr_Occurred()) {

    // currently we just print the error and the stderr handler parses the errors
    PyErr_Print();

/*
    // EXTRA: the format of the ptype and ptraceback is not really documented, so I use PyErr_Print() above
    PyObject *ptype;
    PyObject *pvalue;
    PyObject *ptraceback;
    PyErr_Fetch( &ptype, &pvalue, &ptraceback);

    Py_XDECREF(ptype);
    Py_XDECREF(pvalue);
    Py_XDECREF(ptraceback);
*/
    PyErr_Clear();
    flag = true;
  }
  return flag;
}

void PythonQt::overwriteSysPath(const QStringList& paths)
{
  PythonQtObjectPtr sys;
  sys.setNewRef(PyImport_ImportModule("sys"));
  PyModule_AddObject(sys, "path", PythonQtConv::QStringListToPyList(paths));
}

void PythonQt::setModuleImportPath(PyObject* module, const QStringList& paths)
{
  PyModule_AddObject(module, "__path__", PythonQtConv::QStringListToPyList(paths));
}

void PythonQt::stdOutRedirectCB(const QString& str)
{
  emit PythonQt::self()->pythonStdOut(str);
}

void PythonQt::stdErrRedirectCB(const QString& str)
{
  emit PythonQt::self()->pythonStdErr(str);
}

static PyMethodDef PythonQtMethods[] = {
{NULL, NULL, 0, NULL}
};

void PythonQt::initPythonQtModule(bool redirectStdOut)
{
  _p->_pythonQtModule.setNewRef(Py_InitModule("PythonQt", PythonQtMethods));
  PythonQtVariants::init(_p->_pythonQtModule);

  if (redirectStdOut) {
    PythonQtObjectPtr sys;
    PythonQtObjectPtr out;
    PythonQtObjectPtr err;
    sys.setNewRef(PyImport_ImportModule("sys"));
    // create a redirection object for stdout and stderr
    out = PythonQtStdOutRedirectType.tp_new(&PythonQtStdOutRedirectType,NULL, NULL);
    ((PythonQtStdOutRedirect*)out.object())->_cb = stdOutRedirectCB;
    err = PythonQtStdOutRedirectType.tp_new(&PythonQtStdOutRedirectType,NULL, NULL);
    ((PythonQtStdOutRedirect*)err.object())->_cb = stdErrRedirectCB;
    // replace the built in file objects with our own objects
    PyModule_AddObject(sys, "stdout", out);
    PyModule_AddObject(sys, "stderr", err);
  }
}
