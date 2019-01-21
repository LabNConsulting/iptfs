/*
 * Copyright (c) 2014 by Christian E. Hopps.
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/socket.h>
//#include "Modules/socketmodule.h"

static char module_docstring[] =
    "This module provides python2 and python3 compatible efficient bytestring functions.";

/*
 * FUNCTION: bchr
 *
 *      return bytestring for an integer
 */

static char bstr_bchr_docstring[] =
    "Return a byte string for a single integer value";

static PyObject *
bstr_bchr (PyObject *self, PyObject *args)
{
    int ival;
    char cval;

    /* Parse the input tuple */
    if (!PyArg_ParseTuple(args, "i", &ival))
        return NULL;

    cval = ival;
    if (ival < 0 || ival > 255) {
        PyErr_SetString(PyExc_ValueError,
                        "bchr requires value between 0 and 255");
        return NULL;
    }
#if PY_MAJOR_VERSION >= 3
    return PyBytes_FromStringAndSize(&cval, 1);
#else
    return PyString_FromStringAndSize(&cval, 1);
#endif
}

static char bstr_memspan_docstring[] =
    "Return the span between a containing memoryview and one within it";

static PyObject *
bstr_memspan (PyObject *self, PyObject *args)
{
    PyObject *rv;
    Py_buffer before, after;
    long lval, aptr, bptr, asz, bsz;

    rv = NULL;

    /* Parse the input tuple */
    if (!PyArg_ParseTuple(args, "y*y*:memspan", &before, &after))
        return NULL;

    aptr = (long)after.buf;
    asz = (long)after.len;
    bptr = (long)before.buf;
    bsz = (long)before.len;
    /* Do checking to make sure abuf.buf is withing bbuf.buf or vice versa */
    if (!((aptr >= bptr && aptr + asz <= bptr + bsz) ||
          (bptr >= aptr && bptr + bsz <= aptr + asz))) {
        PyErr_SetString(PyExc_ValueError,
                        "One argument not contained by the other");
        goto out;
    }

    /* Get the pointer difference */
    lval = aptr - bptr;

#if PY_MAJOR_VERSION >= 3
    rv = PyLong_FromLong(lval);
#else
    rv = PyInt_FromLong(lval);
#endif
out:
    PyBuffer_Release(&after);
    PyBuffer_Release(&before);

    return rv;
}

static void
init_iov (PyObject **bufobj, Py_buffer *iovbuf)
{
    memset(bufobj, 0, sizeof(*bufobj) * (IOV_MAX + 1));
}

static int
fill_iov (PyObject *seq, int *niovp, PyObject **bufobj, Py_buffer *iovbuf, struct iovec *iov)
{
    PyObject *iter = NULL;
    int rv = -1;
    int n = 0;

    *niovp = 0;

    if ((iter = PyObject_GetIter(seq)) == NULL)
        goto out;

    while ((bufobj[n] = PyIter_Next(iter)) != NULL) {
        if (n == IOV_MAX) {
            PyErr_SetString(PyExc_IndexError,
                            "Number of input buffers exceeds IOV_MAX");
            goto out;
        }
        if (PyObject_GetBuffer(bufobj[n], &iovbuf[n], PyBUF_SIMPLE) != 0)
            goto out;

        iov[n].iov_base = iovbuf[n].buf;
        iov[n].iov_len = iovbuf[n].len;
        n += 1;

        *niovp = n;
    }

    rv = 0;
out:
    if (iter)
        Py_DECREF(iter);
    return rv;
}

static void
release_iov (int niov, PyObject **bufobj, Py_buffer *iovbuf)
{
    int i;

    for (i = 0; i < niov; i++) {
        PyBuffer_Release(&iovbuf[i]);
    }
    for (i = 0; i < IOV_MAX; i++) {
        if (bufobj[i])
            Py_DECREF(bufobj[i]);
    }
}


static char bstr_sendv_docstring[] =
    "Send out a sequence of buffers to a socket";

