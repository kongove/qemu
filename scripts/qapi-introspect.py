#
# QAPI introspection info generator
#
# Copyright (C) 2014 Red Hat, Inc.
#
# Authors:
#  Amos Kong <akong@redhat.com>
#
# This work is licensed under the terms of the GNU GPLv2.
# See the COPYING.LIB file in the top-level directory.

from ordereddict import OrderedDict
from qapi import *
import sys
import os
import getopt
import errno


try:
    opts, args = getopt.gnu_getopt(sys.argv[1:], "hp:o:",
                                   ["header", "prefix=", "output-dir="])
except getopt.GetoptError, err:
    print str(err)
    sys.exit(1)

output_dir = ""
prefix = ""
h_file = 'qapi-introspect.h'

do_h = False

for o, a in opts:
    if o in ("-p", "--prefix"):
        prefix = a
    elif o in ("-o", "--output-dir"):
        output_dir = a + "/"
    elif o in ("-h", "--header"):
        do_h = True

h_file = output_dir + prefix + h_file

try:
    os.makedirs(output_dir)
except os.error, e:
    if e.errno != errno.EEXIST:
        raise

def maybe_open(really, name, opt):
    if really:
        return open(name, opt)
    else:
        import StringIO
        return StringIO.StringIO()

fdecl = maybe_open(do_h, h_file, 'w')

fdecl.write(mcgen('''
/* AUTOMATICALLY GENERATED, DO NOT MODIFY */

/*
 * Head file to store parsed information of QAPI schema
 *
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * Authors:
 *  Amos Kong <akong@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#ifndef %(guard)s
#define %(guard)s

''',
                  guard=guardname(h_file)))

def extend_schema(expr, parents=[], member=True):
    ret = {}
    recu = 'False'
    name = ""

    if type(expr) is OrderedDict:
        if not member:
            e = expr.popitem(last=False)
            typ = e[0]
            name = e[1]
        else:
            typ = "anonymous-struct"

        if typ == 'enum':
            for key in expr.keys():
                ret[key] = expr[key]
        else:
            ret = {}
            for key in expr.keys():
                ret[key], parents = extend_schema(expr[key], parents)

    elif type(expr) is list:
        typ = 'anonymous-struct'
        ret = []
        for i in expr:
            tmp, parents = extend_schema(i, parents)
            ret.append(tmp)
    elif type(expr) is str:
        name = expr
        if schema_dict.has_key(expr) and expr not in parents:
            parents.append(expr)
            typ = schema_dict[expr][1]
            recu = 'True'
            ret, parents = extend_schema(schema_dict[expr][0].copy(),
                                         parents, False)
            parents.remove(expr)
            ret['_obj_recursive'] = 'True'
            return ret, parents
        else:
            return expr, parents

    return {'_obj_member': "%s" % member, '_obj_type': typ,
            '_obj_name': name, '_obj_recursive': recu,
            '_obj_data': ret}, parents


exprs = parse_schema(sys.stdin)
schema_dict = {}

for expr in exprs:
    if expr.has_key('type') or expr.has_key('enum') or expr.has_key('union'):
        e = expr.copy()

        first = e.popitem(last=False)
        schema_dict[first[1]] = [expr.copy(), first[0]]

fdecl.write('''const char *const qmp_schema_table[] = {
''')

def convert(odict):
    d = {}
    for k, v in odict.items():
        if type(v) is OrderedDict:
            d[k] = convert(v)
        elif type(v) is list:
            l = []
            for j in v:
                if type(j) is OrderedDict:
                    l.append(convert(j))
                else:
                    l.append(j)
            d[k] = l
        else:
            d[k] = v
    return d

count = 0
for expr in exprs:
    fdecl.write('''    /* %s */
''' % expr)

    expr, parents = extend_schema(expr, [], False)
    fdecl.write('''    "%s",

''' % convert(expr))

fdecl.write('''    NULL };

#endif
''')

fdecl.flush()
fdecl.close()
