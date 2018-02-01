#include "Python.h"
#include "interpreter_autograd_function.h"

namespace torch { namespace jit {

using namespace torch::jit::tracer;

static at::Tensor zeroTensorWithType(const TensorType & type) {
  auto device = (type.device() < 0)? at::kCPU : at::kCUDA;
  auto & at_type = at::getType(device, type.scalarType());
  // note: this has to be a contiguous tensor of zeros, because the fusion engine
  // specialized to what is normally here which might be fully dense
  return at_type.zeros(type.sizes());
}

autograd::variable_list InterpreterAutogradFunction::apply(
    const autograd::variable_list& inputs) {
  // Initial correctness checks.
  if (stage_ == stage_details_.size()) {
    throw std::runtime_error(std::string("Function compiled only for ") +
        std::to_string(stage_details_.size() - 1) + " derivatives. Use nderivs argument " +
        "to request more.");
  }
  if (used_) throw std::runtime_error(autograd::ERR_BACKWARD_TWICE);
  used_ |= !keep_graph_;

  const auto & details = stage_details_[stage_];

  // Validate inputs
  std::vector<at::Tensor> tinputs;
  tinputs.reserve(inputs.size());
  TORCH_ASSERT(inputs.size() == static_cast<std::size_t>(num_inputs));
  TORCH_ASSERT(inputs.size() == details.input_flags.size());
  for (std::size_t i = 0; i < (std::size_t)inputs.size(); ++i) {
    auto actual_flags = VariableFlags::of(inputs[i]);
    auto traced_flags = details.input_flags[i];

    // check that this trace is general enough to handle the input
    // flags of the actual tensor. We can't handle the following two cases
    // because we won't have a trace containing computation of either
    // the tensor itself (not defined) or the stage for its gradient (requires_grad=False)
    if(!traced_flags.defined && actual_flags.defined) {
      throw std::runtime_error("JIT intepreter received a defined input, but the"
        " trace was compiled with the input being undefined.");
    }
    if(!traced_flags.requires_grad && actual_flags.requires_grad) {
      throw std::runtime_error("JIT inteperpreter recieved an input with "
        " requires_grad=True, but was compiled with requires_grad=False");
    }

    // The remaining cases we can handle. If the gradient was not
    // required but the trace will compute it, then we just compute it and
    // ignore the result.
    // However, if we are passed an undefined tensor, but the trace
    // expects a defined tensor, then we have to give it one.
    // Undefined tensors are used as stand-ins for zero tensors, so
    // we create a zero-filled tensor of the right size
    if(!actual_flags.defined) {
      // [Temporary workaround for variants] until tracer produces all variants:
      // This case appears commonly when you have a function
      // x, y = fn(z)
      // and only use x then gradient for y
      // will be undefined. If you reuse the same trace with and _sometimes_ use y
      // then in the cases where you don't use it, the grad_y input in stage 1
      // will be undefined. To ensure we can continue, we create a 0 gradient,
      // using trace information to figure out what shape it should be
      if(traced_flags.defined) {
        tinputs.push_back(zeroTensorWithType(interp_.tensorTypeForInput(i)));
      } else {
        tinputs.push_back(at::Tensor());
      }
    } else {
      tinputs.push_back(inputs[i].data());
    }
  }

  // Run the interpreter
  std::vector<at::Tensor> toutputs;
  InterpreterState interp = (keep_graph_) ? interp_.clone() : interp_;
  interp.runOneStage(tinputs, toutputs);

  // Lazily create grad_fn
  std::shared_ptr<Function> grad_fn;
  auto make_grad_fn = [&]() {
    grad_fn = std::make_shared<InterpreterAutogradFunction>(
        std::move(interp), stage_details_, stage_ + 1);

    // Running this next stage is actually not valid (nderiv is too low)
    // but we don't know if the user will ever ask for it so we don't error out here.
    // Instead we have to return early because we rely on stage_details_[stage+1] in the
    // remaining code
    if(stage_ + 1 == stage_details_.size())
      return;

    // Patch next_functions to include prevous stage next_functions
    // This is needed because stage N is really a derivative of
    // all stages from 1 to N-1. If a part of stage x graph is
    // reused in stage y (y > x), it is inlined by the tracer,
    // and so we need to copy next_fns because those Variables
    // aren't real inputs to that stage, so that's the only place
    // where we can get them.
    for (auto copied_idx : stage_details_[stage_ + 1].copied_next_fns) {
      grad_fn->next_functions.push_back(next_functions[copied_idx]);
    }
    // Add grad_fns corresponding to inputs
    for(size_t i = 0; i < inputs.size(); ++i) {
      // If an input isn't used, there's no gradient for it, and next stage
      // won't even have its grad in the trace. Don't create an entry for it.
      if (!details.used_inputs[i]) continue;
      auto & input = inputs[i];
      if (!details.input_flags[i].requires_grad) {
        continue; // See Note [Null-edge pruning]
      } else if (!input.defined() || !input.requires_grad()) {
        // See Note [Temporary workaround for variants]
        grad_fn->next_functions.emplace_back();
        continue;
      }
      grad_fn->next_functions.emplace_back(
        input.grad_fn() ? input.grad_fn() : input.grad_accumulator(),
        input.output_nr());
    }
  };

  // Wrap the outputs
  // TODO: handle views
  autograd::variable_list result;
  JIT_ASSERT(toutputs.size() == details.output_flags.size());
  auto num_outputs = toutputs.size();
  for (std::size_t i = 0; i < num_outputs; ++i) {
    auto & flags = details.output_flags[i];
    if (flags.requires_grad) { // See Note [Null-edge pruning]
      if (!grad_fn) make_grad_fn();
      result.push_back(autograd::make_variable(toutputs[i], grad_fn));
    } else {
      result.push_back(autograd::make_variable(toutputs[i], false));
    }
  }

  return result;
}

InterpreterFunctionFactory::InterpreterFunctionFactory(TracingState *state) {
  code_ = jit::Code(state->graph);
  stage_details_.resize(state->graph->stage() + 1);
  auto graph_inputs = state->graph->inputs();
  auto inputs_it = graph_inputs.begin();
  for (std::size_t stage = 0; stage < state->graph->stage() + 1; ++stage) {
    auto & details = stage_details_[stage];
    std::tie(details.input_flags, details.output_flags) = std::move(state->var_flags[stage]);
    for (std::size_t i = 0; inputs_it != graph_inputs.end() && (*inputs_it)->stage() == stage; ++i, ++inputs_it) {
      details.used_inputs.push_back((*inputs_it)->uses().size() > 0);
    }
    if (stage >= 1) {
      auto & current_outputs = state->output_edges[stage];
      auto & prev_outputs = state->output_edges[stage - 1];
      for (auto & output : current_outputs) {
        // Check if output appears in outputs of previous stage
        auto prev_it = std::find(prev_outputs.begin(), prev_outputs.end(), output);
        if (prev_it == prev_outputs.end()) continue;
        // If yes, find its index and append that to the list of edges that will need
        // to be copied in InterpreterAutogradFunction.
        details.copied_next_fns.push_back(std::distance(prev_outputs.begin(), prev_it));
      }
    }
  }
}

std::shared_ptr<autograd::Function> InterpreterFunctionFactory::construct() {
  return std::make_shared<InterpreterAutogradFunction>(code_, stage_details_);
}

}}