static PyObject *
bstr_sendv (PyObject *self, PyObject *args)
{
    struct msghdr msg;
    struct iovec iov[IOV_MAX + 1];
    Py_buffer iovbuf[IOV_MAX + 1];
    PyObject *bufobj[IOV_MAX + 1];
    PyObject *fdobj, *seq, *rv;
    Py_ssize_t total;
    int fd, niov;

    init_iov(bufobj, iovbuf);

    rv = NULL;
    total = 0;
    niov = 0;

    /* Parse the input tuple */
    if (!PyArg_ParseTuple(args, "OO:sendv", &fdobj, &seq))
        goto out;
    if ((fd = PyObject_AsFileDescriptor(fdobj)) == -1)
        goto out;
    if (fill_iov(seq, &niov, bufobj, iovbuf, iov) == -1)
        goto out;

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = niov;
    Py_BEGIN_ALLOW_THREADS
    total = sendmsg(fd, &msg, 0);
    Py_END_ALLOW_THREADS

    if (total == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        goto out;
    }

#if PY_MAJOR_VERSION >= 3
    rv = PyLong_FromSsize_t(total);
#else
    rv = PyInt_FromSsize_t(total);
#endif
out:
    release_iov(niov, bufobj, iovbuf);
    return rv;
}

#define MIN(x, y) ((x) <= (y) ? (x) : (y))

static char bstr_read_docstring[] =
    "Fill a buffer with bytes from a file descriptor";

static PyObject *
bstr_read (PyObject *self, PyObject *args)
{
    PyObject *fdobj, *rv;
    Py_buffer buf;
    Py_ssize_t n;
    int fd;

    rv = NULL;

    /* Parse the input tuple */
    /* XXX could use "Ow*|n" but then need default value for n */
    if (!PyArg_ParseTuple(args, "Oy*n:read", &fdobj, &buf, &n))
        return NULL;
    if ((fd = PyObject_AsFileDescriptor(fdobj)) == -1)
        goto out;

    /* read from the file descriptor */
    Py_BEGIN_ALLOW_THREADS
    n = MIN(n, buf.len);
    // fprintf(stderr, "read fd: %d buf %p len %ld\n", fd, buf.buf, (long)n);

    n = read(fd, buf.buf, n);
    Py_END_ALLOW_THREADS

#if PY_MAJOR_VERSION >= 3
    rv = PyLong_FromSsize_t(n);
#else
    rv = PyInt_FromSsize_t(n);
#endif
out:
    PyBuffer_Release(&buf);

    return rv;
}

static char bstr_recv_docstring[] =
    "Fill a buffer with bytes from a file descriptor";

static PyObject *
bstr_recv (PyObject *self, PyObject *args)
{
    PyObject *fdobj, *rv;
    Py_buffer buf;
    Py_ssize_t n;
    int fd;

    rv = NULL;

    /* Parse the input tuple */
    /* XXX could use "Ow*|n" but then need default value for n */
    if (!PyArg_ParseTuple(args, "Ow*n:recv", &fdobj, &buf, &n))
        return NULL;
    if ((fd = PyObject_AsFileDescriptor(fdobj)) == -1)
        goto out;

    /* recv from the file descriptor */
    Py_BEGIN_ALLOW_THREADS
    n = recv(fd, buf.buf, MIN(n, buf.len), MSG_WAITALL);
    Py_END_ALLOW_THREADS

    if (n == -1) {
        fprintf(stderr, "recv error: %d %s\n", errno, strerror(errno));
        PyErr_SetFromErrno(PyExc_OSError);
        goto out;
    }

#if PY_MAJOR_VERSION >= 3
    rv = PyLong_FromSsize_t(n);
#else
    rv = PyInt_FromSsize_t(n);
#endif
out:
    PyBuffer_Release(&buf);

    return rv;
}

