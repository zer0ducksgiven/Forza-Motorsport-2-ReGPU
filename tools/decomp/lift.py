"""Lifts game functions from Ghidra's decompiled C into C++ that builds into the port (out/lift, never committed).

  py -3 tools/decomp/lift.py <info.jsonl> [--keep out/lift_check.tsv[,more.tsv]] [--strict] [--out out/lift]

info.jsonl comes from ghidra/LiftInfoFm2.java (the C plus each parameter's register and each global's type).
Each function's C is parsed (pycparser) and written out against guest memory: loads and stores go through
lift::Load/Store (big-endian, 32-bit addresses), parameters come from and results go to the registers Ghidra
says, and calls go to the other functions through the register context, recompiled or lifted. Anything the lifter
doesn't understand rejects the function, which then stays recompiled: the reasons are counted in
out/lift/report.tsv.

Without --keep every lifted function goes in, to be checked: build with -DFM2_STORE_LOG=ON and run with
FM2_DECOMP_CHECK=1, which writes out/lift_check.tsv. With --keep, only functions that ran in the check and never
differed are written.
"""
import collections
import json
import os
import re
import struct
import sys

from pycparser import c_ast, c_parser

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMAGE_BASE = 0x82000000


class Reject(Exception):
    pass


# ---- types ---------------------------------------------------------------------------------------------------

class T:
    """kind: int (size, signed), bool, float (size), ptr (to), func, void, array (to, n), opaque (name, size)"""

    def __init__(self, kind, size=0, signed=False, to=None, n=0, name='', raw=False):
        self.kind, self.size, self.signed, self.to, self.n, self.name = kind, size, signed, to, n, name
        self.raw = raw  # Ghidra's undefinedN: bits of no known type, so casts to and from float reinterpret

    def cpp(self):
        if self.kind == 'int':
            return '%sint%d_t' % ('' if self.signed else 'u', self.size * 8)
        if self.kind == 'bool':
            return 'bool'
        if self.kind == 'float':
            return 'float' if self.size == 4 else 'double'
        if self.kind in ('ptr', 'array'):
            return 'uint32_t'
        raise Reject('type ' + self.kind)

    def bytes(self):
        if self.kind in ('int', 'float', 'opaque'):
            if not self.size:
                raise Reject('size of ' + (self.name or self.kind))
            return self.size
        if self.kind == 'bool':
            return 1
        if self.kind == 'ptr':
            return 4
        if self.kind == 'array':
            return self.to.bytes() * self.n
        raise Reject('size of ' + self.kind)

    def scalar(self):
        return self.kind in ('int', 'bool', 'float', 'ptr')


INT = {
    'undefined': (1, False), 'undefined1': (1, False), 'undefined2': (2, False), 'undefined4': (4, False),
    'undefined8': (8, False), 'byte': (1, False), 'uchar': (1, False), 'char': (1, True), 'sbyte': (1, True),
    'ushort': (2, False), 'short': (2, True), 'word': (2, False), 'uint': (4, False), 'int': (4, True),
    'dword': (4, False), 'longlong': (8, True), 'ulonglong': (8, False), 'qword': (8, False),
    'wchar16': (2, False), 'wchar_t': (2, False),
}
U64 = T('int', 8, False)
I32 = T('int', 4, True)
U32 = T('int', 4, False)


def named_type(names):
    n = ' '.join(names)
    if n in INT:
        return T('int', *INT[n], raw=n.startswith('undefined'))
    if n == 'bool':
        return T('bool')
    if n == 'float':
        return T('float', 4)
    if n == 'double':
        return T('float', 8)
    if n == 'void':
        return T('void')
    if n == 'code':
        return T('func')
    return T('opaque', name=n)


def type_of(node):
    if isinstance(node, c_ast.TypeDecl):
        return named_type(node.type.names)
    if isinstance(node, c_ast.PtrDecl):
        return T('ptr', to=type_of(node.type))
    if isinstance(node, c_ast.ArrayDecl):
        return T('array', to=type_of(node.type), n=int(node.dim.value, 0))
    if isinstance(node, c_ast.Typename):
        return type_of(node.type)
    if isinstance(node, c_ast.FuncDecl):
        return T('func')
    raise Reject('declarator ' + type(node).__name__)


def promote(a, b):
    """C's usual arithmetic conversions, for the result type of a binary operator"""
    if a.kind == 'float' or b.kind == 'float':
        return T('float', max(a.size if a.kind == 'float' else 0, b.size if b.kind == 'float' else 0))

    def rank(t):
        if t.kind == 'bool':
            return T('int', 4, True)
        if t.kind == 'int' and t.size < 4:
            return T('int', 4, True)
        return t
    a, b = rank(a), rank(b)
    if a.size != b.size:
        return a if a.size > b.size else b
    return a if not a.signed else b


def parse_type(name):
    """a type from its Ghidra display name ("int *", "float"), or None"""
    name = name.strip()
    stars = name.count('*')
    t = named_type(name.replace('*', '').split())
    if t.kind == 'opaque' and not stars:
        return None
    for _ in range(stars):
        t = T('ptr', to=t)
    return t


def reg_read(cls, num, t):
    """a register's value as type t: converted where the register holds that kind of value, else bit for bit"""
    if cls == 'r':
        if t.kind == 'float':
            return 'std::bit_cast<%s>(ctx.r%d.u%d)' % (t.cpp(), num, t.size * 8)
        if t.kind == 'bool':  # a byte: the rest of the register is whatever was there
            return 'bool(ctx.r%d.u8)' % num
        return '%s(ctx.r%d.u64)' % (t.cpp(), num)
    if t.kind == 'float':
        return '%s(ctx.f%d.f64)' % (t.cpp(), num)
    if t.kind == 'int' and t.size == 8:
        return '%s(ctx.f%d.u64)' % (t.cpp(), num)
    if t.kind == 'int' and t.size == 4:
        return '%s(std::bit_cast<uint32_t>(float(ctx.f%d.f64)))' % (t.cpp(), num)
    raise Reject('float register as ' + t.kind)


def reg_write(cls, num, t, code):
    if cls == 'r':
        if t.kind == 'float':
            return 'ctx.r%d.u64 = std::bit_cast<uint%d_t>(%s(%s));' % (num, t.size * 8, t.cpp(), code)
        ext = 'int64_t' if t.kind == 'int' and t.signed else 'uint64_t'
        return 'ctx.r%d.u64 = uint64_t(%s(%s));' % (num, ext, code)
    if t.kind == 'float':
        return 'ctx.f%d.f64 = double(%s);' % (num, code)
    if t.kind == 'int' and t.size == 8:
        return 'ctx.f%d.u64 = uint64_t(%s);' % (num, code)
    if t.kind == 'int' and t.size == 4:
        return 'ctx.f%d.f64 = double(std::bit_cast<float>(uint32_t(%s)));' % (num, code)
    raise Reject('float register from ' + t.kind)


