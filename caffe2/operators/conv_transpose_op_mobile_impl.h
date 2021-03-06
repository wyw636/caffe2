// conv_transpose_op_impl.h is the templated implementation of the
// conv_transpose_op.h file.
#ifndef CAFFE2_OPERATORS_CONV_TRANSPOSE_MOBILE_OP_IMPL_H_
#define CAFFE2_OPERATORS_CONV_TRANSPOSE_MOBILE_OP_IMPL_H_

#include "caffe2/core/common.h"

#ifndef CAFFE2_MOBILE
#error "mobile build state not defined"
#endif

#if CAFFE2_MOBILE

#include "caffe2/core/logging.h"
#include "caffe2/operators/conv_transpose_op_mobile.h"
#include "caffe2/utils/cpu_neon.h"
#include "caffe2/utils/fixed_divisor.h"
#include "caffe2/utils/math.h"

namespace caffe2 {

template <typename T, typename Context>
void runTileContiguous(int tileId,
                       int N, int M, int H, int W,
                       int outputH, int outputW,
                       int C, int kernelH, int kernelW,
                       int strideH, int strideW, int padT,
                       const T* filterData,
                       const T* Xdata,
                       T* colBufferData,
                       T* Ydata,
                       Context* context) {
  // The tile size is exactly the length of a single row
  int tileSize = W;

  auto kernelDataSize = C * kernelH * kernelW;
  auto currentTileStart = tileSize * tileId;

  // gemm tile
  math::GemmEx<T, Context>(
      CblasTrans,
      CblasNoTrans,
      kernelDataSize,
      tileSize,
      M,
      1,
      filterData,
      kernelDataSize,
      Xdata + currentTileStart,
      H * W,
      0,
      colBufferData,
      tileSize,
      context);

  // col2im tile
  // We assume that there is no padding in the columns (padL and padR
  // == 0).
  // FIXME: it is actually possible for us to handle padding, figure
  // out how to adjust the bounds

  // We write into Y in a de-interleaved fashion; in other words,
  // every column (mod strideW) == 0 together in one block,
  // every column (mod strideW) == 1 in another,
  // ... and so on.
  int colBlockSize = (W + kernelW / strideW);
  int numColBlocks = strideW;

  for (int c = 0; c < kernelDataSize; ++c) {
    int w_offset = c % kernelW;
    int h_offset = (c / kernelW) % kernelH;
    int c_im = c / kernelH / kernelW;

    // Each row is a separate tile that we handle. First determine the
    // row into which we are writing the output.
    // We can properly handle padding for the rows.
    int rowY = tileId * strideH - padT + h_offset;

    // If this row is out of bounds, then skip it
    if (!math::is_a_ge_zero_and_a_lt_b(rowY, outputH)) {
      continue;
    }

    // FIXME: we don't actually handle a dynamic padL > 0
    constexpr int kPadL = 0;
    int colOffsetStart = -kPadL + w_offset;
    int colBlockY = colOffsetStart % strideW;

    // However, within a block we may not start writing at offset
    // 0. The offset at which we begin writing is determined by
    // colOffsetStart
    int colWithinBlockOffsetY = colOffsetStart / strideW;

    // So, this is where we begin reading/writing in Y
    int colY = colBlockY * colBlockSize + colWithinBlockOffsetY;

    // This is the complete offset into Y from the start
    // Each row has strideW blocks of size colBlockSize
    int offsetY = rowY * colBlockSize * numColBlocks + colY;

    T* colBufferPointer =
      colBufferData + c * tileSize;
    T* yPointer =
      Ydata + c_im * outputH * (colBlockSize * numColBlocks) + offsetY;

    int b = 0;
#ifdef __ARM_NEON__
    // We vectorize the loop within the row
    {
      constexpr int kUnroll = (sizeof(float32x4_t) / sizeof(float)) * 4;
      int limit = (tileSize / kUnroll) * kUnroll;

      for (; b < limit; b += kUnroll) {
        float32x4_t cb0 = vld1q_f32(colBufferPointer + 0);
        float32x4_t cb1 = vld1q_f32(colBufferPointer + 4);
        float32x4_t cb2 = vld1q_f32(colBufferPointer + 8);
        float32x4_t cb3 = vld1q_f32(colBufferPointer + 12);

        float32x4_t y0 = vld1q_f32(yPointer + 0);
        float32x4_t y1 = vld1q_f32(yPointer + 4);
        float32x4_t y2 = vld1q_f32(yPointer + 8);
        float32x4_t y3 = vld1q_f32(yPointer + 12);

        y0 = vaddq_f32(y0, cb0);
        y1 = vaddq_f32(y1, cb1);
        y2 = vaddq_f32(y2, cb2);
        y3 = vaddq_f32(y3, cb3);

        vst1q_f32(yPointer + 0, y0);
        vst1q_f32(yPointer + 4, y1);
        vst1q_f32(yPointer + 8, y2);
        vst1q_f32(yPointer + 12, y3);

        colBufferPointer += kUnroll;
        yPointer += kUnroll;
      }
    }

    {
      constexpr int kUnroll = (sizeof(float32x4_t) / sizeof(float));
      int limit = (tileSize / kUnroll) * kUnroll;

      for (; b < limit; b += kUnroll) {
        float32x4_t cb0 = vld1q_f32(colBufferPointer);
        float32x4_t y0 = vld1q_f32(yPointer);

        y0 = vaddq_f32(y0, cb0);

        vst1q_f32(yPointer, y0);

        colBufferPointer += kUnroll;
        yPointer += kUnroll;
      }
    }
#endif

    // Handle un-vectorizable epilogue
    for (; b < tileSize; ++b) {
      *yPointer += *colBufferPointer;
      ++yPointer;
      ++colBufferPointer;
    }
  }
}

template <typename T, int N>
struct StoreInterleaved {
};

template <>
struct StoreInterleaved<float, 1> {
#ifdef __ARM_NEON__
  inline static void store(float* p, float32x4_t v[1]) {
    vst1q_f32(p, v[0]);
  }
#endif