/* static PyObject * */
/* makesockaddr(struct sockaddr *addr, size_t addlen) */
/* { */
/*     PyObject *addrobj; */
/*     cosnt struct sockaddr_in *in = (const struct sockaddr_in *)addr; */
/*     cosnt struct sockaddr_in *in6 = (const struct sockaddr_in6 *)addr; */
/*     PyObject *ret = NULL; */

/*     if (addrlen == 0) { */
/*         /\* No address -- may be recvfrom() from known socket *\/ */
/*         Py_RETURN_NONE; */
/*     } */

/*     switch (addr->sa_family) { */

/*     case AF_INET: */
/*     { */
/*         addrobj = make_ipv4_addr((const struct sockaddr_in *)addr); */
/*         if (addrobj) { */
/*             ret = Py_BuildValue("Oi", addrobj, ntohs(a->sin_port)); */
/*             Py_DECREF(addrobj); */
/*         } */
/*         return ret; */
/*     } */
/*     case AF_INET6: */
/*     { */
/*         PyObject *addrobj = make_ipv6_addr(a); */
/*         if (addrobj) { */
/*             ret = Py_BuildValue("OiII", */
/*                                 addrobj, */
/*                                 ntohs(a->sin6_port), */
/*                                 ntohl(a->sin6_flowinfo), */
/*                                 a->sin6_scope_id); */
/*             Py_DECREF(addrobj); */
/*         } */
/*         return ret; */
/*     } */
/*     default: */
/*         /\* If we don't know the address family, don't raise an */
/*            exception -- return it as an (int, bytes) tuple. *\/ */
/*         return Py_BuildValue("iy#", */
/*                              addr->sa_family, */
/*                              addr->sa_data, */
/*                              sizeof(addr->sa_data)); */

/*     } */


/* static char bstr_recvfrom_docstring[] = */
/*     "Fill a buffer with bytes from a file descriptor"; */

/* static PyObject * */
/* bstr_recvfrom (PyObject *self, PyObject *args) */
/* { */
/*     PySocketSockObject *s; */
/*     PyObject *rv; */
/*     Py_buffer buf; */
/*     Py_ssize_t n; */
/*     int fd, flags; */

/*     sock_addr_t addrbuf; */
/*     socklen_t addrlen; */

/*     rv = NULL; */

/*     /\* Parse the input tuple *\/ */
/*     /\* R/W Buffer, length to read, flags *\/ */
/*     /\* XXX could use "Ow*|n" but then need default value for n *\/ */
/*     if (!PyArg_ParseTuple(args, "O!w*ni:recvfrom", PySocketModule.Sock_Type, &s, &buf, &n, &flags)) */
/*         return NULL; */

/*     if (!getsockaddrlen(s, &addrlen)) */
/*         goto out; */

/*     /\* recv from the file descriptor *\/ */
/*     { */
/*         Py_BEGIN_ALLOW_THREADS */
/*             n = recvfrom(s.sock_fd, buf.buf, MIN(n, buf.len), MSG_WAITALL|flags); */
/*         Py_END_ALLOW_THREADS */
/*     } */

/*     if (n == -1) { */
/*         fprintf(stderr, "recv error: %d %s\n", errno, strerror(errno)); */
/*         /\* XXX throw an exception *\/ */
/*     } */

/*     rv = Py_BuildValue("nN", n, makesockaddr(s->sock_fd, SAS2SA(&addrbuf), addrlen, s->sock_proto)); */
/* out: */
/*     PyBuffer_Release(&buf); */

/*     return rv; */
/* } */


static char bstr_writev_docstring[] =
    "Write out a sequence of buffers to a socket";

