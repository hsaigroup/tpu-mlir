//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// TPU-MLIR is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//
#include "tpu_mlir/Backend/BM168x/BM1684.h"
#include "tpu_mlir/Backend/BM168x/BM168x.h"
#include "tpu_mlir/Dialect/Tpu/IR/TpuOps.h"
#include "tpu_mlir/Support/MathUtils.h"
#include "tpu_mlir/Support/Module.h"

#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

using namespace llvm;
using namespace tpu_mlir::backend;

namespace tpu_mlir {

namespace bm168x {

class GRUGlobalBuffer : public OpRewritePattern<tpu::GRUOp> {
public:
  using OpRewritePattern<tpu::GRUOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::GRUOp GRUOp,
                                PatternRewriter &rewriter) const override {
    if (!module::isNone(GRUOp.getBuffer())) {
      return failure();
    }
    auto attr = GRUOp.parseParam();
    auto type = module::getStorageType(GRUOp.getInput());
    // add buffer
    std::vector<int64_t> buffer_shape = {5, attr.batch_size, attr.hidden_size};
    auto buffer_type = RankedTensorType::get(buffer_shape, type);
    auto buffer = tpu::BufferOp::create(GRUOp, buffer_type);
    GRUOp.getBuffer().replaceUsesWithIf(buffer, [&](OpOperand &operand) {
      return operand.get() == GRUOp.getBuffer();
    });
    return success();
  }
};

class LSTMGlobalBuffer : public OpRewritePattern<tpu::LSTMOp> {
public:
  using OpRewritePattern<tpu::LSTMOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::LSTMOp lstmOp,
                                PatternRewriter &rewriter) const override {
    if (!module::isNone(lstmOp.getBuffer())) {
      return failure();
    }
    auto attr = lstmOp.parseParam();
    auto type = module::getStorageType(lstmOp.getInput());
    // add buffer
    std::vector<int64_t> buffer_shape = {5, attr.batch_size, attr.hidden_size};
    auto buffer_type = RankedTensorType::get(buffer_shape, type);
    auto buffer = tpu::BufferOp::create(lstmOp, buffer_type);
    lstmOp.getBuffer().replaceUsesWithIf(buffer, [&](OpOperand &operand) {
      return operand.get() == lstmOp.getBuffer();
    });
    return success();
  }
};

