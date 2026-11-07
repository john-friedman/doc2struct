#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <doc2struct/doc2struct.h>
typedef doc2struct_string Str;
typedef doc2struct_document Document;
typedef doc2struct_rules Rules;
typedef doc2struct_run Run;
typedef doc2struct_token Token;
typedef doc2struct_attributes Attrs;
typedef doc2struct_row Row;
typedef doc2struct_column Column;
typedef doc2struct_matrix Matrix;
#define START DOC2STRUCT_START
#define A_COUNT DOC2STRUCT_ATTRIBUTE_COUNT
#define DS_DEFAULT_CACHE_SIZE DOC2STRUCT_DEFAULT_CACHE_SIZE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <limits.h>

enum {
    RK_BLOCK, RK_LINE, RK_ROW, RK_COL, RK_SPAN, RK_TEXT, RK_FLAGS,
    RK_FONT, RK_INDENT, RK_HREF, RK_TAG, RK_SRC, RK_COUNT
};
static const char *const run_names[RK_COUNT] = {
    "block", "line", "row", "col", "span", "text", "flags",
    "font_size", "indent", "href", "tag", "src"
};
static const char *const attribute_names[A_COUNT] = {
    "style", "colspan", "rowspan", "href", "src", "alt", "hidden"
};
static const char *const row_names[] = {
    "id", "level", "type", "text", "matrix", "cleaned", "preamble", "footnotes", "postamble"
};
static const char *const type_names[] = {
    "title", "text", "textsmall", "table", "image", "list", "redundant"
};
#define ROW_KEY_COUNT (sizeof(row_names) / sizeof(*row_names))
#define ROW_TYPE_COUNT (sizeof(type_names) / sizeof(*type_names))

/* Immutable schema strings belong to this module's interpreter. Every export
 * still constructs fresh dictionaries and mutable payloads. */
typedef struct {
    PyObject *parser;
    PyObject *run_keys[RK_COUNT], *attribute_keys[A_COUNT];
    PyObject *row_keys[ROW_KEY_COUNT], *row_types[ROW_TYPE_COUNT];
} ModuleState;

static ModuleState *conversion_state(PyObject *module) {
    ModuleState *state = PyModule_GetState(module);
    if (!state || !state->run_keys[0]) {
        PyErr_SetString(PyExc_RuntimeError, "native module has been cleared");
        return NULL;
    }
    return state;
}

