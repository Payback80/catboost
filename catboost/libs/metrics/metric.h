#pragma once

#include "metric_holder.h"
#include "ders_holder.h"
#include "pfound.h"

#include <catboost/libs/data_types/pair.h>
#include <catboost/libs/data_types/query.h>
#include <catboost/libs/options/loss_description.h>
#include <catboost/libs/options/metric_options.h>

#include <library/threading/local_executor/local_executor.h>
#include <library/containers/2d_array/2d_array.h>

#include <util/generic/hash.h>
#include <catboost/libs/options/loss_description.h>
#include <catboost/libs/options/metric_options.h>

inline constexpr double GetDefaultClassificationBorder() {
    return 0.5;
}

enum class EMetricBestValue {
    Max,
    Min,
    FixedValue,
    Undefined
};

struct TCustomMetricDescriptor {
    void* CustomData;
    TMetricHolder (*EvalFunc)(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        int begin,
        int end,
        void* customData
    ) = nullptr;
    TString (*GetDescriptionFunc)(void* customData) = nullptr;
    bool (*IsMaxOptimalFunc)(void* customData) = nullptr;
    double (*GetFinalErrorFunc)(const TMetricHolder& error, void* customData) = nullptr;
};

struct TCustomObjectiveDescriptor {
    void* CustomData;
    void (*CalcDersRange)(
        int count,
        const double* approxes,
        const float* targets,
        const float* weights,
        TDer1Der2* ders,
        void* customData
    ) = nullptr;
    void (*CalcDersMulti)(
        const TVector<double>& approx,
        float target,
        float weight,
        TVector<double>* ders,
        TArray2D<double>* der2,
        void* customData
    ) = nullptr;
};

struct IMetric {
    virtual TMetricHolder Eval(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end,
        NPar::TLocalExecutor& executor
    ) const = 0;
    virtual TMetricHolder EvalPairwise(
        const TVector<TVector<double>>& approx,
        const TVector<TPair>& pairs,
        int begin,
        int end
    ) const = 0;
    virtual TString GetDescription() const = 0;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const = 0;
    virtual EErrorType GetErrorType() const = 0;
    virtual double GetFinalError(const TMetricHolder& error) const = 0;
    virtual bool IsAdditiveMetric() const = 0;
    virtual ~IMetric()
    {
    }
};

struct TMetric: public IMetric {
    virtual TMetricHolder EvalPairwise(
        const TVector<TVector<double>>& approx,
        const TVector<TPair>& pairs,
        int begin,
        int end
    ) const override;
    virtual EErrorType GetErrorType() const override;
    virtual double GetFinalError(const TMetricHolder& error) const override;
};

template <class TImpl>
struct TAdditiveMetric: public TMetric {
    TMetricHolder Eval(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end,
        NPar::TLocalExecutor& executor
    ) const final {
        NPar::TLocalExecutor::TExecRangeParams blockParams(begin, end);
        blockParams.SetBlockCount(executor.GetThreadCount() + 1);
        const int blockSize = blockParams.GetBlockSize();
        const ui32 blockCount = blockParams.GetBlockCount();
        TVector<TMetricHolder> results(blockCount);
        NPar::ParallelFor(executor, 0, blockCount, [&](int blockId) {
            const int from = begin + blockId * blockSize;
            const int to = Min<int>(begin + (blockId + 1) * blockSize, end);
            Y_ASSERT(from < to);
            results[blockId] = static_cast<const TImpl*>(this)->EvalSingleThread(approx, target, weight, queriesInfo, from, to);
        });

        TMetricHolder result;
        for (const auto& partResult : results) {
            result.Add(partResult);
        }
        return result;
    }

    bool IsAdditiveMetric() const final {
        return true;
    }
};

struct TNonAdditiveMetric: public TMetric {
    bool IsAdditiveMetric() const final {
        return false;
    }
};

struct TPairwiseMetric : public IMetric {
    virtual TMetricHolder Eval(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end,
        NPar::TLocalExecutor& executor
    ) const override;
    virtual EErrorType GetErrorType() const override;
    virtual double GetFinalError(const TMetricHolder& error) const override;
};