class ReduceGlobalBuffer : public OpRewritePattern<tpu::ReduceOp> {
public:
  using OpRewritePattern<tpu::ReduceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::ReduceOp reduceOp,
                                PatternRewriter &rewriter) const override {
    if (!module::isNone(reduceOp.getBuffer())) {
      return failure();
    }
    auto attr = reduceOp.parseParam();
    if (attr.simplified == false) {
      llvm_unreachable("Not Implemented");
    }
    if (module::isBM1684XFamily()) {
      auto type = module::getStorageType(reduceOp.getInput());
      auto input_spec = BM168x::get_input_spec(reduceOp);
      auto output_spec = BM168x::get_output_spec(reduceOp);
      BM168x::fix_shape(input_spec->at(0), {attr.outer_n, attr.outer_c,
                                            attr.axis_dims, attr.inner_dims});
      BM168x::fix_shape(output_spec->at(0),
                        {attr.outer_n, attr.outer_c, 1, attr.inner_dims});
      reduce_full_global_param_t param = {0};
      param.spec.common.axis_num = 1;
      param.spec.common.axis[0] = 2;
      param.spec.common.method = BM168x::get_reduce_type(reduceOp.getMode());
      param.if_getting_buffer_size = true;
      uint64_t buffer_size = 0;
      param.buffer_size_ptr = &buffer_size;
      BM168x::call_global_func("backend_api_reduce_full_global", &param,
                               sizeof(param), input_spec->data(),
                               output_spec->data());
      if (buffer_size > 0) {
        std::vector<int64_t> buffer_shape = {(int64_t)buffer_size};
        auto buffer_type = RankedTensorType::get(buffer_shape, type);
        auto buffer = tpu::BufferOp::create(reduceOp, buffer_type);
        reduceOp.getBuffer().replaceUsesWithIf(buffer, [&](OpOperand &operand) {
          return operand.get() == reduceOp.getBuffer();
        });
      }
    } else if (module::isBM1684Family()) {
      auto type = module::getStorageType(reduceOp.getInput());
      auto in_addr = module::getAddress(reduceOp.getInput());
      auto out_addr = module::getAddress(reduceOp.getOutput());
      int input_dims = module::getShape(reduceOp.getInput()).size();
      uint64_t buffer_size = 0;
      uint64_t *buffer_size_ptr = &buffer_size;
      uint32_t *input_shape = new uint32_t[MAX_SHAPE_DIMS];
      for (auto v : llvm::enumerate(module::getShape(reduceOp.getInput())))
        input_shape[v.index()] = (uint32_t)v.value();
      auto &&axes = reduceOp.getAxes();
      int axis_num = axes.size();
      int axis_list[axis_num];
      for (int i = 0; i < axes.size(); i++)
        axis_list[i] = (axes[i].cast<IntegerAttr>().getInt());
      int method = BM1684::get_reduce_type(reduceOp.getMode());
      uint64_t buffer_addr = module::getAddress(reduceOp.getBuffer());
      if (BM1684::getDataType(reduceOp.getInput()) == DTYPE_FP32 ||
          BM1684::getDataType(reduceOp.getInput()) == DTYPE_INT32 ||
          BM1684::getDataType(reduceOp.getInput()) == DTYPE_UINT32) {
        BM1684::instance().dl_nodechip_reduce_full_v3(
            in_addr, out_addr, (const uint32_t *)input_shape, input_dims,
            axis_list, axis_num, method, buffer_addr, buffer_size_ptr,
            (CMD_ID_NODE *)BM1684::instance().cmdid_node);
      } else {
        int keep_dims = reduceOp.getKeepdims() ? 1 : 0;
        buffer_size =
            BM1684::instance().dl_nodechip_reduce_get_buffer_size_fix8b(
                input_shape, input_dims, axis_list, axis_num, method,
                keep_dims);
      }
      if (buffer_size > 0) {
        std::vector<int64_t> buffer_shape = {(int64_t)buffer_size};
        auto buffer_type = RankedTensorType::get(buffer_shape, type);
        auto buffer = tpu::BufferOp::create(reduceOp, buffer_type);
        reduceOp.getBuffer().replaceUsesWithIf(buffer, [&](OpOperand &operand) {
          return operand.get() == reduceOp.getBuffer();
        });
      }
    }

    return success();
  }
};

class SliceGlobalBuffer : public OpRewritePattern<tpu::SliceOp> {
public:
  using OpRewritePattern<tpu::SliceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::SliceOp sliceOp,
                                PatternRewriter &rewriter) const override {
    if (!module::isNone(sliceOp.getBuffer())) {
      return failure();
    }
    if (!module::isBM1684Family()) {
      return failure();
    }
    if (!module::isUniformQuantized(sliceOp.getInput())) {
      return failure();
    }
    if (!module::isUniformQuantized(sliceOp.getOutput())) {
      return failure();
    }
    auto p = sliceOp.parseParam();
    uint64_t buffer_size = 0;
    int shape_dim = module::getShape(sliceOp.getInput()).size();
    // melloc
    int *input_shape = new int[MAX_SHAPE_DIMS];
    int *begin_index = new int[MAX_SHAPE_DIMS];
    int *end_index = new int[MAX_SHAPE_DIMS];
    int *stride = new int[MAX_SHAPE_DIMS];
    // assign param and call func to get buffer size
    module::getGlobalShape(sliceOp.getInput(), input_shape);
    for (int i = 0; i < shape_dim; ++i) {
      begin_index[i] = p.offset_4[i];
      end_index[i] = p.os_4[i] * p.step_4[i] + p.offset_4[i];
      stride[i] = p.step_4[i];
    }
    BM1684::instance().dl_nodechip_stride_slice_fix8b(
        0, 0, 0, 0, &buffer_size, input_shape, shape_dim, STORE_MODE_4N,
        STORE_MODE_4N, 0, 0, begin_index, end_index, stride, 0, NULL);
    // release
    delete[] input_shape;
    delete[] begin_index;
    delete[] end_index;
    delete[] stride;
    // create bufferOp
    std::vector<int64_t> buffer_shape = {(int64_t)buffer_size};
    auto type = module::getStorageType(sliceOp.getOutput());
    auto buffer_type = RankedTensorType::get(buffer_shape, type);
    auto buffer = tpu::BufferOp::create(sliceOp, buffer_type);
    sliceOp.getBuffer().replaceUsesWithIf(buffer, [&](OpOperand &operand) {
      return operand.get() == sliceOp.getBuffer() &&
             operand.getOwner() == sliceOp;
    });
    return success();
  }
};