ARG_WRITE = re.compile(r'^\s*ctx\.([rf])(\d+)\.\w+\s*=[^=]')
ARG_CALL = re.compile(r'\bsub_([0-9A-F]{8})\(ctx, base\)|PPC_CALL_INDIRECT_FUNC')


def arg_names(kind, n):
    return '%s%d' % (kind, n) if (kind == 'r' and 3 <= n <= 10) or (kind == 'f' and 1 <= n <= 13) else None


def call_setup(body):
    """{call target (an address, or 'indirect'): the argument registers the original writes before calling it}"""
    out = {}
    written = set()
    for line in body.split('\n'):
        w = ARG_WRITE.match(line)
        if w:
            r = arg_names(w.group(1), int(w.group(2)))
            if r:
                written.add(r)
        m = ARG_CALL.search(line)
        if m:
            target = int(m.group(1), 16) if m.group(1) else 'indirect'
            out.setdefault(target, set()).update(written)
            written = set()
    return out


# ---- the function --------------------------------------------------------------------------------------------

REG = re.compile(r'^(r|f)(\d+):(\d+)$')
REG_INPUT = re.compile(r'^(in|extraout|unaff)_([rf])(\d+)(_\d+)?$')


class Lifter:
    def __init__(self, info, names, rets, frame, known, sigs, setup, livein):
        self.setup = setup
        self.livein = livein
        self.known = known
        self.sigs = sigs
        self.info = info
        self.names = names  # sanitized function name -> address
        self.rets = rets  # address -> (storage, type name, size) of the return
        self.frame = frame
        self.vars = {}  # C name -> (T, 'host' | 'guest', stack offset)
        self.globals = {}
        for g in info['globals']:
            m = re.search(r'([0-9a-fA-F]{8})', g['storage']) or re.search(r'_([0-9a-fA-F]{8})$', g['name'])
            if m:
                self.globals[sanitize(g['name'])] = (int(m.group(1), 16), g)
        self.tmp = 0
        self.nblocks = 0
        self.calls = False
        self.addressed = set()
        self.wide = {}  # integer parameter never assigned -> its whole register as it came in
        self.extended = set()  # registers the original extends from 32 bits itself

    def fresh(self):
        self.tmp += 1
        return '_t%d' % self.tmp

    # values: ('r', code, T) rvalue, ('var', name, T) host variable, ('mem', addr code, T) guest memory
    def rv(self, n):
        v = self.ex(n)
        if v[0] == 'r':
            return v[1], v[2]
        if v[0] == 'fn':  # a function as a value: its address
            return '0x%08Xu' % v[1], T('ptr', to=T('func'))
        t = v[2]
        if t.kind == 'array':
            if v[0] == 'mem':
                return v[1], T('ptr', to=t.to)
            raise Reject('host array')
        if t.kind == 'func':
            raise Reject('function value')
        if v[0] == 'var':
            return v[1], t
        if not t.scalar():
            raise Reject('load of ' + t.kind)
        if t.kind == 'bool':
            return 'bool(lift::Load<uint8_t>(base, %s))' % v[1], t
        return 'lift::Load<%s>(base, %s)' % (t.cpp(), v[1]), t

    def ex(self, n):
        if isinstance(n, c_ast.Constant):
            if n.type == 'string':
                raise Reject('string literal')
            if n.type in ('float', 'double'):
                return ('r', n.value, T('float', 8 if not n.value.lower().endswith('f') else 4))
            if n.type == 'char':
                return ('r', n.value, T('int', 4, True))
            v = n.value.rstrip('uUlL')
            val = int(v, 0)
            u = 'u' in n.value.lower() or (v.lower().startswith('0x') and val >= 0x80000000)
            if val >= 1 << 32 or 'll' in n.value.lower():
                t = T('int', 8, val >= 1 << 63 or 'u' in n.value.lower())
                if t.signed is False and val < 1 << 63 and not ('u' in n.value.lower()):
                    t = T('int', 8, True)
                return ('r', '%s(%sull)' % (t.cpp(), hex(val)), t)
            t = T('int', 4, not u)
            return ('r', '%s(%su)' % (t.cpp(), hex(val)), t)
        if isinstance(n, c_ast.ID):
            name = n.name
            if name in self.vars:
                t, where, off = self.vars[name]
                if where == 'reg':
                    return ('r', reg_read(off[0], off[1], t), t)
                if where == 'guest':
                    return ('mem', 'uint32_t(sp + %d)' % off, t)
                return ('var', 'v_' + name, t)
            if name in self.names:
                return ('fn', self.names[name], T('func'))
            if name in self.globals and self.globals[name][0] in self.known:
                # a function Ghidra lists as a global (an undefined byte at its address): used as a value, its address
                return ('fn', self.globals[name][0], T('func'))
            if name in self.globals:
                addr, g = self.globals[name]
                t = self.gtype(g)
                return ('mem', '0x%08Xu' % addr, t)
            if name in ('true', 'false'):
                return ('r', name, T('bool'))
            raise Reject('unknown name ' + name)
        if isinstance(n, c_ast.Cast):
            t = type_of(n.to_type)
            if t.kind == 'void':
                code, _ = self.rv(n.expr)
                return ('r', '(void)(%s)' % code, t)
            if t.kind == 'float' and isinstance(n.expr, c_ast.FuncCall):
                # (float)call(): what's in f1 is the float, whatever integer type Ghidra gave the callee's return
                code, st = self.call(n.expr, None, as_float=True)[1:]
                return ('r', '%s(%s)' % (t.cpp(), code), t)
            if t.kind == 'int' and t.size == 8 and isinstance(n.expr, c_ast.ID):
                # a 32-bit parameter used as 64 bits: the original's 64-bit instruction reads the whole register,
                # whose top half Ghidra doesn't know (it sign-extended a pointer in 8237E0D8)
                w = self.wide.get(n.expr.name)
                v = self.vars.get(n.expr.name)
                if w is None and v and v[1] == 'reg' and v[2][0] == 'r' and v[2][1] not in self.extended:
                    w = 'ctx.r%d.u64' % self.vars[n.expr.name][2][1]
                if w is not None:
                    return ('r', '%s(%s)' % (t.cpp(), w), t)
            v = self.ex(n.expr)
            if v[0] == 'fn':
                return ('r', '0x%08Xu' % v[1], t)
            code, st = self.rv(n.expr)
            if not t.scalar():
                raise Reject('cast to ' + t.kind)
            if st.kind == 'float' and t.kind == 'ptr':
                raise Reject('float to pointer')
            if (t.kind == 'float' and st.kind == 'int' and st.raw) or (st.kind == 'float' and t.kind == 'int' and t.raw):
                if t.size != st.size:
                    raise Reject('reinterpret between sizes')
                return ('r', 'std::bit_cast<%s>(%s(%s))' % (t.cpp(), st.cpp(), code), t)
            return ('r', '%s(%s)' % (t.cpp(), code), t)
        if isinstance(n, c_ast.UnaryOp):
            return self.unary(n)
        if isinstance(n, c_ast.BinaryOp):
            return self.binary(n)
        if isinstance(n, c_ast.Assignment):
            return ('r',) + self.assign(n, False)
        if isinstance(n, c_ast.ArrayRef):
            base_code, bt = self.rv(n.name)
            if bt.kind != 'ptr':
                raise Reject('index of ' + bt.kind)
            ic, it = self.rv(n.subscript)
            return ('mem', 'uint32_t(%s + int64_t(%s) * %d)' % (base_code, ic, bt.to.bytes()), bt.to)
        if isinstance(n, c_ast.TernaryOp):
            c, _ = self.rv(n.cond)
            a, at = self.rv(n.iftrue)
            b, bt = self.rv(n.iffalse)
            t = at if at.kind == 'ptr' else promote(at, bt) if at.kind != bt.kind or at.size != bt.size else at
            return ('r', '((%s) ? %s(%s) : %s(%s))' % (c, t.cpp(), a, t.cpp(), b), t)
        if isinstance(n, c_ast.FuncCall):
            return self.call(n, None)
        if isinstance(n, c_ast.StructRef):
            # Ghidra's piece of a value: x._4_4_ is bytes 4-7 of it in memory order (big-endian: the low half of 8)
            m = re.fullmatch(r'_(\d+)_(\d+)_', n.field.name) if n.type == '.' else None
            if not m:
                raise Reject('expression StructRef')
            code, t = self.rv(n.name)
            off, size = int(m.group(1)), int(m.group(2))
            if t.kind not in ('int', 'ptr') or size not in (1, 2, 4) or off + size > t.bytes():
                raise Reject('piece of ' + t.kind)
            rt = T('int', size, False)
            return ('r', '%s(uint64_t(%s) >> %d)' % (rt.cpp(), code, 8 * (t.bytes() - off - size)), rt)
        if isinstance(n, c_ast.ExprList):
            parts = [self.rv(e) for e in n.exprs]
            return ('r', '(' + ', '.join(p[0] for p in parts) + ')', parts[-1][1])
        raise Reject('expression ' + type(n).__name__)

    def gtype(self, g):
        t = g['type'].strip()
        stars = t.count('*')
        base_name = t.replace('*', '').strip()
        if '[' in base_name or base_name in ('string', 'TerminatedCString', 'unicode', 'TerminatedUnicode'):
            # an array (a string): used as a value, its address
            return T('array', to=T('int', 1, True), n=max(1, g['size']))
        bt = named_type(base_name.split())
        if bt.kind == 'opaque':
            bt.size = g['size'] if not stars else 0
        for _ in range(stars):
            bt = T('ptr', to=bt)
        return bt

    def unary(self, n):
        op = n.op
        if op == '*':
            v = self.ex(n.expr)
            if v[0] == 'fn':
                return v
            code, t = self.rv(n.expr)
            if t.kind != 'ptr':
                raise Reject('deref of ' + t.kind)
            if t.to.kind == 'func':
                return ('fptr', code, t.to)
            return ('mem', code, t.to)
        if op == '&':
            v = self.ex(n.expr)
            if v[0] == 'mem':
                return ('r', 'uint32_t(%s)' % v[1], T('ptr', to=v[2]))
            if v[0] == 'fn':
                return ('r', '0x%08Xu' % v[1], T('ptr', to=T('func')))
            raise Reject('address of a host variable')
        if op == 'sizeof':
            raise Reject('sizeof')
        if op in ('p++', 'p--', '++', '--'):
            v = self.ex(n.expr)
            d = '+' if '+' in op else '-'
            post = op.startswith('p')
            t = v[2]
            step = str(t.to.bytes()) if t.kind == 'ptr' else '1'
            if v[0] == 'var':
                if t.kind == 'ptr':
                    if post:
                        tmp = self.fresh()
                        return ('r', '({ uint32_t %s = %s; %s = uint32_t(%s %s %s); %s; })' % (tmp, v[1], v[1], v[1], d, step, tmp), t)
                    return ('r', '(%s = uint32_t(%s %s %s))' % (v[1], v[1], d, step), t)
                return ('r', ('%s%s' % (v[1], d * 2)) if post else ('%s%s' % (d * 2, v[1])), t)
            if v[0] == 'mem':
                a, o, w = self.fresh(), self.fresh(), self.fresh()
                return ('r', '({ uint32_t %s = %s; %s %s = lift::Load<%s>(base, %s); %s %s = %s(%s %s %s); lift::Store<%s>(base, %s, %s); %s; })' % (
                    a, v[1], t.cpp(), o, t.cpp(), a, t.cpp(), w, t.cpp(), o, d, step, t.cpp(), a, w, o if post else w), t)
            raise Reject('increment of ' + v[0])
        code, t = self.rv(n.expr)
        if op == '!':
            return ('r', '(!(%s))' % code, T('int', 4, True))
        if op in ('-', '~', '+'):
            rt = promote(t, t) if t.kind != 'ptr' else U32
            return ('r', '(%s%s(%s))' % (op, rt.cpp(), code), rt)
        raise Reject('unary ' + op)

    def binary(self, n):
        op = n.op
        a, at = self.rv(n.left)
        b, bt = self.rv(n.right)
        if op in ('&&', '||'):
            return ('r', '(%s %s %s)' % (a, op, b), T('int', 4, True))
        if op in ('==', '!=', '<', '>', '<=', '>='):
            if at.kind == 'ptr' or bt.kind == 'ptr':
                return ('r', '(uint32_t(%s) %s uint32_t(%s))' % (a, op, b), T('int', 4, True))
            return ('r', '(%s %s %s)' % (a, op, b), T('int', 4, True))
        if op in ('+', '-') and at.kind == 'ptr' and bt.kind == 'ptr':
            if op == '+':
                raise Reject('pointer + pointer')
            return ('r', '(int32_t(%s - %s) / %d)' % (a, b, at.to.bytes()), I32)
        if op in ('+', '-') and at.kind == 'ptr':
            return ('r', 'uint32_t(%s %s int64_t(%s) * %d)' % (a, op, b, at.to.bytes()), at)
        if op == '+' and bt.kind == 'ptr':
            return ('r', 'uint32_t(%s + int64_t(%s) * %d)' % (b, a, bt.to.bytes()), bt)
        if at.kind == 'ptr':
            a, at = 'uint32_t(%s)' % a, U32
        if bt.kind == 'ptr':
            b, bt = 'uint32_t(%s)' % b, U32
        if op in ('<<', '>>'):
            rt = promote(at, at)
            return ('r', '(%s %s %s)' % (a, op, b), rt)
        if op in ('+', '-', '*', '/', '%', '&', '|', '^'):
            return ('r', '(%s %s %s)' % (a, op, b), promote(at, bt))
        raise Reject('binary ' + op)

    def assign(self, n, statement):
        """(code, T) of an assignment"""
        lhs = self.ex(n.lvalue)
        t = lhs[2]
        if lhs[0] not in ('var', 'mem') or not t.scalar():  # (registers read in place are 'r')
            raise Reject('assignment to ' + lhs[0] + ' ' + t.kind)
        if isinstance(n.rvalue, c_ast.FuncCall):
            rc, rt = self.call(n.rvalue, t)[1:]
        else:
            rc, rt = self.rv(n.rvalue)
        if (t.kind == 'int' and t.raw and rt.kind == 'float') or (t.kind == 'float' and rt.kind == 'int' and rt.raw):
            # Ghidra's undefinedN holds bits: a float stored into one (828E1A00's double into an undefined8) keeps them
            if t.size != rt.size:
                raise Reject('reinterpret between sizes')
            rc, rt = 'std::bit_cast<%s>(%s(%s))' % (t.cpp(), rt.cpp(), rc), t
        if n.op != '=':
            op = n.op[:-1]
            if t.kind == 'ptr' and op in ('+', '-'):
                rc = '(%s) * %d' % (rc, t.to.bytes())
            cur = lhs[1] if lhs[0] == 'var' else None
            if lhs[0] == 'var':
                return '(%s = %s(%s %s (%s)))' % (lhs[1], t.cpp(), lhs[1], op, rc), t
            a = self.fresh()
            w = self.fresh()
            return '({ uint32_t %s = %s; %s %s = %s(lift::Load<%s>(base, %s) %s (%s)); lift::Store<%s>(base, %s, %s); %s; })' % (
                a, lhs[1], t.cpp(), w, t.cpp(), t.cpp(), a, op, rc, t.cpp(), a, w, w), t
        if lhs[0] == 'var':
            return '(%s = %s(%s))' % (lhs[1], t.cpp(), rc), t
        if statement:
            return 'lift::Store<%s>(base, %s, %s(%s))' % (t.cpp(), lhs[1], t.cpp(), rc), t
        a, w = self.fresh(), self.fresh()
        return '({ uint32_t %s = %s; %s %s = %s(%s); lift::Store<%s>(base, %s, %s); %s; })' % (
            a, lhs[1], t.cpp(), w, t.cpp(), rc, t.cpp(), a, w, w), t

    def builtin(self, n):
        """Ghidra's p-code helpers in its C, or None if this isn't one"""
        if not isinstance(n.name, c_ast.ID):
            return None
        name = n.name.name
        args = n.args.exprs if n.args else []
        m = re.fullmatch(r'(ZEXT|SEXT)(\d)(\d+)', name)
        if m:
            code, t = self.rv(args[0])
            src, dst = int(m.group(2)), int(m.group(3))
            if dst > 8 or src > 8:
                raise Reject('vector-sized ' + name)
            signed = m.group(1) == 'SEXT'
            st, dt = T('int', src, signed), T('int', dst, signed)
            return ('r', '%s(%s(%s))' % (dt.cpp(), st.cpp(), code), dt)
        m = re.fullmatch(r'CONCAT(\d)(\d)', name)
        if m:
            hi, lo = int(m.group(1)), int(m.group(2))
            if hi + lo not in (2, 4, 8) or hi not in (1, 2, 4) or lo not in (1, 2, 4):
                raise Reject('odd-sized ' + name)
            a, b = self.rv(args[0])[0], self.rv(args[1])[0]
            t = T('int', hi + lo, False)
            return ('r', '%s((%s(uint%d_t(%s)) << %d) | uint%d_t(%s))' % (t.cpp(), t.cpp(), hi * 8, a, lo * 8, lo * 8, b), t)
        m = re.fullmatch(r'SUB(\d)(\d)', name)
        if m:
            size = int(m.group(2))
            if size not in (1, 2, 4, 8):
                raise Reject('odd-sized ' + name)
            code, t = self.rv(args[0])
            k = int(self.rv(args[1])[0].split('(')[-1].rstrip('u)'), 16) if 'x' in self.rv(args[1])[0] else None
            if k is None:
                raise Reject('SUB with a variable offset')
            src = T('int', int(m.group(1)), False)
            rt = T('int', size, False)
            return ('r', '%s(%s(%s) >> %d)' % (rt.cpp(), src.cpp(), code, 8 * k), rt)
        if name == 'trapWord':
            return ('r', '(void)0', T('void'))  # XenonRecomp doesn't trap either (twi 31,r0,22 marks the unreachable)
        if name in ('LZCOUNT', 'POPCOUNT'):
            code, t = self.rv(args[0])
            if t.kind != 'int':
                raise Reject(name + ' of ' + t.kind)
            if name == 'POPCOUNT':
                return ('r', 'int32_t(lift::Popcount(%s(%s)))' % (t.cpp(), code), I32)
            f = 'lift::Lzcount64' if t.size == 8 else 'lift::Lzcount32'
            if t.size < 4:
                raise Reject('LZCOUNT of a small type')
            return ('r', 'int32_t(%s(%s(%s)))' % (f, t.cpp(), code), I32)
        if name == 'ABS':
            code, t = self.rv(args[0])
            if t.kind == 'float':
                return ('r', '%s(__builtin_fabs(%s))' % (t.cpp(), code), t)
            return ('r', '%s(%s < 0 ? -%s : %s)' % (t.cpp(), code, code, code), t)
        if name == 'SQRT':
            code, t = self.rv(args[0])
            return ('r', 'double(__builtin_sqrt(double(%s)))' % code, T('float', 8))
        return None

    def call(self, n, want, as_float=False):
        """a call: ('r', code, T); want = the type the result is assigned to, if it is"""
        b = self.builtin(n)
        if b is not None:
            return b
        self.calls = True
        target = self.ex(n.name)
        if target[0] == 'fn':
            if target[1] not in self.known:
                raise Reject('call to an import or non-function %X' % target[1])
            how = 'sub_%08X(ctx, base)' % target[1]
            ret = self.rets.get(target[1])
        elif target[0] == 'fptr':
            how = 'PPC_CALL_INDIRECT_FUNC(%s)' % target[1]
            ret = None
        else:
            raise Reject('call through ' + target[0])
        sets = []
        ri, fi = 3, 1
        sig = self.sigs.get(target[1]) if target[0] == 'fn' else None
        for i, arg in enumerate(n.args.exprs if n.args else []):
            v = self.ex(arg)
            if v[0] == 'fn':
                code, t = '0x%08Xu' % v[1], U32
            else:
                code, t = self.rv(arg)
            m = REG.match(sig[i]) if sig and i < len(sig) else None
            if m:  # where the callee reads it
                sets.append(reg_write(m.group(1), int(m.group(2)), t, code))
                # arguments past what the callee's signature lists go in the registers after these
                if m.group(1) == 'r':
                    ri = max(ri, int(m.group(2)) + 1)
                else:
                    fi = max(fi, int(m.group(2)) + 1)
            elif t.kind == 'float':
                if fi > 13:
                    raise Reject('too many float arguments')
                sets.append(reg_write('f', fi, t, code))
                fi += 1
            else:
                if ri > 10:
                    raise Reject('too many arguments')
                sets.append(reg_write('r', ri, t, code))
                ri += 1
        # what the original passes: registers it sets up before the call that the callee reads (livein.py); Ghidra's
        # C must pass them all (it dropped 82297C38's third and fourth, and 824EBB68's this)
        key = target[1] if target[0] == 'fn' else 'indirect'
        reads = set(self.livein.get('%08X' % key, ['r%d' % i for i in range(3, 11)] + ['f%d' % i for i in range(1, 14)])) \
            if key != 'indirect' else {'r%d' % i for i in range(3, 11)} | {'f%d' % i for i in range(1, 14)}
        passed = set(re.findall(r'ctx\.([rf]\d+)\.', ' '.join(sets)))
        if (self.setup.get(key, set()) & reads) - passed:
            raise Reject('call drops an argument the original passes')
        frame_in = 'ctx.r1.u32 = uint32_t(sp - %d); ' % self.frame if self.frame else ''
        frame_out = ' ctx.r1.u32 = sp;' if self.frame else ''
        body = '%s %s%s;%s' % (' '.join(sets), frame_in, how, frame_out)
        rt, cls = None, 'r'
        if ret and ret[0] != '<VOID>':
            m = REG.match(ret[0])
            if not m:
                raise Reject('callee return in ' + ret[0])
            cls = m.group(1)
            rt = T('float', 8) if as_float and cls == 'f' else parse_type(ret[1])
            if rt is None or not rt.scalar():
                rt = T('float', int(m.group(3))) if cls == 'f' else T('int', int(m.group(3)), False)
        elif want is not None:
            rt = want
            cls = 'f' if want.kind == 'float' else 'r'
        if rt is None:
            return ('r', '({ %s })' % body, T('void'))
        return ('r', '({ %s %s; })' % (body, reg_read(cls, 1 if cls == 'f' else 3, rt)), rt)

    # ---- statements

    def st(self, n, out, ind):
        p = '    ' * ind
        if isinstance(n, c_ast.Compound):
            for s in n.block_items or []:
                self.st(s, out, ind)
        elif isinstance(n, c_ast.Decl):
            raise Reject('declaration inside the body')
        elif isinstance(n, c_ast.Assignment):
            out.append(p + self.assign(n, True)[0] + ';')
        elif isinstance(n, c_ast.FuncCall):
            out.append(p + self.call(n, None)[1] + ';')
        elif isinstance(n, (c_ast.UnaryOp, c_ast.BinaryOp, c_ast.Cast, c_ast.ID, c_ast.ExprList, c_ast.TernaryOp)):
            out.append(p + '(void)(' + self.rv(n)[0] + ');')
        elif isinstance(n, c_ast.Return):
            self.ret_stmt(n, out, p)
        elif isinstance(n, c_ast.If):
            out.append(p + 'if (%s) {' % self.rv(n.cond)[0])
            out.append(p + '    ' + self.block())
            self.st(n.iftrue, out, ind + 1)
            out.append(p + '} else {')
            out.append(p + '    ' + self.block())
            if n.iffalse:
                self.st(n.iffalse, out, ind + 1)
            out.append(p + '}')
        elif isinstance(n, c_ast.While):
            out.append(p + 'while (%s) {' % self.rv(n.cond)[0])
            out.append(p + '    ' + self.block())
            self.st(n.stmt, out, ind + 1)
            out.append(p + '}')
        elif isinstance(n, c_ast.DoWhile):
            out.append(p + 'do {')
            self.st(n.stmt, out, ind + 1)
            out.append(p + '} while (%s);' % self.rv(n.cond)[0])
        elif isinstance(n, c_ast.For):
            init = self.rv(n.init)[0] if n.init and not isinstance(n.init, c_ast.DeclList) else ''
            if isinstance(n.init, c_ast.Assignment):
                init = self.assign(n.init, False)[0]
            cond = self.rv(n.cond)[0] if n.cond else ''
            nxt = (self.assign(n.next, False)[0] if isinstance(n.next, c_ast.Assignment) else self.rv(n.next)[0]) if n.next else ''
            out.append(p + 'for (%s; %s; %s) {' % (init, cond, nxt))
            out.append(p + '    ' + self.block())
            self.st(n.stmt, out, ind + 1)
            out.append(p + '}')
        elif isinstance(n, c_ast.Goto):
            out.append(p + 'goto %s;' % n.name)
        elif isinstance(n, c_ast.Label):
            out.append('%s:;' % n.name)
            out.append(p + self.block())
            self.st(n.stmt, out, ind)
        elif isinstance(n, c_ast.Break):
            out.append(p + 'break;')
        elif isinstance(n, c_ast.Continue):
            out.append(p + 'continue;')
        elif isinstance(n, c_ast.Switch):
            out.append(p + 'switch (%s) {' % self.rv(n.cond)[0])
            self.st(n.stmt, out, ind + 1)
            out.append(p + '}')
        elif isinstance(n, c_ast.Case):
            out.append(p + 'case %s:' % self.rv(n.expr)[0])
            out.append(p + '    ' + self.block())
            for s in n.stmts or []:
                self.st(s, out, ind + 1)
        elif isinstance(n, c_ast.Default):
            out.append(p + 'default:')
            out.append(p + '    ' + self.block())
            for s in n.stmts or []:
                self.st(s, out, ind + 1)
        elif isinstance(n, c_ast.EmptyStatement):
            pass
        else:
            raise Reject('statement ' + type(n).__name__)

    def block(self):
        """a branch's marker: a check records which ran, and lift.py keeps a function only once all have (lift.h)"""
        if self.nblocks >= 64:
            raise Reject('more than 64 branches')
        self.nblocks += 1
        return 'LIFT_BLOCK(%d);' % (self.nblocks - 1)

    def ret_stmt(self, n, out, p):
        r = self.info['ret']
        if n.expr is None or r['storage'] == '<VOID>':
            if n.expr is not None:
                out.append(p + '(void)(%s);' % self.rv(n.expr)[0])
            out.append(p + 'return;')
            return
        m = REG.match(r['storage'])
        if not m:
            raise Reject('return in ' + r['storage'])
        if isinstance(n.expr, c_ast.FuncCall):
            code, t = self.call(n.expr, self.rtype)[1:]
        else:
            code, t = self.rv(n.expr)
        out.append(p + reg_write(m.group(1), int(m.group(2)), self.rtype, '%s(%s)' % (self.rtype.cpp(), code)) + ' return;')

    def lift(self, fn):
        decl = fn.decl.type
        params = {p['name']: p for p in self.info['params']}
        locals_ = {l['name']: l for l in self.info['locals']}
        r = self.info['ret']
        if r['storage'] != '<VOID>':
            self.rtype = type_of(decl.type)
            if not self.rtype.scalar():
                raise Reject('return type ' + self.rtype.kind)
        self.ret_kind = 'LiftReturn::None' if r['storage'] == '<VOID>' else 'LiftReturn::Float' if r['storage'].startswith('f') else 'LiftReturn::Int'

        # which variables have their address taken (they live in guest memory, on the stack)
        class Addr(c_ast.NodeVisitor):
            def __init__(s):
                s.names = set()

            def visit_UnaryOp(s, u):
                if u.op == '&' and isinstance(u.expr, c_ast.ID):
                    s.names.add(u.expr.name)
                s.generic_visit(u)
        av = Addr()
        av.visit(fn.body)

        class Assigned(c_ast.NodeVisitor):
            def __init__(s):
                s.names = set()

            def visit_Assignment(s, a):
                if isinstance(a.lvalue, c_ast.ID):
                    s.names.add(a.lvalue.name)
                s.generic_visit(a)

            def visit_UnaryOp(s, u):
                if u.op in ('++', '--', 'p++', 'p--') and isinstance(u.expr, c_ast.ID):
                    s.names.add(u.expr.name)
                s.generic_visit(u)
        assigned = Assigned()
        assigned.visit(fn.body)

        head = []
        for p in (decl.args.params if decl.args else []):
            if isinstance(p, c_ast.EllipsisParam):
                raise Reject('varargs')
            if isinstance(p, c_ast.Typename):  # (void)
                continue
            t = type_of(p.type)
            info = params.get(p.name)
            if not info:
                raise Reject('parameter without storage')
            m = REG.match(info['storage'])
            if not m:
                raise Reject('parameter in ' + info['storage'])
            if p.name in av.names:
                raise Reject('address of a parameter')
            self.vars[p.name] = (t, 'host', 0)
            if not t.scalar():
                raise Reject('parameter type ' + t.kind)
            head.append('    %s v_%s = %s;' % (t.cpp(), p.name, reg_read(m.group(1), int(m.group(2)), t)))
            if m.group(1) == 'r' and t.kind in ('int', 'ptr') and p.name not in assigned.names and int(m.group(2)) not in self.extended:
                head.append('    const uint64_t w_%s = ctx.r%s.u64; (void)w_%s;' % (p.name, m.group(2), p.name))
                self.wide[p.name] = 'w_' + p.name
        body = fn.body.block_items or []
        stmts = []
        for s in body:
            if isinstance(s, c_ast.Decl):
                t = type_of(s.type)
                info = locals_.get(s.name)
                storage = info['storage'] if info else ''
                m = REG_INPUT.match(s.name)
                if m:
                    # a register Ghidra didn't see set up (garbage passed on, a callee's second result, a saved
                    # register): its value where it's used, which the lifted code's same calls leave the same
                    if not t.scalar():
                        raise Reject('register input type')
                    self.vars[s.name] = (t, 'reg', (m.group(2), int(m.group(3))))
                    continue
                if s.name.startswith(('in_', 'unaff_', 'extraout_')):
                    raise Reject('register input')
                if t.kind == 'array' or s.name in av.names:
                    m = re.match(r'Stack\[(-?0x[0-9a-fA-F]+|-?\d+)\]', storage)
                    if not m:
                        raise Reject('addressed local not on the stack')
                    self.vars[s.name] = (t, 'guest', int(m.group(1), 0))
                    self.addressed.add(s.name)
                else:
                    if not t.scalar():
                        raise Reject('local type ' + t.kind)
                    self.vars[s.name] = (t, 'host', 0)
                    head.append('    %s v_%s{};' % (t.cpp(), s.name))
                if s.init:
                    raise Reject('initialised declaration')
            else:
                stmts.append(s)
        out = []
        for s in stmts:
            self.st(s, out, 1)
        if self.addressed and self.calls and os.environ.get('LIFT_STACK_CALLS') != '1':
            # a callee can write a local through its address, and Ghidra's C doesn't always allow for that (it folded
            # a local's constant through such a call in 826209B0)
            raise Reject('stack locals and calls')
        lines = ['static void lifted_%s(PPCContext& __restrict ctx, uint8_t* base)' % self.info['addr'], '{',
                 '    const uint32_t sp = ctx.r1.u32; (void)sp;'] + head + out + ['}']
        return '\n'.join(lines)


