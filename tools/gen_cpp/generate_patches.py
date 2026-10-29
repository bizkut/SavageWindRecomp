#!/usr/bin/env python3
"""Translate the restricted patches.py DSL into the C++ patch implementation."""
import ast
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
source = (ROOT / 'patches.py').read_text()
tree = ast.parse(source)
functions = {n.name: n for n in tree.body if isinstance(n, ast.FunctionDef)}
table = next(n.value for n in tree.body if isinstance(n, ast.Assign)
             and any(isinstance(t, ast.Name) and t.id == 'PATCHES' for t in n.targets))


def quote(s):
    import json
    return json.dumps(s, ensure_ascii=True)

def string_expr(s):
    chunks = [s[i:i + 55] for i in range(0, len(s), 55)] or ['']
    return 'std::string(' + ('\n            '.join(quote(c) for c in chunks)) + ')'



def expr(n):
    if isinstance(n, ast.Constant):
        if isinstance(n.value, str):
            return string_expr(n.value)
        return str(n.value)
    if isinstance(n, ast.Name):
        return n.id
    if isinstance(n, ast.Subscript):
        if isinstance(n.value, ast.Attribute) and n.value.attr == 'sites':
            return 'ctx.sites.at(' + expr(n.slice) + ')'
        if isinstance(n.value, ast.Constant) and n.value.value == 'XY':
            return 'std::string(1, "XY"[' + expr(n.slice) + '])'
        raise ValueError(ast.dump(n))
    if isinstance(n, ast.Call):
        name = n.func.id
        if name == 'next':
            expected = ('next((_word(gen, a - 4 * i) for i in range(1, 5) '
                        'if _word(gen, a - 4 * i) & 4290774015 == 4181721704))')
            if ast.unparse(n) != expected:
                raise ValueError('unsupported next expression: ' + ast.unparse(n))
            return 'previous_aim_word(ctx, a)'
        args = [expr(x) for x in n.args if not (isinstance(x, ast.Name) and x.id == 'gen')]
        if name == '_word':
            return 'word(ctx, ' + ', '.join(args) + ')'
        if name == '_fn':
            return 'patch_fn(ctx, ' + ', '.join(args) + ')'
        if name == 'adrp_page':
            return 'adrp_page(' + ', '.join(args) + ')'
        if name == 'sext':
            return 'sext(' + ', '.join(args) + ')'
        if name == '_sub_sp':
            return '_sub_sp(' + ', '.join(args) + ')'
        if name in functions:
            return name + '(ctx, ' + ', '.join(args) + ')'
        raise ValueError(ast.dump(n))
    if isinstance(n, ast.JoinedStr):
        parts = []
        for item in n.values:
            if isinstance(item, ast.Constant):
                parts.append(string_expr(item.value))
            elif isinstance(item, ast.FormattedValue):
                value = expr(item.value)
                fmt = item.format_spec
                spec = ''.join(x.value for x in fmt.values) if fmt else ''
                if spec == '#x':
                    parts.append('hex(' + value + ')')
                elif spec == 'x':
                    parts.append('hex(' + value + ').substr(2)')
                elif not spec:
                    if isinstance(item.value, (ast.Call, ast.Subscript)) and (
                            isinstance(item.value, ast.Call) and item.value.func.id in ('_fn', '_quit_dialog',
                                                                                       '_reopen_options') or
                            isinstance(item.value, ast.Subscript)):
                        parts.append(value)
                    elif isinstance(item.value, ast.Name) and item.value.id in ('req', 'op', 'owner', 'fn',
                                                                                'close', 'redraw'):
                        parts.append(value)
                    else:
                        parts.append('std::to_string(' + value + ')')
                else:
                    raise ValueError(spec)
        return '(' + ' +\n        '.join(parts) + ')'
    if isinstance(n, ast.BinOp):
        ops = {ast.Add: '+', ast.Sub: '-', ast.Mult: '*', ast.BitAnd: '&', ast.BitOr: '|', ast.LShift: '<<',
               ast.RShift: '>>'}
        return '(' + expr(n.left) + ' ' + ops[type(n.op)] + ' ' + expr(n.right) + ')'
    if isinstance(n, ast.UnaryOp) and isinstance(n.op, ast.USub):
        return '(-' + expr(n.operand) + ')'
    if isinstance(n, ast.Compare):
        ops = {ast.Eq: '==', ast.NotEq: '!='}
        return '(' + expr(n.left) + ' ' + ops[type(n.ops[0])] + ' ' + expr(n.comparators[0]) + ')'
    if isinstance(n, ast.BoolOp):
        op = ' && ' if isinstance(n.op, ast.And) else ' || '
        return '(' + op.join(expr(x) for x in n.values) + ')'
    if isinstance(n, ast.IfExp):
        return '(' + expr(n.test) + ' ? ' + expr(n.body) + ' : ' + expr(n.orelse) + ')'
    raise ValueError(ast.dump(n))