struct TPairwiseAdditiveMetric : public TPairwiseMetric {
    bool IsAdditiveMetric() const final {
        return true;
    }
};

struct TCrossEntropyMetric: public TAdditiveMetric<TCrossEntropyMetric> {
    explicit TCrossEntropyMetric(ELossFunction lossFunction, double border = GetDefaultClassificationBorder());
    TMetricHolder EvalSingleThread(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end
    ) const;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
private:
    ELossFunction LossFunction;
    double Border = GetDefaultClassificationBorder();
};

class TCtrFactorMetric : public TAdditiveMetric<TCtrFactorMetric> {
public:
    explicit TCtrFactorMetric(double border = GetDefaultClassificationBorder()) : Border(border) {}
    TMetricHolder EvalSingleThread(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end
    ) const;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
private:
    double Border;
};

struct TRMSEMetric: public TAdditiveMetric<TRMSEMetric> {
    TMetricHolder EvalSingleThread(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end
    ) const;
    virtual TString GetDescription() const override;
    virtual double GetFinalError(const TMetricHolder& error) const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
};

class TQuantileMetric : public TAdditiveMetric<TQuantileMetric> {
public:
    explicit TQuantileMetric(ELossFunction lossFunction, double alpha = 0.5);
    TMetricHolder EvalSingleThread(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end
    ) const;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
private:
    ELossFunction LossFunction;
    double Alpha;
};

class TLogLinQuantileMetric : public TAdditiveMetric<TLogLinQuantileMetric> {
public:
    explicit TLogLinQuantileMetric(double alpha = 0.5);
    TMetricHolder EvalSingleThread(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end
    ) const;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
private:
    double Alpha;
};

struct TMAPEMetric : public TAdditiveMetric<TMAPEMetric> {
    TMetricHolder EvalSingleThread(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end
    ) const;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
};

struct TPoissonMetric : public TAdditiveMetric<TPoissonMetric> {
    TMetricHolder EvalSingleThread(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end
    ) const;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
};

struct TMultiClassMetric : public TAdditiveMetric<TMultiClassMetric> {
    TMetricHolder EvalSingleThread(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end
    ) const;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
};

struct TMultiClassOneVsAllMetric : public TAdditiveMetric<TMultiClassOneVsAllMetric> {
    TMetricHolder EvalSingleThread(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end
    ) const;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
};

struct TPairLogitMetric : public TPairwiseAdditiveMetric {
    virtual TMetricHolder EvalPairwise(
        const TVector<TVector<double>>& approx,
        const TVector<TPair>& pairs,
        int begin,
        int end
    ) const override;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
};

struct TQueryRMSEMetric : public TAdditiveMetric<TQueryRMSEMetric> {
    TMetricHolder EvalSingleThread(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int queryStartIndex,
        int queryEndIndex
    ) const;
    virtual EErrorType GetErrorType() const override;
    virtual double GetFinalError(const TMetricHolder& error) const override;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
private:
    double CalcQueryAvrg(
        int start,
        int count,
        const TVector<double>& approxes,
        const TVector<float>& targets,
        const TVector<float>& weights
    ) const;
};

struct TPFoundMetric : public TAdditiveMetric<TPFoundMetric> {
    explicit TPFoundMetric(int topSize = -1, double decay = 0.85);
    TMetricHolder EvalSingleThread(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int queryStartIndex,
        int queryEndIndex
    ) const;
    virtual EErrorType GetErrorType() const override;
    virtual double GetFinalError(const TMetricHolder& error) const override;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
private:
    int TopSize;
    double Decay;
};

struct TQuerySoftMaxMetric : public TAdditiveMetric<TQuerySoftMaxMetric> {
    TMetricHolder EvalSingleThread(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int queryStartIndex,
        int queryEndIndex
    ) const;
    virtual EErrorType GetErrorType() const override;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
private:
    TMetricHolder EvalSingleQuery(
        int start,
        int count,
        const TVector<double>& approxes,
        const TVector<float>& targets,
        const TVector<float>& weights,
        TVector<double>* softmax
    ) const;
};