static PyObject *
bstr_writev (PyObject *self, PyObject *args)
{
    struct iovec iov[IOV_MAX + 1];
    Py_buffer iovbuf[IOV_MAX + 1];
    PyObject *bufobj[IOV_MAX + 1];
    PyObject *fdobj, *seq, *rv;
    Py_ssize_t total;
    int fd, niov;

    init_iov(bufobj, iovbuf);

    rv = NULL;
    total = 0;
    niov = 0;

    /* Parse the input tuple */
    if (!PyArg_ParseTuple(args, "OO:writev", &fdobj, &seq))
        goto out;
    if ((fd = PyObject_AsFileDescriptor(fdobj)) == -1)
        goto out;
    if (fill_iov(seq, &niov, bufobj, iovbuf, iov) == -1)
        goto out;

    Py_BEGIN_ALLOW_THREADS
    total = writev(fd, iov, niov);
    Py_END_ALLOW_THREADS

    if (total == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        goto out;
    }

#if PY_MAJOR_VERSION >= 3
    rv = PyLong_FromSsize_t(total);
#else
    rv = PyInt_FromSsize_t(total);
#endif
out:
    release_iov(niov, bufobj, iovbuf);
    return rv;
}

static char bstr_write_docstring[] =
    "Write out a buffer to a socket";

static PyObject *
bstr_write (PyObject *self, PyObject *args)
{
    // const char *cbuf;
    Py_buffer buf;
    // PyByteArrayObject *baobj;
    PyObject *fdobj, *rv;
    Py_ssize_t n;
    int fd;

    rv = NULL;

    /* Parse the input tuple */
    if (!PyArg_ParseTuple(args, "Oy*:write", &fdobj, &buf))
        goto out;
    if ((fd = PyObject_AsFileDescriptor(fdobj)) == -1)
        goto out;
    Py_BEGIN_ALLOW_THREADS
        // n = write(fd, baobj->ob_start, n);
        // n = buf.len;
        // fprintf(stderr, "write fd: %d buf %p len %ld\n", fd, buf.buf, (long)n);
        n = write(fd, buf.buf, buf.len);
    Py_END_ALLOW_THREADS

        if (n == -1) {
            PyErr_SetFromErrno(PyExc_OSError);
            goto out;
        }

#if PY_MAJOR_VERSION >= 3
    rv = PyLong_FromSsize_t(n);
#else
    rv = PyInt_FromSsize_t(n);
#endif
out:
    PyBuffer_Release(&buf);
    return rv;
}


/*
 * Initialize the module
 */

static PyMethodDef module_methods[] = {
    { "bchr", bstr_bchr, METH_VARARGS, bstr_bchr_docstring },
    { "memspan", bstr_memspan, METH_VARARGS, bstr_memspan_docstring },
    { "read", bstr_read, METH_VARARGS, bstr_read_docstring },
    { "recv", bstr_recv, METH_VARARGS, bstr_recv_docstring },
//    { "recvfrom", bstr_recvfrom, METH_VARARGS, bstr_recvfrom_docstring },
    { "sendv", bstr_sendv, METH_VARARGS, bstr_sendv_docstring },
    { "write", bstr_write, METH_VARARGS, bstr_write_docstring },
    { "writev", bstr_writev, METH_VARARGS, bstr_writev_docstring },
    { NULL, NULL, 0, NULL },
};

// int PySocketModule_ImportModuleAndAPI(void);

#if PY_MAJOR_VERSION >= 3
PyMODINIT_FUNC
PyInit_bstr(void)
{
    PyObject *m;
    static struct PyModuleDef moduledef = {
        PyModuleDef_HEAD_INIT, "bstr", module_docstring, -1, module_methods, };

    /* if (PySocketModule_ImportModuleAndAPI()) */
    /*     return NULL; */

    if ((m = PyModule_Create(&moduledef))) {
        PyModule_AddIntConstant(m, "IOV_MAX", IOV_MAX);
    }

    return m;
}
#else
PyMODINIT_FUNC
initbstr(void)
{
    PyObject *m;
    if ((m = Py_InitModule3("bstr", module_methods, module_docstring))) {
        PyModule_AddIntConstant(m, "IOV_MAX", IOV_MAX);
    }
}
#endif

/* Local Variables: */
/* mode: c */
/* flycheck-gcc-include-path: "/usr/include/python3.4" */
/* End: */