# ---- input --------------------------------------------------------------------------------------------------

HEADER = ''.join('typedef int %s;\n' % n for n in list(INT) + ['bool', 'code', 'float10', 'undefined3', 'undefined5',
                                                                'undefined6', 'undefined7', 'undefined16', 'uint3',
                                                                'int3', 'uint5', 'uint6', 'uint7'] if n not in ('char', 'short', 'int'))
QUALIFIED = re.compile(r'[A-Za-z_~][\w~]*(?:::[A-Za-z_~][\w~]*)+')


# Ghidra's warnings that don't make its C wrong: the register save/restore helpers it's told to treat as nothing
# (ghidra/ImportFm2Symbols.java), dead blocks dropped, switch tables we supplied, overlapping global names, and a
# tail jump through a pointer written as a call (which it is, then a return), and parameters we set ourselves
# (ghidra/ApplyParamsFm2.java, as custom storage).
HARMLESS = re.compile(r'replaced with injection: fm2_saverest|Removing unreachable block|Switch is manually overridden'
                      r'|overlap smaller symbols|Treating indirect jump as call|Could not recover jumptable'
                      r'|Unknown calling convention')


def sanitize(name):
    return re.sub(r'[^\w]', '_', name.replace('::~', '__dtor_').replace('::', '__'))