def statements(body):
    lines = []
    for n in body:
        if isinstance(n, ast.Assert):
            lines.append('    validate(' + expr(n.test) + ', a, word(ctx, a));')
        elif isinstance(n, ast.Assign):
            target = n.targets[0]
            if isinstance(target, ast.Tuple):
                if isinstance(n.value, ast.Name) and n.value.id == 'regs':
                    continue
                for t, v in zip(target.elts, n.value.elts):
                    lines.append('    auto ' + t.id + ' = ' + expr(v) + ';')
            else:
                lines.append('    auto ' + target.id + ' = ' + expr(n.value) + ';')
        elif isinstance(n, ast.Return):
            lines.append('    return ' + expr(n.value) + ';')
        elif isinstance(n, ast.Expr) and isinstance(n.value, ast.Constant):
            continue
        else:
            raise ValueError(ast.dump(n))
    return lines


def generate():
    out = ['// Generated by generate_patches.py from ../patches.py; do not edit.', 'namespace generated_patches {']
    out.extend(['static uint64_t _sub_sp(uint64_t n)', '{',
                '    return ' + expr(functions['_sub_sp'].body[0].value) + ';', '}'])
    skip = {'_word', '_fn', '_sub_sp', '_frustum_load', '_cam_reverse', 'table'}
    for name, node in functions.items():
        if name in skip:
            continue
        args = ['const PatchContext &ctx']
        for arg in node.args.args:
            if arg.arg == 'gen':
                continue
            args.append(('const std::string & ' if arg.arg == 'owner' else 'uint64_t ') + arg.arg)
        out.append('static std::string ' + name + '(' + ', '.join(args) + ')')
        out.append('{')
        out.extend(statements(node.body))
        out.append('}')
    for outer, args in (('_frustum_load', 'uint64_t expected, uint64_t r0, uint64_t r1, const std::string &what'),
                        ('_cam_reverse', 'uint64_t axis')):
        node = functions[outer].body[0]
        out.append('static std::string ' + outer + '(const PatchContext &ctx, uint64_t a,')
        out.append('                                 ' + args + ')')
        out.append('{')
        for line in statements(node.body):
            out.append(line.replace('== word)', '== expected)'))
        out.append('}')
    out.append('static std::string apply(const PatchContext &ctx, const std::string &name, uint64_t a)')
    out.append('{')
    for key, val in zip(table.keys, table.values):
        name = key.value
        if isinstance(val, ast.Name):
            call = val.id + '(ctx, a)'
        else:
            if val.func.id == '_frustum_load':
                word, regs, what = val.args
                values = [expr(word), *(expr(x) for x in regs.elts), expr(what)]
                call = '_frustum_load(ctx, a, ' + ', '.join(values) + ')'
            else:
                call = '_cam_reverse(ctx, a, ' + expr(val.args[0]) + ')'
        out.append('    if (name == ' + quote(name) + ')')
        out.append('        return ' + call + ';')
    out.append('    throw std::runtime_error("unknown patch: " + name);')
    out.append('}')
    out.append('static const char *const names[] = {')
    out.extend('    ' + quote(k.value) + ',' for k in table.keys)
    out.extend(['};', '}'])
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    text = generate()
    if len(sys.argv) == 2 and sys.argv[1] == '--check':
        if (pathlib.Path(__file__).with_name('patches_generated.h')).read_text() != text:
            sys.exit('patches_generated.h diverges from patches.py; run generate_patches.py')
    else:
        pathlib.Path(__file__).with_name('patches_generated.h').write_text(text)