class ReshapeGlobalBuffer : public OpRewritePattern<tpu::ReshapeOp> {
public:
  using OpRewritePattern<tpu::ReshapeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::ReshapeOp reshapeOp,
                                PatternRewriter &rewriter) const override {
    //only used for 4N ndim reshape!
    if (!module::isBM1684Family()) {
      return failure();
    }
    if (!module::isNone(reshapeOp.getBuffer())) {
      return failure();
    }
    if (!module::isUniformQuantized(reshapeOp.getInput())) {
      return failure();
    }
    if (!module::isUniformQuantized(reshapeOp.getOutput())) {
      return failure();
    }
    int64_t in, ic, ih, iw, on, oc, oh, ow;
    module::getNCHW(reshapeOp.getInput(), in, ic, ih, iw);
    module::getNCHW(reshapeOp.getOutput(), on, oc, oh, ow);
    if (on == in) {
      return failure();
    }

    uint64_t buffer_size = on * oc * oh * ow;;

    // create bufferOp
    std::vector<int64_t> buffer_shape = {(int64_t)buffer_size};
    auto type = module::getStorageType(reshapeOp.getOutput());
    auto buffer_type = RankedTensorType::get(buffer_shape, type);
    auto buffer = tpu::BufferOp::create(reshapeOp, buffer_type);
    reshapeOp.getBuffer().replaceUsesWithIf(buffer, [&](OpOperand &operand) {
      return operand.get() == reshapeOp.getBuffer() && operand.getOwner() == reshapeOp;
    });
    return success();
  }
};
class SoftmaxGlobalBuffer : public OpRewritePattern<tpu::SoftmaxOp> {
public:
  using OpRewritePattern<tpu::SoftmaxOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::SoftmaxOp softmaxOp,
                                PatternRewriter &rewriter) const override {
    if (!module::isNone(softmaxOp.getBuffer())) {
      return failure();
    }
    if (!module::isBM1684Family()) {
      return failure();
    }
    if (!module::isUniformQuantized(softmaxOp.getInput())) {
      return failure();
    }
    int64_t n, c, h, w;
    module::getNCHW(softmaxOp.getInput(), n, c, h, w);
    std::vector<int64_t> buffer_shape = {ceiling_func(n, (int64_t)4), c, h, w};
    auto type = module::getStorageType(softmaxOp.getOutput());
    auto buffer_type = RankedTensorType::get(buffer_shape, type);
    auto buffer = tpu::BufferOp::create(softmaxOp, buffer_type);
    softmaxOp.getBuffer().replaceUsesWithIf(buffer, [&](OpOperand &operand) {
      return operand.get() == softmaxOp.getBuffer();
    });
    return success();
  }
};