static Str str_view(const char *p) { return (Str){p,strlen(p)}; }
static int str_eq(Str a,Str b) { return a.n==b.n && (!a.n || !memcmp(a.p,b.p,a.n)); }
static int str_is(Str a,const char *b) { return str_eq(a,str_view(b)); }
static int native_error(const char *message) {
    if (!message || !*message) return 0;
    if (!strcmp(message,"out of memory")) PyErr_NoMemory();
    else PyErr_SetString(PyExc_ValueError,message);
    return 1;
}
static Document *new_document(void) {
    doc2struct_error error;
    Document *d=doc2struct_document_new(&error);
    if(!d)native_error(error.message);
    return d;
}
static Str string(Document *d,PyObject *o) {
    Py_ssize_t n; const char *p;
    if(!o || o==Py_None)return (Str){0};
    if(!PyUnicode_Check(o)) { PyErr_SetString(PyExc_TypeError,"expected a string");return (Str){0}; }
    p=PyUnicode_AsUTF8AndSize(o,&n);
    return p?doc2struct_document_copy(d,(Str){p,(size_t)n}):(Str){0};
}
static PyObject *unicode(Str s) {
    if (!s.p)
        return Py_NewRef(Py_None);
    if (s.n > PY_SSIZE_T_MAX) {
        PyErr_NoMemory();
        return NULL;
    }
    return PyUnicode_DecodeUTF8(s.p, (Py_ssize_t)s.n, "strict");
}
static PyObject *get(PyObject *d, PyObject *key) {
    PyObject *o;
    if (!PyDict_Check(d)) {
        PyErr_SetString(PyExc_TypeError, "expected a dictionary");
        return NULL;
    }
    o = PyDict_GetItem(d, key);
    if (!o)
        PyErr_Format(PyExc_KeyError, "missing field: %U", key);
    return o;
}
static int set(PyObject *d, PyObject *key, PyObject *value) {
    int rc;
    if (!value)
        return 0;
    rc = PyDict_SetItem(d, key, value);
    Py_DECREF(value);
    return rc == 0;
}
static void capsule_free(PyObject *c) {
    Rules *r=PyCapsule_GetPointer(c,"doc2struct.Rules");
    if(r)doc2struct_rules_free(r);else PyErr_Clear();
}
static PyObject *rules_capsule(Rules *r,doc2struct_error *error) {
    PyObject *c;
    if(!r){native_error(error->message);return NULL;}
    c=PyCapsule_New(r,"doc2struct.Rules",capsule_free);
    if(!c)doc2struct_rules_free(r);
    return c;
}
static PyObject *prepare_rules(PyObject *self,PyObject *config) {
    PyObject *serialized=NULL;const char *p;Py_ssize_t n;
    doc2struct_error error;Rules *r;
    if(PyDict_Check(config)) {
        PyObject *json=PyImport_ImportModule("json");
        if(!json)return NULL;
        serialized=PyObject_CallMethod(json,"dumps","O",config);Py_DECREF(json);
        if(!serialized)return NULL;
        config=serialized;
    }
    p=PyUnicode_AsUTF8AndSize(config,&n);
    if(!p){Py_XDECREF(serialized);return NULL;}
    r=doc2struct_rules_json(p,(size_t)n,&error);
    Py_XDECREF(serialized);return rules_capsule(r,&error);
}
static PyObject *prepare_preset(PyObject *self,PyObject *name) {
    doc2struct_error error;const char *p=NULL;Rules *r;Py_ssize_t n;
    if(name!=Py_None) {
        p=PyUnicode_AsUTF8AndSize(name,&n);if(!p)return NULL;
        if(memchr(p,0,(size_t)n)){PyErr_SetString(PyExc_ValueError,"preset name contains NUL");return NULL;}
    }
    r=doc2struct_rules_preset(p,&error);return rules_capsule(r,&error);
}
static PyObject *defaults_json(PyObject *self,PyObject *unused) { return unicode(doc2struct_defaults_json()); }
static PyObject *preset_packs(PyObject *self,PyObject *unused) {
    size_t i,n=doc2struct_preset_count();PyObject *list=PyList_New((Py_ssize_t)n);
    if(!list)return NULL;
    for(i=0;i<n;i++) {
        PyObject *v=unicode(doc2struct_preset_pack(i));
        if(!v){Py_DECREF(list);return NULL;}PyList_SET_ITEM(list,(Py_ssize_t)i,v);
    }
    return list;
}
static PyObject *resolve_rules(PyObject *self,PyObject *args) {
    const char *input;Py_ssize_t n;int preset=0;size_t size;char *out;
    doc2struct_error error;PyObject *result;
    if(!PyArg_ParseTuple(args,"s#|p:resolve_rules",&input,&n,&preset))return NULL;
    out=doc2struct_rules_resolve(input,(size_t)n,preset,&size,&error);
    if(!out){native_error(error.message);return NULL;}
    result=unicode((Str){out,size});doc2struct_buffer_free(out);return result;
}
static PyObject *merge_rules(PyObject *self,PyObject *args) {
    const char *base,*extra;Py_ssize_t nb,ne;size_t size;char *out;
    doc2struct_error error;PyObject *result;
    if(!PyArg_ParseTuple(args,"s#s#:merge_rules",&base,&nb,&extra,&ne))return NULL;
    out=doc2struct_rules_merge_json((Str){base,(size_t)nb},(Str){extra,(size_t)ne},&size,&error);
    if(!out){native_error(error.message);return NULL;}
    result=unicode((Str){out,size});doc2struct_buffer_free(out);return result;
}
static int input(PyObject *o, Str *out, PyObject **owner) {
    *owner = NULL;
    if (PyUnicode_Check(o)) {
        Py_ssize_t n;
        const char *p = PyUnicode_AsUTF8AndSize(o, &n);
        if (!p) return 0;
        *out = (Str){p, (size_t)n};
        return 1;
    }
    if (PyByteArray_Check(o)) {
        *owner = PyBytes_FromObject(o);
        if (!*owner)
            return 0;
        o = *owner;
    }
    if (!PyBytes_Check(o)) {
        PyErr_SetString(PyExc_TypeError, "input must be bytes, bytearray, or str");
        return 0;
    }
    *out = (Str){PyBytes_AS_STRING(o), (size_t)PyBytes_GET_SIZE(o)};
    return 1;
}
static int parse_input(PyObject *o, Str *out) {
    if (!PyBytes_Check(o)) {
        PyErr_SetString(PyExc_TypeError, "document input must be bytes");
        return 0;
    }
    *out = (Str){PyBytes_AS_STRING(o), (size_t)PyBytes_GET_SIZE(o)};
    return 1;
}
static PyObject *export_tokens(PyObject *module, Document *d) {
    ModuleState *state = conversion_state(module);
    if (!state)
        return NULL;
    PyObject *list = PyList_New((Py_ssize_t)doc2struct_token_count(d));
    size_t i = 0, j;
    doc2struct_token_iterator iterator={0};
    const Token *t;
    if (!list)
        return NULL;
    while ((t=doc2struct_token_next(d,&iterator))) {
            PyObject *attrs = Py_NewRef(Py_None), *tuple, *text = unicode(t->value);
            if (!text)
                goto error;
            if (t->kind == START) {
                Py_SETREF(attrs, PyDict_New());
                if (!attrs) {
                    Py_DECREF(text);
                    goto error;
                }
                for (j = 0; j < A_COUNT; j++)
                    if (t->attrs && t->attrs->value[j].p &&
                        !set(attrs, state->attribute_keys[j], unicode(t->attrs->value[j]))) {
                        Py_DECREF(text);
                        Py_DECREF(attrs);
                        goto error;
                    }
            }
            tuple = PyTuple_New(3);
            if (!tuple) {
                Py_DECREF(text);
                Py_DECREF(attrs);
                goto error;
            }
            {
                PyObject *kind = PyLong_FromLong(t->kind);
                if (!kind) {
                    Py_DECREF(tuple);
                    Py_DECREF(text);
                    Py_DECREF(attrs);
                    goto error;
                }
                PyTuple_SET_ITEM(tuple, 0, kind);
            }
            PyTuple_SET_ITEM(tuple, 1, text);
            PyTuple_SET_ITEM(tuple, 2, attrs);
            PyList_SET_ITEM(list, (Py_ssize_t)i++, tuple);
    }
    return list;
error:
    Py_DECREF(list);
    return NULL;
}
static PyObject *tokenize(PyObject *self, PyObject *html) {
    Document *d = NULL;
    Str text;
    PyObject *owner = NULL, *result = NULL;
    int ok;
    if (!input(html, &text, &owner))
        goto done;
    if(!(d=new_document()))goto done;
    Py_BEGIN_ALLOW_THREADS ok = doc2struct_tokenize(d, text);
    Py_END_ALLOW_THREADS if (!ok) {
        native_error(doc2struct_document_error(d));
        goto done;
    }
    result = export_tokens(self, d);
done:
    doc2struct_document_free(d);
    Py_XDECREF(owner);
    return result;
}
static PyObject *export_runs(PyObject *module, Document *d) {
    ModuleState *state = conversion_state(module);
    PyObject **keys;
    if (!state)
        return NULL;
    keys = state->run_keys;
    size_t nruns;
    const Run *runs=doc2struct_document_runs(d,&nruns);
    PyObject *list = PyList_New((Py_ssize_t)nruns);
    size_t i;
    if (!list)
        return NULL;
    for (i = 0; i < nruns; i++) {
        const Run *r = runs + i;
        PyObject *row = PyDict_New();
        if (!row)
            goto error;
        if (!set(row, keys[RK_BLOCK], PyLong_FromSize_t(r->block)) ||
            !set(row, keys[RK_LINE], PyLong_FromSize_t(r->line)) ||
            !set(row, keys[RK_ROW], r->in_table ? PyLong_FromSize_t(r->row) : Py_NewRef(Py_None)) ||
            !set(row, keys[RK_COL], r->in_table ? PyLong_FromSize_t(r->col) : Py_NewRef(Py_None)) ||
            !set(row, keys[RK_SPAN],
                 r->rowspan > 1 || r->colspan > 1
                     ? PyUnicode_FromFormat("%zux%zu", r->rowspan, r->colspan)
                     : Py_NewRef(Py_None)) ||
            !set(row, keys[RK_TEXT], unicode(r->text)) ||
            !set(row, keys[RK_FLAGS], PyLong_FromUnsignedLong(r->flags)) ||
            !set(row, keys[RK_FONT], r->has_font ? PyFloat_FromDouble(r->font) : Py_NewRef(Py_None)) ||
            !set(row, keys[RK_INDENT],
                 r->has_indent ? PyFloat_FromDouble(r->indent) : Py_NewRef(Py_None)) ||
            !set(row, keys[RK_HREF], unicode(r->href)) || !set(row, keys[RK_TAG], unicode(r->tag)) ||
            !set(row, keys[RK_SRC], unicode(r->src))) {
            Py_DECREF(row);
            goto error;
        }
        PyList_SET_ITEM(list, (Py_ssize_t)i, row);
    }
    return list;
error:
    Py_DECREF(list);
    return NULL;
}
static int import_tokens(PyObject *module, Document *d, PyObject *tokens) {
    ModuleState *state = conversion_state(module);
    if (!state)
        return 0;
    PyObject *it = PyObject_GetIter(tokens), *o;
    if (!it)
        return 0;
    while ((o = PyIter_Next(it))) {
        PyObject *t = PySequence_Fast(o, "token must be a triple");
        Token *dst;
        size_t j;
        Py_DECREF(o);
        if (!t)
            goto error;
        if (PySequence_Fast_GET_SIZE(t) != 3) {
            Py_DECREF(t);
            PyErr_SetString(PyExc_ValueError, "token must contain three values");
            goto error;
        }
        dst = doc2struct_add_token(d);
        if (!dst) {
            Py_DECREF(t);
            native_error(doc2struct_document_error(d));
            goto error;
        }
        dst->kind = (int)PyLong_AsLong(PySequence_Fast_GET_ITEM(t, 0));
        dst->attrs = NULL;
        dst->value = string(d, PySequence_Fast_GET_ITEM(t, 1));
        if (dst->kind == START) {
            PyObject *attrs = PySequence_Fast_GET_ITEM(t, 2);
            if (!PyDict_Check(attrs)) {
                Py_DECREF(t);
                PyErr_SetString(PyExc_TypeError, "start token attributes must be a dict");
                goto error;
            }
            dst->attrs = doc2struct_add_attributes(d);
            if (!dst->attrs) {
                Py_DECREF(t);
                native_error(doc2struct_document_error(d));
                goto error;
            }
            for (j = 0; j < A_COUNT; j++) {
                PyObject *v = PyDict_GetItem(attrs, state->attribute_keys[j]);
                if (v)
                    dst->attrs->value[j] = string(d, v);
            }
        }
        Py_DECREF(t);
        if (PyErr_Occurred())
            goto error;
    }
    Py_DECREF(it);
    return !PyErr_Occurred() && !native_error(doc2struct_document_error(d));
error:
    Py_DECREF(it);
    return 0;
}
static PyObject *normalize(PyObject *self, PyObject *args) {
    PyObject *tokens, *capsule, *result = NULL;
    Document *d = NULL;
    Rules *rules;
    int ok = 0;
    if (!PyArg_ParseTuple(args, "OO:normalize", &tokens, &capsule))
        return NULL;
    rules = PyCapsule_GetPointer(capsule, "doc2struct.Rules");
    if (!rules) return NULL;
    if (!(d=new_document()) || !import_tokens(self, d, tokens))
        goto done;
    Py_BEGIN_ALLOW_THREADS ok = doc2struct_normalize(d, rules);
    Py_END_ALLOW_THREADS if (ok) result = export_runs(self, d);
    else native_error(doc2struct_document_error(d));
done:
    doc2struct_document_free(d);
    return result;
}
static PyObject *html_instructions(PyObject *self, PyObject *args) {
    PyObject *html, *capsule, *owner = NULL, *result = NULL;
    Document *d = NULL;
    Rules *rules;
    Str text;
    int ok;
    if (!PyArg_ParseTuple(args, "OO:html_instructions", &html, &capsule))
        return NULL;
    rules = PyCapsule_GetPointer(capsule, "doc2struct.Rules");
    if (!rules) return NULL;
    if (!input(html, &text, &owner) || !(d = new_document()))
        goto done;
    Py_BEGIN_ALLOW_THREADS
    ok = doc2struct_tokenize(d, text) && doc2struct_normalize(d, rules);
    Py_END_ALLOW_THREADS
    if (ok) result = export_runs(self, d);
    else native_error(doc2struct_document_error(d));
done:
    doc2struct_document_free(d);
    Py_XDECREF(owner);
    return result;
}
static int import_runs(PyObject *module, Document *d, PyObject *instructions) {
    ModuleState *state = conversion_state(module);
    PyObject **keys;
    if (!state)
        return 0;
    keys = state->run_keys;
    PyObject *seq = PySequence_Fast(instructions, "instructions must be a sequence");
    Py_ssize_t i;
    if (!seq)
        return 0;
    for (i = 0; i < PySequence_Fast_GET_SIZE(seq); i++) {
        PyObject *o = PySequence_Fast_GET_ITEM(seq, i), *v;
        Run *r = doc2struct_add_run(d);
        if (!r)
            goto error;
        r->rowspan = r->colspan = 1;
#define SIZE_FIELD(name, key)                                                                      \
    v = get(o, keys[key]);                                                                         \
    if (!v)                                                                                        \
        goto error;                                                                                \
    r->name = PyLong_AsSize_t(v)
        SIZE_FIELD(block, RK_BLOCK);
        SIZE_FIELD(line, RK_LINE);
        v = get(o, keys[RK_ROW]);
        if (!v)
            goto error;
        if (v != Py_None) {
            r->in_table = 1;
            r->row = PyLong_AsSize_t(v);
            SIZE_FIELD(col, RK_COL);
        }
#undef SIZE_FIELD
        r->text = string(d, get(o, keys[RK_TEXT]));
        if (!r->text.p && !PyErr_Occurred())
            r->text = str_view("");
        r->href = string(d, get(o, keys[RK_HREF]));
        r->src = string(d, get(o, keys[RK_SRC]));
        v = PyDict_GetItem(o, keys[RK_TAG]);
        if (v)
            r->tag = string(d, v);
        v = get(o, keys[RK_FLAGS]);
        if (!v)
            goto error;
        r->flags = v == Py_None ? 0 : (unsigned)PyLong_AsUnsignedLong(v);
        v = get(o, keys[RK_FONT]);
        if (!v)
            goto error;
        if (v != Py_None) {
            r->has_font = 1;
            r->font = PyFloat_AsDouble(v);
        }
        v = get(o, keys[RK_INDENT]);
        if (!v)
            goto error;
        if (v != Py_None) {
            r->has_indent = 1;
            r->indent = PyFloat_AsDouble(v);
        }
        v = get(o, keys[RK_SPAN]);
        if (!v)
            goto error;
        if (v != Py_None && PyObject_IsTrue(v)) {
            Str span = string(d, v);
            char *end;
            unsigned long long rs, cs = 0;
            if (!span.p)
                goto error;
            rs = strtoull(span.p, &end, 10);
            if (end && *end == 'x')
                cs = strtoull(end + 1, &end, 10);
            if (!rs || !cs || *end || rs > SIZE_MAX || cs > SIZE_MAX) {
                PyErr_SetString(PyExc_ValueError, "invalid instruction span");
                goto error;
            }
            r->rowspan = (size_t)rs;
            r->colspan = (size_t)cs;
        }
        if (PyErr_Occurred())
            goto error;
    }
    Py_DECREF(seq);
    return !native_error(doc2struct_document_error(d));
error:
    Py_DECREF(seq);
    if (!PyErr_Occurred())
        native_error(doc2struct_document_error(d));
    return 0;
}
static PyObject *matrix(const Matrix *m) {
    PyObject *rows = PyList_New((Py_ssize_t)m->nr);
    size_t i, j;
    if (!rows)
        return NULL;
    for (i = 0; i < m->nr; i++) {
        PyObject *row = PyList_New((Py_ssize_t)m->nc);
        if (!row) {
            Py_DECREF(rows);
            return NULL;
        }
        PyList_SET_ITEM(rows, (Py_ssize_t)i, row);
        for (j = 0; j < m->nc; j++) {
            PyObject *v = unicode(m->cells[i * m->nc + j]);
            if (!v) {
                Py_DECREF(rows);
                return NULL;
            }
            PyList_SET_ITEM(row, (Py_ssize_t)j, v);
        }
    }
    return rows;
}
static PyObject *notes(const Row *r) {
    PyObject *list;
    size_t i;
    if (!r->nnotes)
        return Py_NewRef(Py_None);
    list = PyList_New((Py_ssize_t)r->nnotes);
    if (!list)
        return NULL;
    for (i = 0; i < r->nnotes; i++) {
        PyObject *pair = PyList_New(2), *id, *text;
        if (!pair) {
            Py_DECREF(list);
            return NULL;
        }
        PyList_SET_ITEM(list, (Py_ssize_t)i, pair);
        id = unicode(r->notes[i].id);
        if (!id) {
            Py_DECREF(list);
            return NULL;
        }
        PyList_SET_ITEM(pair, 0, id);
        text = unicode(r->notes[i].text);
        if (!text) {
            Py_DECREF(list);
            return NULL;
        }
        PyList_SET_ITEM(pair, 1, text);
    }
    return list;
}
static PyObject *cast(PyObject *value, Str dtype) {
    PyObject *out;
    if (!value || value == Py_None)
        return value;
    if (str_is(dtype, "int"))
        out = PyUnicode_Check(value) ? PyLong_FromUnicodeObject(value, 10) : PyNumber_Long(value);
    else if (str_is(dtype, "bool")) {
        int truth = PyObject_IsTrue(value);
        out = truth < 0 ? NULL : PyBool_FromLong(truth);
    } else
        out = PyObject_Str(value);
    Py_DECREF(value);
    if (!out &&
        (PyErr_ExceptionMatches(PyExc_ValueError) || PyErr_ExceptionMatches(PyExc_TypeError))) {
        PyErr_Clear();
        return Py_NewRef(Py_None);
    }
    return out;
}
static PyObject *source(const Run *r, Str name) {
    if (str_is(name, "href"))
        return unicode(r->href);
    if (str_is(name, "src"))
        return unicode(r->src);
    if (str_is(name, "font_size"))
        return r->has_font ? PyFloat_FromDouble(r->font) : Py_NewRef(Py_None);
    if (str_is(name, "indent"))
        return r->has_indent ? PyFloat_FromDouble(r->indent) : Py_NewRef(Py_None);
    if (str_is(name, "flags"))
        return PyLong_FromUnsignedLong(r->flags);
    return Py_NewRef(Py_None);
}
static PyObject *custom(Document *d, const Column *c, const Row *r) {
    size_t i, j;
    int all = str_is(c->which, "all") || str_is(c->dtype, "list");
    PyObject *values;
    if (!doc2struct_column_applies(c, r))
        return Py_NewRef(Py_None);
    if (str_is(c->source, "text") || str_is(c->source, "header_title")) {
        Str text = str_is(c->source, "header_title") ? r->title : r->text;
        if (!doc2struct_column_text(d,c,text,&text)) {
            native_error(doc2struct_document_error(d));return NULL;
        }
        return cast(unicode(text), c->dtype);
    }
    values = PyList_New(0);
    if (!values)
        return NULL;
    for (i = 0; i < r->nitems; i++) {
        size_t count=0;
        const Str *links=str_is(c->source,"href")?doc2struct_row_links(r,i,&count):NULL;
        if(!links)count=1;
        for (j = 0; j < count; j++) {
            PyObject *v = links ? unicode(links[j]) : source(doc2struct_row_item(r,i), c->source);
            PyObject *converted;
            if (!v)
                goto error;
            if (v == Py_None) {
                Py_DECREF(v);
                continue;
            }
            converted = cast(v, str_is(c->dtype, "list") ? str_view("str") : c->dtype);
            if (!converted)
                goto error;
            if (PyList_Append(values, converted) < 0) {
                Py_DECREF(converted);
                goto error;
            }
            Py_DECREF(converted);
        }
    }
    if (all)
        return values;
    {
        Py_ssize_t n = PyList_GET_SIZE(values);
        PyObject *result =
            n ? Py_NewRef(PyList_GET_ITEM(values, str_is(c->which, "first") ? 0 : n - 1))
              : Py_NewRef(Py_None);
        Py_DECREF(values);
        return result;
    }
error:
    Py_DECREF(values);
    return NULL;
}
static PyObject *cleanup_info(const Row *row) {
    PyObject *list;
    size_t i;
    if (row->cleaned < 0)
        return Py_NewRef(Py_None);
    list = PyList_New(0);
    if (!list)
        return NULL;
    for (i = 0; doc2struct_cleanup_name(i); i++)
        if (row->cleaned & (1u << i)) {
            PyObject *name = PyUnicode_FromString(doc2struct_cleanup_name(i));
            if (!name || PyList_Append(list, name) < 0) {
                Py_XDECREF(name);
                Py_DECREF(list);
                return NULL;
            }
            Py_DECREF(name);
        }
    return list;
}
static PyObject *row_type(ModuleState *state, Str type) {
    size_t i;
    for (i = 0; i < ROW_TYPE_COUNT; i++)
        if (str_is(type, type_names[i]))
            return Py_NewRef(state->row_types[i]);
    return unicode(type);
}
static PyObject *export_rows(PyObject *module, Document *d, Rules *r, int with_rules) {
    ModuleState *state = conversion_state(module);
    PyObject **keys;
    if (!state)
        return NULL;
    keys = state->row_keys;
    size_t nrows,ncolumns;
    const Row *rows=doc2struct_document_rows(d,&nrows);
    const Column *columns=doc2struct_rules_columns(r,&ncolumns);
    PyObject *list = PyList_New((Py_ssize_t)nrows);
    PyObject *column_keys = NULL;
    size_t i, j;
    if (!list)
        return NULL;
    if (ncolumns && nrows) {
        if (ncolumns > PY_SSIZE_T_MAX) { PyErr_NoMemory(); goto error; }
        column_keys = PyTuple_New((Py_ssize_t)ncolumns);
        if (!column_keys)
            goto error;
        for (i = 0; i < ncolumns; i++) {
            PyObject *key = unicode(columns[i].name);
            if (!key)
                goto error;
            PyTuple_SET_ITEM(column_keys, (Py_ssize_t)i, key);
        }
    }
    for (i = 0; i < nrows; i++) {
        const Row *row = rows + i;
        PyObject *out = PyDict_New();
        if (!out)
            goto error;
        if (!set(out, keys[0], PyLong_FromSize_t(row->id)) ||
            !set(out, keys[1], PyLong_FromSize_t(row->level)) ||
            !set(out, keys[2], row_type(state, row->type)) ||
            !set(out, keys[3], unicode(row->text)) ||
            !set(out, keys[4], row->matrix.nr ? matrix(&row->matrix) : Py_NewRef(Py_None)) ||
            !set(out, keys[5], cleanup_info(row)) ||
            !set(out, keys[6], unicode(row->preamble)) || !set(out, keys[7], notes(row)) ||
            !set(out, keys[8], unicode(row->postamble))) {
            Py_DECREF(out);
            goto error;
        }
        for (j = 0; j < ncolumns; j++) {
            PyObject *key = PyTuple_GET_ITEM(column_keys, (Py_ssize_t)j);
            if (!set(out, key, custom(d, columns + j, row))) {
                Py_DECREF(out);
                goto error;
            }
        }
        PyList_SET_ITEM(list, (Py_ssize_t)i, out);
    }
    Py_XDECREF(column_keys);
    return list;
error:
    Py_XDECREF(column_keys);
    Py_DECREF(list);
    return NULL;
}
static PyObject *standardize(PyObject *self, PyObject *args) {
    PyObject *instructions, *capsule, *result = NULL;
    Document *d = NULL;
    Rules *r;
    int with_rules, ok;
    if (!PyArg_ParseTuple(args, "OOp:standardize", &instructions, &capsule, &with_rules))
        return NULL;
    r = PyCapsule_GetPointer(capsule, "doc2struct.Rules");
    if (!r)
        return NULL;
    if (!(d=new_document()) || !import_runs(self, d, instructions))
        goto done;
    Py_BEGIN_ALLOW_THREADS ok = doc2struct_standardize(d, r);
    Py_END_ALLOW_THREADS if (ok) result = export_rows(self, d, r, with_rules);
    else native_error(doc2struct_document_error(d));
done:
    doc2struct_document_free(d);
    return result;
}
typedef struct {
    doc2struct_parser *context;
    int busy, closed;
} NativeParser;

