/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#if GOOGLE_CUDA

#define EIGEN_USE_GPU

#include "tensorflow/core/kernels/random_op_gpu.h"
#include "tensorflow/core/kernels/stateful_random_ops_cpu_gpu.h"
#include "tensorflow/core/util/gpu_launch_config.h"
#include "tensorflow/core/util/gpu_kernel_helper.h"

namespace tensorflow {

using random::PhiloxRandom;

__device__ int thread_counter;

template <typename Distribution>
__global__ void FillKernel(
    Distribution dist, int64 state_size, int64 output_size,
    StateElementType* state_data,
    typename Distribution::ResultElementType* output_data) {
  // Threads in this block share `philox`. Thread 0 is responsible for
  // initializing it.
  __shared__ char philox_raw[sizeof(PhiloxRandom)];
  auto philox = reinterpret_cast<PhiloxRandom*>(philox_raw);
  if (threadIdx.x == 0) {
    *philox = GetPhiloxRandomFromMem(state_data);
  }
  __syncthreads();
  functor::FillPhiloxRandomKernel<Distribution,
                                  Distribution::kVariableSamplesPerOutput>()
      .Run(*philox, output_data, output_size, dist);
  // The last thread updates the state.
  auto total_thread_count = gridDim.x * blockDim.x;
  auto old_counter_value = atomicAdd(&thread_counter, 1);
  if (old_counter_value == total_thread_count - 1) {
    UpdateMemWithPhiloxRandom(*philox, output_size, state_data);
  }
}

template <typename Distribution>
void UpdateVariableAndFill_Philox<GPUDevice, Distribution>::operator()(
    OpKernelContext* ctx, const GPUDevice& d, int64 output_size,
    int64 alg_tag_skip, ScopedUnlockUnrefVar* not_used, Tensor* state_tensor,
    typename Distribution::ResultElementType* output_data) {
  OP_REQUIRES(
      ctx, alg_tag_skip == 0,
      errors::InvalidArgument(
          "GPU kernel doesn't support reading algorithm from state variable, "
          "so alg_tag_skip must be 0; got",
          alg_tag_skip));
  auto state_tensor_flat = state_tensor->flat<StateElementType>();
  auto state_size = state_tensor_flat.size();
  auto state_data = state_tensor_flat.data();

  // maximize occupancy
  const int kGroupSize = Distribution::kResultElementCount;
  int work_element_count = (output_size + kGroupSize - 1) / kGroupSize;
  GpuLaunchConfig cfg = GetGpuLaunchConfig(work_element_count, d,
                                             FillKernel<Distribution>, 0, 0);

  int zero = 0;
  cudaMemcpyToSymbol(thread_counter, &zero, sizeof(int));
  GPU_LAUNCH_KERNEL(FillKernel<Distribution>, cfg.block_count,
                               cfg.thread_per_block, 0, d.stream(),
                               Distribution(), state_size, output_size,
                               state_data, output_data);
}

// Explicit instantiation of the GPU distributions functors.

// clang-format off
// NVCC cannot handle ">>" properly
template struct UpdateVariableAndFill_Philox<
    GPUDevice, random::NormalDistribution<random::PhiloxRandom, Eigen::half> >;
template struct UpdateVariableAndFill_Philox<
    GPUDevice, random::NormalDistribution<random::PhiloxRandom, float> >;
template struct UpdateVariableAndFill_Philox<
    GPUDevice, random::NormalDistribution<random::PhiloxRandom, double> >;
// clang-format on

}  // end namespace tensorflow

#endif  // GOOGLE_CUDA
