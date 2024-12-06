/*
 * Copyright (C) 2022-2024 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "framework/l0/levelzero.h"
#include "framework/l0/utility/usm_helper.h"
#include "framework/test_case/register_test_case.h"
#include "framework/utility/file_helper.h"
#include "framework/utility/timer.h"

#include "definitions/multi_argument_kernel.h"

#include <gtest/gtest.h>

static TestResult run(const MultiArgumentKernelTimeArguments &arguments, Statistics &statistics) {
    MeasurementFields typeSelector(MeasurementUnit::Microseconds, MeasurementType::Cpu);

    if (isNoopRun()) {
        statistics.pushUnitAndType(typeSelector.getUnit(), typeSelector.getType());
        return TestResult::Nooped;
    }

    // Setup
    LevelZero levelzero;
    Timer timer;

    // Create kernel
    auto spirvModule = FileHelper::loadBinaryFile("api_overhead_benchmark_multi_arg_kernel.spv");
    if (spirvModule.size() == 0) {
        return TestResult::KernelNotFound;
    }
    ze_module_handle_t module;
    ze_kernel_handle_t kernel;
    ze_module_desc_t moduleDesc{ZE_STRUCTURE_TYPE_MODULE_DESC};
    moduleDesc.format = ZE_MODULE_FORMAT_IL_SPIRV;
    moduleDesc.pInputModule = reinterpret_cast<const uint8_t *>(spirvModule.data());
    moduleDesc.inputSize = spirvModule.size();
    ASSERT_ZE_RESULT_SUCCESS(zeModuleCreate(levelzero.context, levelzero.device, &moduleDesc, &module, nullptr));
    ze_kernel_desc_t kernelDesc{ZE_STRUCTURE_TYPE_KERNEL_DESC};

    std::string kernelName = "kernelWith" + std::to_string(arguments.argumentCount);

    if (arguments.useGlobalIds) {
        kernelName += "WithIds";
    }

    kernelDesc.pKernelName = kernelName.c_str();
    ASSERT_ZE_RESULT_SUCCESS(zeKernelCreate(module, &kernelDesc, &kernel));

    // Configure kernel
    uint32_t groupSizeX = static_cast<uint32_t>(arguments.lws);
    ASSERT_ZE_RESULT_SUCCESS(zeKernelSetGroupSize(kernel, groupSizeX, 1u, 1u));

    std::vector<void *> allocations(arguments.argumentCount);

    for (auto allocationId = 0u; allocationId < arguments.argumentCount; allocationId++) {
        ASSERT_ZE_RESULT_SUCCESS(L0::UsmHelper::allocate(UsmMemoryPlacement::Device, levelzero, 4096u, &allocations[allocationId]));
    }

    for (auto argumentId = 0u; argumentId < arguments.argumentCount; ++argumentId) {
        ASSERT_ZE_RESULT_SUCCESS(zeKernelSetArgumentValue(kernel, argumentId, sizeof(void *), &allocations[argumentId]));
    }

    // Create command list and warmup
    const ze_group_count_t dispatchTraits{static_cast<uint32_t>(arguments.groupCount), 1u, 1u};
    ze_command_list_desc_t cmdListDesc{};

    ze_command_list_handle_t cmdList;
    cmdListDesc.flags = ZE_COMMAND_LIST_FLAG_IN_ORDER;
    ASSERT_ZE_RESULT_SUCCESS(zeCommandListCreate(levelzero.context, levelzero.device, &cmdListDesc, &cmdList));
    for (auto j = 0u; j < arguments.count; ++j) {
        ASSERT_ZE_RESULT_SUCCESS(zeCommandListAppendLaunchKernel(cmdList, kernel, &dispatchTraits, nullptr, 0, nullptr));
    }
    ASSERT_ZE_RESULT_SUCCESS(zeCommandListClose(cmdList));

    if (arguments.exec) {
        ASSERT_ZE_RESULT_SUCCESS(zeCommandQueueExecuteCommandLists(levelzero.commandQueue, 1, &cmdList, nullptr));
        ASSERT_ZE_RESULT_SUCCESS(zeCommandQueueSynchronize(levelzero.commandQueue, std::numeric_limits<uint64_t>::max()));
    }

    // Benchmark
    for (auto i = 0u; i < arguments.iterations; ++i) {
        if (arguments.measureSetKernelArg) {
            timer.measureStart();
        }
        for (auto argumentId = 0u; argumentId < arguments.argumentCount; ++argumentId) {
            ASSERT_ZE_RESULT_SUCCESS(zeKernelSetArgumentValue(kernel, argumentId, sizeof(void *), &allocations[argumentId]));
        }

        ASSERT_ZE_RESULT_SUCCESS(zeCommandListReset(cmdList));

        if (!arguments.measureSetKernelArg) {
            timer.measureStart();
        }

        for (auto kernelId = 0u; kernelId < arguments.count; ++kernelId) {
            ASSERT_ZE_RESULT_SUCCESS(zeCommandListAppendLaunchKernel(cmdList, kernel, &dispatchTraits, nullptr, 0, nullptr));
        }

        ASSERT_ZE_RESULT_SUCCESS(zeCommandListClose(cmdList));

        if (arguments.exec) {
            ASSERT_ZE_RESULT_SUCCESS(zeCommandQueueExecuteCommandLists(levelzero.commandQueue, 1, &cmdList, nullptr));
            ASSERT_ZE_RESULT_SUCCESS(zeCommandQueueSynchronize(levelzero.commandQueue, std::numeric_limits<uint64_t>::max()));
        }

        timer.measureEnd();

        statistics.pushValue(timer.get() / arguments.count, typeSelector.getUnit(), typeSelector.getType());
    }

    ASSERT_ZE_RESULT_SUCCESS(zeCommandListDestroy(cmdList));

    for (auto i = 0u; i < arguments.argumentCount; ++i) {
        ASSERT_ZE_RESULT_SUCCESS(L0::UsmHelper::deallocate(UsmMemoryPlacement::Device, levelzero, allocations[i]));
    }

    ASSERT_ZE_RESULT_SUCCESS(zeKernelDestroy(kernel));
    ASSERT_ZE_RESULT_SUCCESS(zeModuleDestroy(module));
    return TestResult::Success;
}

static RegisterTestCaseImplementation<MultiArgumentKernelTime> registerTestCase(run, Api::L0);