static void parser_free(PyObject *capsule) {
    NativeParser *parser = PyCapsule_GetPointer(capsule, "doc2struct.Parser");
    if (!parser) {
        PyErr_Clear();
        return;
    }
    doc2struct_parser_free(parser->context);
    free(parser);
}

static PyObject *create_parser(size_t cache_size) {
    NativeParser *parser;
    PyObject *capsule;
    parser = calloc(1, sizeof(*parser));
    if (!parser)
        return PyErr_NoMemory();
    parser->context=doc2struct_parser_new(cache_size,NULL);
    if(!parser->context){free(parser);return PyErr_NoMemory();}
    capsule = PyCapsule_New(parser, "doc2struct.Parser", parser_free);
    if (!capsule) {
        doc2struct_parser_free(parser->context);free(parser);
    }
    return capsule;
}

static PyObject *parser_new(PyObject *self, PyObject *cache_size) {
    Py_ssize_t size = PyLong_AsSsize_t(cache_size);
    if (size == -1 && PyErr_Occurred())
        return NULL;
    if (size < 0) {
        PyErr_SetString(PyExc_ValueError, "cache_size must be nonnegative");
        return NULL;
    }
    return create_parser((size_t)size);
}

static NativeParser *idle_parser(PyObject *capsule) {
    NativeParser *parser = PyCapsule_GetPointer(capsule, "doc2struct.Parser");
    if (parser && parser->busy) {
        PyErr_SetString(PyExc_RuntimeError, "Parser is already parsing a document");
        return NULL;
    }
    return parser;
}