class PermuteGlobalBuffer : public OpRewritePattern<tpu::PermuteOp> {
public:
  using OpRewritePattern<tpu::PermuteOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::PermuteOp permuteOp,
                                PatternRewriter &rewriter) const override {
    if (!module::isNone(permuteOp.getBuffer())) {
      return failure();
    }
    uint64_t buffer_size = 0;
    auto attr = permuteOp.parseParam();
    if (module::isBM1684XFamily()) {
      transpose_param_t param = {0};
      param.if_getting_buffer_size = 1;
      for (int i = 0, n = attr.order_fix.size(); i < n; ++i) {
        param.spec.order[i] = attr.order_fix[i];
      }
      param.buffer_size_ptr = &buffer_size;
      auto input_spec = BM168x::get_input_spec(permuteOp);
      auto output_spec = BM168x::get_output_spec(permuteOp);
      BM168x::fix_shape(input_spec->at(0), attr.in_shape_fix);
      BM168x::fix_shape(output_spec->at(0), attr.out_shape_fix);
      BM168x::call_global_func("backend_api_transpose_global", &param,
                               sizeof(param), input_spec->data(),
                               output_spec->data());
    } else if (module::isBM1684Family()) {
      // melloc
      uint32_t *input_shape = new uint32_t[MAX_SHAPE_DIMS];
      int *order = new int[MAX_SHAPE_DIMS];
      uint64_t *buffer_size_ptr = &buffer_size;

      auto input = permuteOp.getInput();
      auto output = permuteOp.getOutput();
      i32_array_t in_order = module::getI32Array(permuteOp.getOrder());
      auto input_addr = module::getAddress(input);
      auto output_addr = module::getAddress(output);
      int input_dims = module::getShape(input).size();
      auto input_dtype = BM1684::getDataType(input);
      auto output_dtype = BM1684::getDataType(output);
      int type_len = BM1684::getFmtBytes(input_dtype);
      int store_mode;
      for (auto v : llvm::enumerate(module::getShape(input)))
        input_shape[v.index()] = (uint32_t)v.value();
      memcpy(order, in_order->data(), (*in_order).size() * sizeof(int));
      if (input_dtype == DTYPE_FP32 || input_dtype == DTYPE_INT32 ||
          input_dtype == DTYPE_UINT32) {
        store_mode = STORE_MODE_1N;
        BM1684::instance().dl_nodechip_transpose(
            input_addr, output_addr, input_shape, order, input_dims, type_len,
            store_mode, 0, buffer_size_ptr,
            (CMD_ID_NODE *)BM1684::instance().cmdid_node);
      } else if (input_dtype == DTYPE_INT8 || input_dtype == DTYPE_UINT8) {
        store_mode = STORE_MODE_4N;
        assert(output_dtype == DTYPE_INT8 || output_dtype == DTYPE_UINT8);
        BM1684::instance().dl_nodechip_transpose_fix8b(
            input_addr, output_addr, input_shape, order, input_dims, store_mode,
            store_mode, 0, buffer_size_ptr,
            (CMD_ID_NODE *)BM1684::instance().cmdid_node);
      } else {
        llvm_unreachable("Not Implemented");
        return failure();
      }
      // release
      delete[] input_shape;
      delete[] order;
      buffer_size_ptr = NULL;
      ;
    } else {
      return failure();
    }
    auto type = module::getStorageType(permuteOp.getInput());
    // add buffer
    if (buffer_size > 0) {
      auto buffer_type = RankedTensorType::get({(int64_t)buffer_size}, type);
      auto buffer = tpu::BufferOp::create(permuteOp, buffer_type);
      permuteOp.setOperand(1, buffer);
      return success();
    }
    return failure();
  }
};

class NonZeroGlobalBuffer : public OpRewritePattern<tpu::NonZeroOp> {
public:
  using OpRewritePattern<tpu::NonZeroOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::NonZeroOp op,
                                PatternRewriter &rewriter) const override {
    if (!module::isNone(op.getBuffer())) {
      return failure();
    }
    auto type = module::getStorageType(op.getInput());
    // add buffer
    auto buffer_type =
        RankedTensorType::get(module::getShape(op.getInput()), type);
    auto buffer = tpu::BufferOp::create(op, buffer_type);
    op.getBuffer().replaceUsesWithIf(buffer, [&](OpOperand &operand) {
      return operand.get() == op.getBuffer();
    });
    return success();
  }
};

class InterpGlobalBuffer : public OpRewritePattern<tpu::InterpOp> {
public:
  using OpRewritePattern<tpu::InterpOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::InterpOp interpOp,
                                PatternRewriter &rewriter) const override {
    if (!module::isBM1686())
      return failure();

    if (!module::isNone(interpOp.getBuffer())) {
      return failure();
    }

    interp_global_param_t param = {0};
    param.if_getting_buffer_size = true;
    uint64_t buffer_size = 0;
    param.buffer_size_ptr = &buffer_size;
    auto input_spec = BM168x::get_input_spec(interpOp);
    auto output_spec = BM168x::get_output_spec(interpOp);
    BM168x::call_global_func("backend_api_interp_global", &param, sizeof(param),
                             input_spec->data(), output_spec->data());
    // add buffer
    if (buffer_size > 0) {
      auto type = ::mlir::Builder(getContext()).getIntegerType(8);
      auto buffer_type = RankedTensorType::get({(int64_t)buffer_size}, type);
      auto buffer = tpu::BufferOp::create(interpOp, buffer_type);
      interpOp.setOperand(1, buffer);
      return success();
    }

    return failure();
  }
};

