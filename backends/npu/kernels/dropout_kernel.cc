// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "kernels/funcs/npu_funcs.h"
#include "kernels/funcs/npu_op_runner.h"

namespace custom_kernel {

template <typename T, typename Context>
void DropoutRawKernel(const Context& dev_ctx,
                      const phi::DenseTensor& x,
                      const paddle::optional<phi::DenseTensor>& seed_tensor,
                      const phi::Scalar& p,
                      bool is_test,
                      const std::string& mode,
                      int seed,
                      bool fix_seed,
                      phi::DenseTensor* out,
                      phi::DenseTensor* mask) {
  auto dropout_prob = p.to<float>();

  dev_ctx.template Alloc<T>(out);
  auto stream = dev_ctx.stream();

  if (!is_test) {
    // mask used in `DropOutGenMask` NPU OP is different from
    // the output `Mask`.
    uint32_t length = (x.numel() + 128 - 1) / 128 * 128;
    mask->Resize({length / 8});
    dev_ctx.template Alloc<uint8_t>(mask);
  }

  if (dropout_prob == 1.) {
    const auto& runner_zeros_out = NpuOpRunner("ZerosLike", {*out}, {*out});
    runner_zeros_out.Run(stream);
    ACL_CHECK(aclrtMemsetAsync(mask->data(),
                               mask->numel() * sizeof(uint8_t),
                               0,
                               mask->numel() * sizeof(uint8_t),
                               stream));
    return;
  }

  // only achieve the default `upscale_in_train` method
  if (!is_test) {
    if (x.dtype() == phi::DataType::FLOAT64) {
      // transform x
      phi::DenseTensor tmp_x;
      phi::DenseTensorMeta tmp_x_meta = {phi::DataType::FLOAT32, x.dims()};
      tmp_x.set_meta(tmp_x_meta);
      dev_ctx.template Alloc<float>(&tmp_x);
      NpuOpRunner runner;
      runner.SetType("Cast")
          .AddInput(x)
          .AddOutput(tmp_x)
          .AddAttr("dst_type", ACL_FLOAT)
          .Run(stream);
      phi::DenseTensor tmp_out;
      phi::DenseTensorMeta tmp_out_meta = {phi::DataType::FLOAT32, out->dims()};
      tmp_out.set_meta(tmp_out_meta);
      dev_ctx.template Alloc<float>(&tmp_out);

      if (x.dims().size() == 1) {
        // DropOutDoMask will get error result when input
        // is 1-D. Make it become 2-D.
        std::vector<int> vec_dim = phi::vectorize<int>(x.dims());
        tmp_x.Resize(phi::make_ddim({vec_dim[0], 1}));
        tmp_out.Resize(phi::make_ddim({vec_dim[0], 1}));
      }

      int seed = 0;
      int seed2 = 0;
      float keep_prob = static_cast<float>(1. - dropout_prob);
      if (seed_tensor) {
        MemCpyD2H(nullptr, &seed, seed_tensor->data(), sizeof(int));
      } else {
        seed = fix_seed ? seed : 0;
      }

      phi::DenseTensor keep_prob_tensor;
      phi::DenseTensorMeta keep_prob_tensor_meta = {phi::DataType::FLOAT32,
                                                    {1}};
      keep_prob_tensor.set_meta(keep_prob_tensor_meta);
      AsyncMemCpyH2D(nullptr,
                     static_cast<C_Stream>(
                         SecondaryStream::Instance().Get(dev_ctx.stream())),
                     dev_ctx.template Alloc<float>(&keep_prob_tensor),
                     &keep_prob,
                     sizeof(float));
      NpuOpRunner runner_gen_mask;
      runner_gen_mask.SetType("DropOutGenMask")
          .AddInput(dev_ctx, phi::vectorize(tmp_out.dims()))
          .AddInput(keep_prob_tensor)
          .AddOutput(*mask)
          .AddAttr("seed", seed)
          .AddAttr("seed2", seed2);
      runner_gen_mask.Run(SecondaryStream::Instance().Get(dev_ctx.stream()));
      SecondaryStream::Instance().RecordBefore(dev_ctx.stream());

      NpuOpRunner runner_dropout;
      runner_dropout.SetType("DropOutDoMask")
          .AddInput(tmp_x)
          .AddInput(*mask)
          .AddInput(keep_prob_tensor)
          .AddOutput(tmp_out);
      runner_dropout.Run(stream);

      NpuOpRunner runner_cast;
      runner_cast.SetType("Cast")
          .AddInput(tmp_out)
          .AddOutput(*out)
          .AddAttr("dst_type", ACL_DOUBLE)
          .Run(stream);
    } else {
      phi::DenseTensor tmp_x(x);
      phi::DenseTensor tmp_out(*out);

      if (x.dims().size() == 1) {
        // DropOutDoMask will get error result when input
        // is 1-D. Make it become 2-D.
        std::vector<int> vec_dim = phi::vectorize<int>(x.dims());
        tmp_x.Resize(phi::make_ddim({vec_dim[0], 1}));
        tmp_out.Resize(phi::make_ddim({vec_dim[0], 1}));
      }

      int seed = 0;
      int seed2 = 0;
      T keep_prob = static_cast<T>(1. - dropout_prob);
      if (seed_tensor) {
        MemCpyD2H(nullptr, &seed, seed_tensor->data(), sizeof(int));
      } else {
        seed = fix_seed ? seed : 0;
      }

      phi::DenseTensor keep_prob_tensor;
      phi::DenseTensorMeta keep_prob_tensor_meta = {x.dtype(), {1}};
      keep_prob_tensor.set_meta(keep_prob_tensor_meta);
      AsyncMemCpyH2D(nullptr,
                     static_cast<C_Stream>(
                         SecondaryStream::Instance().Get(dev_ctx.stream())),
                     dev_ctx.template Alloc<T>(&keep_prob_tensor),
                     &keep_prob,
                     sizeof(T));

      // TODO(pangyoki): `keep_prob` used in `DropOutGenMask` NPU
      // OP must be a scalar with shape[0]. At present, the shape
      // of the `prob` Tensor of this OP is forced to be set to 0
      // in `npu_op_runner.cc`, which needs to be optimized later.
      NpuOpRunner runner_gen_mask;
      runner_gen_mask.SetType("DropOutGenMask")
          .AddInput(dev_ctx, phi::vectorize(tmp_out.dims()))
          .AddInput(keep_prob_tensor)
          .AddOutput(*mask)
          .AddAttr("seed", seed)
          .AddAttr("seed2", seed2);
      runner_gen_mask.Run(SecondaryStream::Instance().Get(dev_ctx.stream()));
      SecondaryStream::Instance().RecordBefore(dev_ctx.stream());

      NpuOpRunner runner_dropout;
      runner_dropout.SetType("DropOutDoMask")
          .AddInput(tmp_x)
          .AddInput(*mask)
          .AddInput(keep_prob_tensor)
          .AddOutput(tmp_out);
      runner_dropout.Run(stream);
    }
  } else {
    TensorCopy(dev_ctx, x, false, out);
  }
}

template <typename T, typename Context>
void DropoutGradRawKernel(const Context& dev_ctx,
                          const phi::DenseTensor& mask,
                          const phi::DenseTensor& dout,
                          const phi::Scalar& p,
                          bool is_test,
                          const std::string& mode,
                          phi::DenseTensor* dx) {
  auto dropout_prob = p.to<float>();

  dev_ctx.template Alloc<T>(dx);
  auto stream = dev_ctx.stream();

  if (dropout_prob == 1.) {
    const auto& runner_zeros = NpuOpRunner("ZerosLike", {*dx}, {*dx});
    runner_zeros.Run(stream);
    return;
  }

  if (dx->dtype() == phi::DataType::FLOAT64) {
    phi::DenseTensor keep_prob_tensor;
    phi::DenseTensorMeta keep_prob_tensor_meta = {phi::DataType::FLOAT32, {1}};
    keep_prob_tensor.set_meta(keep_prob_tensor_meta);
    FillNpuTensorWithConstant<float>(
        &keep_prob_tensor, dev_ctx, static_cast<float>(1 - dropout_prob));
    // transform dx
    phi::DenseTensor tmp_dx;
    phi::DenseTensorMeta tmp_dx_meta = {phi::DataType::FLOAT32, dx->dims()};
    tmp_dx.set_meta(tmp_dx_meta);
    dev_ctx.template Alloc<float>(&tmp_dx);
    // transform dout
    phi::DenseTensor tmp_dout;
    phi::DenseTensorMeta tmp_dout_meta = {phi::DataType::FLOAT32, dout.dims()};
    tmp_dout.set_meta(tmp_dout_meta);
    dev_ctx.template Alloc<float>(&tmp_dout);
    NpuOpRunner runner1;
    runner1.SetType("Cast")
        .AddInput(dout)
        .AddOutput(tmp_dout)
        .AddAttr("dst_type", ACL_FLOAT)
        .Run(stream);
    NpuOpRunner runner_dropout;
    runner_dropout.SetType("DropOutDoMask")
        .AddInput(tmp_dout)
        .AddInput(mask)
        .AddInput(keep_prob_tensor)
        .AddOutput(tmp_dx)
        .Run(stream);
    NpuOpRunner runner2;
    runner2.SetType("Cast")
        .AddInput(tmp_dx)
        .AddOutput(*dx)
        .AddAttr("dst_type", ACL_DOUBLE)
        .Run(stream);
  } else {
    phi::DenseTensor keep_prob_tensor;
    phi::DenseTensorMeta keep_prob_tensor_meta = {dx->dtype(), {1}};
    keep_prob_tensor.set_meta(keep_prob_tensor_meta);
    FillNpuTensorWithConstant<T>(
        &keep_prob_tensor, dev_ctx, static_cast<T>(1 - dropout_prob));

    NpuOpRunner runner_dropout;
    runner_dropout.SetType("DropOutDoMask")
        .AddInput(dout)
        .AddInput(mask)
        .AddInput(keep_prob_tensor)
        .AddOutput(*dx);
    runner_dropout.Run(stream);
  }
  return;
}

}  // namespace custom_kernel

PD_REGISTER_PLUGIN_KERNEL(dropout,
                          npu,
                          ALL_LAYOUT,
                          custom_kernel::DropoutRawKernel,
                          float,
                          phi::dtype::float16,
                          double) {}

PD_REGISTER_PLUGIN_KERNEL(dropout_grad,
                          npu,
                          ALL_LAYOUT,
                          custom_kernel::DropoutGradRawKernel,
                          float,
                          phi::dtype::float16,
                          double) {}