static PyObject *parser_clear(PyObject *self, PyObject *capsule) {
    NativeParser *parser = idle_parser(capsule);
    if (!parser)
        return NULL;
    doc2struct_parser_clear(parser->context);
    Py_RETURN_NONE;
}

static PyObject *parser_close(PyObject *self, PyObject *capsule) {
    NativeParser *parser = idle_parser(capsule);
    if (!parser)
        return NULL;
    parser->closed = 1;
    doc2struct_parser_close(parser->context);
    Py_RETURN_NONE;
}

static PyObject *parser_info(PyObject *self, PyObject *capsule) {
    NativeParser *parser = idle_parser(capsule);
    if (!parser)
        return NULL;
    return Py_BuildValue("(KKO)", (unsigned long long)doc2struct_parser_cache_limit(parser->context),
                         (unsigned long long)doc2struct_parser_cached_bytes(parser->context),
                         parser->closed ? Py_True : Py_False);
}

static PyObject *default_parser(PyObject *module) {
    ModuleState *state = PyModule_GetState(module);
    if (!state || !state->parser) {
        PyErr_SetString(PyExc_RuntimeError, "native module has been cleared");
        return NULL;
    }
    return state->parser;
}

static PyObject *clear_parser_cache(PyObject *self, PyObject *unused) {
    PyObject *capsule = default_parser(self);
    return capsule ? parser_clear(self, capsule) : NULL;
}