class DeformGatherGlobalBuffer : public OpRewritePattern<tpu::DeformGatherOp> {
public:
  using OpRewritePattern<tpu::DeformGatherOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::DeformGatherOp Op,
                                PatternRewriter &rewriter) const override {
    if (!module::isNone(Op.getBuffer())) {
      return failure();
    }
    if (!module::isBM1684XFamily()) {
      return failure();
    }
    deform_gather_attr_t p = Op.parseParam();
    uint64_t buffer_size = 0;
    auto conved_H = ((p.ih - (p.dh * (p.kh - 1) + 1) + p.pht + p.phb) / p.sh + 1);
    auto conved_W = ((p.iw - (p.dw * (p.kw - 1) + 1) + p.pwl + p.pwr) / p.sw + 1);
    auto full_size = p.kh * p.kw * conved_H * conved_W * sizeof(float);
    buffer_size = 2 * p.n * p.deform_groups * full_size;
    if (p.use_mask)
      buffer_size = std::max(buffer_size, (uint64_t) p.n * p.ic * p.deform_groups * full_size);
    auto type = module::getStorageType(Op.getInput());
    auto buffer_type = RankedTensorType::get({(int64_t) buffer_size}, type);
    auto buffer = tpu::BufferOp::create(Op, buffer_type);
    Op.getBuffer().replaceUsesWithIf(buffer, [&](OpOperand &operand) {
      return operand.get() == Op.getBuffer();
    });
    return success();
  }
};

class TileGlobalBuffer : public OpRewritePattern<tpu::TileOp> {
public:
  using OpRewritePattern<tpu::TileOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::TileOp tileOp,
                                PatternRewriter &rewriter) const override {
    if (!module::isNone(tileOp.getBuffer())) {
      return failure();
    }
    if (module::isBM1684Family()) {
      auto input_shape = module::getShape(tileOp.getInput());
      int input_dim = input_shape.size();
      uint32_t in_shape[input_dim];

      for (int i =0 ;i < input_dim ; i++) {
        in_shape[i] = input_shape[i];
      }
      auto output_shape = module::getShape(tileOp.getOutput());
      int tile_coeff[8];
      auto type = module::getStorageType(tileOp.getInput());
      int64_t type_len = module::getDtypeSize(tileOp.getInput());
      uint64_t buffer_size = 0;
      int tile_count = 0;
      int max_tile = 1;
      uint64_t total_size = 1;
      if (type_len == 4) {
        for (int i = 0; i < input_dim; ++i) {
          tile_coeff[i] =
              output_shape[i] < 0 ? 1 : output_shape[i] / input_shape[i];
          if (tile_coeff[i] > 1)
            tile_count++;
          if (tile_coeff[i] > max_tile)
            max_tile = tile_coeff[i];
            total_size *= output_shape[i];
        }
        if (tile_count > 1) {
          buffer_size = total_size / max_tile * type_len;
        }
        if (buffer_size > 0) {
          std::vector<int64_t> buffer_shape = {(int64_t)buffer_size};
          auto buffer_type = RankedTensorType::get(buffer_shape, type);
          auto buffer = tpu::BufferOp::create(tileOp, buffer_type);
          tileOp.getBuffer().replaceUsesWithIf(
              buffer, [&](OpOperand &operand) {
                return operand.get() == tileOp.getBuffer();
              });
        }
      }
      else if (type_len == 1) {
        auto input = tileOp.getInput();
        auto output = tileOp.getOutput();
        auto input_dtype = BM1684::getDataType(input);
        auto input_format = BM168x::getGdmaFormat(input_dtype);
        auto output_dtype = BM1684::getDataType(output);
        auto output_format = BM168x::getGdmaFormat(output_dtype);
        BM1684::instance().dl_nodechip_tile_full_fix8b(
            0, 0, 0, &buffer_size, (const uint32_t *)in_shape, (const int *)tile_coeff, input_dim, input_format, output_format, 0,
            (CMD_ID_NODE *)BM1684::instance().cmdid_node);
        if (buffer_size > 0) {
          std::vector<int64_t> buffer_shape = {(int64_t)buffer_size};
          auto buffer_type = RankedTensorType::get(buffer_shape, type);
          auto buffer = tpu::BufferOp::create(tileOp, buffer_type);
          tileOp.getBuffer().replaceUsesWithIf(
              buffer, [&](OpOperand &operand) {
                return operand.get() == tileOp.getBuffer();
              });
        }
      } else {
          llvm_unreachable("Not Implemented");
        }
        return success();
      }
      return failure();;
    }
  };

