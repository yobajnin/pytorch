import re
import yaml

try:
    # use faster C loader if available
    from yaml import CLoader as Loader
except ImportError:
    from yaml import Loader


def parse_default(s):
    if s.lower() == 'true':
        return True
    elif s.lower() == 'false':
        return False
    elif s == 'nullptr':
        return s
    elif s == '{}':
        return '{}'
    try:
        return int(s)
    except Exception:
        return float(s)


def sanitize_types(typ):
    # split tuples into constituent list
    if typ[0] == '(' and typ[-1] == ')':
        return [x.strip() for x in typ[1:-1].split(',')]
    elif typ == 'Generator*':
        return ['Generator *']
    return [typ]


def parse_arguments(args, func):
    arguments = []
    python_default_inits = func.get('python_default_init', {})

    # TODO: Use a real parser here; this will get bamboozled
    # by signatures that contain things like std::array<bool, 2> (note the space)
    for arg in args.split(', '):
        t, name = [a.strip() for a in arg.rsplit(' ', 1)]
        default = None
        python_default_init = None

        if '=' in name:
            ns = name.split('=', 1)
            name, default = ns[0], parse_default(ns[1])

        if name in python_default_inits:
            assert default is None
            python_default_init = python_default_inits[name]

        typ = sanitize_types(t)
        assert len(typ) == 1
        argument_dict = {'type': typ[0].rstrip('?'), 'name': name, 'is_nullable': typ[0].endswith('?')}
        match = re.match(r'IntList\[(\d+)\]', argument_dict['type'])
        if match:
            argument_dict['type'] = 'IntList'
            argument_dict['size'] = int(match.group(1))
        if default is not None:
            argument_dict['default'] = default
        if python_default_init is not None:
            argument_dict['python_default_init'] = python_default_init

        arguments.append(argument_dict)
    return arguments


def parse_native_yaml(path):
    with open(path, 'r') as f:
        return yaml.load(f, Loader=Loader)


def run(paths):
    declarations = []
    for path in paths:
        for func in parse_native_yaml(path):
            declaration = {'mode': 'native'}
            if '->' in func['func']:
                func_decl, return_type = [x.strip() for x in func['func'].split('->')]
                return_type = sanitize_types(return_type)
            else:
                func_decl = func['func']
                return_type = None
            fn_name, arguments = func_decl.split('(')
            arguments = arguments.split(')')[0]
            declaration['name'] = func.get('name', fn_name)
            declaration['return'] = list(func.get('return', return_type))
            declaration['variants'] = func.get('variants', ['method', 'function'])
            declaration['arguments'] = func.get('arguments', parse_arguments(arguments, func))
            declaration['type_method_definition_dispatch'] = func.get('dispatch', declaration['name'])
            declarations.append(declaration)

    return declarations
