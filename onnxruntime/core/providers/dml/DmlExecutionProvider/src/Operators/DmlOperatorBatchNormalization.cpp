// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "precomp.h"

namespace Dml
{

class DmlOperatorBatchNormalization : public DmlOperator, BatchNormalizationHelper
{
    // This order matches the ONNX schema.
    enum OnnxInputIndex
    {
        X, // Input
        Scale,
        Bias,
        Mean,
        Variance,
        Count,
    };

public:
    DmlOperatorBatchNormalization(const MLOperatorKernelCreationContext& kernelCreationContext)
    :   DmlOperator(kernelCreationContext),
        BatchNormalizationHelper(kernelCreationContext, kernelCreationContext.GetTensorShapeDescription())
    {
        // DML's BatchNormalization and ONNX order the input tensors differently (with DML as X, Mean, Variance, Scale, Bias),
        // and normally we'd need to specify kernelInputIndices to Initialize, but we'll utilize DMLX's mapping instead.
        // Passing both reordered kernelInputIndices to Initialize would otherwise confuse DMLX.
        DmlOperator::Initialize(kernelCreationContext);

        ML_CHECK_VALID_ARGUMENT(m_inputTensorDescs.size() == 5);
        ML_CHECK_VALID_ARGUMENT(m_outputTensorDescs.size() >= 1);

        const float epsilon = kernelCreationContext.GetOptionalAttribute<float>(AttrName::Epsilon, 0.0f);
        const int spatial = kernelCreationContext.GetOptionalAttribute<int>(AttrName::Spatial, 1);
        const std::optional<ActivationOperatorDesc> fusedActivation = FusionHelpers::TryGetFusedActivationDesc(kernelCreationContext);
        DML_OPERATOR_DESC fusedActivationDmlDesc = fusedActivation ? fusedActivation->GetDmlDesc() : DML_OPERATOR_DESC();

        m_inputTensorDescs[0] = CreateTensorDescFromInput(kernelCreationContext, 0, TensorAxis::DoNotCoerce, TensorAxis::N, TensorAxis::LeftAligned);

        // Massage each of these 1D tensors (of length C) into ND tensors of the form [1,C,1,1,...].
        for (uint32_t i = Scale; i < OnnxInputIndex::Count; ++i)
        {
            m_inputTensorDescs[i] = CreateTensorDescFromInput(kernelCreationContext, i, TensorAxis::DoNotCoerce, TensorAxis::C, TensorAxis::LeftAligned, std::nullopt, m_inputTensorDescs[0].GetDimensionCount());
        }

        m_outputTensorDescs[0] = CreateTensorDescFromOutput(kernelCreationContext, 0, TensorAxis::DoNotCoerce, TensorAxis::N, TensorAxis::LeftAligned, std::nullopt, m_inputTensorDescs[0].GetDimensionCount());

        std::vector<DML_TENSOR_DESC> inputDescs = GetDmlInputDescs();
        std::vector<DML_TENSOR_DESC> outputDescs = GetDmlOutputDescs();

        dml::Graph graph(m_dmlDevice.Get());
        dml::TensorDesc inputTensorDesc = inputDescs[OnnxInputIndex::X];
        dml::TensorDesc scaleTensorDesc = inputDescs[OnnxInputIndex::Scale];
        dml::TensorDesc biasTensorDesc = inputDescs[OnnxInputIndex::Bias];
        dml::Expression input = dml::InputTensor(graph, OnnxInputIndex::X, inputTensorDesc);
        dml::Expression scale = dml::InputTensor(graph, OnnxInputIndex::Scale, scaleTensorDesc);
        dml::Expression bias = dml::InputTensor(graph, OnnxInputIndex::Bias, biasTensorDesc);
        dml::Expression mean = dml::InputTensor(graph, OnnxInputIndex::Mean, inputDescs[OnnxInputIndex::Mean]);
        dml::Expression variance = dml::InputTensor(graph, OnnxInputIndex::Variance, inputDescs[OnnxInputIndex::Variance]);

        // If scale and bias have different data types than input, then coerce them.
        if (scaleTensorDesc.dataType != inputTensorDesc.dataType)
        {
            scale = dml::Cast(scale, inputTensorDesc.dataType);
        }
        if (biasTensorDesc.dataType != inputTensorDesc.dataType)
        {
            bias = dml::Cast(bias, inputTensorDesc.dataType);
        }

        dml::Expression batchNormalization = dml::BatchNormalization(
            input,
            mean,
            variance,
            scale,
            bias,
            static_cast<BOOL>(spatial),
            epsilon,
            fusedActivation ? &fusedActivationDmlDesc : nullptr
        );

        DML_EXECUTION_FLAGS executionFlags = GetExecutionFlags();
        m_compiledOperator.Attach(graph.Compile(executionFlags, { batchNormalization }).Detach());
    }

    void Compute(const MLOperatorKernelContext& kernelContext) override
    {
        std::vector<IMLOperatorTensor*> inputTensors = GetInputTensorsForExecute(kernelContext);
        std::vector<IMLOperatorTensor*> outputTensors = GetOutputTensorsForExecute(kernelContext);

        ORT_THROW_IF_FAILED(m_executionProvider->ExecuteOperator(
            m_compiledOperator.Get(),
            m_persistentResourceBinding ? &*m_persistentResourceBinding : nullptr,
            gsl::make_span(inputTensors),
            gsl::make_span(outputTensors)
        ));
    }
};

void CALLBACK QueryBatchNormalization(IMLOperatorSupportQueryContextPrivate* context, /*out*/ bool* isSupported)
{
    // training_mode=1 is unsupported as it isn't needed for inference (https://github.com/onnx/onnx/pull/3333).

    MLOperatorAttributes attributes(context);
    int32_t trainingMode = attributes.GetOptionalAttribute<int32_t>(AttrName::TrainingMode, 0);
    *isSupported = (trainingMode == 0);
}

DML_OP_DEFINE_CREATION_FUNCTION(BatchNormalization, DmlOperatorBatchNormalization);
DML_OP_DEFINE_CREATION_FUNCTION(BatchNormalization15, DmlOperatorBatchNormalization);
DML_OP_DEFINE_CREATION_FUNCTION(FusedBatchNormalization, DmlOperatorBatchNormalization);

} // namespace Dml
