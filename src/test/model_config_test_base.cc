// Copyright (c) 2018, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "src/test/model_config_test_base.h"

#include <stdlib.h>
#include <fstream>
#include <memory>
#include "src/core/constants.h"
#include "src/core/logging.h"
#include "src/core/utils.h"
#include "src/servables/caffe2/netdef_bundle.pb.h"
#include "src/servables/custom/custom_bundle.pb.h"
#include "src/servables/tensorflow/graphdef_bundle.pb.h"
#include "src/servables/tensorflow/savedmodel_bundle.pb.h"
#include "src/servables/tensorrt/plan_bundle.pb.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow_serving/config/platform_config.pb.h"

namespace tfs = tensorflow::serving;

namespace nvidia { namespace inferenceserver { namespace test {

bool
ModelConfigTestBase::ValidateInit(
    const std::string& model_path, bool autofill, BundleInitFunc init_func,
    std::string* result)
{
  result->clear();

  ModelConfig config;

  tfs::PlatformConfigMap platform_map;

  // Make sure platform config map has corresponding keys
  {
    GraphDefBundleSourceAdapterConfig graphdef_config;
    SavedModelBundleSourceAdapterConfig saved_model_config;
    NetDefBundleSourceAdapterConfig netdef_config;
    PlanBundleSourceAdapterConfig plan_config;
    CustomBundleSourceAdapterConfig custom_config;

    (*platform_map.mutable_platform_configs())[kTensorFlowGraphDefPlatform]
          .mutable_source_adapter_config()->PackFrom(graphdef_config);
    (*platform_map.mutable_platform_configs())[kTensorFlowSavedModelPlatform]
          .mutable_source_adapter_config()->PackFrom(saved_model_config);
    (*platform_map.mutable_platform_configs())[kCaffe2NetDefPlatform]
          .mutable_source_adapter_config()->PackFrom(netdef_config);
    (*platform_map.mutable_platform_configs())[kTensorRTPlanPlatform]
          .mutable_source_adapter_config()->PackFrom(plan_config);
    (*platform_map.mutable_platform_configs())[kCustomPlatform]
          .mutable_source_adapter_config()->PackFrom(custom_config);
  }

  tensorflow::Status status =
      GetNormalizedModelConfig(model_path, platform_map, autofill, &config);
  if (!status.ok()) {
    result->append(status.ToString());
    return false;
  }

  status = ValidateModelConfig(config, std::string());
  if (!status.ok()) {
    result->append(status.ToString());
    return false;
  }

  // ModelConfig unit tests assume model version "1"
  const std::string version_path = tensorflow::io::JoinPath(model_path, "1");

  status = init_func(version_path, config);
  if (!status.ok()) {
    result->append(status.ToString());
    return false;
  }

  *result = config.DebugString();
  return true;
}

void
ModelConfigTestBase::ValidateAll(
    const std::string& platform, BundleInitFunc init_func)
{
  // Sanity tests without autofill and forcing the platform.
  ValidateOne(
      "inference_server/src/test/testdata/model_config_sanity",
      false /* autofill */, platform, init_func);

  // Sanity tests with autofill and no platform.
  ValidateOne(
      "inference_server/src/test/testdata/autofill_sanity", true /* autofill */,
      std::string() /* platform */, init_func);
}

void
ModelConfigTestBase::ValidateOne(
    const std::string& test_repository_rpath, bool autofill,
    const std::string& platform, BundleInitFunc init_func)
{
  const std::string model_base_path =
      tensorflow::io::JoinPath(getenv("TEST_SRCDIR"), test_repository_rpath);

  std::vector<std::string> models;
  TF_CHECK_OK(
      tensorflow::Env::Default()->GetChildren(model_base_path, &models));

  for (const auto& model_name : models) {
    const auto model_path =
        tensorflow::io::JoinPath(model_base_path, model_name);

    // If a platform is specified and there is a configuration file
    // then must change the configuration to use that platform. We
    // modify the config file in place... not ideal but for how our CI
    // testing is done it is not a problem.
    if (!platform.empty()) {
      const auto config_path =
          tensorflow::io::JoinPath(model_path, kModelConfigPbTxt);
      if (tensorflow::Env::Default()->FileExists(config_path).ok()) {
        ModelConfig config;
        TF_CHECK_OK(
            ReadTextProto(tensorflow::Env::Default(), config_path, &config));
        config.set_platform(platform);
        TF_CHECK_OK(
            WriteTextProto(tensorflow::Env::Default(), config_path, config));
      }
    }

    LOG_INFO << "Testing " << model_name;
    std::string actual, fail_expected;
    ValidateInit(model_path, autofill, init_func, &actual);

    // The actual output must match *one of* the "expected*" files.
    std::vector<std::string> children;
    if (tensorflow::Env::Default()->GetChildren(model_path, &children).ok()) {
      for (const auto& child : children) {
        std::string real_child = child.substr(0, child.find_first_of('/'));
        if (real_child.find("expected") == 0) {
          const auto expected_path =
              tensorflow::io::JoinPath(model_path, real_child);
          LOG_INFO << "Comparing with " << expected_path;

          std::ifstream expected_file(expected_path);
          std::string expected(
              (std::istreambuf_iterator<char>(expected_file)),
              (std::istreambuf_iterator<char>()));
          std::string truncated_actual;
          if (expected.size() < actual.size()) {
            truncated_actual = actual.substr(0, expected.size());
          } else {
            truncated_actual = actual;
          }

          if (expected != truncated_actual) {
            fail_expected = expected;
          } else {
            fail_expected.clear();
            break;
          }
        }
      }
    }

    EXPECT_TRUE(fail_expected.empty());
    if (!fail_expected.empty()) {
      LOG_ERROR << "Expected:" << std::endl << fail_expected;
      LOG_ERROR << "Actual:" << std::endl << actual;
    }
  }
}

}}}  // namespace nvidia::inferenceserver::test
