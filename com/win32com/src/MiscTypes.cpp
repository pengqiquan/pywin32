// MiscTypes.cpp - misc Python types.
// @doc
#include "stdafx.h"
#include "PythonCOM.h"

// ### FUTURE: since we have our own type, we can use it to expose values
// ###         from the PyComTypeObjects. For example, "override" the getattr
// ###         slot and be able to return the base type. If we added an IID
// ###         to the type, then we could return that, too (which would
// ###         provide a nice way to see what interface is being exposed by
// ###         an object).
static PyTypeObject PyInterfaceType_Type = {
	PyObject_HEAD_INIT(&PyType_Type)
	0,			/* Number of items for varobject */
	"interface-type",			/* Name of this type */
	sizeof(PyTypeObject),	/* Basic object size */
	0,			/* Item size for varobject */
	0,			/*tp_dealloc*/
	0,			/*tp_print*/
	0, /*tp_getattr*/
	0,			/*tp_setattr*/
	0,			/*tp_compare*/
	0,	/*tp_repr*/
	0,			/*tp_as_number*/
	0,			/*tp_as_sequence*/
	0,			/*tp_as_mapping*/
	0,			/*tp_hash*/
	0,			/*tp_call*/
	0,			/*tp_str*/
	PyObject_GenericGetAttr,			/*tp_getattro */
	0,			/*tp_setattro */
	0,			/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,			/* tp_flags */
	"Define the behavior of a PythonCOM Interface type.",          /* tp_doc */
};

PyComTypeObject::PyComTypeObject( const char *name, PyComTypeObject *pBase, int typeSize, struct PyMethodDef* methodList, PyIUnknown * (* thector)(IUnknown *))
{
// originally, this copied the typeobject of the parent, but as it is impossible
// to gurantee order of static object construction, I went this way.  This is 
// probably better, as is forces _all_ python objects have the same type sig.
	static const PyTypeObject type_template = {
#ifdef OLD_PYTHON_TYPES
		PyObject_HEAD_INIT(&PyInterfaceType_Type)
#else
		PyObject_HEAD_INIT(&PyType_Type)
#endif
		0,													/*ob_size*/
		"PythonComTypeTemplate",							/*tp_name*/
		sizeof(PyIBase), 									/*tp_basicsize*/
		0,													/*tp_itemsize*/
		/* methods */
		(destructor) PyIBase::dealloc, 						/*tp_dealloc*/
		0,													/*tp_print*/
		0, 					/*tp_getattr*/
		(setattrfunc) PyIBase::setattr,						/*tp_setattr*/
		PyIBase::cmp,										/*tp_compare*/
		(reprfunc)PyIBase::repr,							/*tp_repr*/
    	0,													/*tp_as_number*/
		0,			/*tp_as_sequence*/
		0,			/*tp_as_mapping*/
		0,			/*tp_hash*/
		0,			/*tp_call*/
		0,			/*tp_str*/
		PyIBase::getattro, /* tp_getattro */
		0,			/*tp_setattro */
		0,			/* tp_as_buffer */
		0,			/* tp_flags */
		0,          /* tp_doc */
		0,    /* tp_traverse */
		0,                              /* tp_clear */
		0,                              /* tp_richcompare */
		0,                              /* tp_weaklistoffset */
		0,					/* tp_iter */
		0,					/* tp_iternext */
		0,					/* tp_methods */	
		0,					/* tp_members */
		0,					/* tp_getset */
#ifdef OLD_PYTHON_TYPES
		0,					/* tp_base */
#else
		&PyInterfaceType_Type,
#endif
	};

	*((PyTypeObject *)this) = type_template;

	chain.methods = methodList;
	chain.link = pBase ? &pBase->chain : NULL;

	baseType = pBase;
	ctor = thector;

	// cast away const, as Python doesnt use it.
	tp_name = (char *)name;
	tp_basicsize = typeSize;
}
PyComTypeObject::~PyComTypeObject()
{
}

/* static */ BOOL PyComTypeObject::is_interface_type(const PyObject *ob)
{
#ifdef OLD_PYTHON_TYPES
	return ob->ob_type == &PyInterfaceType_Type;
#else
	return ob->ob_type == &PyType_Type && 
	       ((PyTypeObject *)ob)->tp_base == &PyInterfaceType_Type;
#endif
}

// Our type for IEnum* interfaces
PyComEnumTypeObject::PyComEnumTypeObject( const char *name, PyComTypeObject *pBase, int typeSize, struct PyMethodDef* methodList, PyIUnknown * (* thector)(IUnknown *)) :
	PyComTypeObject( name, pBase, typeSize, methodList, thector)
{
	tp_iter = iter;
	tp_iternext = iternext;
	tp_flags |= Py_TPFLAGS_HAVE_ITER;
}

// PyIEnum iter methods - generic for any "standard" COM IEnum interface, but
// if the object provides a real one, we use it.
PyObject *PyComEnumTypeObject::iter(PyObject *self)
{
	assert(!PyErr_Occurred());
	PyObject *rc = ((PyIBase *)self)->iter();
	if (rc || PyErr_Occurred())
		return rc;
	Py_INCREF(self);
	return self;
}

PyObject *PyComEnumTypeObject::iternext(PyObject *self)
{
	PyObject *ret = ((PyIBase *)self)->iter();
	if (ret || PyErr_Occurred())
		return ret;
	PyObject *method = PyObject_GetAttrString(self, "Next");
	if (!method)
		return NULL;
	PyObject *args=Py_BuildValue("(i)", 1);
	PyObject *result = PyObject_Call(method, args, NULL);
	Py_DECREF(method);
	Py_DECREF(args);
	if (!result)
		return NULL;
	if (PySequence_Length(result)==0){
		PyErr_SetNone(PyExc_StopIteration);
		ret = NULL;
	} else
		ret = PySequence_GetItem(result, 0);
	Py_DECREF(result);
	return ret;
}