struct TR2Metric: public TNonAdditiveMetric {
    virtual TMetricHolder Eval(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end,
        NPar::TLocalExecutor& executor
    ) const override;
    virtual TString GetDescription() const override;
    virtual double GetFinalError(const TMetricHolder& error) const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
};

struct TAUCMetric: public TNonAdditiveMetric {
    explicit TAUCMetric(double border = GetDefaultClassificationBorder())
        : Border(border)
    {
    }
    explicit TAUCMetric(int positiveClass);
    virtual TMetricHolder Eval(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end,
        NPar::TLocalExecutor& executor) const override;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
private:
    int PositiveClass = 1;
    bool IsMultiClass = false;
    double Border = GetDefaultClassificationBorder();
};

struct TAccuracyMetric : public TAdditiveMetric<TAccuracyMetric> {
    explicit TAccuracyMetric(double border = GetDefaultClassificationBorder())
        : Border(border)
    {
    }
    TMetricHolder EvalSingleThread(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end
    ) const;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
private:
    double Border = GetDefaultClassificationBorder();
};

struct TPrecisionMetric : public TNonAdditiveMetric {
    explicit TPrecisionMetric(double border = GetDefaultClassificationBorder())
        : Border(border)
    {
    }
    explicit TPrecisionMetric(int positiveClass);
    virtual TMetricHolder Eval(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end,
        NPar::TLocalExecutor& executor
    ) const override;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
private:
    int PositiveClass = 1;
    bool IsMultiClass = false;
    double Border = GetDefaultClassificationBorder();
};

struct TRecallMetric: public TNonAdditiveMetric {
    explicit TRecallMetric(double border = GetDefaultClassificationBorder())
        : Border(border)
    {
    }
    explicit TRecallMetric(int positiveClass);
    virtual TMetricHolder Eval(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end,
        NPar::TLocalExecutor& executor
    ) const override;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
private:
    int PositiveClass = 1;
    bool IsMultiClass = false;
    double Border = GetDefaultClassificationBorder();
};

struct TF1Metric: public TNonAdditiveMetric {
    static THolder<TF1Metric> CreateF1Multiclass(int positiveClass);
    static THolder<TF1Metric> CreateF1BinClass(double border = GetDefaultClassificationBorder());
    virtual TMetricHolder Eval(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end,
        NPar::TLocalExecutor& executor
    ) const override;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
private:
    TF1Metric()
    {
    }
private:
    int PositiveClass = 1;
    bool IsMultiClass = false;
    double Border = GetDefaultClassificationBorder();
};

struct TTotalF1Metric : public TNonAdditiveMetric {
    virtual TMetricHolder Eval(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end,
        NPar::TLocalExecutor& executor
    ) const override;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
};

struct TMCCMetric : public TNonAdditiveMetric {
    virtual TMetricHolder Eval(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end,
        NPar::TLocalExecutor& executor
    ) const override;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
};

struct TPairAccuracyMetric : public TPairwiseAdditiveMetric {
    virtual TMetricHolder EvalPairwise(
        const TVector<TVector<double>>& approx,
        const TVector<TPair>& pairs,
        int begin,
        int end
    ) const override;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
};

class TCustomMetric: public IMetric {
public:
    explicit TCustomMetric(const TCustomMetricDescriptor& descriptor);
    virtual TMetricHolder Eval(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end,
        NPar::TLocalExecutor& executor
    ) const override;
    virtual TMetricHolder EvalPairwise(
        const TVector<TVector<double>>& approx,
        const TVector<TPair>& pairs,
        int begin,
        int end
    ) const override;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
    virtual EErrorType GetErrorType() const override;
    virtual double GetFinalError(const TMetricHolder& error) const override;
    //we don't now anything about custom metrics
    bool IsAdditiveMetric() const final {
        return false;
    }
private:
    TCustomMetricDescriptor Descriptor;
};

class TUserDefinedPerObjectMetric : public TMetric {
public:
    explicit TUserDefinedPerObjectMetric(const TMap<TString, TString>& params);
    virtual TMetricHolder Eval(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int begin,
        int end,
        NPar::TLocalExecutor& executor
    ) const override;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
    bool IsAdditiveMetric() const final {
        return true;
    }
private:
    double Alpha;
};