static PyObject *default_parser_info(PyObject *self, PyObject *unused) {
    PyObject *capsule = default_parser(self);
    return capsule ? parser_info(self, capsule) : NULL;
}

typedef struct {
    Document *document;
    Rules *rules;
    PyObject *source, *rules_owner, *parser_owner;
    int with_rules;
} NativeDocument;

static void native_document_free(PyObject *capsule) {
    NativeDocument *native = PyCapsule_GetPointer(capsule, "doc2struct.NativeDocument");
    if (!native) {
        PyErr_Clear();
        return;
    }
    if (native->parser_owner) {
        NativeParser *parser = PyCapsule_GetPointer(native->parser_owner, "doc2struct.Parser");
        if (!parser)
            PyErr_Clear();
        if (!parser || parser->busy || parser->closed)
            doc2struct_document_detach_cache(native->document);
    }
    doc2struct_document_free(native->document);
    Py_XDECREF(native->source);
    Py_XDECREF(native->rules_owner);
    Py_XDECREF(native->parser_owner);
    free(native);
}

static PyObject *native_rows(PyObject *self, PyObject *capsule) {
    NativeDocument *native = PyCapsule_GetPointer(capsule, "doc2struct.NativeDocument");
    if (!native)
        return NULL;
    return export_rows(self, native->document, native->rules, native->with_rules);
}

