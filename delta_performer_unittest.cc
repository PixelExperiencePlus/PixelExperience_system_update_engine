//
// Copyright (C) 2012 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "update_engine/delta_performer.h"

#include <inttypes.h>

#include <string>
#include <vector>

#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_util.h>
#include <base/strings/stringprintf.h>
#include <google/protobuf/repeated_field.h>
#include <gtest/gtest.h>

#include "update_engine/bzip.h"
#include "update_engine/constants.h"
#include "update_engine/fake_hardware.h"
#include "update_engine/fake_prefs.h"
#include "update_engine/fake_system_state.h"
#include "update_engine/payload_constants.h"
#include "update_engine/payload_generator/extent_ranges.h"
#include "update_engine/payload_generator/payload_file.h"
#include "update_engine/payload_generator/payload_signer.h"
#include "update_engine/test_utils.h"
#include "update_engine/update_metadata.pb.h"
#include "update_engine/utils.h"

namespace chromeos_update_engine {

using std::string;
using std::vector;
using test_utils::System;
using test_utils::kRandomString;

extern const char* kUnittestPrivateKeyPath;
extern const char* kUnittestPublicKeyPath;

static const char* kBogusMetadataSignature1 =
    "awSFIUdUZz2VWFiR+ku0Pj00V7bPQPQFYQSXjEXr3vaw3TE4xHV5CraY3/YrZpBv"
    "J5z4dSBskoeuaO1TNC/S6E05t+yt36tE4Fh79tMnJ/z9fogBDXWgXLEUyG78IEQr"
    "YH6/eBsQGT2RJtBgXIXbZ9W+5G9KmGDoPOoiaeNsDuqHiBc/58OFsrxskH8E6vMS"
    "BmMGGk82mvgzic7ApcoURbCGey1b3Mwne/hPZ/bb9CIyky8Og9IfFMdL2uAweOIR"
    "fjoTeLYZpt+WN65Vu7jJ0cQN8e1y+2yka5112wpRf/LLtPgiAjEZnsoYpLUd7CoV"
    "pLRtClp97kN2+tXGNBQqkA==";

namespace {
// Different options that determine what we should fill into the
// install_plan.metadata_signature to simulate the contents received in the
// Omaha response.
enum MetadataSignatureTest {
  kEmptyMetadataSignature,
  kInvalidMetadataSignature,
  kValidMetadataSignature,
};

// Compressed data without checksum, generated with:
// echo -n a | xz -9 --check=none | hexdump -v -e '"    " 12/1 "0x%02x, " "\n"'
const uint8_t kXzCompressedData[] = {
    0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00, 0x00, 0x00, 0xff, 0x12, 0xd9, 0x41,
    0x02, 0x00, 0x21, 0x01, 0x1c, 0x00, 0x00, 0x00, 0x10, 0xcf, 0x58, 0xcc,
    0x01, 0x00, 0x00, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x11, 0x01,
    0xad, 0xa6, 0x58, 0x04, 0x06, 0x72, 0x9e, 0x7a, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x59, 0x5a,
};

}  // namespace

class DeltaPerformerTest : public ::testing::Test {
 protected:

  // Test helper placed where it can easily be friended from DeltaPerformer.
  void RunManifestValidation(const DeltaArchiveManifest& manifest,
                             bool full_payload,
                             ErrorCode expected) {
    // The install plan is for Full or Delta.
    install_plan_.is_full_update = full_payload;

    // The Manifest we are validating.
    performer_.manifest_.CopyFrom(manifest);

    EXPECT_EQ(expected, performer_.ValidateManifest());
  }

  chromeos::Blob GeneratePayload(const chromeos::Blob& blob_data,
                                 const vector<AnnotatedOperation>& aops,
                                 bool sign_payload,
                                 int32_t minor_version) {
    string blob_path;
    EXPECT_TRUE(utils::MakeTempFile("Blob-XXXXXX", &blob_path, nullptr));
    ScopedPathUnlinker blob_unlinker(blob_path);
    EXPECT_TRUE(utils::WriteFile(blob_path.c_str(),
                                 blob_data.data(),
                                 blob_data.size()));

    PayloadGenerationConfig config;
    config.major_version = kChromeOSMajorPayloadVersion;
    config.minor_version = minor_version;

    PayloadFile payload;
    EXPECT_TRUE(payload.Init(config));

    PartitionConfig old_part(kLegacyPartitionNameRoot);
    PartitionConfig new_part(kLegacyPartitionNameRoot);
    new_part.path = blob_path;
    new_part.size = blob_data.size();

    payload.AddPartition(old_part, new_part, aops);

    string payload_path;
    EXPECT_TRUE(utils::MakeTempFile("Payload-XXXXXX", &payload_path, nullptr));
    ScopedPathUnlinker payload_unlinker(payload_path);
    EXPECT_TRUE(payload.WritePayload(payload_path, blob_path,
        sign_payload ? kUnittestPrivateKeyPath : "",
        &install_plan_.metadata_size));

    chromeos::Blob payload_data;
    EXPECT_TRUE(utils::ReadFile(payload_path, &payload_data));
    return payload_data;
  }