  inline static void store(float* p, float v[1]) {
    p[0] = v[0];
  }
};

template <>
struct StoreInterleaved<float, 2> {
#ifdef __ARM_NEON__
  inline static void store(float* p, float32x4_t v[2]) {
    float32x4x2_t x = {{v[0], v[1]}};
    vst2q_f32(p, x);
  }
#endif

  inline static void store(float* p, float v[2]) {
    p[0] = v[0];
    p[1] = v[1];
  }
};

template <>
struct StoreInterleaved<float, 3> {
#ifdef __ARM_NEON__
  inline static void store(float* p, float32x4_t v[3]) {
    float32x4x3_t x = {{v[0], v[1], v[2]}};
    vst3q_f32(p, x);
  }
#endif

  inline static void store(float* p, float v[3]) {
    p[0] = v[0];
    p[1] = v[1];
    p[2] = v[2];
  }
};

template <>
struct StoreInterleaved<float, 4> {
#ifdef __ARM_NEON__
  inline static void store(float* p, float32x4_t v[4]) {
    float32x4x4_t x = {{v[0], v[1], v[2], v[3]}};
    vst4q_f32(p, x);
  }
#endif

  inline static void store(float* p, float v[4]) {
    p[0] = v[0];
    p[1] = v[1];
    p[2] = v[2];
    p[3] = v[3];
  }
};

template <int kStrideW>
void reinterleaveRows(const float* src,
                      const float* bias,
                      int c, int h,
                      float* dst,
                      int outputC, int outputH, int outputW,
                      int inputW, int kernelW, int strideW, int adjH) {
  // Each row in src is of the form:
  // [w mod strideW == 0 elements]...[w mod strideW == strideW - 1
  // elements]
  // We need to re-interleave the values and write them in the output
  int colBlockSize = inputW + kernelW / kStrideW;
  int noAdjOutputW = (inputW - 1) * kStrideW + kernelW;

  int point = c * outputH + h;
  src += point * colBlockSize * kStrideW;
  dst += point * outputW;

  float b = bias[c];
#ifdef __ARM_NEON__
  float32x4_t biasV = vdupq_n_f32(b);
#endif

  int w = 0;
#ifdef __ARM_NEON__
  constexpr int kUnroll = (sizeof(float32x4_t) / sizeof(float)) * 2;
  int limit = ((inputW - 1) / kUnroll) * kUnroll;

  for (; w < limit; w += kUnroll) {
    // We need to interleave in terms of kStrideW units
    float32x4_t v0[kStrideW];
    float32x4_t v1[kStrideW];

    for (int i = 0; i < kStrideW; ++i) {
      v0[i] = vld1q_f32(src + i * colBlockSize);
      v1[i] = vld1q_f32(src + i * colBlockSize + 4);
    }

    // add per-channel bias
    for (int i = 0; i < kStrideW; ++i) {
      v0[i] = vaddq_f32(v0[i], biasV);
      v1[i] = vaddq_f32(v1[i], biasV);
    }

    // Write interleaved into the output
    StoreInterleaved<float, kStrideW>::store(dst + 0 * kStrideW, v0);
    StoreInterleaved<float, kStrideW>::store(dst + 4 * kStrideW, v1);

    src += kUnroll;
    dst += kUnroll * kStrideW;
  }
#endif

  // Handle non-vectorizable remainder
  for (; w < inputW - 1; ++w) {
    float v[kStrideW];

    for (int i = 0; i < kStrideW; ++i) {
      v[i] = src[i * colBlockSize];
    }

    // add per-channel bias
    for (int i = 0; i < kStrideW; ++i) {
      v[i] += b;
    }

    // Write interleaved into the output
    StoreInterleaved<float, kStrideW>::store(dst, v);

    src += 1;
    dst += kStrideW;
  }

  // We have handled 0 .. (inputW - 1) * stride inclusive so far.
  // Handle the remainder
  int outputPoint = (inputW - 1) * kStrideW;
  int block = 0;

  // Output width may include adjustment into which we don't
  // write; ignore it
  while (outputPoint < noAdjOutputW) {
    float v = src[block * colBlockSize];
    dst[0] = v + b;
    ++outputPoint;
    dst += 1;

    ++block;
    if (block >= kStrideW) {
      block = 0;
      src += 1;
    }
  }

  // Remainder of the buffer comprised of just the `adj` must have
  // bias added
  for (; outputPoint < outputW; ++outputPoint) {
    dst[0] = b;
    dst += 1;
  }
}

template <int N, typename T, typename Context>
void reinterleaveMultithreaded(const Tensor<Context>& y0,
                               const Tensor<Context>& bias,
                               T* y,
                               int outputC, int outputH, int outputW,
                               int inputW,
                               int kernelW, int strideW, int adjH,
                               ThreadPool* pool) {
  // # channels times height
  size_t totalTiles = (size_t) outputC * outputH;
  FixedDivisor<int> divOutputH(outputH);

#define REINTERLEAVE(N)                                                 \
    do {                                                                \
      reinterleaveRows<N>(y0.template data<T>(),                        \
                          bias.template data<T>(),                      \
                          c, h,                                         \
                          y,                                            \
                          outputC, outputH, outputW, inputW,            \
                          kernelW, strideW, adjH);                      \
    } while (false)

  std::function<void(int, size_t)> fnReinterleave =
    [&](int threadId, size_t tileId) {
    int h;
    int c;
    divOutputH.divMod((int) tileId, c, h);

    REINTERLEAVE(N);
  };

#undef REINTERLEAVE

  pool->run(fnReinterleave, totalTiles);
}

#ifdef __ARM_NEON__
template <int N>
struct SumMultiple {
  static void sumInto(float* acc, float** toSum, size_t size);
};

template <>
struct SumMultiple<1> {
  static void sumInto(float* acc, float** toSum, size_t size) {
    constexpr int kUnroll = (sizeof(float32x4_t) / sizeof(float));
    int limit = (size / kUnroll) * kUnroll;

    auto toSum0 = toSum[0];

    size_t i = 0;
    for (; i < limit; i += kUnroll) {
      float32x4_t v0 = vld1q_f32_aligned(acc + i);
      float32x4_t v1 = vld1q_f32_aligned(toSum0 + i);

      v0 = vaddq_f32(v0, v1);

      vst1q_f32_aligned(acc + i, v0);
    }

    for (; i < size; ++i) {
      float v0 = acc[i];
      float v1 = toSum0[i];

      v0 += v1;

      acc[i] = v0;
    }
  }
};

template <>
struct SumMultiple<2> {
  static void sumInto(float* acc, float** toSum, size_t size) {
    constexpr int kUnroll = (sizeof(float32x4_t) / sizeof(float));
    int limit = (size / kUnroll) * kUnroll;

    auto toSum0 = toSum[0];
    auto toSum1 = toSum[1];

    size_t i = 0;
    for (; i < limit; i += kUnroll) {
      float32x4_t v0 = vld1q_f32_aligned(acc + i);
      float32x4_t v1 = vld1q_f32_aligned(toSum0 + i);
      float32x4_t v2 = vld1q_f32_aligned(toSum1 + i);

      v0 = vaddq_f32(v0, v1);
      v0 = vaddq_f32(v0, v2);

      vst1q_f32_aligned(acc + i, v0);
    }

    for (; i < size; ++i) {
      float v0 = acc[i];
      float v1 = toSum0[i];
      float v2 = toSum1[i];

      v0 += v1;
      v0 += v2;

      acc[i] = v0;
    }
  }
};

template <>
struct SumMultiple<3> {
  static void sumInto(float* acc, float** toSum, size_t size) {
    constexpr int kUnroll = (sizeof(float32x4_t) / sizeof(float));
    int limit = (size / kUnroll) * kUnroll;

    auto toSum0 = toSum[0];
    auto toSum1 = toSum[1];
    auto toSum2 = toSum[2];

    size_t i = 0;
    for (; i < limit; i += kUnroll) {
      float32x4_t v0 = vld1q_f32_aligned(acc + i);
      float32x4_t v1 = vld1q_f32_aligned(toSum0 + i);
      float32x4_t v2 = vld1q_f32_aligned(toSum1 + i);
      float32x4_t v3 = vld1q_f32_aligned(toSum2 + i);

      v0 = vaddq_f32(v0, v1);
      v2 = vaddq_f32(v2, v3);
      v0 = vaddq_f32(v0, v2);

      vst1q_f32_aligned(acc + i, v0);
    }

    for (; i < size; ++i) {
      float v0 = acc[i];
      float v1 = toSum0[i];
      float v2 = toSum1[i];
      float v3 = toSum2[i];

      v0 += v1;
      v2 += v3;
      v0 += v2;

      acc[i] = v0;
    }
  }
};
#endif

// Performs acc[i] += sum_j toSum_j[i] pointwise
void sumInto(float* acc,
             std::vector<float*>& toSum,
             size_t size) {
#ifdef __ARM_NEON__
  if (toSum.size() == 1) {
    SumMultiple<1>::sumInto(acc, toSum.data(), size);
    return;
  } else if (toSum.size() == 2) {
    SumMultiple<2>::sumInto(acc, toSum.data(), size);
    return;
  } else if (toSum.size() == 3) {
    SumMultiple<3>::sumInto(acc, toSum.data(), size);
    return;
  }
#endif

  // Otherwise, use fallback implementation
  EigenVectorArrayMap<float> accT(acc, size);

  for (auto p : toSum) {
    accT += ConstEigenVectorArrayMap<float>(p, size);
  }
}

template <typename T, class Context>
bool ConvTransposeMobileOp<T, Context>::RunOnDeviceWithOrderNCHW() {
  const Tensor<Context>& X = Input(INPUT);
  auto& filter = Input(FILTER);
  auto& bias = Input(BIAS);
  Tensor<Context>* Y = Output(0);
  const int N = X.dim32(0), M = X.dim32(1), H = X.dim32(2), W = X.dim32(3);
  CAFFE_ENFORCE(filter.ndim() == 4, "filter must be 4D tensor");
  CAFFE_ENFORCE(
      filter.dim32(0) == M,
      "filter number must be equal to input channel number");
  const int C = filter.dim32(1);
  CAFFE_ENFORCE(
      filter.dim32(2) == kernel_h_,
      "filter height must be equal to kernel height");
  CAFFE_ENFORCE(
      filter.dim32(3) == kernel_w_,
      "filter width must be equal to kernel width");
  CAFFE_ENFORCE(bias.ndim() == 1, "bias must be 1D tensor");
  CAFFE_ENFORCE(
      bias.dim32(0) == C,
      "bias dimension must be equal to output channel number");

  ConvTransposeUnpoolBase<Context>::SetOutputSize(X, Y, C);

  const int outputH = Y->dim32(2);
  const int outputW = Y->dim32(3);
  const int outputPlaneSize = outputH * outputW;
  const int outputBatchElementSize = Y->dim32(1) * outputPlaneSize;

  auto Xdata = X.template data<T>();
  auto Ydata = Y->template mutable_data<T>();

  auto pool = ws_->GetThreadPool();
  auto numThreads = pool->getNumThreads();

  // Initialize per-thread buffers for output
  // The main thread will write directly into the output Y, we just
  // need buffers for the worker threads
  if (threadYBuffers_.size() != numThreads) {
    threadYBuffers_.resize(numThreads);

    // Each Y buffer is not the size of Y, but instead has one of
    // these for each point modulo stride_w:
    int colBlockSize = W + kernel_w_ / stride_w_;

    for (auto& t : threadYBuffers_) {
      // Each buffer only needs to be as large as a single batch element
      t.Resize(C * outputH * colBlockSize * stride_w_);
    }
  }

  // Initialize per-thread column buffers; we need one for the current
  // (main) thread as well
  if (threadColBuffers_.size() != numThreads) {
    threadColBuffers_.resize(numThreads);
    for (auto& t : threadColBuffers_) {
      t.Resize(C, kernel_h_, kernel_w_, W); // tile size is input W
    }
  }

  std::function<void(int, size_t)> fnWork = [&](int threadId, size_t tileId) {
    auto localYData =
    threadYBuffers_[threadId].template mutable_data<T>();

    auto localColBufferData =
    threadColBuffers_[threadId].template mutable_data<T>();

    runTileContiguous<T, Context>(tileId,
                                  N, M, H, W,
                                  outputH, outputW,
                                  C, kernel_h_, kernel_w_,
                                  stride_h_, stride_w_, pad_t_,
                                  filter.template data<T>(),
                                  Xdata,
                                  localColBufferData,
                                  localYData,
                                  &context_);
  };

  // Group together thread buffers for accumulation
  std::vector<T*> toSum(threadYBuffers_.size() - 1);
  for (int i = 1; i < threadYBuffers_.size(); ++i) {
    toSum[i - 1] = threadYBuffers_[i].template mutable_data<T>();
  }

  for (auto image_id = 0; image_id < N; ++image_id) {
    // Each time through, we have to reset all per-thread output
    // buffers, since the output buffer is only per-batch element
    // The column buffers are overwritten by the matrix multiplication
    // each time, so we need not clear them out each round
    for (auto& t : threadYBuffers_) {
      math::Set<T, Context>(t.size(), 0,
                            t.template mutable_data<T>(),
                            &context_);
    }

    // Run tiled gemm and col2im in our threadpool; all of these tiles
    // are guaranteed to be full tiles
    // Each tile handles a single row of the input
    pool->run(fnWork, H);

    // We need to accumulate the per-thread results into the output
    // Y; the first worker thread (main thread) already produced its
    // results in Y
    auto& y0 = threadYBuffers_[0];
    sumInto(y0.template mutable_data<T>(), toSum, y0.size());

    // y0 now contains the final output, but it is in deinterleaved
    // form. We have to re-interleave it to produce the final form in Y
    // This operation also handles adding the per-channel bias.
#define REINTERLEAVE(N)                                                 \
    do {                                                                \
      reinterleaveMultithreaded<N, T, Context>(                         \
        y0, bias, Ydata,                                                \
        Y->dim32(1), Y->dim32(2), Y->dim32(3), W,                       \
        kernel_w_, stride_w_, this->adj_h_,                             \
        pool);                                                          \
    } while (false)

    if (stride_w_ == 1) {
      REINTERLEAVE(1);
    } else if (stride_w_ == 2) {
      REINTERLEAVE(2);
    } else if (stride_w_ == 3) {
      REINTERLEAVE(3);
    } else if (stride_w_ == 4) {
      REINTERLEAVE(4);
    }

#undef REINTERLEAVE

    Xdata += M * H * W;
    Ydata += Y->size() / Y->dim32(0);
  }

  return true;
}

template <typename T, class Context>
bool ConvTransposeMobileOp<T, Context>::RunOnDeviceWithOrderNHWC() {
  CAFFE_THROW("Not implemented.");
}

} // namespace caffe2

#endif // CAFFE2_MOBILE

#endif // CAFFE2_OPERATORS_CONV_TRANSPOSE_MOBILE_OP_IMPL_H_