static PyObject *native_len(PyObject *self, PyObject *capsule) {
    NativeDocument *native = PyCapsule_GetPointer(capsule, "doc2struct.NativeDocument");
    size_t count;
    if(!native)return NULL;
    doc2struct_document_rows(native->document,&count);return PyLong_FromSize_t(count);
}

static PyObject *native_json(PyObject *self, PyObject *capsule) {
    NativeDocument *native = PyCapsule_GetPointer(capsule, "doc2struct.NativeDocument");
    doc2struct_error error;size_t size;char *json;PyObject *result=NULL;
    if(!native)return NULL;
    Py_BEGIN_ALLOW_THREADS
    json=doc2struct_document_json(native->document,native->rules,native->with_rules,&size,&error);
    Py_END_ALLOW_THREADS
    if(json)result=PyBytes_FromStringAndSize(json,(Py_ssize_t)size);
    else native_error(error.message);
    doc2struct_buffer_free(json);return result;
}

static PyObject *native_columns(PyObject *self, PyObject *capsule) {
    ModuleState *state = conversion_state(self);
    if (!state)
        return NULL;
    NativeDocument *native = PyCapsule_GetPointer(capsule, "doc2struct.NativeDocument");
    PyObject *columns, *lists[9] = {0};
    size_t i, j, nrows, ncolumns;
    const Row *rows;const Column *definitions;
    if (!native)
        return NULL;
    rows=doc2struct_document_rows(native->document,&nrows);
    definitions=doc2struct_rules_columns(native->rules,&ncolumns);
    columns = PyDict_New();
    if (!columns)
        return NULL;
    for (j = 0; j < 9; j++) {
        lists[j] = PyList_New((Py_ssize_t)nrows);
        if (!lists[j] || PyDict_SetItem(columns, state->row_keys[j], lists[j]) < 0)
            goto error;
    }
    for (i = 0; i < nrows; i++) {
        const Row *r = rows + i;
        PyObject *values[9];
        values[0] = PyLong_FromSize_t(r->id);
        values[1] = PyLong_FromSize_t(r->level);
        values[2] = row_type(state, r->type);
        values[3] = unicode(r->text);
        values[4] = r->matrix.nr ? matrix(&r->matrix) : Py_NewRef(Py_None);
        values[5] = cleanup_info(r);
        values[6] = unicode(r->preamble);
        values[7] = notes(r);
        values[8] = unicode(r->postamble);
        for (j = 0; j < 9; j++) {
            if (!lists[j])
                continue;
            if (!values[j]) {
                size_t k;
                for (k = 0; k < 9; k++)
                    Py_XDECREF(values[k]);
                goto error;
            }
        }
        for (j = 0; j < 9; j++)
            if (lists[j])
                PyList_SET_ITEM(lists[j], (Py_ssize_t)i, values[j]);
    }
    for (j = 0; j < ncolumns; j++) {
        const Column *c = definitions + j;
        PyObject *key = unicode(c->name), *values = PyList_New((Py_ssize_t)nrows);
        if (!key || !values) {
            Py_XDECREF(key);
            Py_XDECREF(values);
            goto error;
        }
        for (i = 0; i < nrows; i++) {
            PyObject *value = custom(native->document, c, rows + i);
            if (!value) {
                Py_DECREF(key);
                Py_DECREF(values);
                goto error;
            }
            PyList_SET_ITEM(values, (Py_ssize_t)i, value);
        }
        if (PyDict_SetItem(columns, key, values) < 0) {
            Py_DECREF(key);
            Py_DECREF(values);
            goto error;
        }
        Py_DECREF(key);
        Py_DECREF(values);
    }
    for (j = 0; j < 9; j++)
        Py_XDECREF(lists[j]);
    return columns;
error:
    for (j = 0; j < 9; j++)
        Py_XDECREF(lists[j]);
    Py_DECREF(columns);
    return NULL;
}