  // Apply |payload_data| on partition specified in |source_path|.
  chromeos::Blob ApplyPayload(const chromeos::Blob& payload_data,
                              const string& source_path) {
    return ApplyPayloadToData(payload_data, source_path, chromeos::Blob());
  }

  // Apply the payload provided in |payload_data| reading from the |source_path|
  // file and writing the contents to a new partition. The existing data in the
  // new target file are set to |target_data| before applying the payload.
  // Returns the result of the payload application.
  chromeos::Blob ApplyPayloadToData(const chromeos::Blob& payload_data,
                                    const string& source_path,
                                    const chromeos::Blob& target_data) {
    string new_part;
    EXPECT_TRUE(utils::MakeTempFile("Partition-XXXXXX", &new_part, nullptr));
    ScopedPathUnlinker partition_unlinker(new_part);
    EXPECT_TRUE(utils::WriteFile(new_part.c_str(), target_data.data(),
                                 target_data.size()));

    install_plan_.source_path = source_path;
    install_plan_.kernel_source_path = "/dev/null";
    install_plan_.install_path = new_part;
    install_plan_.kernel_install_path = "/dev/null";

    EXPECT_EQ(0, performer_.Open(new_part.c_str(), 0, 0));
    EXPECT_TRUE(performer_.OpenSourceRootfs(source_path.c_str()));
    EXPECT_TRUE(performer_.Write(payload_data.data(), payload_data.size()));
    EXPECT_EQ(0, performer_.Close());

    chromeos::Blob partition_data;
    EXPECT_TRUE(utils::ReadFile(new_part, &partition_data));
    return partition_data;
  }

  // Calls delta performer's Write method by pretending to pass in bytes from a
  // delta file whose metadata size is actual_metadata_size and tests if all
  // checks are correctly performed if the install plan contains
  // expected_metadata_size and that the result of the parsing are as per
  // hash_checks_mandatory flag.
  void DoMetadataSizeTest(uint64_t expected_metadata_size,
                          uint64_t actual_metadata_size,
                          bool hash_checks_mandatory) {
    install_plan_.hash_checks_mandatory = hash_checks_mandatory;
    EXPECT_EQ(0, performer_.Open("/dev/null", 0, 0));
    EXPECT_TRUE(performer_.OpenKernel("/dev/null"));

    // Set a valid magic string and version number 1.
    EXPECT_TRUE(performer_.Write("CrAU", 4));
    uint64_t version = htobe64(kChromeOSMajorPayloadVersion);
    EXPECT_TRUE(performer_.Write(&version, 8));

    install_plan_.metadata_size = expected_metadata_size;
    ErrorCode error_code;
    // When filling in size in manifest, exclude the size of the 20-byte header.
    uint64_t size_in_manifest = htobe64(actual_metadata_size - 20);
    bool result = performer_.Write(&size_in_manifest, 8, &error_code);
    if (expected_metadata_size == actual_metadata_size ||
        !hash_checks_mandatory) {
      EXPECT_TRUE(result);
    } else {
      EXPECT_FALSE(result);
      EXPECT_EQ(ErrorCode::kDownloadInvalidMetadataSize, error_code);
    }

    EXPECT_LT(performer_.Close(), 0);
  }