class TUserDefinedQuerywiseMetric : public TAdditiveMetric<TUserDefinedQuerywiseMetric> {
public:
    explicit TUserDefinedQuerywiseMetric(const TMap<TString, TString>& params);
    TMetricHolder EvalSingleThread(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int queryStartIndex,
        int queryEndIndex
    ) const;
    virtual EErrorType GetErrorType() const override;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
private:
    double Alpha;
};

class TQueryAverage : public TAdditiveMetric<TQueryAverage> {
public:
    explicit TQueryAverage(float topSize) : TopSize(topSize) {
        CB_ENSURE(topSize > 0, "top size for QueryAverage should be greater than 0");
        CB_ENSURE(topSize == (int)topSize, "top size for QueryAverage should be an integer value");
    }

    TMetricHolder EvalSingleThread(
        const TVector<TVector<double>>& approx,
        const TVector<float>& target,
        const TVector<float>& weight,
        const TVector<TQueryInfo>& queriesInfo,
        int queryStartIndex,
        int queryEndIndex
    ) const;
    virtual EErrorType GetErrorType() const override;
    virtual TString GetDescription() const override;
    virtual void GetBestValue(EMetricBestValue* valueType, float* bestValue) const override;
private:
    int TopSize;
};


TVector<THolder<IMetric>> CreateMetricFromDescription(const NCatboostOptions::TLossDescription& description, int approxDimension);
TVector<THolder<IMetric>> CreateMetricFromDescription(const TString& description, int approxDimension);
TVector<TString> GetMetricsDescription(const TVector<THolder<IMetric>>& metrics);
TVector<THolder<IMetric>> CreateMetrics(
    const NCatboostOptions::TOption<NCatboostOptions::TLossDescription>& lossFunctionOption,
    const NCatboostOptions::TCpuOnlyOption<NCatboostOptions::TMetricOptions>& evalMetricOptions,
    const TMaybe<TCustomMetricDescriptor>& evalMetricDescriptor,
    int approxDimension
);


inline TVector<THolder<IMetric>> CreateMetricsFromDescription(const TVector<TString>& description, int approxDim) {
    TVector<THolder<IMetric>> metrics;
    for (const auto& metricDescription : description) {
        auto metricsBatch = CreateMetricFromDescription(metricDescription, approxDim);
        for (ui32 i = 0; i < metricsBatch.size(); ++i) {
            metrics.push_back(std::move(metricsBatch[i]));
        }
    }
    return metrics;
}

double EvalErrors(
    const TVector<TVector<double>>& avrgApprox,
    const TVector<float>& target,
    const TVector<float>& weight,
    const TVector<TQueryInfo>& queriesInfo,
    const TVector<TPair>& pairs,
    const THolder<IMetric>& error,
    int queryStartIndex,
    int queryEndIndex,
    int begin,
    int end,
    NPar::TLocalExecutor* localExecutor
);

inline bool IsMaxOptimal(const IMetric& metric) {
    EMetricBestValue bestValueType;
    float bestPossibleValue;
    metric.GetBestValue(&bestValueType, &bestPossibleValue);
    return bestValueType == EMetricBestValue::Max;
}

inline void CheckTarget(const TVector<float>& target, ELossFunction lossFunction) {
    if (lossFunction == ELossFunction::CrossEntropy) {
        float minTarget = *MinElement(target.begin(), target.end());
        float maxTarget = *MaxElement(target.begin(), target.end());
        CB_ENSURE(minTarget >= 0, "Min target less than 0: " + ToString(minTarget));
        CB_ENSURE(maxTarget <= 1, "Max target greater than 1: " + ToString(minTarget));
    }

    if (lossFunction == ELossFunction::QuerySoftMax) {
        float minTarget = *MinElement(target.begin(), target.end());
        CB_ENSURE(minTarget >= 0, "Min target less than 0: " + ToString(minTarget));
    }

    if (IsMultiClassError(lossFunction)) {
        CB_ENSURE(AllOf(target, [](float x) { return int(x) == x && x >= 0; }), "if loss-function is MultiClass then each target label should be nonnegative integer");
    }
}