// Our type for IEnum provider interfaces
PyComEnumProviderTypeObject::PyComEnumProviderTypeObject( 
                                        const char *name, 
                                        PyComTypeObject *pBase, 
                                        int typeSize, 
                                        struct PyMethodDef* methodList, 
                                        PyIUnknown * (* thector)(IUnknown *),
                                        const char *penum_method_name) :
	PyComTypeObject( name, pBase, typeSize, methodList, thector),
	enum_method_name(penum_method_name)
{
	tp_iter = iter;
	// tp_iternext remains NULL
	tp_flags |= Py_TPFLAGS_HAVE_ITER;
}

// PyIEnumProvider iter methods - generic for COM object that can provide an IEnum*
// interface via a method call taking no args.
PyObject *PyComEnumProviderTypeObject::iter(PyObject *self)
{
	PyObject *result = ((PyIBase *)self)->iter();
	if (result || PyErr_Occurred())
		return result;
	PyComEnumProviderTypeObject *t = (PyComEnumProviderTypeObject *)self->ob_type;
	PyObject *method = PyObject_GetAttrString(self, (char *)t->enum_method_name);
	if (!method)
		return NULL;
	PyObject *args=PyTuple_New(0);
	result = PyObject_Call(method, args, NULL);
	Py_DECREF(method);
	Py_DECREF(args);
	if (result==Py_None) {
		// If we returned None for the iterator (but there is
		// no error) then we simulate an empty iterator
		// Otherwise we get:
		// TypeError: iter() returned non-iterator of type 'NoneType'
		Py_DECREF(result);
		PyObject *dummy = PyTuple_New(0);
		if (!dummy)
			return NULL;
		result = PySeqIter_New(dummy);
		Py_DECREF(dummy);
	}
	return result;
}


/////////////////////////////////////////////////////////////////////////////
// class PyOleEmpty
PyOleEmpty::PyOleEmpty()
{
	ob_type = &PyOleEmptyType;
	_Py_NewReference(this);
}

static void empty_dealloc(PyOleEmpty *o)
{
	delete o;
}

PyTypeObject PyOleEmptyType =
{
	PyObject_HEAD_INIT(&PyType_Type)
	0,
	"PyOleEmpty",
	sizeof(PyOleEmpty),
	0,
	(destructor)empty_dealloc,  /*tp_dealloc*/
	0,                      /*tp_print*/
	0,                      /*tp_getattr*/
	0,                      /*tp_setattr*/
	0,                      /*tp_compare*/
	0,                      /*tp_repr*/
	0,                      /*tp_as_number*/
	0,                      /*tp_as_sequence*/
	0,                      /*tp_as_mapping*/
};

/////////////////////////////////////////////////////////////////////////////
// class PyOleMissing
PyOleMissing::PyOleMissing()
{
	ob_type = &PyOleMissingType;
	_Py_NewReference(this);
}

static void missing_dealloc(PyOleMissing *o)
{
	delete o;
}

PYCOM_EXPORT PyTypeObject PyOleMissingType =
{
	PyObject_HEAD_INIT(&PyType_Type)
	0,
	"PyOleMissing",
	sizeof(PyOleMissing),
	0,
	(destructor)missing_dealloc,  /*tp_dealloc*/
	0,                      /*tp_print*/
	0,                      /*tp_getattr*/
	0,                      /*tp_setattr*/
	0,                      /*tp_compare*/
	0,                      /*tp_repr*/
	0,                      /*tp_as_number*/
	0,                      /*tp_as_sequence*/
	0,                      /*tp_as_mapping*/
};
/////////////////////////////////////////////////////////////////////////////
// class PyOleArgNotFound
PyOleArgNotFound::PyOleArgNotFound()
{
	ob_type = &PyOleArgNotFoundType;
	_Py_NewReference(this);
}

static void notfound_dealloc(PyOleArgNotFound *o)
{
	delete o;
}

PyTypeObject PyOleArgNotFoundType =
{
	PyObject_HEAD_INIT(&PyType_Type)
	0,
	"ArgNotFound",
	sizeof(PyOleArgNotFound),
	0,
	(destructor)notfound_dealloc,  /*tp_dealloc*/
	0,                      /*tp_print*/
	0,                      /*tp_getattr*/
	0,                      /*tp_setattr*/
	0,                      /*tp_compare*/
	0,                      /*tp_repr*/
	0,                      /*tp_as_number*/
	0,                      /*tp_as_sequence*/
	0,                      /*tp_as_mapping*/
};

// These aren't really types, but may be some day :)
// @object PyOLEMENUGROUPWIDTHS|Tuple containing 6 ints indicating nbr of options in each menu group
BOOL PyObject_AsOLEMENUGROUPWIDTHS( PyObject *oblpMenuWidths, OLEMENUGROUPWIDTHS *pWidths)
{
	return PyArg_ParseTuple(oblpMenuWidths, "iiiiii",
							&pWidths->width[0], &pWidths->width[1],
							&pWidths->width[2], &pWidths->width[3],
							&pWidths->width[4], &pWidths->width[5]) != NULL;
}

PyObject *PyObject_FromOLEMENUGROUPWIDTHS(const OLEMENUGROUPWIDTHS *pWidths)
{
	if (!pWidths) {
		Py_INCREF(Py_None);
		return Py_None;
	}
	return Py_BuildValue("(iiiiii)",
						 pWidths->width[0], pWidths->width[1],
						 pWidths->width[2], pWidths->width[3],
						 pWidths->width[4], pWidths->width[5]);
}

////////////////////////////////