  // Generates a valid delta file but tests the delta performer by suppling
  // different metadata signatures as per metadata_signature_test flag and
  // sees if the result of the parsing are as per hash_checks_mandatory flag.
  void DoMetadataSignatureTest(MetadataSignatureTest metadata_signature_test,
                               bool sign_payload,
                               bool hash_checks_mandatory) {

    // Loads the payload and parses the manifest.
    chromeos::Blob payload = GeneratePayload(chromeos::Blob(),
        vector<AnnotatedOperation>(), sign_payload,
        kFullPayloadMinorVersion);

    LOG(INFO) << "Payload size: " << payload.size();

    install_plan_.hash_checks_mandatory = hash_checks_mandatory;

    DeltaPerformer::MetadataParseResult expected_result, actual_result;
    ErrorCode expected_error, actual_error;

    // Fill up the metadata signature in install plan according to the test.
    switch (metadata_signature_test) {
      case kEmptyMetadataSignature:
        install_plan_.metadata_signature.clear();
        expected_result = DeltaPerformer::kMetadataParseError;
        expected_error = ErrorCode::kDownloadMetadataSignatureMissingError;
        break;

      case kInvalidMetadataSignature:
        install_plan_.metadata_signature = kBogusMetadataSignature1;
        expected_result = DeltaPerformer::kMetadataParseError;
        expected_error = ErrorCode::kDownloadMetadataSignatureMismatch;
        break;

      case kValidMetadataSignature:
      default:
        // Set the install plan's metadata size to be the same as the one
        // in the manifest so that we pass the metadata size checks. Only
        // then we can get to manifest signature checks.
        ASSERT_TRUE(PayloadSigner::GetMetadataSignature(
            payload.data(),
            install_plan_.metadata_size,
            kUnittestPrivateKeyPath,
            &install_plan_.metadata_signature));
        EXPECT_FALSE(install_plan_.metadata_signature.empty());
        expected_result = DeltaPerformer::kMetadataParseSuccess;
        expected_error = ErrorCode::kSuccess;
        break;
    }

    // Ignore the expected result/error if hash checks are not mandatory.
    if (!hash_checks_mandatory) {
      expected_result = DeltaPerformer::kMetadataParseSuccess;
      expected_error = ErrorCode::kSuccess;
    }

    // Use the public key corresponding to the private key used above to
    // sign the metadata.
    EXPECT_TRUE(utils::FileExists(kUnittestPublicKeyPath));
    performer_.set_public_key_path(kUnittestPublicKeyPath);

    // Init actual_error with an invalid value so that we make sure
    // ParsePayloadMetadata properly populates it in all cases.
    actual_error = ErrorCode::kUmaReportedMax;
    actual_result = performer_.ParsePayloadMetadata(payload, &actual_error);

    EXPECT_EQ(expected_result, actual_result);
    EXPECT_EQ(expected_error, actual_error);

    // Check that the parsed metadata size is what's expected. This test
    // implicitly confirms that the metadata signature is valid, if required.
    EXPECT_EQ(install_plan_.metadata_size, performer_.GetMetadataSize());
  }