class PadGlobalBuffer : public OpRewritePattern<tpu::PadOp> {
public:
  using OpRewritePattern<tpu::PadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::PadOp padOp,
                                PatternRewriter &rewriter) const override {
    if (!module::isNone(padOp.getBuffer())) {
      return failure();
    }
    if (!module::isBM1684Family()) {
      return failure();
    }
    std::vector<int64_t> shape = module::getShape(padOp.getInput());
    if (shape.size() < 5 && !module::isUniformQuantized(padOp.getInput())) {
      return failure();
    }
    if (shape.size() == 3) {
      std::vector<int64_t> buffer_shape = {ceiling_func(shape[0], (int64_t)4),
                                           shape[1], shape[2]};
      auto type = module::getStorageType(padOp.getOutput());
      auto buffer_type = RankedTensorType::get(buffer_shape, type);
      auto buffer = tpu::BufferOp::create(padOp, buffer_type);
      padOp.getBuffer().replaceUsesWithIf(buffer, [&](OpOperand &operand) {
        return operand.get() == padOp.getBuffer();
      });
      return success();
    } else if (shape.size() == 4) {
      std::vector<int64_t> buffer_shape = {ceiling_func(shape[0], (int64_t)4),
                                           shape[1], shape[2], shape[3]};
      auto type = module::getStorageType(padOp.getOutput());
      auto buffer_type = RankedTensorType::get(buffer_shape, type);
      auto buffer = tpu::BufferOp::create(padOp, buffer_type);
      padOp.getBuffer().replaceUsesWithIf(buffer, [&](OpOperand &operand) {
        return operand.get() == padOp.getBuffer();
      });
      return success();
    } else if (shape.size() == 5) {
      std::vector<int64_t> buffer_shape = {ceiling_func(shape[0], (int64_t)4),
                                           shape[1], shape[2], shape[3],
                                           shape[4]};
      auto type = module::getStorageType(padOp.getOutput());
      auto buffer_type = RankedTensorType::get(buffer_shape, type);
      auto buffer = tpu::BufferOp::create(padOp, buffer_type);
      padOp.getBuffer().replaceUsesWithIf(buffer, [&](OpOperand &operand) {
        return operand.get() == padOp.getBuffer();
      });
      return success();
    } else {
      return failure();
    }
  }
};

class GatherGlobalBuffer : public OpRewritePattern<tpu::GatherOp> {
public:
  using OpRewritePattern<tpu::GatherOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::GatherOp GatherOp,
                                PatternRewriter &rewriter) const override {
    if (!module::isNone(GatherOp.getBuffer())) {
      return failure();
    }
    if (!module::isBM1684Family()) {
      return failure();
    }
    // only dynamic index_select need buffer
    if (module::isWeight(GatherOp.getIndices())) {
      return failure();
    }
    auto elment_num = module::getNumElements(GatherOp.getInput());
    auto type = module::getStorageType(GatherOp.getInput());
    // add buffer
    std::vector<int64_t> buffer_shape = {2, elment_num}; // double buffer
    auto buffer_type = RankedTensorType::get(buffer_shape, type);
    auto buffer = tpu::BufferOp::create(GatherOp, buffer_type);
    GatherOp.getBuffer().replaceUsesWithIf(buffer, [&](OpOperand &operand) {
      return operand.get() == GatherOp.getBuffer();
    });
    return success();
  }
};

} // namespace bm168x
namespace tpu {
using namespace bm168x;
void populateGlobalBufferBM168xPatterns(RewritePatternSet *patterns) {
  // clang-format off
  patterns->add<
      GatherGlobalBuffer,
      GRUGlobalBuffer,
      LSTMGlobalBuffer,
      ReduceGlobalBuffer,
      SliceGlobalBuffer,
      ReshapeGlobalBuffer,
      SoftmaxGlobalBuffer,
      PermuteGlobalBuffer,
      InterpGlobalBuffer,
      NonZeroGlobalBuffer,
      DeformGatherGlobalBuffer,
      TileGlobalBuffer,
      PadGlobalBuffer
  >(patterns->getContext());
  // clang-format on
}

} // namespace tpu
} // namespace tpu_mlir
