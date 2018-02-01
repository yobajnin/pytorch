# Generates C++ autograd functions for the derivatives of ATen operations
#
# This writes two files:
#  Functions.h/cpp: subclasses of autograd::Function
#  python_functions.h/cpp: Python bindings for the above classes
#
import re
from .utils import nested_dict, CodeTemplate, write
from .gen_autograd import VIEW_FUNCTIONS, template_path
from .utils import IDENT_REGEX

FUNCTIONS_H = CodeTemplate.from_file(template_path + '/Functions.h')
FUNCTIONS_CPP = CodeTemplate.from_file(template_path + '/Functions.cpp')
PY_FUNCTIONS_H = CodeTemplate.from_file(template_path + '/python_functions.h')
PY_FUNCTIONS_CPP = CodeTemplate.from_file(template_path + '/python_functions.cpp')

FUNCTION_DECLARATION = CodeTemplate("""\
struct ${op} : public ${superclass} {
  using ${superclass}::${superclass};
  variable_list apply(const variable_list& grads) override;
  std::string name() override { return "${op}"; }
  void releaseVariables() override {
    ${release_variables}
  }
  ${saved_variables}
  ${saved_list_sizes}
};
""")

FUNCTION_DEFINITION = CodeTemplate("""\
variable_list ${op}::apply(const variable_list& grads) {
  IndexRangeGenerator gen;
  ${compute_index_ranges}
  variable_list grad_inputs(gen.size());
  ${body}
  return grad_inputs;
}
""")

PY_FUNCTION_DEFINITION = CodeTemplate("""\
static PyTypeObject ${op}Class;
addClass<${op}>(${op}Class, "${op}");
""")

GRAD_INPUT_MASK = CodeTemplate("""\
  auto grad_input_mask = std::array<bool, ${n}>{
    ${masks}
  };\
""")

DERIVATIVE_SINGLE = CodeTemplate("""\
if (should_compute_output({ ${name}_ix })) {
  auto grad_result = ${derivative};
  copy_range(grad_inputs, ${name}_ix, grad_result);
}
""")

DERIVATIVE_MULTI = CodeTemplate("""\
if (should_compute_output({ ${idx_ranges} })) {
  ${grad_input_mask}
  auto grad_result = ${derivative};
  ${copy_ranges}
}
""")

# These functions have backwards which cannot be traced, and so must have
# their backward functions traced opaquely.
# VIEW_FUNCTIONS are not traceable because they use as_strided, which
# has an untraceable backwards, see
# https://github.com/pytorch/pytorch/issues/4250
# TODO: This is probably not exhaustive, but it's a start
UNTRACEABLE_FUNCTIONS = VIEW_FUNCTIONS


def gen_autograd_functions(out, autograd_functions):
    """Functions.h and Functions.cpp body

    These contain the auto-generated subclasses of torch::autograd::Function
    for each every differentiable torch function.
    """
    function_definitions = []
    function_declarations = []
    py_function_initializers = []

    for func in autograd_functions:
        env = process_function(func)

        function_declarations.append(FUNCTION_DECLARATION.substitute(env))
        function_definitions.append(FUNCTION_DEFINITION.substitute(env))
        py_function_initializers.append(PY_FUNCTION_DEFINITION.substitute(env))

    top_env = {
        'autograd_function_definitions': function_definitions,
        'autograd_function_declarations': function_declarations,
        'py_function_initializers': py_function_initializers,
    }

    write(out, 'Functions.h', FUNCTIONS_H, top_env)
    write(out, 'Functions.cpp', FUNCTIONS_CPP, top_env)
    write(out, 'python_functions.h', PY_FUNCTIONS_H, top_env)
    write(out, 'python_functions.cpp', PY_FUNCTIONS_CPP, top_env)


def process_function(func):
    env = {}
    saved_variables = []
    release_variables = []
    saved_list_sizes = []
    unpack = []

    env['compute_index_ranges'] = []
    for arg in func['args_with_gradients']:
        if arg['type'] == 'TensorList':
            size = '{}_size_'.format(arg['name'])
            saved_list_sizes.append('size_t {}_size_;'.format(arg['name']))
        else:
            size = '1'
        env['compute_index_ranges'].append('auto {}_ix = gen.range({});'.format(arg['name'], size))

    def save_arg(arg, is_output):
        name = arg['name']
        if arg['type'] == 'Tensor' or (arg['type'] == 'Scalar' and is_output):
            saved_variables.append('SavedVariable {}_;'.format(name))
            release_variables.append('{}_.data.reset();'.format(name))
            ptr = 'shared_from_this()' if is_output else ''
            unpack.append('auto {} = {}_.unpack({});'.format(name, name, ptr))
        elif arg['type'] == 'IntList':
            saved_variables.append('std::vector<int64_t> {};'.format(name))
        else:
            saved_variables.append('{} {};'.format(arg['type'], name))

    for arg in func['saved_inputs']:
        save_arg(arg, is_output=False)
    for arg in func['saved_outputs']:
        save_arg(arg, is_output=True)
    env['saved_variables'] = saved_variables
    env['release_variables'] = release_variables
    env['saved_list_sizes'] = saved_list_sizes

    body = []

    if uses_single_grad(func):
        body.append('auto& grad = grads[0];')

    def emit_derivative(derivative):
        formula = derivative['formula']
        var_names = derivative['var_names']
        if len(var_names) == 1:
            return DERIVATIVE_SINGLE.substitute(name=var_names[0], derivative=formula)
        else:
            if 'grad_input_mask' in formula:
                masks = ['should_compute_output({{ {}_ix }}),'.format(n) for n in var_names]
                grad_input_mask = GRAD_INPUT_MASK.substitute(masks=masks, n=len(var_names))
            else:
                grad_input_mask = ''
            idx_ranges = ', '.join("{}_ix".format(n) for n in var_names)
            copy_ranges = []
            for i, n in enumerate(var_names):
                copy_ranges.append("copy_range(grad_inputs, {}_ix, std::get<{}>(grad_result));".format(n, i))
            return DERIVATIVE_MULTI.substitute(
                idx_ranges=idx_ranges, copy_ranges=copy_ranges,
                derivative=formula,
                grad_input_mask=grad_input_mask)

    body.extend(unpack)
    for derivative in func['derivatives']:
        body.append(emit_derivative(derivative))

    env['body'] = body
    if func['name'] in UNTRACEABLE_FUNCTIONS:
        env['superclass'] = 'Function'
    else:
        env['superclass'] = 'TraceableFunction'
    return nested_dict(env, func)


def uses_single_grad(func):
    if func is None:
        return False
    for derivative in func['derivatives']:
        formula = derivative['formula']
        if re.search(IDENT_REGEX.format('grad'), formula):
            return True
    return False