  void SetSupportedMajorVersion(uint64_t major_version) {
    performer_.supported_major_version_ = major_version;
  }
  FakePrefs prefs_;
  InstallPlan install_plan_;
  FakeSystemState fake_system_state_;
  DeltaPerformer performer_{&prefs_, &fake_system_state_, &install_plan_};
};

TEST_F(DeltaPerformerTest, FullPayloadWriteTest) {
  install_plan_.is_full_update = true;
  chromeos::Blob expected_data = chromeos::Blob(std::begin(kRandomString),
                                                std::end(kRandomString));
  expected_data.resize(4096);  // block size
  vector<AnnotatedOperation> aops;
  AnnotatedOperation aop;
  *(aop.op.add_dst_extents()) = ExtentForRange(0, 1);
  aop.op.set_data_offset(0);
  aop.op.set_data_length(expected_data.size());
  aop.op.set_type(InstallOperation::REPLACE);
  aops.push_back(aop);

  chromeos::Blob payload_data = GeneratePayload(expected_data, aops, false,
      kFullPayloadMinorVersion);

  EXPECT_EQ(expected_data, ApplyPayload(payload_data, "/dev/null"));
}

TEST_F(DeltaPerformerTest, ReplaceOperationTest) {
  chromeos::Blob expected_data = chromeos::Blob(std::begin(kRandomString),
                                                std::end(kRandomString));
  expected_data.resize(4096);  // block size
  vector<AnnotatedOperation> aops;
  AnnotatedOperation aop;
  *(aop.op.add_dst_extents()) = ExtentForRange(0, 1);
  aop.op.set_data_offset(0);
  aop.op.set_data_length(expected_data.size());
  aop.op.set_type(InstallOperation::REPLACE);
  aops.push_back(aop);

  chromeos::Blob payload_data = GeneratePayload(expected_data, aops, false,
                                                kSourceMinorPayloadVersion);

  EXPECT_EQ(expected_data, ApplyPayload(payload_data, "/dev/null"));
}

TEST_F(DeltaPerformerTest, ReplaceBzOperationTest) {
  chromeos::Blob expected_data = chromeos::Blob(std::begin(kRandomString),
                                                std::end(kRandomString));
  expected_data.resize(4096);  // block size
  chromeos::Blob bz_data;
  EXPECT_TRUE(BzipCompress(expected_data, &bz_data));

  vector<AnnotatedOperation> aops;
  AnnotatedOperation aop;
  *(aop.op.add_dst_extents()) = ExtentForRange(0, 1);
  aop.op.set_data_offset(0);
  aop.op.set_data_length(bz_data.size());
  aop.op.set_type(InstallOperation::REPLACE_BZ);
  aops.push_back(aop);

  chromeos::Blob payload_data = GeneratePayload(bz_data, aops, false,
                                                kSourceMinorPayloadVersion);

  EXPECT_EQ(expected_data, ApplyPayload(payload_data, "/dev/null"));
}

TEST_F(DeltaPerformerTest, ReplaceXzOperationTest) {
  chromeos::Blob xz_data(std::begin(kXzCompressedData),
                         std::end(kXzCompressedData));
  // The compressed xz data contains only a single "a", but the operation should
  // pad the rest of the two blocks with zeros.
  chromeos::Blob expected_data = chromeos::Blob(4096, 0);
  expected_data[0] = 'a';

  AnnotatedOperation aop;
  *(aop.op.add_dst_extents()) = ExtentForRange(0, 1);
  aop.op.set_data_offset(0);
  aop.op.set_data_length(xz_data.size());
  aop.op.set_type(InstallOperation::REPLACE_XZ);
  vector<AnnotatedOperation> aops = {aop};

  chromeos::Blob payload_data = GeneratePayload(xz_data, aops, false,
                                                kSourceMinorPayloadVersion);

  EXPECT_EQ(expected_data, ApplyPayload(payload_data, "/dev/null"));
}

TEST_F(DeltaPerformerTest, ZeroOperationTest) {
  chromeos::Blob existing_data = chromeos::Blob(4096 * 10, 'a');
  chromeos::Blob expected_data = existing_data;
  // Blocks 4, 5 and 7 should have zeros instead of 'a' after the operation is
  // applied.
  std::fill(expected_data.data() + 4096 * 4, expected_data.data() + 4096 * 6,
            0);
  std::fill(expected_data.data() + 4096 * 7, expected_data.data() + 4096 * 8,
            0);

  AnnotatedOperation aop;
  *(aop.op.add_dst_extents()) = ExtentForRange(4, 2);
  *(aop.op.add_dst_extents()) = ExtentForRange(7, 1);
  aop.op.set_type(InstallOperation::ZERO);
  vector<AnnotatedOperation> aops = {aop};

  chromeos::Blob payload_data = GeneratePayload(chromeos::Blob(), aops, false,
                                                kSourceMinorPayloadVersion);

  EXPECT_EQ(expected_data,
            ApplyPayloadToData(payload_data, "/dev/null", existing_data));
}

TEST_F(DeltaPerformerTest, SourceCopyOperationTest) {
  chromeos::Blob expected_data = chromeos::Blob(std::begin(kRandomString),
                                                std::end(kRandomString));
  expected_data.resize(4096);  // block size
  vector<AnnotatedOperation> aops;
  AnnotatedOperation aop;
  *(aop.op.add_src_extents()) = ExtentForRange(0, 1);
  *(aop.op.add_dst_extents()) = ExtentForRange(0, 1);
  aop.op.set_type(InstallOperation::SOURCE_COPY);
  aops.push_back(aop);

  chromeos::Blob payload_data = GeneratePayload(chromeos::Blob(), aops, false,
                                                kSourceMinorPayloadVersion);
  string source_path;
  EXPECT_TRUE(utils::MakeTempFile("Source-XXXXXX",
                                  &source_path, nullptr));
  ScopedPathUnlinker path_unlinker(source_path);
  EXPECT_TRUE(utils::WriteFile(source_path.c_str(),
                               expected_data.data(),
                               expected_data.size()));

  EXPECT_EQ(expected_data, ApplyPayload(payload_data, source_path));
}

TEST_F(DeltaPerformerTest, ExtentsToByteStringTest) {
  uint64_t test[] = {1, 1, 4, 2, 0, 1};
  COMPILE_ASSERT(arraysize(test) % 2 == 0, array_size_uneven);
  const uint64_t block_size = 4096;
  const uint64_t file_length = 4 * block_size - 13;

  google::protobuf::RepeatedPtrField<Extent> extents;
  for (size_t i = 0; i < arraysize(test); i += 2) {
    *(extents.Add()) = ExtentForRange(test[i], test[i + 1]);
  }

  string expected_output = "4096:4096,16384:8192,0:4083";
  string actual_output;
  EXPECT_TRUE(DeltaPerformer::ExtentsToBsdiffPositionsString(extents,
                                                             block_size,
                                                             file_length,
                                                             &actual_output));
  EXPECT_EQ(expected_output, actual_output);
}

TEST_F(DeltaPerformerTest, ValidateManifestFullGoodTest) {
  // The Manifest we are validating.
  DeltaArchiveManifest manifest;
  manifest.mutable_new_kernel_info();
  manifest.mutable_new_rootfs_info();
  manifest.set_minor_version(kFullPayloadMinorVersion);

  RunManifestValidation(manifest, true, ErrorCode::kSuccess);
}

TEST_F(DeltaPerformerTest, ValidateManifestDeltaGoodTest) {
  // The Manifest we are validating.
  DeltaArchiveManifest manifest;
  manifest.mutable_old_kernel_info();
  manifest.mutable_old_rootfs_info();
  manifest.mutable_new_kernel_info();
  manifest.mutable_new_rootfs_info();
  manifest.set_minor_version(DeltaPerformer::kSupportedMinorPayloadVersion);

  RunManifestValidation(manifest, false, ErrorCode::kSuccess);
}

TEST_F(DeltaPerformerTest, ValidateManifestFullUnsetMinorVersion) {
  // The Manifest we are validating.
  DeltaArchiveManifest manifest;

  RunManifestValidation(manifest, true, ErrorCode::kSuccess);
}

TEST_F(DeltaPerformerTest, ValidateManifestDeltaUnsetMinorVersion) {
  // The Manifest we are validating.
  DeltaArchiveManifest manifest;

  RunManifestValidation(manifest, false,
                        ErrorCode::kUnsupportedMinorPayloadVersion);
}

TEST_F(DeltaPerformerTest, ValidateManifestFullOldKernelTest) {
  // The Manifest we are validating.
  DeltaArchiveManifest manifest;
  manifest.mutable_old_kernel_info();
  manifest.mutable_new_kernel_info();
  manifest.mutable_new_rootfs_info();
  manifest.set_minor_version(DeltaPerformer::kSupportedMinorPayloadVersion);

  RunManifestValidation(manifest, true, ErrorCode::kPayloadMismatchedType);
}

TEST_F(DeltaPerformerTest, ValidateManifestFullOldRootfsTest) {
  // The Manifest we are validating.
  DeltaArchiveManifest manifest;
  manifest.mutable_old_rootfs_info();
  manifest.mutable_new_kernel_info();
  manifest.mutable_new_rootfs_info();
  manifest.set_minor_version(DeltaPerformer::kSupportedMinorPayloadVersion);

  RunManifestValidation(manifest, true, ErrorCode::kPayloadMismatchedType);
}

TEST_F(DeltaPerformerTest, ValidateManifestBadMinorVersion) {
  // The Manifest we are validating.
  DeltaArchiveManifest manifest;

  // Generate a bad version number.
  manifest.set_minor_version(DeltaPerformer::kSupportedMinorPayloadVersion +
                             10000);

  RunManifestValidation(manifest, false,
                        ErrorCode::kUnsupportedMinorPayloadVersion);
}

TEST_F(DeltaPerformerTest, BrilloMetadataSignatureSizeTest) {
  SetSupportedMajorVersion(kBrilloMajorPayloadVersion);
  EXPECT_EQ(0, performer_.Open("/dev/null", 0, 0));
  EXPECT_TRUE(performer_.OpenKernel("/dev/null"));
  EXPECT_TRUE(performer_.Write(kDeltaMagic, sizeof(kDeltaMagic)));

  uint64_t major_version = htobe64(kBrilloMajorPayloadVersion);
  EXPECT_TRUE(performer_.Write(&major_version, 8));

  uint64_t manifest_size = rand() % 256;
  uint64_t manifest_size_be = htobe64(manifest_size);
  EXPECT_TRUE(performer_.Write(&manifest_size_be, 8));

  uint32_t metadata_signature_size = rand() % 256;
  uint32_t metadata_signature_size_be = htobe32(metadata_signature_size);
  EXPECT_TRUE(performer_.Write(&metadata_signature_size_be, 4));

  EXPECT_LT(performer_.Close(), 0);

  EXPECT_TRUE(performer_.IsHeaderParsed());
  EXPECT_EQ(kBrilloMajorPayloadVersion, performer_.GetMajorVersion());
  uint64_t manifest_offset;
  EXPECT_TRUE(performer_.GetManifestOffset(&manifest_offset));
  EXPECT_EQ(24, manifest_offset);  // 4 + 8 + 8 + 4
  EXPECT_EQ(24 + manifest_size + metadata_signature_size,
            performer_.GetMetadataSize());
}

TEST_F(DeltaPerformerTest, BadDeltaMagicTest) {
  EXPECT_EQ(0, performer_.Open("/dev/null", 0, 0));
  EXPECT_TRUE(performer_.OpenKernel("/dev/null"));
  EXPECT_TRUE(performer_.Write("junk", 4));
  EXPECT_FALSE(performer_.Write("morejunk", 8));
  EXPECT_LT(performer_.Close(), 0);
}

TEST_F(DeltaPerformerTest, WriteUpdatesPayloadState) {
  EXPECT_EQ(0, performer_.Open("/dev/null", 0, 0));
  EXPECT_TRUE(performer_.OpenKernel("/dev/null"));

  EXPECT_CALL(*(fake_system_state_.mock_payload_state()),
              DownloadProgress(4)).Times(1);
  EXPECT_CALL(*(fake_system_state_.mock_payload_state()),
              DownloadProgress(8)).Times(1);

  EXPECT_TRUE(performer_.Write("junk", 4));
  EXPECT_FALSE(performer_.Write("morejunk", 8));
  EXPECT_LT(performer_.Close(), 0);
}

TEST_F(DeltaPerformerTest, MissingMandatoryMetadataSizeTest) {
  DoMetadataSizeTest(0, 75456, true);
}

TEST_F(DeltaPerformerTest, MissingNonMandatoryMetadataSizeTest) {
  DoMetadataSizeTest(0, 123456, false);
}

TEST_F(DeltaPerformerTest, InvalidMandatoryMetadataSizeTest) {
  DoMetadataSizeTest(13000, 140000, true);
}

TEST_F(DeltaPerformerTest, InvalidNonMandatoryMetadataSizeTest) {
  DoMetadataSizeTest(40000, 50000, false);
}

TEST_F(DeltaPerformerTest, ValidMandatoryMetadataSizeTest) {
  DoMetadataSizeTest(85376, 85376, true);
}

TEST_F(DeltaPerformerTest, MandatoryEmptyMetadataSignatureTest) {
  DoMetadataSignatureTest(kEmptyMetadataSignature, true, true);
}

TEST_F(DeltaPerformerTest, NonMandatoryEmptyMetadataSignatureTest) {
  DoMetadataSignatureTest(kEmptyMetadataSignature, true, false);
}

TEST_F(DeltaPerformerTest, MandatoryInvalidMetadataSignatureTest) {
  DoMetadataSignatureTest(kInvalidMetadataSignature, true, true);
}

TEST_F(DeltaPerformerTest, NonMandatoryInvalidMetadataSignatureTest) {
  DoMetadataSignatureTest(kInvalidMetadataSignature, true, false);
}

TEST_F(DeltaPerformerTest, MandatoryValidMetadataSignature1Test) {
  DoMetadataSignatureTest(kValidMetadataSignature, false, true);
}

TEST_F(DeltaPerformerTest, MandatoryValidMetadataSignature2Test) {
  DoMetadataSignatureTest(kValidMetadataSignature, true, true);
}

TEST_F(DeltaPerformerTest, NonMandatoryValidMetadataSignatureTest) {
  DoMetadataSignatureTest(kValidMetadataSignature, true, false);
}

TEST_F(DeltaPerformerTest, UsePublicKeyFromResponse) {
  base::FilePath key_path;

  // The result of the GetPublicKeyResponse() method is based on three things
  //
  //  1. Whether it's an official build; and
  //  2. Whether the Public RSA key to be used is in the root filesystem; and
  //  3. Whether the response has a public key
  //
  // We test all eight combinations to ensure that we only use the
  // public key in the response if
  //
  //  a. it's not an official build; and
  //  b. there is no key in the root filesystem.

  FakeHardware* fake_hardware = fake_system_state_.fake_hardware();

  string temp_dir;
  EXPECT_TRUE(utils::MakeTempDirectory("PublicKeyFromResponseTests.XXXXXX",
                                       &temp_dir));
  string non_existing_file = temp_dir + "/non-existing";
  string existing_file = temp_dir + "/existing";
  EXPECT_EQ(0, System(base::StringPrintf("touch %s", existing_file.c_str())));

  // Non-official build, non-existing public-key, key in response -> true
  fake_hardware->SetIsOfficialBuild(false);
  performer_.public_key_path_ = non_existing_file;
  install_plan_.public_key_rsa = "VGVzdAo="; // result of 'echo "Test" | base64'
  EXPECT_TRUE(performer_.GetPublicKeyFromResponse(&key_path));
  EXPECT_FALSE(key_path.empty());
  EXPECT_EQ(unlink(key_path.value().c_str()), 0);
  // Same with official build -> false
  fake_hardware->SetIsOfficialBuild(true);
  EXPECT_FALSE(performer_.GetPublicKeyFromResponse(&key_path));

  // Non-official build, existing public-key, key in response -> false
  fake_hardware->SetIsOfficialBuild(false);
  performer_.public_key_path_ = existing_file;
  install_plan_.public_key_rsa = "VGVzdAo="; // result of 'echo "Test" | base64'
  EXPECT_FALSE(performer_.GetPublicKeyFromResponse(&key_path));
  // Same with official build -> false
  fake_hardware->SetIsOfficialBuild(true);
  EXPECT_FALSE(performer_.GetPublicKeyFromResponse(&key_path));

  // Non-official build, non-existing public-key, no key in response -> false
  fake_hardware->SetIsOfficialBuild(false);
  performer_.public_key_path_ = non_existing_file;
  install_plan_.public_key_rsa = "";
  EXPECT_FALSE(performer_.GetPublicKeyFromResponse(&key_path));
  // Same with official build -> false
  fake_hardware->SetIsOfficialBuild(true);
  EXPECT_FALSE(performer_.GetPublicKeyFromResponse(&key_path));

  // Non-official build, existing public-key, no key in response -> false
  fake_hardware->SetIsOfficialBuild(false);
  performer_.public_key_path_ = existing_file;
  install_plan_.public_key_rsa = "";
  EXPECT_FALSE(performer_.GetPublicKeyFromResponse(&key_path));
  // Same with official build -> false
  fake_hardware->SetIsOfficialBuild(true);
  EXPECT_FALSE(performer_.GetPublicKeyFromResponse(&key_path));

  // Non-official build, non-existing public-key, key in response
  // but invalid base64 -> false
  fake_hardware->SetIsOfficialBuild(false);
  performer_.public_key_path_ = non_existing_file;
  install_plan_.public_key_rsa = "not-valid-base64";
  EXPECT_FALSE(performer_.GetPublicKeyFromResponse(&key_path));

  EXPECT_TRUE(test_utils::RecursiveUnlinkDir(temp_dir));
}

TEST_F(DeltaPerformerTest, ConfVersionsMatch) {
  // Test that the versions in update_engine.conf that is installed to the
  // image match the supported delta versions in the update engine.
  uint32_t minor_version;
  chromeos::KeyValueStore store;
  EXPECT_TRUE(store.Load(base::FilePath("update_engine.conf")));
  EXPECT_TRUE(utils::GetMinorVersion(store, &minor_version));
  EXPECT_EQ(DeltaPerformer::kSupportedMinorPayloadVersion, minor_version);

  string major_version_str;
  uint64_t major_version;
  EXPECT_TRUE(store.GetString("PAYLOAD_MAJOR_VERSION", &major_version_str));
  EXPECT_TRUE(base::StringToUint64(major_version_str, &major_version));
  EXPECT_EQ(DeltaPerformer::kSupportedMajorPayloadVersion, major_version);
}

}  // namespace chromeos_update_engine