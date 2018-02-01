#include "torch/csrc/autograd/saved_variable.h"

#include "torch/csrc/autograd/function.h"

using namespace at;

namespace torch { namespace autograd {

SavedVariable::SavedVariable(const Variable& variable, bool is_output)
  : SavedVariable() {
  if (!variable.defined()) {
    return;
  }
  data = variable.data();
  requires_grad = variable.requires_grad();
  expected_version = variable.current_version();
  version = variable.get()->version_counter.save();
  has_grad_fn = !variable.is_leaf();
  output_nr = variable.output_nr();
  if (!has_grad_fn) {
    grad_accumulator = variable.grad_accumulator();
  }
  if (!is_output) {
    _grad_fn = variable.grad_fn();
  }
  if (variable.tracing_state()) {
    tracing_state.reset(new jit::tracer::ValueTracingState(*variable.tracing_state()));
  }
}

auto SavedVariable::unpack(std::shared_ptr<Function> saved_for) const -> Variable {
  if (!data.defined()) {
    if (version.defined()) {
      throw std::runtime_error(ERR_BACKWARD_TWICE);
    }
    return Variable();
  }

  if (version.is_modified()) {
    throw std::runtime_error(
        "one of the variables needed for gradient computation has been "
        "modified by an inplace operation");
  }

  auto grad_fn = _grad_fn;
  if (has_grad_fn && !grad_fn) {
    if (!saved_for) {
      // If saving the grad_fn would create a circular reference, then it must
      // be passed in to the unpack function.
      throw std::runtime_error("No grad_fn for non-leaf saved variable");
    }
    grad_fn = std::move(saved_for);
  }

  // NB: saved views are unpacked as normal Variables (not views) even though
  // they still share the same storage. This works only because we never call
  // in-place functions on unpacked variables.
  Variable var;
  if (grad_fn) {
    var = make_variable(data, output_nr, std::move(grad_fn));
  } else {
    var = make_variable(data, requires_grad);
  }
  var.version_counter() = version;

  // If a Variable is a leaf (no grad_fn saved), and it requires_grad, then we
  // should have saved the grad accumulator. Even if the Variable no longer
  // alive, the accumulator should be kept alive by the references in the graph).
  if (requires_grad && !var.grad_fn() && grad_accumulator.expired())
    throw std::logic_error("No grad accumulator for a saved leaf!");
  var.get()->grad_accumulator = grad_accumulator;
  if (tracing_state)
    var.tracing_state().reset(new jit::tracer::ValueTracingState(*tracing_state));

  return var;
}

const char* ERR_BACKWARD_TWICE =
    "Trying to backward through the graph a second time, but the buffers have "
    "already been freed. Specify retain_graph=True when calling backward "
    "the first time.";

}} // namespace torch::autograd
