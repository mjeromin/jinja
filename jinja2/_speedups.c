/**
 * jinja2._speedups
 * ~~~~~~~~~~~~~~~~
 *
 * This module implements a few functions in C for better performance.  It
 * also defines a `tb_set_next` function that is used to patch the debug
 * traceback.  If the speedups module is not compiled a ctypes implementation
 * is used.
 *
 * :copyright: 2008 by Armin Ronacher, Mickaël Guérin.
 * :license: BSD.
 */

#include <Python.h>

#define ESCAPED_CHARS_TABLE_SIZE 63
#define UNICHR(x) (((PyUnicodeObject*)PyUnicode_DecodeASCII(x, strlen(x), NULL))->str);

static PyObject* markup;
static Py_ssize_t escaped_chars_delta_len[ESCAPED_CHARS_TABLE_SIZE];
static Py_UNICODE *escaped_chars_repl[ESCAPED_CHARS_TABLE_SIZE];

static int
init_constants(void)
{
	/* happing of characters to replace */
	escaped_chars_repl['"'] = UNICHR("&#34;");
	escaped_chars_repl['\''] = UNICHR("&#39;");
	escaped_chars_repl['&'] = UNICHR("&amp;");
	escaped_chars_repl['<'] = UNICHR("&lt;");
	escaped_chars_repl['>'] = UNICHR("&gt;");

	/* lengths of those characters when replaced - 1 */
	memset(escaped_chars_delta_len, 0, sizeof (escaped_chars_delta_len));
	escaped_chars_delta_len['"'] = escaped_chars_delta_len['\''] = \
		escaped_chars_delta_len['&'] = 4;
	escaped_chars_delta_len['<'] = escaped_chars_delta_len['>'] = 3;
	
	/* import markup type so that we can mark the return value */
	PyObject *module = PyImport_ImportModule("jinja2.utils");
	if (!module)
		return 0;
	markup = PyObject_GetAttrString(module, "Markup");
	Py_DECREF(module);

	return 1;
}

static PyObject*
escape_unicode(PyUnicodeObject *in)
{
	PyUnicodeObject *out;
	Py_UNICODE *inp = in->str;
	const Py_UNICODE *inp_end = in->str + in->length;
	Py_UNICODE *next_escp;
	Py_UNICODE *outp;
	Py_ssize_t delta=0, erepl=0, delta_len=0;

	/* First we need to figure out how long the escaped string will be */
	while (*(inp) || inp < inp_end) {
		if (*inp < ESCAPED_CHARS_TABLE_SIZE && escaped_chars_delta_len[*inp]) {
			delta += escaped_chars_delta_len[*inp];
			++erepl;
		}
		++inp;
	}

	/* Do we need to escape anything at all? */
	if (!erepl) {
		Py_INCREF(in);
		return (PyObject*)in;
	}

	out = (PyUnicodeObject*)PyUnicode_FromUnicode(NULL, in->length + delta);
	if (!out)
		return NULL;

	outp = out->str;
	inp = in->str;
	while (erepl-- > 0) {
		/* look for the next substitution */
		next_escp = inp;
		while (next_escp < inp_end) {
			if (*next_escp < ESCAPED_CHARS_TABLE_SIZE &&
			    (delta_len = escaped_chars_delta_len[*next_escp])) {
				++delta_len;
				break;
			}
			++next_escp;
		}
		
		if (next_escp > inp) {
			/* copy unescaped chars between inp and next_escp */
			Py_UNICODE_COPY(outp, inp, next_escp-inp);
			outp += next_escp-inp;
		}

		/* escape 'next_escp' */
		Py_UNICODE_COPY(outp, escaped_chars_repl[*next_escp], delta_len);
		outp += delta_len;

		inp = next_escp + 1;
	}
	if (inp < inp_end)
		Py_UNICODE_COPY(outp, inp, in->length - (inp - in->str));

	return (PyObject*)out;
}


static PyObject*
escape(PyObject *self, PyObject *text)
{
	PyObject *s = NULL, *rv = NULL;

	/* we don't have to escape integers, bools or floats */
	if (PyInt_CheckExact(text) || PyLong_CheckExact(text) ||
	    PyFloat_CheckExact(text) || PyBool_Check(text) ||
	    text == Py_None)
		return PyObject_CallFunctionObjArgs(markup, text, NULL);

	/* if the object has an __html__ method that performs the escaping */
	PyObject *html = PyObject_GetAttrString(text, "__html__");
	if (html) {
		rv = PyObject_CallObject(html, NULL);
		Py_DECREF(html);
		return rv;
	}

	/* otherwise make the object unicode if it isn't, then escape */
	PyErr_Clear();
	if (!PyUnicode_Check(text)) {
		PyObject *unicode = PyObject_Unicode(text);
		if (!unicode)
			return NULL;
		s = escape_unicode((PyUnicodeObject*)unicode);
		Py_DECREF(unicode);
	}
	else
		s = escape_unicode((PyUnicodeObject*)text);

	/* convert the unicode string into a markup object. */
	rv = PyObject_CallFunctionObjArgs(markup, (PyObject*)s, NULL);
	Py_DECREF(s);
	return rv;
}


static PyObject*
soft_unicode(PyObject *self, PyObject *s)
{
	if (!PyUnicode_Check(s))
		return PyObject_Unicode(s);
	Py_INCREF(s);
	return s;
}


static PyObject *
tb_set_next(PyObject *self, PyObject *args)
{
	PyTracebackObject *tb, *old;
	PyObject *next;

	if (!PyArg_ParseTuple(args, "O!O:tb_set_next", &PyTraceBack_Type, &tb, &next))
		return NULL;
	if (next == Py_None)
		next = NULL;
	else if (!PyTraceBack_Check(next)) {
		PyErr_SetString(PyExc_TypeError,
				"tb_set_next arg 2 must be traceback or None");
		return NULL;
	}
	else
		Py_INCREF(next);

	old = tb->tb_next;
	tb->tb_next = (PyTracebackObject*)next;
	Py_XDECREF(old);

	Py_INCREF(Py_None);
	return Py_None;
}


static PyMethodDef module_methods[] = {
	{"escape", (PyCFunction)escape, METH_O,
	 "escape(s) -> markup\n\n"
	 "Convert the characters &, <, >, and \" in string s to HTML-safe\n"
	 "sequences.  Use this if you need to display text that might contain\n"
	 "such characters in HTML.  Marks return value as markup string."},
	{"soft_unicode", (PyCFunction)soft_unicode, METH_O,
	 "soft_unicode(object) -> string\n\n"
         "Make a string unicode if it isn't already.  That way a markup\n"
         "string is not converted back to unicode."},
	{"tb_set_next", (PyCFunction)tb_set_next, METH_VARARGS,
	 "Set the tb_next member of a traceback object."},
	{NULL, NULL, 0, NULL}		/* Sentinel */
};


#ifndef PyMODINIT_FUNC	/* declarations for DLL import/export */
#define PyMODINIT_FUNC void
#endif
PyMODINIT_FUNC
init_speedups(void)
{
	if (!init_constants())
		return;

	Py_InitModule3("jinja2._speedups", module_methods, "");
}