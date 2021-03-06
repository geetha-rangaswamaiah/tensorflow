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

#include "tensorflow/lite/delegates/gpu/cl/kernels/convolution_transposed.h"

#include <string>
#include <utility>

#include "absl/strings/substitute.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/util.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/work_group_picking.h"
#include "tensorflow/lite/delegates/gpu/cl/precision.h"
#include "tensorflow/lite/delegates/gpu/cl/tensor_type.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"

namespace tflite {
namespace gpu {
namespace cl {

ConvolutionTransposed::ConvolutionTransposed(
    const OperationDef& definition, const ConvolutionTransposedAttributes& attr,
    const DeviceInfo& device_info)
    : GPUOperation(definition),
      stride_(attr.stride.w, attr.stride.h),
      block_size_(2, 2, 2) {
  const bool weights_are_buffer = device_info.IsMali();
  const bool is_f16 = definition.precision == CalculationsPrecision::F16;
  if (device_info.IsMali()) {
    if (device_info.mali_info.IsMidgard()) {
      block_size_ = is_f16 ? int3(2, 1, 2) : int3(2, 1, 1);
    } else {
      block_size_ = is_f16 ? int3(2, 2, 2) : int3(2, 2, 1);
    }
  }
  const int dst_depth = DivideRoundUp(attr.weights.shape.o, 4);
  if (dst_depth == 1 || dst_depth == 3) {
    if (!device_info.IsMali()) {
      block_size_.y *= block_size_.z;
    }
    block_size_.z = 1;
  }

  args_.AddInt("stride_x", stride_.x);
  args_.AddInt("stride_y", stride_.y);
  args_.AddInt("padding_x", attr.padding.prepended.w);
  args_.AddInt("padding_y", attr.padding.prepended.h);
  args_.AddInt("kernel_size_x", attr.weights.shape.w);
  args_.AddInt("kernel_size_y", attr.weights.shape.h);
  code_ = GenerateConvolutionTransposedCode(definition_, device_info,
                                            weights_are_buffer, block_size_);
  UploadWeights(attr.weights, weights_are_buffer);
}

ConvolutionTransposed::ConvolutionTransposed(ConvolutionTransposed&& operation)
    : GPUOperation(std::move(operation)),
      stride_(operation.stride_),
      block_size_(operation.block_size_) {}

ConvolutionTransposed& ConvolutionTransposed::operator=(
    ConvolutionTransposed&& operation) {
  if (this != &operation) {
    std::swap(stride_, operation.stride_);
    std::swap(block_size_, operation.block_size_);
    GPUOperation::operator=(std::move(operation));
  }
  return *this;
}

std::string ConvolutionTransposed::GenerateConvolutionTransposedCode(
    const OperationDef& op_def, const DeviceInfo& device_info,
    bool weights_are_buffer, const int3& block_size) {
  auto src_desc = op_def.src_tensors[0];
  src_desc.SetTextureAddressMode(TextureAddressMode::ZERO);
  AddSrcTensor("src_tensor", src_desc);

  AddDstTensor("dst_tensor", op_def.dst_tensors[0]);

  const auto src_tensor_type = op_def.src_tensors[0].storage_type;
  bool image_buffer = src_tensor_type == TensorStorageType::IMAGE_BUFFER;
  bool manual_clamp =
      image_buffer || src_tensor_type == TensorStorageType::BUFFER;

  std::string c = GetCommonDefines(op_def.precision);

  for (int z = 0; z < block_size.z; ++z) {
    const std::string f0 =
        weights_are_buffer ? "weights_cache[" + std::to_string(z) + "].s0123"
                           : "f" + std::to_string(z * 4 + 0);
    const std::string f1 =
        weights_are_buffer ? "weights_cache[" + std::to_string(z) + "].s4567"
                           : "f" + std::to_string(z * 4 + 1);
    const std::string f2 =
        weights_are_buffer ? "weights_cache[" + std::to_string(z) + "].s89ab"
                           : "f" + std::to_string(z * 4 + 2);
    const std::string f3 =
        weights_are_buffer ? "weights_cache[" + std::to_string(z) + "].scdef"
                           : "f" + std::to_string(z * 4 + 3);
    switch (op_def.precision) {
      case CalculationsPrecision::F32:
      case CalculationsPrecision::F16:
        c += "#define CONV" + std::to_string(z) + "(R, S)    \\\n";
        c += "R += S.x * " + f0 + "; \\\n";
        c += "R += S.y * " + f1 + "; \\\n";
        c += "R += S.z * " + f2 + "; \\\n";
        c += "R += S.w * " + f3 + ";   \n";
        break;
      case CalculationsPrecision::F32_F16:
        c += "#define CONV" + std::to_string(z) + "(R, S) \\\n";
        c += "R += convert_float4(S.x * " + f0 + " + S.y * " + f1 +
             " + S.z * " + f2 + " + S.w * " + f3 + ");\n";
        break;
    }
  }

  switch (op_def.precision) {
    case CalculationsPrecision::F32:
      c += "#define FLT16 float16\n";
      break;
    case CalculationsPrecision::F32_F16:
    case CalculationsPrecision::F16:
      c += "#define FLT16 half16\n";
      break;
  }

  c += "__kernel void main_function(\n";
  c += "$0) {\n";
  if (op_def.IsBatchSupported()) {
    c += "  int linear_id = get_global_id(0);\n";
    c += "  int dst_x = (linear_id / args.dst_tensor.Batch());\n";
    c += "  int B = linear_id % args.dst_tensor.Batch();\n";
    c += "  args.dst_tensor.SetBatchRef(B);\n";
    c += "  args.src_tensor.SetBatchRef(B);\n";
  } else {
    c += "  int dst_x = get_global_id(0);\n";
  }
  c += "  int rem_x = dst_x % args.stride_x;\n";
  c += "  int ceil_x = dst_x / args.stride_x;\n";
  c += "  dst_x = ceil_x * args.stride_x * " + std::to_string(block_size.x) +
       " + rem_x;\n";
  c += "  int dst_y = get_global_id(1);\n";
  c += "  int rem_y = dst_y % args.stride_y;\n";
  c += "  int ceil_y = dst_y / args.stride_y;\n";
  c += "  dst_y = ceil_y * args.stride_y * " + std::to_string(block_size.y) +
       " + rem_y;\n";
  c += "  int dst_z = get_global_id(2) * " + std::to_string(block_size.z) +
       ";\n";
  c += "  if (dst_x >= args.dst_tensor.Width() || dst_y >= "
       "args.dst_tensor.Height() || dst_z >= "
       "args.dst_tensor.Slices()) return;\n";
  if (weights_are_buffer) {
    c += "  int f_base = dst_z * args.src_tensor.Slices() * args.kernel_size_x "
         "* args.kernel_size_y;\n";
  }
  for (int i = 0; i < block_size.x * block_size.y * block_size.z; ++i) {
    c += "  ACCUM_FLT4 r" + std::to_string(i) +
         " = (ACCUM_FLT4)(0.0f, 0.0f, 0.0f, 0.0f);\n";
  }
  c += "  int kernel_first_dst_x = dst_x + args.padding_x;\n";
  c += "  int kernel_first_dst_y = dst_y + args.padding_y;\n";
  c += "  int kernel_last_dst_x = kernel_first_dst_x - args.kernel_size_x;\n";
  c += "  int kernel_last_dst_y = kernel_first_dst_y - args.kernel_size_y;\n";
  c += "  int offset_x = abs(args.padding_x);\n";
  c += "  int offset_x_strided = offset_x * args.stride_x;\n";
  c +=
      "  int src_x = (kernel_first_dst_x + offset_x_strided) / args.stride_x - "
      "offset_x;\n";
  c += "  int offset_y = abs(args.padding_y);\n";
  c += "  int offset_y_strided = offset_y * args.stride_y;\n";
  c +=
      "  int src_y = (kernel_first_dst_y + offset_y_strided) / args.stride_y - "
      "offset_y;\n";
  c += "  int src_as_dst_y = src_y * args.stride_y;\n";
  c += "  for (;src_as_dst_y > kernel_last_dst_y; src_y -= 1, src_as_dst_y -= "
       "args.stride_y) {\n";
  for (int y = 0; y < block_size.y; ++y) {
    const std::string yindex = std::to_string(y);
    c += "    int sy" + yindex + " = src_y + " + yindex + ";\n";
    if (manual_clamp) {
      c += "    bool in_y" + yindex + " = sy" + yindex + " >= 0 && sy" +
           yindex + " < args.src_tensor.Height();\n";
      if (!image_buffer) {
        c += "    sy" + yindex + " = clamp(sy" + yindex +
             ", 0, args.src_tensor.Height() - 1);\n";
      }
    }
  }
  c += "    int kernel_y = kernel_first_dst_y - src_as_dst_y;\n";
  c += "    int src_as_dst_x = src_x * args.stride_x;\n";
  c += "    int src_x_copy = src_x;\n";
  c += "    for (;src_as_dst_x > kernel_last_dst_x; src_x_copy -= 1, "
       "src_as_dst_x "
       "-= args.stride_x) {\n";
  for (int x = 0; x < block_size.x; ++x) {
    const std::string xindex = std::to_string(x);
    c += "      int sx" + xindex + " = src_x_copy + " + xindex + ";\n";
    if (manual_clamp) {
      c += "      bool in_x" + xindex + " = sx" + xindex + " >= 0 && sx" +
           xindex + " < args.src_tensor.Width();\n";
      if (!image_buffer) {
        c += "      sx" + xindex + " = clamp(sx" + xindex +
             ", 0, args.src_tensor.Width() - 1);\n";
      }
    }
  }
  for (int y = 0; y < block_size.y; ++y) {
    const std::string yindex = std::to_string(y);
    for (int x = 0; x < block_size.x; ++x) {
      const std::string xindex = std::to_string(x);
      const std::string id = std::to_string(y * block_size.x + x);
      c += "      args.src_tensor.GetAddress(addr_" + id + ", sx" + xindex +
           ", sy" + yindex + ", 0);\n";
      if (image_buffer) {
        c += "      addr_" + id + " = select(-1, addr_" + id + ", (in_x" +
             xindex + " && in_y" + yindex + "));\n";
        c += absl::Substitute(
            "      int dz_$0 = select(0, args.src_tensor.SliceStride(), "
            "(in_x$1 && in_y$2));\n",
            y * block_size.x + x, x, y);
      }
    }
  }
  if (src_tensor_type == TensorStorageType::BUFFER) {
    c += "      int dz = args.src_tensor.SliceStride();\n";
  }
  if (block_size.x == 1 && block_size.y == 1 && manual_clamp) {
    c += "      if (!in_x0 || !in_y0) continue;\n";
  }
  c += "      int kernel_x = kernel_first_dst_x - src_as_dst_x;\n";
  c += "      int kernel_index = kernel_y * args.kernel_size_x + kernel_x;\n";
  if (weights_are_buffer) {
    c += "      int f_offset = f_base + kernel_index * "
         "args.src_tensor.Slices() * " +
         std::to_string(block_size.z) + ";\n";
  } else {
    c += "      int x_c = kernel_index * args.src_tensor.Slices();\n";
  }
  c += "      for (int s = 0; s < args.src_tensor.Slices(); ++s) {\n";
  const bool conditional_read = device_info.IsMali();
  for (int y = 0; y < block_size.y; ++y) {
    const std::string yindex = std::to_string(y);
    for (int x = 0; x < block_size.x; ++x) {
      const std::string xindex = std::to_string(x);
      const std::string id = std::to_string(y * block_size.x + x);
      if (image_buffer) {
        c += "        FLT4 src" + id + " = args.src_tensor.Read(addr_" + id +
             "); addr_" + id + " += dz_" + id + ";\n";
      } else if (manual_clamp) {
        if (conditional_read) {
          c += "        FLT4 src" + id + " = in_x" + xindex + " && in_y" +
               yindex + " ? args.src_tensor.Read(addr_" + id +
               ") : (FLT4)(0.0f); addr_" + id + " += dz;\n";
        } else {
          c += "        FLT4 src" + id + " = args.src_tensor.Read(addr_" + id +
               ") * (FLT)(in_x" + xindex + " && in_y" + yindex + "); addr_" +
               id + " += dz;\n";
        }
      } else {
        c += "        FLT4 src" + id + " = args.src_tensor.Read(sx" + xindex +
             ", sy" + yindex + ", s);\n";
      }
    }
  }
  if (weights_are_buffer) {
    c += "        __global FLT16* weights_cache = "
         "args.weights.GetPtr(f_offset);\n";
    c += "        f_offset += " + std::to_string(block_size.z) + ";\n";
  } else {
    for (int z = 0; z < block_size.z; ++z) {
      c += absl::Substitute(
          R"(        FLT4 f$1 = args.weights0.Read(dst_z + $0, x_c);
        FLT4 f$2 = args.weights1.Read(dst_z + $0, x_c);
        FLT4 f$3 = args.weights2.Read(dst_z + $0, x_c);
        FLT4 f$4 = args.weights3.Read(dst_z + $0, x_c);
)",
          z, z * 4 + 0, z * 4 + 1, z * 4 + 2, z * 4 + 3);
    }
    c += "        x_c++;\n";
  }
  for (int z = 0; z < block_size.z; ++z) {
    for (int i = 0; i < block_size.x * block_size.y; ++i) {
      c += "        CONV" + std::to_string(z) + "(r" +
           std::to_string(i + z * block_size.x * block_size.y) + ", src" +
           std::to_string(i) + ");\n";
    }
  }
  c += "      }\n";
  c += "    }\n";
  c += "  }\n";
  for (int z = 0; z < block_size.z; ++z) {
    c += "  if (dst_z < args.dst_tensor.Slices()) {\n";
    c += "    FLT4 bias_val = args.biases.Read(dst_z);\n";
    for (int y = 0; y < block_size.y; ++y) {
      for (int x = 0; x < block_size.x; ++x) {
        const std::string id =
            std::to_string((z * block_size.y + y) * block_size.x + x);
        c += "    {\n";
        c += "      int xc = dst_x + args.stride_x * " + std::to_string(x) +
             ";\n";
        c += "      int yc = dst_y + args.stride_y * " + std::to_string(y) +
             ";\n";
        c += "      if (xc < args.dst_tensor.Width() && yc < "
             "args.dst_tensor.Height()) {\n";
        c += "        FLT4 res = TO_FLT4(r" + id + ") + bias_val;\n";
        c += "        args.dst_tensor.Write(res, xc, yc, dst_z);\n";
        c += "      }\n";
        c += "    }\n";
      }
    }
    c += "  }\n";
    c += "  dst_z++;\n";
  }
  c += "}\n";
  return c;
}

int3 ConvolutionTransposed::GetGridSize() const {
  const int aligned_w = AlignByN(dst_[0]->Width(), stride_.x * block_size_.x);
  const int aligned_h = AlignByN(dst_[0]->Height(), stride_.y * block_size_.y);
  const int grid_x = DivideRoundUp(aligned_w, block_size_.x) * dst_[0]->Batch();
  const int grid_y = DivideRoundUp(aligned_h, block_size_.y);
  const int grid_z = DivideRoundUp(dst_[0]->Slices(), block_size_.z);
  return int3(grid_x, grid_y, grid_z);
}

void ConvolutionTransposed::GetPossibleKernelWorkGroups(
    TuningType tuning_type, const DeviceInfo& device_info,
    const KernelInfo& kernel_info, std::vector<int3>* work_groups) const {
  GetPossibleWorkGroupsConv(tuning_type, device_info, kernel_info, grid_size_,
                            work_groups);
}

ConvolutionTransposed CreateConvolutionTransposed(
    const DeviceInfo& device_info, const OperationDef& definition,
    const ConvolutionTransposedAttributes& attr) {
  ConvolutionTransposed result(definition, attr, device_info);

  TensorLinearDescriptor desc;
  desc.storage_type =
      DeduceLinearStorageType(definition.GetPrimaryStorageType());
  desc.element_type = definition.GetDataType();
  desc.UploadLinearData(attr.bias);
  result.args_.AddObject(
      "biases", absl::make_unique<TensorLinearDescriptor>(std::move(desc)));
  return result;
}

}  // namespace cl
}  // namespace gpu
}  // namespace tflite