static PyObject *parse_html_impl(PyObject *self, PyObject *args, int keep_native) {
    PyObject *html, *capsule, *owner = NULL, *result = NULL, *parser_capsule = Py_None;
    Rules *r;
    Document *document = NULL;
    doc2struct_error error;
    NativeDocument *native = NULL;
    NativeParser *parser = NULL;
    Str text;
    int with_rules, ok = 0;
    if (!PyArg_ParseTuple(args, "OOp|O:parse_html", &html, &capsule, &with_rules, &parser_capsule))
        return NULL;
    r = PyCapsule_GetPointer(capsule, "doc2struct.Rules");
    if (!r)
        return NULL;
    if (parser_capsule == Py_None) {
        parser_capsule = default_parser(self);
        if (!parser_capsule)
            return NULL;
        parser = PyCapsule_GetPointer(parser_capsule, "doc2struct.Parser");
        if (!parser)
            return NULL;
        /* The GIL protects this loan. An overlapping or reentrant call uses
         * its own arena, so it cannot mutate the busy default pool. */
        if (parser->busy)
            parser = NULL;
    } else {
        parser = idle_parser(parser_capsule);
        if (!parser)
            return NULL;
        if (parser->closed) {
            PyErr_SetString(PyExc_ValueError, "Parser is closed");
            return NULL;
        }
    }
    if (keep_native) {
        native = calloc(1, sizeof(*native));
        if (!native)
            return PyErr_NoMemory();
    }
    if (!parse_input(html, &text))
        goto done;
    if (parser)
        parser->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    document=doc2struct_parse(parser?parser->context:NULL,text,r,&error);
    ok=document!=NULL;
    Py_END_ALLOW_THREADS if (ok && keep_native) {
        native->document=document;
        native->rules = r;
        native->with_rules = with_rules;
        native->source = owner ? owner : Py_NewRef(html);
        owner = NULL;
        native->rules_owner = Py_NewRef(capsule);
        native->parser_owner = parser ? Py_NewRef(parser_capsule) : NULL;
        result = PyCapsule_New(native, "doc2struct.NativeDocument", native_document_free);
        if (result) {
            if (parser)
                parser->busy = 0;
            return result;
        }
    }
    else if (ok) result = export_rows(self, document, r, with_rules);
    else native_error(error.message);
done:
    doc2struct_document_free(document);
    if (parser)
        parser->busy = 0;
    if (native) {
        Py_XDECREF(native->source);
        Py_XDECREF(native->rules_owner);
        Py_XDECREF(native->parser_owner);
        free(native);
    }
    Py_XDECREF(owner);
    return result;
}
static PyObject *parse_html(PyObject *self, PyObject *args) {
    return parse_html_impl(self, args, 0);
}
static PyObject *parse_native(PyObject *self, PyObject *args) {
    return parse_html_impl(self, args, 1);
}
static PyObject *pdfium_init(PyObject *self, PyObject *path) {
    const char *name = PyUnicode_AsUTF8(path);
    doc2struct_error error;
    int ok;
    if (!name) return NULL;
    Py_BEGIN_ALLOW_THREADS
    ok = doc2struct_pdfium_init(name, &error);
    Py_END_ALLOW_THREADS
    if (!ok) { native_error(error.message); return NULL; }
    Py_RETURN_NONE;
}

static PyObject *pdf_impl(PyObject *self, PyObject *args, int instructions) {
    PyObject *pdf, *capsule = NULL, *parser_capsule = Py_None, *owner = NULL, *result = NULL;
    NativeParser *parser;
    Rules *rules = NULL;
    Document *document;
    doc2struct_error error;
    Str bytes;
    int with_rules = 0;
    if (instructions) {
        if (!PyArg_ParseTuple(args, "OO:pdf_instructions", &pdf, &capsule)) return NULL;
    } else {
        if (!PyArg_ParseTuple(args, "OOp|O:parse_pdf", &pdf, &capsule, &with_rules, &parser_capsule)) return NULL;
    }
    rules = PyCapsule_GetPointer(capsule, "doc2struct.Rules");
    if (!rules) return NULL;
    if (parser_capsule == Py_None) {
        parser_capsule = default_parser(self);
        if (!parser_capsule) return NULL;
        parser = PyCapsule_GetPointer(parser_capsule, "doc2struct.Parser");
        if (!parser) return NULL;
        if (parser->busy) parser = NULL;
    } else {
        parser = idle_parser(parser_capsule);
        if (!parser) return NULL;
        if (parser->closed) { PyErr_SetString(PyExc_ValueError, "Parser is closed"); return NULL; }
    }
    if (instructions) {
        if (!PyBytes_Check(pdf) && !PyByteArray_Check(pdf)) {
            PyErr_SetString(PyExc_TypeError, "PDF input must be bytes or bytearray");
            return NULL;
        }
        if (!input(pdf, &bytes, &owner)) return NULL;
    } else if (!parse_input(pdf, &bytes)) return NULL;
    if (parser) parser->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    document = instructions
        ? doc2struct_pdf_instructions(parser ? parser->context : NULL, bytes, rules, &error)
        : doc2struct_parse_pdf(parser ? parser->context : NULL, bytes, rules, &error);
    Py_END_ALLOW_THREADS
    if (document) result = instructions ? export_runs(self, document) : export_rows(self, document,rules,with_rules);
    else native_error(error.message);
    doc2struct_document_free(document);
    if (parser) parser->busy = 0;
    Py_XDECREF(owner);
    return result;
}
static PyObject *parse_pdf(PyObject *self, PyObject *args) { return pdf_impl(self,args,0); }
static PyObject *pdf_instructions(PyObject *self, PyObject *args) { return pdf_impl(self,args,1); }

static PyObject *text_impl(PyObject *self, PyObject *args, int partial, int instructions) {
    PyObject *source, *owner = NULL, *result = NULL, *capsule = NULL, *parser_capsule = Py_None;
    NativeParser *parser;
    Rules *rules = NULL;
    int with_rules = 0;
    Str bytes;
    Document *document;
    doc2struct_error error;
    if (instructions) {
        if (!PyArg_ParseTuple(args, "OO:text_instructions", &source, &capsule)) return NULL;
    } else {
        if (!PyArg_ParseTuple(args, "OOp|O:parse_text", &source, &capsule, &with_rules, &parser_capsule)) return NULL;
    }
    rules = PyCapsule_GetPointer(capsule, "doc2struct.Rules");
    if (!rules) return NULL;
    if (parser_capsule == Py_None) {
        parser_capsule = default_parser(self);
        if (!parser_capsule) return NULL;
        parser = PyCapsule_GetPointer(parser_capsule, "doc2struct.Parser");
        if (!parser) return NULL;
        if (parser->busy) parser = NULL;
    } else {
        parser = idle_parser(parser_capsule);
        if (!parser) return NULL;
        if (parser->closed) { PyErr_SetString(PyExc_ValueError, "Parser is closed"); return NULL; }
    }
    if (instructions) {
        if (!input(source, &bytes, &owner)) return NULL;
    } else if (!parse_input(source, &bytes)) return NULL;
    if (parser) parser->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    if (instructions) document = partial
        ? doc2struct_partialsgml_instructions(parser ? parser->context : NULL, bytes, rules, &error)
        : doc2struct_text_instructions(parser ? parser->context : NULL, bytes, rules, &error);
    else document = partial
        ? doc2struct_parse_partialsgml(parser ? parser->context : NULL, bytes, rules, &error)
        : doc2struct_parse_text(parser ? parser->context : NULL, bytes, rules, &error);
    Py_END_ALLOW_THREADS
    if (document) result = instructions ? export_runs(self, document) : export_rows(self, document, rules, with_rules);
    else native_error(error.message);
    doc2struct_document_free(document);
    if (parser) parser->busy = 0;
    Py_XDECREF(owner);
    return result;
}
static PyObject *text_instructions(PyObject *self, PyObject *args) { return text_impl(self, args, 0, 1); }
static PyObject *partialsgml_instructions(PyObject *self, PyObject *args) { return text_impl(self, args, 1, 1); }
static PyObject *parse_text(PyObject *self, PyObject *args) { return text_impl(self, args, 0, 0); }
static PyObject *parse_partialsgml(PyObject *self, PyObject *args) { return text_impl(self, args, 1, 0); }

