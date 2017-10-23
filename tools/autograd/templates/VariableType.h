#pragma once

// ${generated_comment}

#include <ATen/ATen.h>
#include <string>

namespace torch { namespace autograd {

struct Variable;
using at::Context;
using at::Generator;
using at::IntList;
using at::Scalar;
using at::SparseTensor;
using at::Storage;
using at::Tensor;
using at::TensorList;
using at::Type;

struct VariableType : public at::Type {
  VariableType(Context* context, at::Type* baseType);
  virtual at::ScalarType scalarType() const override;
  virtual at::Backend backend() const override;
  virtual bool isCuda() const override;
  virtual bool isSparse() const override;
  virtual bool isDistributed() const override;
  virtual std::unique_ptr<at::Storage> storage() const override;
  virtual std::unique_ptr<at::Storage> storage(size_t size) const override;
  virtual std::unique_ptr<at::Storage> storageFromBlob(void * data, int64_t size, const std::function<void(void*)> & deleter) const override;
  virtual std::unique_ptr<at::Generator> generator() const override;
  virtual const char * toString() const override;
  virtual at::TypeID ID() const override;
  virtual size_t elementSizeInBytes() const override;
  static const char * typeString();
  at::Tensor unsafeTensorFromTH(void * th_pointer, bool retain) const override;

  virtual void s_copy(const Tensor & src, Tensor & dst) const override;
  ${type_derived_method_declarations}

private:
  // checks that t is actually a Variable with the given expected_type
  static Variable & checked_cast(const Type & expected_type, const Tensor & t, const char * name, int pos);
  at::Tensor & unpack(const Tensor & t, const char * name, int pos) const;
  at::Tensor & unpack_long(const Tensor & t, const char * name, int pos) const;
  at::Tensor & unpack_byte(const Tensor & t, const char * name, int pos) const;
  at::Tensor & unpack_any(const Tensor & t, const char * name, int pos) const;
  at::Tensor unpack_opt(const Tensor & t, const char * name, int pos) const;
  std::vector<at::Tensor> unpack(const at::TensorList &tl, const char *name, int pos) const;

  Variable as_variable(const Scalar & scalar) const;
  Variable as_variable(Tensor tensor) const;
  std::tuple<Variable, Variable> as_variable(std::tuple<Tensor, Tensor> tensor) const;
  std::tuple<Variable, Variable, Variable> as_variable(std::tuple<Tensor, Tensor, Tensor> tensor) const;

private:
  at::Type* baseType;
  std::string str;
};

}} // namespace torch::autograd