def prepare(c, names_by_raw):
    """Ghidra's C -> (parseable C, sanitized name -> address for the qualified names it uses)"""
    c = re.sub(r'/\*.*?\*/', '', c, flags=re.S)
    found = {}

    def repl(m):
        s = sanitize(m.group(0))
        if m.group(0) in names_by_raw:
            found[s] = names_by_raw[m.group(0)]
        return s
    c = QUALIFIED.sub(repl, c)
    return c, found


def main(argv):
    args = argv[1:]
    if not args:
        print(__doc__)
        return 1
    info_path = args.pop(0)
    keep = None
    follow = True
    out_dir = os.path.join(ROOT, 'out', 'lift')
    while args:
        a = args.pop(0)
        if a == '--keep':
            # one check file or several (comma separated, say from different routes): kept if it never differed, (void
            # by Ghidra) left r3 as the original did, as callers may use it, and every branch ran in a passing check.
            # Calls a check couldn't judge (raced: another thread changed what the lifted run read; unchecked: it
            # reached an import or a hook) count neither way, so at least one call has to have been judged (and passed):
            # three functions without branches whose every call reached an import were kept unchecked, and crashed boot
            keep = {}
            failed, covered, branches, judged = set(), collections.Counter(), {}, collections.Counter()
            for path in args.pop(0).split(','):
                for line in open(path):
                    f = line.rstrip('\n').split('\t')
                    if f[0] == 'address' or int(f[1]) == 0:
                        continue
                    if int(f[2]) or int(f[3]):
                        failed.add(f[0])
                    keep[f[0]] = int(f[1])
                    judged[f[0]] += int(f[1]) - (int(f[8]) + int(f[9]) if len(f) > 9 else 0)
                    if len(f) > 6:
                        branches[f[0]] = int(f[5])
                        covered[f[0]] |= int(f[6], 16)
            for a in list(keep):
                if a in failed or covered[a] != (1 << branches.get(a, 0)) - 1 or judged[a] <= 0:
                    del keep[a]
        elif a == '--strict':
            follow = False
        elif a == '--out':
            out_dir = args.pop(0)

    symbols = json.load(open(os.path.join(ROOT, 'out', 'decomp', 'symbols.json')))
    livein = json.load(open(os.path.join(ROOT, 'out', 'decomp', 'livein.json')))  # livein.py
    # the registers besides r3 and f1 that callers read after each function returns (livein.py), for the check to
    # compare: bits r3-r10 0-7, f1-f13 8-20, as LiftStats::outs
    liveout = {}
    for a, regs in json.load(open(os.path.join(ROOT, 'out', 'decomp', 'liveout.json'))).items():
        bits = 0
        for r in regs:
            n = int(r[1:])
            if r[0] == 'r' and 4 <= n <= 10:
                bits |= 1 << (n - 3)
            elif r[0] == 'f' and 2 <= n <= 13:
                bits |= 1 << (8 + n - 1)
        liveout[int(a, 16)] = bits
    img = open(os.path.join(ROOT, 'out', 'tu4', 'image.bin'), 'rb').read()
    funcs = [json.loads(l) for l in open(info_path, encoding='utf-8')]

    # names Ghidra prints for functions: FUN_xxxxxxxx, and the symbol names (unique ones only)
    count = collections.Counter(s['name'] for s in symbols.values())
    names_by_raw = {s['name']: int(a, 16) for a, s in symbols.items() if count[s['name']] == 1}
    rets = {int(f['addr'], 16): (f['ret']['storage'], f['ret']['type'], f['ret']['size']) for f in funcs if 'ret' in f}
    sigs = {int(f['addr'], 16): [p['storage'] for p in f['params']] for f in funcs if 'params' in f}

    # functions not to lift: those the port already replaces, and those with stores the check can't see
    taken = set()  # replaced in src/decomp (they run their originals inside a check)
    for d, _, files in os.walk(os.path.join(ROOT, 'src')):
        for f in files:
            if f.endswith('.cpp'):
                t = open(os.path.join(d, f), encoding='utf-8', errors='replace').read()
                taken |= {int(a, 16) for a in re.findall(r'TU_SUB\(([0-9A-F]{8})\)', t)}
    hooked = set()  # replaced by the port's own C++ (their stores aren't logged)
    passthrough = set()  # hooks that run their original inside a check (FM2_SHADOW_ORIGINAL): checks follow them
    for d in ('build/cov/remapped', 'build/tu/remapped'):
        for dd, _, files in os.walk(os.path.join(ROOT, d)):
            for f in files:
                if f.endswith('.cpp'):
                    t = open(os.path.join(dd, f), encoding='utf-8', errors='replace').read()
                    hooked |= {int(a, 16) for a in re.findall(r'PPC_FUNC\(sub_([0-9A-F]{8})\)', t)}
                    passthrough |= {int(a, 16) for a in re.findall(r'PPC_FUNC\(sub_([0-9A-F]{8})\)\s*\{\s*FM2_SHADOW_ORIGINAL', t)}
    for d in ('build/cov/generated', 'build/tu/generated'):
        for dd, _, files in os.walk(os.path.join(ROOT, d)):
            for f in files:
                if f.endswith('.cpp'):
                    t = open(os.path.join(dd, f), encoding='utf-8', errors='replace').read()
                    taken |= {int(a, 16) for a in re.findall(r'PPC_FUNC\(sub_([0-9A-F]{8})\)', t)}
    taken |= hooked
    # A check runs the lifted version against an overlay, so everything it calls has to keep to loads and stores
    # that go through it (tools/patch_mem.py routes the vector and block ones there, and setjmp/longjmp, which may
    # only jump within the run), and no hooks (the port's C++). Imports and calls through pointers are caught as they happen (the run is abandoned and
    # the function isn't kept); --strict leaves out functions that reach them at all.
    calls, impure, setups, extended = {}, set(), {}, {}
    ppc = os.path.join(ROOT, 'out', 'ppc_tu4')
    for f in os.listdir(ppc):
        if f.startswith('ppc_recomp.') and f.endswith('.cpp'):
            t = open(os.path.join(ppc, f), encoding='utf-8', errors='replace').read()
            for m in re.finditer(r'PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\) \{(.*?)\n\}', t, re.S):
                a, body = int(m.group(1), 16), m.group(2)
                calls[a] = {int(c, 16) for c in re.findall(r'\bsub_([0-9A-F]{8})\(ctx, base\)', body)}
                setups[a] = call_setup(body)
                # registers the original sign- or zero-extends from 32 bits: where Ghidra's C casts one of those to
                # 64 bits, the cast is that extension (8263E068's extsw), not a read of the register's top half
                extended[a] = {int(r) for r in re.findall(r'// (?:extsw|clrldi|rldicl) r\d+,r(\d+)', body)}
                follows = not follow and re.search(r'PPC_CALL_INDIRECT_FUNC|__imp__(?!sub_)\w+\(ctx, base\)', body)
                # (vector stores, lwarx/stwcx. and dcbz go through the overlay too: tools/patch_mem.py)
                if re.search(r'setjmp|longjmp|simde_mm_store\w*\(\(\w+\*\)\(base|[^_]memset\(base|memcpy\(base', body) or (a in hooked and a not in passthrough) or follows:
                    impure.add(a)
    changed = True
    while changed:
        changed = False
        for a, cs in calls.items():
            if a not in impure and cs & impure:
                impure.add(a)
                changed = True
    unlogged = impure

    known = {int(a, 16) for a in re.findall(r'\{ 0x([0-9A-F]+), sub_', open(os.path.join(ppc, 'ppc_func_mapping.cpp')).read())}
    parser = c_parser.CParser()
    reasons = collections.Counter()
    why = {}
    done = []
    trace = open(os.environ['LIFT_TRACE'], 'w') if os.environ.get('LIFT_TRACE') else None  # the function being lifted
    for f in funcs:
        addr = int(f['addr'], 16)
        if trace:
            trace.write(f['addr'] + '\n')
            trace.flush()
        if keep is not None and f['addr'] not in keep:
            continue
        try:
            if 'error' in f:
                raise Reject('decompile failed')
            if addr in taken:
                raise Reject('already replaced')
            if addr in unlogged:
                raise Reject('hooks in its calls')
            c = f['c']
            if len(c) > 150000:  # 82750DE0, the 74 KB D3D function: too big for this lifter to finish
                raise Reject('too big')
            if 'halt_baddata' in c:
                raise Reject('Ghidra warning: bad instruction data')
            for w in re.findall(r'WARNING: ([^*]*)\*/', c):
                if not HARMLESS.search(w):
                    raise Reject('Ghidra warning: ' + re.sub(r'0x[0-9a-f]+|\d+', 'N', w.strip())[:40])
            src, found = prepare(c, names_by_raw)
            names = {'FUN_%08x' % a: a for a in []}
            for m in re.finditer(r'\b(?:FUN|thunk_FUN)_([0-9a-f]{8})\b', src):
                names[m.group(0)] = int(m.group(1), 16)
            for ident in set(re.findall(r'\b[A-Za-z_]\w*\b', src)):
                if ident in names_by_raw:
                    names[ident] = names_by_raw[ident]
            names.update(found)
            # opaque type names in casts and declarations
            extra = set(re.findall(r'\(\s*(\w+)\s*\*+\s*\)', src)) | set(re.findall(r'^\s*(\w+)\s+\**\w+\s*(?:\[\w+\])?\s*;', src, re.M))
            builtin = set(INT) | {'bool', 'code', 'void', 'float', 'double', 'char', 'short', 'int', 'return', 'goto'}
            hdr = HEADER + ''.join('typedef int %s;\n' % e for e in sorted(extra - builtin) if not e.isdigit())
            # the function's own name may be qualified; give it a plain one
            try:
                ast = parser.parse(hdr + src)
            except Exception as e:
                raise Reject('parse')
            fn = [d for d in ast.ext if isinstance(d, c_ast.FuncDef)]
            if len(fn) != 1:
                raise Reject('parse')
            o = struct.unpack_from('>16I', img, addr - IMAGE_BASE)
            frame = 0
            for w in o:
                if w >> 16 == 0x9421:
                    frame = -(w & 0xFFFF) + 0x10000 if w & 0x8000 else 0
                    break
            lifter = Lifter(f, names, rets, frame, known, sigs, setups.get(addr, {}), livein)
            lifter.extended = extended.get(addr, set())
            code = lifter.lift(fn[0])
            done.append((addr, code, lifter.ret_kind, lifter.nblocks))
            reasons['lifted'] += 1
        except Reject as e:
            r = str(e)
            reasons['unknown name' if r.startswith('unknown name') else r[:60]] += 1
            why[f['addr']] = r
        except RecursionError:
            reasons['too deep'] += 1

    os.makedirs(out_dir, exist_ok=True)
    for old in os.listdir(out_dir):
        if old.endswith('.cpp'):
            os.remove(os.path.join(out_dir, old))
    per = 400
    for i in range(0, len(done), per):
        with open(os.path.join(out_dir, 'lift_%03d.cpp' % (i // per)), 'w', encoding='utf-8') as out:
            out.write('// Lifted by tools/decomp/lift.py from Ghidra\'s C: the game\'s own code, keep out of the repository.\n')
            out.write('#include <bit>\n\n#include "decomp/lift.h"\n#include "ppc_recomp_shared.h"\n\n')
            if i == 0:
                # what a check's lifted run can't follow a call through a pointer into: hooks, vector and block
                # stores, and whatever calls those
                out.write('static const uint32_t kUnfollowable[] = { %s };\n' % ', '.join('0x%08X' % h for h in sorted(impure)))
                out.write('static const bool kListed = (LiftRegisterHooked(kUnfollowable, %d), true);\n\n' % len(impure))
            for addr, code, rk, nb in done[i:i + per]:
                out.write(code + '\n')
                out.write('PPC_FUNC_IMPL(__imp__sub_%08X);\n' % addr)
                out.write('PPC_FUNC(sub_%08X)\n{\n    static LiftStats s(0x%08X, %s, %d, 0x%X);\n    LiftRun(s, ctx, base, __imp__sub_%08X, lifted_%08X);\n}\n\n'
                          % (addr, addr, rk, nb, liveout.get(addr, 0), addr, addr))
    with open(os.path.join(out_dir, 'reasons.tsv'), 'w') as f:  # each function left out, and why
        for a, r in sorted(why.items()):
            f.write('%s\t%s\n' % (a, r))
    with open(os.path.join(out_dir, 'report.tsv'), 'w') as rep:
        for r, n in reasons.most_common():
            rep.write('%d\t%s\n' % (n, r))
    print('lifted %d of %d (%s)' % (reasons['lifted'], len(funcs) if keep is None else len(keep),
                                     ', '.join('%s %d' % (r, n) for r, n in reasons.most_common(12) if r != 'lifted')))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