static PyMethodDef methods[] = {
    {"parse_text", parse_text, METH_VARARGS, "Parse plain text through native layout and standardization."},
    {"parse_partialsgml", parse_partialsgml, METH_VARARGS, "Parse partial SGML through native layout and standardization."},
    {"text_instructions", text_instructions, METH_VARARGS, "Convert plain text to native instructions."},
    {"partialsgml_instructions", partialsgml_instructions, METH_VARARGS, "Convert text with partial SGML to native instructions."},
    {"pdfium_init", pdfium_init, METH_O, "Initialize the required private PDFium engine."},
    {"parse_pdf", parse_pdf, METH_VARARGS, "Extract, lay out and standardize PDF bytes in C."},
    {"pdf_instructions", pdf_instructions, METH_VARARGS, "Extract PDF instructions in C."},
    {"merge_rules",merge_rules,METH_VARARGS,"Extend rules using native merge semantics."},
    {"prepare_preset",prepare_preset,METH_O,"Compile a bundled preset in C."},
    {"defaults_json",defaults_json,METH_NOARGS,"Return canonical defaults."},
    {"preset_packs",preset_packs,METH_NOARGS,"Return embedded preset packs."},
    {"resolve_rules",resolve_rules,METH_VARARGS,"Resolve configuration using C defaults."},
    {"clear_parser_cache", clear_parser_cache, METH_NOARGS,
     "Release default parser scratch blocks."},
    {"default_parser_info", default_parser_info, METH_NOARGS, "Return default parser cache state."},
    {"parser_new", parser_new, METH_O, "Create a single-owner native parser context."},
    {"parser_clear", parser_clear, METH_O, "Release a parser's cached memory blocks."},
    {"parser_close", parser_close, METH_O, "Close a parser and release cached memory blocks."},
    {"parser_info", parser_info, METH_O, "Return cache limit, retained bytes, and closed state."},
    {"prepare_rules", prepare_rules, METH_O, "Compile immutable native rules."},
    {"parse_html", parse_html, METH_VARARGS, "Parse HTML with native intermediate storage."},
    {"parse_native", parse_native, METH_VARARGS, "Experimental: retain a native document."},
    {"native_rows", native_rows, METH_O, "Experimental: convert a native document to rows."},
    {"native_columns", native_columns, METH_O,
     "Experimental: convert a native document to columns."},
    {"native_len", native_len, METH_O, "Experimental: native document row count."},
    {"native_json", native_json, METH_O,
     "Experimental: serialize a native document to UTF-8 JSON."},
    {"tokenize", tokenize, METH_O, "Return tokens from the native scanner."},
    {"normalize", normalize, METH_VARARGS, "Normalize a Python token iterable."},
    {"html_instructions", html_instructions, METH_VARARGS, "Convert HTML directly to instruction rows."},
    {"standardize", standardize, METH_VARARGS, "Standardize Python instruction rows."},
    {NULL, NULL, 0, NULL}};
static int module_traverse(PyObject *module, visitproc visit, void *arg) {
    ModuleState *state = PyModule_GetState(module);
    size_t i;
    Py_VISIT(state->parser);
    for (i = 0; i < RK_COUNT; i++) Py_VISIT(state->run_keys[i]);
    for (i = 0; i < A_COUNT; i++) Py_VISIT(state->attribute_keys[i]);
    for (i = 0; i < ROW_KEY_COUNT; i++) Py_VISIT(state->row_keys[i]);
    for (i = 0; i < ROW_TYPE_COUNT; i++) Py_VISIT(state->row_types[i]);
    return 0;
}

static int module_clear(PyObject *module) {
    ModuleState *state = PyModule_GetState(module);
    size_t i;
    Py_CLEAR(state->parser);
    for (i = 0; i < RK_COUNT; i++) Py_CLEAR(state->run_keys[i]);
    for (i = 0; i < A_COUNT; i++) Py_CLEAR(state->attribute_keys[i]);
    for (i = 0; i < ROW_KEY_COUNT; i++) Py_CLEAR(state->row_keys[i]);
    for (i = 0; i < ROW_TYPE_COUNT; i++) Py_CLEAR(state->row_types[i]);
    return 0;
}

static void module_free(void *module) { module_clear(module); }

static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT, "_native",    "Native doc2struct engine.",
    sizeof(ModuleState),   methods,      NULL,
    module_traverse,       module_clear, module_free};
static int intern_names(PyObject **keys, const char *const *names, size_t count) {
    size_t i;
    for (i = 0; i < count; i++)
        if (!(keys[i] = PyUnicode_InternFromString(names[i])))
            return 0;
    return 1;
}
PyMODINIT_FUNC PyInit__native(void) {
    PyObject *result = PyModule_Create(&module);
    ModuleState *state;
    if (!result)
        return NULL;
    state = PyModule_GetState(result);
    state->parser = create_parser(DS_DEFAULT_CACHE_SIZE);
    if (!state->parser ||
        !intern_names(state->run_keys, run_names, RK_COUNT) ||
        !intern_names(state->attribute_keys, attribute_names, A_COUNT) ||
        !intern_names(state->row_keys, row_names, ROW_KEY_COUNT) ||
        !intern_names(state->row_types, type_names, ROW_TYPE_COUNT)) {
        Py_DECREF(result);
        return NULL;
    }
    return result;
}
