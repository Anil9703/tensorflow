/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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

#include "xla/pjrt/pjrt_c_api_client.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/inlined_vector.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/Bytecode/BytecodeWriter.h"  // from @llvm-project
#include "mlir/Dialect/Arith/IR/Arith.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/MLProgram/IR/MLProgram.h"  // from @llvm-project
#include "mlir/Dialect/Shape/IR/Shape.h"  // from @llvm-project
#include "mlir/IR/DialectRegistry.h"  // from @llvm-project
#include "mlir/Parser/Parser.h"  // from @llvm-project
#include "mlir/Pass/PassManager.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "stablehlo/dialect/Register.h"  // from @stablehlo
#include "xla/client/xla_computation.h"
#include "xla/hlo/ir/hlo_module.h"
#include "xla/layout.h"
#include "xla/literal.h"
#include "xla/mlir_hlo/mhlo/IR/register.h"
#include "xla/mlir_hlo/mhlo/transforms/passes.h"
#include "xla/pjrt/c/pjrt_c_api.h"
#include "xla/pjrt/c/pjrt_c_api_helpers.h"
#include "xla/pjrt/compile_options.pb.h"
#include "xla/pjrt/pjrt_api.h"
#include "xla/pjrt/pjrt_client.h"
#include "xla/pjrt/pjrt_common.h"
#include "xla/pjrt/pjrt_compiler.h"
#include "xla/pjrt/pjrt_device_description.h"
#include "xla/pjrt/pjrt_executable.h"
#include "xla/pjrt/pjrt_future.h"
#include "xla/service/computation_placer.h"
#include "xla/service/hlo.pb.h"
#include "xla/service/hlo_module_config.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/status.h"
#include "xla/statusor.h"
#include "xla/translate/mhlo_to_hlo/mlir_hlo_to_hlo.h"
#include "xla/util.h"
#include "xla/xla.pb.h"
#include "xla/xla_data.pb.h"
#include "tsl/framework/allocator.h"
#include "tsl/platform/casts.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/fingerprint.h"
#include "tsl/platform/status.h"
#include "tsl/platform/statusor.h"

namespace xla {

// Helper macros

// Return error future if not success and frees the PJRT_Error returned by
// `expr`.
#define RETURN_FUTURE_IF_ERROR(expr, c_api)                             \
  do {                                                                  \
    PJRT_Error* error = (expr);                                         \
    std::unique_ptr<PJRT_Error, pjrt::PJRT_ErrorDeleter> _error(        \
        error, pjrt::MakeErrorDeleter(c_api));                          \
    xla::Status _status = pjrt::PjrtErrorToStatus(_error.get(), c_api); \
    if (!_status.ok()) {                                                \
      return PjRtFuture<Status>(_status);                               \
    }                                                                   \
  } while (false)

// ---------------------------------- Client -----------------------------------

static StatusOr<const PjRtCApiTopologyDescription> InitTopologyDescription(
    const PJRT_Api* c_api, PJRT_Client* c_client) {
  StatusOr<const PJRT_TopologyDescription*> c_topo =
      pjrt::GetTopologyDescription(c_client, c_api);
  TF_RETURN_IF_ERROR(c_topo.status());
  return PjRtCApiTopologyDescription(c_api, *c_topo);
}

PjRtCApiClient::PjRtCApiClient(
    const PJRT_Api* c_api, PJRT_Client* c_client,
    std::unique_ptr<pjrt::PJRT_KeyValueCallbackData> kv_callback_data)
    : c_api_(c_api),
      c_client_(std::unique_ptr<PJRT_Client, ::pjrt::PJRT_ClientDeleter>(
          c_client, ::pjrt::MakeClientDeleter(c_api))),
      kv_callback_data_(std::move(kv_callback_data)),
      topo_desc_(InitTopologyDescription(c_api, c_client)),
      // Example platform version string:
      //   PJRT C API
      //   TFRT TPU v2
      //   Built on Mar 4 2021 15:25:57 (1614900357) cl/360760169
      platform_version_(absl::StrCat(
          "PJRT C API\n", ::pjrt::GetPlatformVersion(c_client, c_api))),
      platform_name_(::pjrt::GetPlatformName(c_client, c_api)),
      platform_id_(tsl::Fingerprint64(platform_name_)) {
  InitDevicesAndMemorySpaces();
  LOG(INFO) << "PjRtCApiClient created.";
}

void PjRtCApiClient::InitDevicesAndMemorySpaces() {
  // Initialize devices.
  PJRT_Client_Devices_Args devices_args;
  devices_args.struct_size = PJRT_Client_Devices_Args_STRUCT_SIZE;
  devices_args.priv = nullptr;
  devices_args.client = c_client_.get();

  pjrt::LogFatalIfPjrtError(c_api_->PJRT_Client_Devices(&devices_args), c_api_);

  const size_t num_devices = devices_args.num_devices;
  c_to_cpp_device_map_.reserve(num_devices);
  owned_devices_.reserve(num_devices);
  devices_.reserve(num_devices);

  for (int i = 0; i < num_devices; ++i) {
    PJRT_Device* device = devices_args.devices[i];
    std::unique_ptr<PjRtCApiDevice>& cpp_device = owned_devices_.emplace_back(
        std::make_unique<PjRtCApiDevice>(device, this));
    devices_.push_back(cpp_device.get());
    c_to_cpp_device_map_[device] = cpp_device.get();
  }

  // Initialize addressable devices.
  PJRT_Client_AddressableDevices_Args address_args;
  address_args.struct_size = PJRT_Client_AddressableDevices_Args_STRUCT_SIZE;
  address_args.priv = nullptr;
  address_args.client = c_client_.get();

  pjrt::LogFatalIfPjrtError(
      c_api_->PJRT_Client_AddressableDevices(&address_args), c_api_);

  const size_t num_addressable_devices = address_args.num_addressable_devices;
  addressable_devices_.reserve(num_addressable_devices);

  for (int i = 0; i < num_addressable_devices; ++i) {
    PJRT_Device* c_device = address_args.addressable_devices[i];
    addressable_devices_.push_back(GetCppDevice(c_device));
  }

  // Initialize addressable memory spaces.
  // TODO(yueshengys): Initialize global memory spaces when supported.
  PJRT_Client_AddressableMemories_Args memory_args;
  memory_args.struct_size = PJRT_Client_AddressableMemories_Args_STRUCT_SIZE;
  memory_args.priv = nullptr;
  memory_args.client = c_client_.get();

  std::unique_ptr<PJRT_Error, pjrt::PJRT_ErrorDeleter> client_error(
      c_api_->PJRT_Client_AddressableMemories(&memory_args),
      pjrt::MakeErrorDeleter(c_api_));
  if (client_error == nullptr) {
    const size_t num_memories = memory_args.num_addressable_memories;
    c_to_cpp_memory_map_.reserve(num_memories);
    owned_memory_spaces_.reserve(num_memories);
    addressable_memory_spaces_.reserve(num_memories);

    for (int i = 0; i < num_memories; ++i) {
      PJRT_Memory* memory = memory_args.addressable_memories[i];
      std::unique_ptr<PjRtCApiMemorySpace>& cpp_memory =
          owned_memory_spaces_.emplace_back(
              std::make_unique<PjRtCApiMemorySpace>(memory, this));
      addressable_memory_spaces_.push_back(cpp_memory.get());
      c_to_cpp_memory_map_[memory] = cpp_memory.get();
    }
  } else if (pjrt::GetErrorCode(client_error.get(), c_api_) !=
             PJRT_Error_Code_UNIMPLEMENTED) {
    pjrt::LogFatalIfPjrtError(client_error.get(), c_api_);
  }

  // Attach memory spaces to devices.
  // TODO(yueshengys): switch to global devices when supported.
  for (const auto& device : addressable_devices_) {
    PjRtCApiDevice* cpp_device = tensorflow::down_cast<PjRtCApiDevice*>(device);
    PJRT_Device* c_device = cpp_device->c_device();
    PJRT_Device_AddressableMemories_Args args;
    args.struct_size = PJRT_Device_AddressableMemories_Args_STRUCT_SIZE;
    args.priv = nullptr;
    args.device = c_device;

    std::unique_ptr<PJRT_Error, pjrt::PJRT_ErrorDeleter> device_error(
        c_api_->PJRT_Device_AddressableMemories(&args),
        pjrt::MakeErrorDeleter(c_api_));
    if (device_error != nullptr) {
      if (pjrt::GetErrorCode(device_error.get(), c_api_) !=
          PJRT_Error_Code_UNIMPLEMENTED) {
        pjrt::LogFatalIfPjrtError(device_error.get(), c_api_);
      }
      break;
    }

    const size_t num_memories = args.num_memories;
    cpp_device->memory_spaces_.reserve(num_memories);
    for (int i = 0; i < num_memories; ++i) {
      cpp_device->memory_spaces_.push_back(GetCppMemory(args.memories[i]));
    }
  }

  // Attach devices to memory spaces.
  // TODO(yueshengys): switch to global memories when supported.
  for (const auto& memory : addressable_memory_spaces_) {
    PjRtCApiMemorySpace* cpp_memory =
        tensorflow::down_cast<PjRtCApiMemorySpace*>(memory);
    PJRT_Memory* c_memory = cpp_memory->c_memory();
    PJRT_Memory_AddressableByDevices_Args args;
    args.struct_size = PJRT_Memory_AddressableByDevices_Args_STRUCT_SIZE;
    args.priv = nullptr;
    args.memory = c_memory;
    pjrt::LogFatalIfPjrtError(c_api_->PJRT_Memory_AddressableByDevices(&args),
                              c_api_);

    const size_t num_attached_devices = args.num_devices;
    cpp_memory->devices_.reserve(num_attached_devices);

    for (int i = 0; i < num_attached_devices; ++i) {
      cpp_memory->devices_.push_back(GetCppDevice(args.devices[i]));
    }
  }
}

int PjRtCApiClient::device_count() const { return devices_.size(); }

int PjRtCApiClient::addressable_device_count() const {
  return addressable_devices_.size();
}

absl::Span<PjRtDevice* const> PjRtCApiClient::devices() const {
  return devices_;
}

absl::Span<PjRtDevice* const> PjRtCApiClient::addressable_devices() const {
  return addressable_devices_;
}

int PjRtCApiClient::process_index() const {
  PJRT_Client_ProcessIndex_Args process_index_args;
  process_index_args.struct_size = PJRT_Client_ProcessIndex_Args_STRUCT_SIZE;
  process_index_args.priv = nullptr;
  process_index_args.client = c_client_.get();
  pjrt::LogFatalIfPjrtError(
      c_api_->PJRT_Client_ProcessIndex(&process_index_args), c_api_);

  return process_index_args.process_index;
}

absl::string_view PjRtCApiClient::platform_version() const {
  return platform_version_;
}

static DeviceAssignment CalculateDefaultAssignment(
    int num_replicas, int num_partitions,
    absl::Span<const int> device_assignment) {
  DeviceAssignment cpp_device_assignment(num_replicas, num_partitions);
  const int* iterator = device_assignment.begin();
  for (int replica = 0; replica < num_replicas; ++replica) {
    for (int partition = 0; partition < num_partitions; ++partition) {
      cpp_device_assignment(replica, partition) = *(iterator++);
    }
  }
  return cpp_device_assignment;
}

StatusOr<DeviceAssignment> PjRtCApiClient::GetDefaultDeviceAssignment(
    int num_replicas, int num_partitions) const {
  PJRT_Client_DefaultDeviceAssignment_Args args;
  args.struct_size = PJRT_Client_DefaultDeviceAssignment_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.client = c_client_.get();
  args.num_replicas = num_replicas;
  args.num_partitions = num_partitions;
  std::vector<int> assignment_buffer(num_replicas * num_partitions);
  args.default_assignment_size = assignment_buffer.size();
  args.default_assignment = assignment_buffer.data();
  RETURN_STATUS_IF_PJRT_ERROR(
      c_api_->PJRT_Client_DefaultDeviceAssignment(&args), c_api_);
  absl::Span<const int> param{args.default_assignment,
                              args.default_assignment_size};
  return CalculateDefaultAssignment(args.num_replicas, args.num_partitions,
                                    param);
}

StatusOr<std::optional<std::string>> PjRtCApiClient::ExecutableFingerprint(
    const PjRtLoadedExecutable& executable) const {
  PJRT_LoadedExecutable_Fingerprint_Args args;
  args.struct_size = PJRT_LoadedExecutable_Fingerprint_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.executable =
      tensorflow::down_cast<const PjRtCApiLoadedExecutable*>(&executable)
          ->c_loaded_executable();
  std::unique_ptr<PJRT_Error, pjrt::PJRT_ErrorDeleter> error(
      c_api_->PJRT_LoadedExecutable_Fingerprint(&args),
      pjrt::MakeErrorDeleter(c_api_));

  if (error && pjrt::GetErrorCode(error.get(), c_api_) ==
                   PJRT_Error_Code_UNIMPLEMENTED) {
    return {std::nullopt};
  }
  if (error) {
    xla::Status s = ::pjrt::PjrtErrorToStatus(error.get(), c_api_);
    return s;
  }
  if (args.executable_fingerprint == nullptr ||
      args.executable_fingerprint_size == 0) {
    return {std::nullopt};
  }
  std::string fingerprint = std::string(args.executable_fingerprint,
                                        args.executable_fingerprint_size);
  return {fingerprint};
}

StatusOr<PjRtDevice*> PjRtCApiClient::LookupDevice(int device_id) const {
  PJRT_Client_LookupDevice_Args args;
  args.struct_size = PJRT_Client_LookupDevice_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.client = c_client_.get();
  args.id = device_id;
  RETURN_STATUS_IF_PJRT_ERROR(c_api_->PJRT_Client_LookupDevice(&args), c_api_);
  return GetCppDevice(args.device);
}

StatusOr<PjRtDevice*> PjRtCApiClient::LookupAddressableDevice(
    int local_hardware_id) const {
  PJRT_Client_LookupAddressableDevice_Args args;
  args.struct_size = PJRT_Client_LookupAddressableDevice_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.client = c_client_.get();
  args.local_hardware_id = local_hardware_id;
  RETURN_STATUS_IF_PJRT_ERROR(
      c_api_->PJRT_Client_LookupAddressableDevice(&args), c_api_);
  return GetCppDevice(args.addressable_device);
}

absl::Span<PjRtMemorySpace* const> PjRtCApiClient::memory_spaces() const {
  return addressable_memory_spaces_;
}

// Initializes `PJRT_Client_Compile_Args`, which will be used to call
// API PJRT_Client_Compile().
static StatusOr<std::unique_ptr<PjRtLoadedExecutable>> InitializeArgsAndCompile(
    PjRtCApiClient* api_client, const PJRT_Api* c_api, PJRT_Client* client,
    const CompileOptions& options, const std::string& code,
    const std::string& format) {
  PJRT_Client_Compile_Args args;
  args.struct_size = PJRT_Client_Compile_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.client = client;
  TF_ASSIGN_OR_RETURN(const CompileOptionsProto options_proto,
                      options.ToProto());
  std::string options_str = options_proto.SerializeAsString();
  args.compile_options = options_str.c_str();
  args.compile_options_size = options_str.size();

  PJRT_Program program;
  program.struct_size = PJRT_Program_STRUCT_SIZE;
  program.priv = nullptr;
  program.code = const_cast<char*>(code.c_str());
  program.code_size = code.size();
  program.format = format.c_str();
  program.format_size = format.size();
  args.program = &program;

  RETURN_STATUS_IF_PJRT_ERROR(c_api->PJRT_Client_Compile(&args), c_api);
  std::unique_ptr<PjRtLoadedExecutable> ret =
      std::make_unique<PjRtCApiLoadedExecutable>(api_client, args.executable);
  return ret;
}

StatusOr<std::unique_ptr<PjRtLoadedExecutable>> PjRtCApiClient::Compile(
    const XlaComputation& computation, CompileOptions options) {
  std::string module_str = computation.proto().SerializeAsString();
  std::string format(pjrt::kHloFormat);
  return InitializeArgsAndCompile(this, c_api_, c_client_.get(), options,
                                  module_str, format);
}

StatusOr<std::unique_ptr<PjRtLoadedExecutable>> PjRtCApiClient::Compile(
    mlir::ModuleOp module, CompileOptions options) {
  std::string module_bytecode;
  {
    llvm::raw_string_ostream os(module_bytecode);
    mlir::BytecodeWriterConfig config;
    // Pin bytecode version to 1 until transition to stable.
    // TODO(285913864): Remove post enabling frameworks to set it.
    config.setDesiredBytecodeVersion(1);
    if (mlir::failed(mlir::writeBytecodeToFile(module, os, config)))
      return absl::UnknownError("writeBytecodeToFile() failed.");
  }
  std::string format(pjrt::kMlirFormat);
  return InitializeArgsAndCompile(this, c_api_, c_client_.get(), options,
                                  module_bytecode, format);
}

StatusOr<std::unique_ptr<PjRtLoadedExecutable>>
PjRtCApiClient::DeserializeExecutable(absl::string_view serialized,
                                      std::optional<CompileOptions> options) {
  PJRT_Executable_DeserializeAndLoad_Args des_args;

  des_args.struct_size = PJRT_Executable_DeserializeAndLoad_Args_STRUCT_SIZE;
  des_args.priv = nullptr;
  des_args.client = c_client_.get();
  des_args.serialized_executable = serialized.data();
  des_args.serialized_executable_size = serialized.length();

  const PJRT_Api* api = pjrt_c_api();

  RETURN_STATUS_IF_PJRT_ERROR(
      api->PJRT_Executable_DeserializeAndLoad(&des_args), api);
  PJRT_LoadedExecutable* c_exec = des_args.loaded_executable;
  CHECK(c_exec != nullptr);
  return std::unique_ptr<PjRtLoadedExecutable>(
      std::make_unique<PjRtCApiLoadedExecutable>(this, c_exec));
}

StatusOr<const PjRtTopologyDescription*>
PjRtCApiClient::GetTopologyDescription() const {
  if (!topo_desc_.ok()) {
    return topo_desc_.status();
  }
  return &(*topo_desc_);
}

StatusOr<std::uintptr_t> PjRtCApiClient::UnsafeBufferPointer(
    PjRtBuffer* buffer) {
  // Validate that the buffer's client matches the function call's client, since
  // that could be a common error.
  // Not doing input nullptr validation since such cases should be rare, and
  // crashes should bubble up the call stack to higher layers. See b/248334153
  // for the considerations that went into this.
  if (buffer->client() != this) {
    return InvalidArgument(
        "buffer passed to PjRtCApiClient::UnsafeBufferPointer() is from a "
        "different client than that of the function call. Buffer's client "
        "platform: '%s', function call's client platform: '%s'.",
        buffer->client()->platform_name(), this->platform_name());
  }

  PJRT_Buffer_UnsafePointer_Args args;
  args.struct_size = PJRT_Buffer_UnsafePointer_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer =
      tensorflow::down_cast<const PjRtCApiBuffer*>(buffer)->c_buffer();

  RETURN_STATUS_IF_PJRT_ERROR(c_api_->PJRT_Buffer_UnsafePointer(&args), c_api_);

  return args.buffer_pointer;
}

StatusOr<std::unique_ptr<PjRtBuffer>>
PjRtCApiClient::BufferFromHostBufferInternalImpl(
    const void* data, PrimitiveType type, absl::Span<int64_t const> dims,
    std::optional<absl::Span<int64_t const>> byte_strides,
    HostBufferSemantics host_buffer_semantics,
    std::function<void()> on_done_with_host_buffer,
    std::variant<PjRtDevice*, PjRtMemorySpace*> device_or_memory,
    const Layout* device_layout) {
  if (host_buffer_semantics != HostBufferSemantics::kImmutableOnlyDuringCall &&
      host_buffer_semantics != HostBufferSemantics::kZeroCopy &&
      host_buffer_semantics !=
          HostBufferSemantics::kImmutableUntilTransferCompletes) {
    return Unimplemented(
        "PJRT C API does not support HostBufferSemantics other than "
        "HostBufferSemantics::kImmutableOnlyDuringCall, "
        "HostBufferSemantics::kZeroCopy and "
        "HostBufferSemantics::kImmutableUntilTransferCompletes.");
  }

  PJRT_Client_BufferFromHostBuffer_Args args;
  args.struct_size = PJRT_Client_BufferFromHostBuffer_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.client = c_client_.get();
  args.data = data;
  args.type = ::pjrt::ConvertToPjRtBufferType(type);

  args.dims = dims.data();
  args.num_dims = dims.size();
  if (byte_strides.has_value()) {
    args.byte_strides = byte_strides.value().data();
    args.num_byte_strides = byte_strides.value().size();
  } else {
    args.byte_strides = nullptr;
    args.num_byte_strides = 0;
  }
  pjrt::BufferMemoryLayoutData c_layout_data;
  if (device_layout != nullptr) {
    TF_ASSIGN_OR_RETURN(c_layout_data,
                        pjrt::ConvertToBufferMemoryLayoutData(*device_layout));
    args.device_layout = &c_layout_data.c_layout;
  } else {
    args.device_layout = nullptr;
  }

  args.host_buffer_semantics =
      ::pjrt::ConvertToPjRtHostBufferSemantics(host_buffer_semantics);
  if (std::holds_alternative<PjRtDevice*>(device_or_memory)) {
    args.device = tensorflow::down_cast<PjRtCApiDevice*>(
                      std::get<PjRtDevice*>(device_or_memory))
                      ->c_device();
    args.memory = nullptr;
  } else {
    CHECK(std::holds_alternative<PjRtMemorySpace*>(device_or_memory));
    args.device = nullptr;
    args.memory = tensorflow::down_cast<PjRtCApiMemorySpace*>(
                      std::get<PjRtMemorySpace*>(device_or_memory))
                      ->c_memory();
  }

  RETURN_STATUS_IF_PJRT_ERROR(c_api_->PJRT_Client_BufferFromHostBuffer(&args),
                              c_api_);

  auto buffer = std::unique_ptr<PjRtBuffer>(
      std::make_unique<PjRtCApiBuffer>(this, args.buffer));

  std::unique_ptr<PJRT_Event, ::pjrt::PJRT_EventDeleter> event(
      args.done_with_host_buffer, ::pjrt::MakeEventDeleter(c_api_));

  if (on_done_with_host_buffer) {
    PJRT_Event_OnReady_Args event_args;
    event_args.struct_size = PJRT_Event_OnReady_Args_STRUCT_SIZE;
    event_args.priv = nullptr;
    event_args.event = event.get();
    event_args.user_arg = new std::function<void(PJRT_Error*)>(
        [on_done_with_host_buffer = std::move(on_done_with_host_buffer),
         c_api = c_api_](PJRT_Error* error) {
          if (error) {
            ::pjrt::MakeErrorDeleter(c_api)(error);
          }
          on_done_with_host_buffer();
        });
    event_args.callback = [](PJRT_Error* error, void* args) {
      std::function<void(PJRT_Error*)>* on_done_with_host_buffer =
          reinterpret_cast<std::function<void(PJRT_Error*)>*>(args);
      (*on_done_with_host_buffer)(error);
      delete on_done_with_host_buffer;
    };

    RETURN_STATUS_IF_PJRT_ERROR(c_api_->PJRT_Event_OnReady(&event_args),
                                c_api_);
  }

  return buffer;
}

StatusOr<std::unique_ptr<PjRtBuffer>> PjRtCApiClient::BufferFromHostBuffer(
    const void* data, PrimitiveType type, absl::Span<int64_t const> dims,
    std::optional<absl::Span<int64_t const>> byte_strides,
    HostBufferSemantics host_buffer_semantics,
    std::function<void()> on_done_with_host_buffer,
    PjRtMemorySpace* memory_space, const Layout* device_layout) {
  return BufferFromHostBufferInternalImpl(
      data, type, dims, byte_strides, host_buffer_semantics,
      on_done_with_host_buffer, memory_space, device_layout);
}

StatusOr<std::unique_ptr<PjRtBuffer>> PjRtCApiClient::BufferFromHostBuffer(
    const void* data, PrimitiveType type, absl::Span<int64_t const> dims,
    std::optional<absl::Span<int64_t const>> byte_strides,
    HostBufferSemantics host_buffer_semantics,
    std::function<void()> on_done_with_host_buffer, PjRtDevice* device,
    const Layout* device_layout) {
  return BufferFromHostBufferInternalImpl(
      data, type, dims, byte_strides, host_buffer_semantics,
      on_done_with_host_buffer, device, device_layout);
}

StatusOr<std::unique_ptr<PjRtBuffer>> PjRtCApiClient::BufferFromHostBuffer(
    const void* data, PrimitiveType type, absl::Span<int64_t const> dims,
    std::optional<absl::Span<int64_t const>> byte_strides,
    HostBufferSemantics host_buffer_semantics,
    std::function<void()> on_done_with_host_buffer, PjRtDevice* device) {
  return BufferFromHostBufferInternalImpl(
      data, type, dims, byte_strides, host_buffer_semantics,
      on_done_with_host_buffer, device, /*device_layout=*/nullptr);
}

StatusOr<std::unique_ptr<PjRtBuffer>> PjRtCApiClient::CreateViewOfDeviceBuffer(
    void* device_ptr, const Shape& shape, PjRtDevice* device,
    std::function<void()> on_delete_callback,
    std::optional<std::intptr_t> stream) {
  PJRT_Client_CreateViewOfDeviceBuffer_Args args;
  args.struct_size = PJRT_Client_CreateViewOfDeviceBuffer_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.client = c_client_.get();
  args.device_buffer_ptr = device_ptr;
  args.dims = shape.dimensions().data();
  args.num_dims = shape.dimensions().size();
  args.element_type = pjrt::ConvertToPjRtBufferType(shape.element_type());
  pjrt::BufferMemoryLayoutData c_layout_data;
  if (shape.has_layout()) {
    TF_ASSIGN_OR_RETURN(c_layout_data,
                        pjrt::ConvertToBufferMemoryLayoutData(shape.layout()));
    args.layout = &(c_layout_data.c_layout);
  } else {
    args.layout = nullptr;
  }
  if (on_delete_callback != nullptr) {
    args.on_delete_callback_arg =
        new std::function(std::move(on_delete_callback));
    args.on_delete_callback = [](void* device_buffer_ptr, void* user_arg) {
      auto* c_func = reinterpret_cast<std::function<void()>*>(user_arg);
      (*c_func)();
      delete c_func;
    };
  } else {
    args.on_delete_callback = nullptr;
    args.on_delete_callback_arg = nullptr;
  }
  args.device = tensorflow::down_cast<PjRtCApiDevice*>(device)->c_device();
  if (stream.has_value()) {
    args.stream = *stream;
  } else {
    args.stream = reinterpret_cast<intptr_t>(nullptr);
  }
  const PJRT_Api* c_api = pjrt_c_api();
  // TODO(jieying): To be removed after 12/29/2023.
  if (c_api->pjrt_api_version.minor_version < 33) {
    return Unimplemented(
        "The plugin does not support CreateViewOfDeviceBuffer");
  }
  RETURN_STATUS_IF_PJRT_ERROR(
      c_api->PJRT_Client_CreateViewOfDeviceBuffer(&args), c_api);

  return std::unique_ptr<PjRtBuffer>(
      std::make_unique<PjRtCApiBuffer>(this, args.buffer));
}

const PJRT_Api* PjRtCApiClient::pjrt_c_api() const { return c_api_; }

// --------------------------------- Devices -----------------------------------

PjRtCApiDeviceDescription::PjRtCApiDeviceDescription(
    const PJRT_Api* c_api, PJRT_DeviceDescription* device_description)
    : c_api_(c_api), device_description_(device_description) {
  InitAttributes();
}

int PjRtCApiDeviceDescription::id() const {
  PJRT_DeviceDescription_Id_Args args;
  args.struct_size = PJRT_DeviceDescription_Id_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.device_description = device_description_;
  pjrt::LogFatalIfPjrtError(c_api_->PJRT_DeviceDescription_Id(&args), c_api_);
  return args.id;
}

int PjRtCApiDeviceDescription::process_index() const {
  PJRT_DeviceDescription_ProcessIndex_Args args;
  args.struct_size = PJRT_DeviceDescription_ProcessIndex_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.device_description = device_description_;
  pjrt::LogFatalIfPjrtError(c_api_->PJRT_DeviceDescription_ProcessIndex(&args),
                            c_api_);
  return args.process_index;
}

void PjRtCApiDeviceDescription::InitAttributes() {
  attributes_ = {};
  PJRT_DeviceDescription_Attributes_Args args;
  args.struct_size = PJRT_DeviceDescription_Attributes_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.device_description = device_description_;
  pjrt::LogFatalIfPjrtError(c_api_->PJRT_DeviceDescription_Attributes(&args),
                            c_api_);

  for (int i = 0; i < args.num_attributes; ++i) {
    const auto& attribute = args.attributes[i];
    std::string attribute_name(attribute.name, attribute.name_size);
    switch (attribute.type) {
      case PJRT_NamedValue_Type::PJRT_NamedValue_kString: {
        std::string string_value(attribute.string_value, attribute.value_size);
        attributes_[attribute_name] = PjRtDeviceAttribute(string_value);
        break;
      }
      case PJRT_NamedValue_Type::PJRT_NamedValue_kInt64: {
        attributes_[attribute_name] =
            PjRtDeviceAttribute(attribute.int64_value);
        break;
      }
      case PJRT_NamedValue_Type::PJRT_NamedValue_kInt64List: {
        const int64_t* array_ptr(attribute.int64_array_value);
        std::vector<int64_t> int64_array(array_ptr,
                                         array_ptr + attribute.value_size);
        attributes_[attribute_name] = PjRtDeviceAttribute(int64_array);
        break;
      }
      // Do not allow other types (such as
      // PJRT_NamedValue::PJRT_NamedValue_kFloat) since device attributes
      // currently should not return other types. Also C API client currently
      // does not support forward compatibility (such as if the underlying
      // PJRT library is a newer version that returns types not supported by
      // this client). Failing here to prevent undefined behavior.
      default: {
        LOG(FATAL) << "PJRT_DeviceDescription_Attributes() returned attribute '"
                   << attribute_name << "' with unsupported type "
                   << attribute.type
                   << " to PjRtCApiDeviceDescription::InitAttributes()";
        break;
      }
    }
  }
}

const absl::flat_hash_map<std::string, PjRtDeviceAttribute>&
PjRtCApiDeviceDescription::Attributes() const {
  return attributes_;
}

absl::string_view PjRtCApiDeviceDescription::device_kind() const {
  PJRT_DeviceDescription_Kind_Args args;
  args.struct_size = PJRT_DeviceDescription_Kind_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.device_description = device_description_;

  pjrt::LogFatalIfPjrtError(c_api_->PJRT_DeviceDescription_Kind(&args), c_api_);

  absl::string_view device_kind(args.device_kind, args.device_kind_size);
  return device_kind;
}

absl::string_view PjRtCApiDeviceDescription::DebugString() const {
  PJRT_DeviceDescription_DebugString_Args args;
  args.struct_size = PJRT_DeviceDescription_DebugString_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.device_description = device_description_;
  pjrt::LogFatalIfPjrtError(c_api_->PJRT_DeviceDescription_DebugString(&args),
                            c_api_);
  absl::string_view debug_string(args.debug_string, args.debug_string_size);
  return debug_string;
}

absl::string_view PjRtCApiDeviceDescription::ToString() const {
  PJRT_DeviceDescription_ToString_Args args;
  args.struct_size = PJRT_DeviceDescription_ToString_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.device_description = device_description_;
  pjrt::LogFatalIfPjrtError(c_api_->PJRT_DeviceDescription_ToString(&args),
                            c_api_);
  absl::string_view to_string(args.to_string, args.to_string_size);
  return to_string;
}

PjRtCApiDevice::PjRtCApiDevice(PJRT_Device* device, PjRtCApiClient* client)
    : client_(client),
      device_(device),
      description_(client->pjrt_c_api(),
                   pjrt::GetDeviceDescription(client->pjrt_c_api(), device)) {}

PjRtClient* PjRtCApiDevice::client() const { return client_; }

bool PjRtCApiDevice::IsAddressable() const {
  PJRT_Device_IsAddressable_Args args;
  args.struct_size = PJRT_Device_IsAddressable_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.device = device_;
  const PJRT_Api* api = client_->pjrt_c_api();
  pjrt::LogFatalIfPjrtError(api->PJRT_Device_IsAddressable(&args), api);
  return args.is_addressable;
}

int PjRtCApiDevice::local_hardware_id() const {
  PJRT_Device_LocalHardwareId_Args args;
  args.struct_size = PJRT_Device_LocalHardwareId_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.device = device_;
  const PJRT_Api* api = client_->pjrt_c_api();
  pjrt::LogFatalIfPjrtError(api->PJRT_Device_LocalHardwareId(&args), api);
  return args.local_hardware_id;
}

StatusOr<PjRtMemorySpace*> PjRtCApiDevice::default_memory_space() const {
  PJRT_Device_DefaultMemory_Args args;
  args.struct_size = PJRT_Device_DefaultMemory_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.device = device_;
  const PJRT_Api* api = client_->pjrt_c_api();
  RETURN_STATUS_IF_PJRT_ERROR(api->PJRT_Device_DefaultMemory(&args), api);
  return client_->GetCppMemory(args.memory);
}

StatusOr<tsl::AllocatorStats> PjRtCApiDevice::GetAllocatorStats() const {
  PJRT_Device_MemoryStats_Args args;
  args.struct_size = PJRT_Device_MemoryStats_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.device = device_;
  const PJRT_Api* api = client_->pjrt_c_api();
  RETURN_STATUS_IF_PJRT_ERROR(api->PJRT_Device_MemoryStats(&args), api);

  tsl::AllocatorStats result;
  result.bytes_in_use = args.bytes_in_use;

  // The PJRT C API supports optionally returning most fields, but only some
  // fields in tsl::AllocatorStats are optional. Return -1 for unset,
  // non-optional fields. We could change tsl::AllocatorStats to have all
  // optional fields, but that requires changing a lot of callers.
  if (args.peak_bytes_in_use_is_set) {
    result.peak_bytes_in_use = args.peak_bytes_in_use;
  } else {
    result.peak_bytes_in_use = -1;
  }
  if (args.num_allocs_is_set) {
    result.num_allocs = args.num_allocs;
  } else {
    result.num_allocs = -1;
  }
  if (args.largest_alloc_size_is_set) {
    result.largest_alloc_size = args.largest_alloc_size;
  } else {
    result.largest_alloc_size = -1;
  }
  if (args.bytes_limit_is_set) {
    result.bytes_limit = args.bytes_limit;
  }
  if (args.bytes_reserved_is_set) {
    result.bytes_reserved = args.bytes_reserved;
  } else {
    result.bytes_reserved = -1;
  }
  if (args.peak_bytes_reserved_is_set) {
    result.peak_bytes_reserved = args.peak_bytes_reserved;
  } else {
    result.peak_bytes_reserved = -1;
  }
  if (args.bytes_reservable_limit_is_set) {
    result.bytes_reservable_limit = args.bytes_reservable_limit;
  }
  if (args.largest_free_block_bytes_is_set) {
    result.largest_free_block_bytes = args.largest_free_block_bytes;
  } else {
    result.largest_free_block_bytes = -1;
  }
  if (args.pool_bytes_is_set) {
    result.pool_bytes = args.pool_bytes;
  }
  if (args.peak_pool_bytes_is_set) {
    result.peak_pool_bytes = args.peak_pool_bytes;
  }
  return result;
}

// ------------------------------- Memory --------------------------------------

const PJRT_Api* PjRtCApiMemorySpace::pjrt_c_api() const {
  return client_->pjrt_c_api();
}

PjRtClient* PjRtCApiMemorySpace::client() const { return client_; }

int PjRtCApiMemorySpace::id() const {
  PJRT_Memory_Id_Args args;
  args.struct_size = PJRT_Memory_Id_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.memory = c_memory_;
  pjrt::LogFatalIfPjrtError(pjrt_c_api()->PJRT_Memory_Id(&args), pjrt_c_api());
  return args.id;
}

absl::string_view PjRtCApiMemorySpace::memory_space_kind() const {
  PJRT_Memory_Kind_Args args;
  args.struct_size = PJRT_Memory_Kind_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.memory = c_memory_;

  pjrt::LogFatalIfPjrtError(pjrt_c_api()->PJRT_Memory_Kind(&args),
                            pjrt_c_api());

  return absl::string_view(args.memory_kind, args.memory_kind_size);
}

absl::string_view PjRtCApiMemorySpace::DebugString() const {
  PJRT_Memory_DebugString_Args args;
  args.struct_size = PJRT_Memory_DebugString_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.memory = c_memory_;
  pjrt::LogFatalIfPjrtError(pjrt_c_api()->PJRT_Memory_DebugString(&args),
                            pjrt_c_api());
  return absl::string_view(args.debug_string, args.debug_string_size);
}

absl::string_view PjRtCApiMemorySpace::ToString() const {
  PJRT_Memory_ToString_Args args;
  args.struct_size = PJRT_Memory_ToString_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.memory = c_memory_;
  pjrt::LogFatalIfPjrtError(pjrt_c_api()->PJRT_Memory_ToString(&args),
                            pjrt_c_api());
  return absl::string_view(args.to_string, args.to_string_size);
}

// ------------------------------- Executables ---------------------------------

PjRtCApiExecutable::PjRtCApiExecutable(const PJRT_Api* c_api,
                                       PJRT_Executable* executable)
    : c_api_(c_api),
      executable_(executable, ::pjrt::MakeExecutableDeleter(c_api)) {}

absl::string_view PjRtCApiExecutable::name() const {
  auto* c_api = pjrt_c_api();
  auto* executable = c_executable();
  PJRT_Executable_Name_Args args;
  args.executable = executable;
  args.struct_size = PJRT_Executable_Name_Args_STRUCT_SIZE;
  args.priv = nullptr;
  pjrt::LogFatalIfPjrtError(c_api->PJRT_Executable_Name(&args), c_api);

  return absl::string_view(args.executable_name, args.executable_name_size);
}

int PjRtCApiExecutable::num_replicas() const {
  auto* c_api = pjrt_c_api();
  auto* executable = c_executable();
  PJRT_Executable_NumReplicas_Args args;
  args.executable = executable;
  args.struct_size = PJRT_Executable_NumReplicas_Args_STRUCT_SIZE;
  args.priv = nullptr;
  pjrt::LogFatalIfPjrtError(c_api->PJRT_Executable_NumReplicas(&args), c_api);

  return args.num_replicas;
}

int PjRtCApiExecutable::num_partitions() const {
  auto* c_api = pjrt_c_api();
  auto* executable = c_executable();
  PJRT_Executable_NumPartitions_Args args;
  args.executable = executable;
  args.struct_size = PJRT_Executable_NumPartitions_Args_STRUCT_SIZE;
  args.priv = nullptr;
  pjrt::LogFatalIfPjrtError(c_api->PJRT_Executable_NumPartitions(&args), c_api);

  return args.num_partitions;
}

int64_t PjRtCApiExecutable::SizeOfGeneratedCodeInBytes() const {
  auto* c_api = pjrt_c_api();
  auto* executable = c_executable();
  PJRT_Executable_SizeOfGeneratedCodeInBytes_Args args;
  args.struct_size =
      PJRT_Executable_SizeOfGeneratedCodeInBytes_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.executable = executable;

  pjrt::LogFatalIfPjrtError(
      c_api->PJRT_Executable_SizeOfGeneratedCodeInBytes(&args), c_api);
  return args.size_in_bytes;
}

StatusOr<absl::flat_hash_map<std::string, PjRtValueType>>
PjRtCApiExecutable::GetCostAnalysis() const {
  // Initialize function call args
  PJRT_Executable_GetCostAnalysis_Args args;
  args.struct_size = PJRT_Executable_GetCostAnalysis_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.executable = c_executable();

  // Make PJRT C API call
  const PJRT_Api* c_api = pjrt_c_api();
  RETURN_STATUS_IF_PJRT_ERROR(c_api->PJRT_Executable_GetCostAnalysis(&args),
                              c_api);

  // Copy returned properties to output map
  return pjrt::ConvertFromPjRtNamedValueList(args.properties,
                                             args.num_properties);
}

StatusOr<std::vector<std::vector<PrimitiveType>>>
PjRtCApiExecutable::GetOutputElementTypes() const {
  PJRT_Executable_OutputElementTypes_Args args;
  args.struct_size = PJRT_Executable_OutputElementTypes_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.executable = c_executable();

  const PJRT_Api* c_api = pjrt_c_api();

  // TODO(yueshengys): To be removed after 11/29/2023.
  if (c_api->PJRT_Executable_OutputElementTypes == nullptr) {
    return Unimplemented("PJRT C API does not support GetOutputElementTypes");
  }

  RETURN_STATUS_IF_PJRT_ERROR(c_api->PJRT_Executable_OutputElementTypes(&args),
                              c_api);

  std::vector<PrimitiveType> out;
  out.reserve(args.num_output_types);
  for (int i = 0; i < args.num_output_types; ++i) {
    out.push_back(pjrt::ConvertFromPjRtBufferType(args.output_types[i]));
  }
  return std::vector<std::vector<PrimitiveType>>{std::move(out)};
}

StatusOr<std::vector<std::vector<DimensionVector>>>
PjRtCApiExecutable::GetOutputDimensions() const {
  PJRT_Executable_OutputDimensions_Args args;
  args.struct_size = PJRT_Executable_OutputDimensions_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.executable = c_executable();

  const PJRT_Api* c_api = pjrt_c_api();

  // TODO(yueshengys): To be removed after 11/29/2023.
  if (c_api->PJRT_Executable_OutputDimensions == nullptr) {
    return Unimplemented("PJRT C API does not support GetOutputDimensions");
  }

  RETURN_STATUS_IF_PJRT_ERROR(c_api->PJRT_Executable_OutputDimensions(&args),
                              c_api);

  std::vector<DimensionVector> out;
  out.reserve(args.num_outputs);
  int index = 0;
  for (int i = 0; i < args.num_outputs; ++i) {
    DimensionVector dimensions;
    dimensions.reserve(args.dim_sizes[i]);
    for (int j = 0; j < args.dim_sizes[i]; ++j) {
      dimensions.push_back(args.dims[index++]);
    }
    out.push_back(std::move(dimensions));
  }
  return std::vector<std::vector<DimensionVector>>{std::move(out)};
}

StatusOr<std::vector<std::vector<absl::string_view>>>
PjRtCApiExecutable::GetOutputMemoryKinds() const {
  PJRT_Executable_OutputMemoryKinds_Args args;
  args.struct_size = PJRT_Executable_OutputMemoryKinds_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.executable = c_executable();

  const PJRT_Api* c_api = pjrt_c_api();
  RETURN_STATUS_IF_PJRT_ERROR(c_api->PJRT_Executable_OutputMemoryKinds(&args),
                              c_api);

  std::vector<absl::string_view> out;
  out.reserve(args.num_outputs);
  for (int i = 0; i < args.num_outputs; ++i) {
    out.push_back(
        absl::string_view(args.memory_kinds[i], args.memory_kind_sizes[i]));
  }
  return std::vector<std::vector<absl::string_view>>{std::move(out)};
}

StatusOr<std::vector<std::shared_ptr<HloModule>>>
PjRtCApiExecutable::GetHloModules() const {
  auto* c_api = pjrt_c_api();
  auto* executable = c_executable();
  PJRT_Executable_OptimizedProgram_Args args;
  args.struct_size = PJRT_Executable_OptimizedProgram_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.executable = executable;
  PJRT_Program program;
  program.struct_size = PJRT_Program_STRUCT_SIZE;
  program.priv = nullptr;
  program.code = nullptr;
  args.program = &program;

  RETURN_STATUS_IF_PJRT_ERROR(c_api->PJRT_Executable_OptimizedProgram(&args),
                              c_api);

  constexpr size_t TWO_GIBIBYTES = 2ull * 1024 * 1024 * 1024;
  const size_t code_size = args.program->code_size;
  CHECK(code_size < TWO_GIBIBYTES);
  std::string code(code_size, ' ');
  args.program->code = code.data();
  RETURN_STATUS_IF_PJRT_ERROR(c_api->PJRT_Executable_OptimizedProgram(&args),
                              c_api);

  absl::string_view program_format(program.format, program.format_size);
  if (program_format != ::pjrt::kHloWithConfigFormat &&
      program_format != ::pjrt::kMlirFormat) {
    return xla::InternalError(
        "expected program format `hlo_with_config` or `mlir` but got %s",
        program_format);
  }

  if (program_format == ::pjrt::kMlirFormat) {
    xla::HloProto hlo_proto;
    mlir::MLIRContext ctx;
    mlir::DialectRegistry registry;
    registry.insert<mlir::arith::ArithDialect, mlir::func::FuncDialect,
                    mlir::ml_program::MLProgramDialect,
                    mlir::shape::ShapeDialect>();
    mlir::stablehlo::registerAllDialects(registry);
    mlir::mhlo::registerAllMhloDialects(registry);
    ctx.appendDialectRegistry(registry);
    auto module = mlir::parseSourceString<mlir::ModuleOp>(code, &ctx);
    if (!module) return xla::InternalError("failed to parse source module");
    mlir::PassManager pm(&ctx);
    pm.addPass(mlir::mhlo::createStablehloLegalizeToHloPass());
    if (mlir::failed(pm.run(module.get())))
      return xla::InternalError("failed to convert to MHLO");
    mlir::MlirToHloConversionOptions options;
    // TODO(jieying): Tuple args should really come from GetCompileOptions (or
    // equivalent) once implemented.
    TF_RETURN_IF_ERROR(mlir::ConvertMlirHloToHlo(
        module.get(), &hlo_proto, /*use_tuple_args=*/false,
        /*return_tuple=*/false, options));
    xla::DebugOptions debug_options;
    TF_ASSIGN_OR_RETURN(xla::HloModuleConfig module_config,
                        xla::HloModule::CreateModuleConfigFromProto(
                            hlo_proto.hlo_module(), debug_options));
    std::vector<std::shared_ptr<HloModule>> out;
    TF_ASSIGN_OR_RETURN(
        std::unique_ptr<HloModule> hlo_module,
        HloModule::CreateFromProto(hlo_proto.hlo_module(), module_config));
    out.push_back(std::move(hlo_module));
    return out;
  }

  HloModuleProtoWithConfig proto;
  proto.ParseFromString(code);
  std::vector<std::shared_ptr<HloModule>> out;
  TF_ASSIGN_OR_RETURN(std::unique_ptr<HloModule> module,
                      HloModule::CreateFromProtoWithConfig(proto));
  out.push_back(std::move(module));
  return out;
}

StatusOr<std::string> PjRtCApiExecutable::SerializeExecutable() const {
  auto* c_api = pjrt_c_api();
  auto* executable = c_executable();
  PJRT_Executable_Serialize_Args ser_args;
  ser_args.struct_size = PJRT_Executable_Serialize_Args_STRUCT_SIZE;
  ser_args.priv = nullptr;
  ser_args.executable = executable;
  ser_args.serialized_executable = nullptr;

  RETURN_STATUS_IF_PJRT_ERROR(c_api->PJRT_Executable_Serialize(&ser_args),
                              c_api);
  absl::Cleanup cleanup = [&ser_args] {
    ser_args.serialized_executable_deleter(ser_args.serialized_executable);
  };
  return std::string(ser_args.serialized_bytes, ser_args.serialized_bytes_size);
}

StatusOr<std::string> PjRtCApiExecutable::FingerprintExecutable() const {
  PJRT_Executable_Fingerprint_Args args;
  args.struct_size = PJRT_Executable_Fingerprint_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.executable = c_executable();

  RETURN_STATUS_IF_PJRT_ERROR(c_api_->PJRT_Executable_Fingerprint(&args),
                              c_api_);

  return std::string(args.executable_fingerprint,
                     args.executable_fingerprint_size);
}

// ------------------------ Loaded Executables ---------------------------------

PjRtCApiLoadedExecutable::PjRtCApiLoadedExecutable(
    PjRtCApiClient* client, PJRT_LoadedExecutable* executable)
    : client_(client),
      loaded_executable_(executable, ::pjrt::MakeLoadedExecutableDeleter(
                                         client->pjrt_c_api())) {
  PJRT_LoadedExecutable_GetExecutable_Args args;
  args.struct_size = PJRT_LoadedExecutable_GetExecutable_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.loaded_executable = c_loaded_executable();
  args.executable = nullptr;
  pjrt::LogFatalIfPjrtError(
      pjrt_c_api()->PJRT_LoadedExecutable_GetExecutable(&args), pjrt_c_api());
  executable_ =
      std::make_unique<PjRtCApiExecutable>(pjrt_c_api(), args.executable);
  InitDevices();
}

void PjRtCApiLoadedExecutable::InitDevices() {
  PJRT_LoadedExecutable_AddressableDevices_Args args;
  args.struct_size = PJRT_LoadedExecutable_AddressableDevices_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.executable = c_loaded_executable();
  args.addressable_devices = nullptr;
  args.num_addressable_devices = 0;

  const PJRT_Api* api = pjrt_c_api();
  pjrt::LogFatalIfPjrtError(
      api->PJRT_LoadedExecutable_AddressableDevices(&args), api);

  const size_t num_addressable_devices = args.num_addressable_devices;
  addressable_devices_.reserve(num_addressable_devices);

  for (size_t i = 0; i < num_addressable_devices; ++i) {
    PJRT_Device* device = args.addressable_devices[i];
    PjRtCApiDevice* c_api_device = client_->GetCppDevice(device);
    addressable_devices_.push_back(c_api_device);
  }
}

static std::vector<std::vector<PJRT_Buffer*>> Convert2DCppBuffersToCBuffers(
    absl::Span<const std::vector<PjRtBuffer*>> cpp_lists) {
  std::vector<std::vector<PJRT_Buffer*>> c_lists;
  c_lists.reserve(cpp_lists.size());
  for (const auto& cpp_list : cpp_lists) {
    auto& c_list = c_lists.emplace_back();
    c_list.reserve(cpp_list.size());
    for (PjRtBuffer* buffer : cpp_list) {
      auto* c_api_argument = tensorflow::down_cast<PjRtCApiBuffer*>(buffer);
      c_list.push_back(c_api_argument->c_buffer());
    }
  }
  return c_lists;
}

static std::vector<std::vector<std::unique_ptr<PjRtBuffer>>>
Convert2DCBuffersToCppBuffers(PJRT_Buffer** const* c_lists, size_t outer_size,
                              int inner_size, xla::PjRtCApiClient* client) {
  std::vector<std::vector<std::unique_ptr<PjRtBuffer>>> ret;
  for (size_t i = 0; i < outer_size; ++i) {
    auto& output_list = ret.emplace_back();
    output_list.reserve(inner_size);
    for (size_t j = 0; j < inner_size; ++j) {
      output_list.push_back(
          std::make_unique<PjRtCApiBuffer>(client, c_lists[i][j]));
    }
  }
  return ret;
}

// Wraps original `xla::SendCallback` inside `PJRT_SendCallbackInfo` using
// 1) void* `user_arg` to capture `cpp_send_callback.callback` (std::function)
// 2) `PJRT_SendCallback` function pointer, which reinterprets and calls
// `user_arg` to call `cpp_send_callback.callback` function. This appends to
// `send_callback_functions`, which must be kept alive for as lnog as the
// returned PJRT_SendCallbackInfo is needed.
//
// TODO(yeounoh) move this to pjrt_c_api_helpers after implementing C API for
// the opaque types `PJRT_Chunk` and `PJRT_CopyToDeviceStream`.
PJRT_SendCallbackInfo CppSendCallbackToC(
    const xla::SendCallback& cpp_send_callback,
    PjRtCApiLoadedExecutable::SendCallbackFunction* send_callback_function) {
  *send_callback_function =
      [&send_callback = cpp_send_callback.callback](
          PJRT_Chunk* chunk, PJRT_CallbackError* callback_error,
          size_t total_size_in_bytes, bool done) -> PJRT_Error* {
    // PJRT C API doesn't support
    // use_major_to_minor_data_layout_for_callbacks = false
    xla::Shape dummy_shape;
    xla::Status status = send_callback(xla::PjRtTransferMetadata{dummy_shape},
                                       ::pjrt::ConvertToCppChunk(*chunk),
                                       total_size_in_bytes, done);
    if (!status.ok()) {
      absl::string_view message = status.message();
      return (*callback_error)(pjrt::StatusCodeToPjrtErrorCode(status.code()),
                               message.data(), message.size());
    }
    return nullptr;
  };
  return PJRT_SendCallbackInfo{
      /*channel_id=*/cpp_send_callback.channel_id,
      /*user_arg=*/send_callback_function,
      /*send_callback=*/
      [](PJRT_Chunk* chunk, PJRT_CallbackError* callback_error,
         size_t total_size_in_bytes, bool done, void* user_arg) -> PJRT_Error* {
        // PJRT_SendCallback, `send_callback` is internal C interface callback
        // representation that cpatures the client C++ callback in void*
        // `user_arg` and reinterprets in the lower-level runtime for execution.
        // `user_arg` captures `send_callback_function` which is
        // SendCallbackFunction*.
        PjRtCApiLoadedExecutable::SendCallbackFunction* send_callback =
            reinterpret_cast<PjRtCApiLoadedExecutable::SendCallbackFunction*>(
                user_arg);
        return (*send_callback)(chunk, callback_error, total_size_in_bytes,
                                done);
      }};
}

CApiCopyToDeviceStream::CApiCopyToDeviceStream(
    PJRT_CopyToDeviceStream* c_stream, const PJRT_Api* c_api)
    : CopyToDeviceStream(/*total_bytes=*/0, /*granule_bytes=*/0),
      c_stream_(c_stream),
      c_api_(c_api) {
  PJRT_CopyToDeviceStream_TotalBytes_Args total_bytes_args;
  total_bytes_args.struct_size =
      PJRT_CopyToDeviceStream_TotalBytes_Args_STRUCT_SIZE;
  total_bytes_args.priv = nullptr;
  total_bytes_args.stream = c_stream_;
  pjrt::LogFatalIfPjrtError(
      c_api_->PJRT_CopyToDeviceStream_TotalBytes(&total_bytes_args), c_api_);
  total_bytes_ = total_bytes_args.total_bytes;

  PJRT_CopyToDeviceStream_GranuleSize_Args granule_size_args;
  granule_size_args.struct_size =
      PJRT_CopyToDeviceStream_GranuleSize_Args_STRUCT_SIZE;
  granule_size_args.priv = nullptr;
  granule_size_args.stream = c_stream_;
  pjrt::LogFatalIfPjrtError(
      c_api_->PJRT_CopyToDeviceStream_GranuleSize(&granule_size_args), c_api_);
  granule_bytes_ = granule_size_args.granule_size_in_bytes;
}

CApiCopyToDeviceStream::~CApiCopyToDeviceStream() {
  PJRT_CopyToDeviceStream_Destroy_Args destroy_args;
  destroy_args.struct_size = PJRT_CopyToDeviceStream_Destroy_Args_STRUCT_SIZE;
  destroy_args.priv = nullptr;
  destroy_args.stream = c_stream_;
  pjrt::LogFatalIfPjrtError(
      c_api_->PJRT_CopyToDeviceStream_Destroy(&destroy_args), c_api_);
}

PjRtFuture<Status> CApiCopyToDeviceStream::AddChunk(PjRtChunk chunk) {
  PJRT_Chunk c_chunk = ::pjrt::ConvertFromCppChunk(std::move(chunk));

  PJRT_CopyToDeviceStream_AddChunk_Args add_chunk_args;
  add_chunk_args.struct_size =
      PJRT_CopyToDeviceStream_AddChunk_Args_STRUCT_SIZE;
  add_chunk_args.priv = nullptr;
  add_chunk_args.stream = c_stream_;
  add_chunk_args.chunk = &c_chunk;

  PJRT_CopyToDeviceStream_CurrentBytes_Args current_bytes_args;
  current_bytes_args.struct_size =
      PJRT_CopyToDeviceStream_CurrentBytes_Args_STRUCT_SIZE;
  current_bytes_args.priv = nullptr;
  current_bytes_args.stream = c_stream_;

  {
    absl::MutexLock lock(&mu_);
    RETURN_FUTURE_IF_ERROR(
        c_api_->PJRT_CopyToDeviceStream_AddChunk(&add_chunk_args), c_api_);
    RETURN_FUTURE_IF_ERROR(
        c_api_->PJRT_CopyToDeviceStream_CurrentBytes(&current_bytes_args),
        c_api_);
    current_bytes_ = current_bytes_args.current_bytes;
  }

  CHECK(add_chunk_args.transfer_complete != nullptr);
  return ::pjrt::ConvertCEventToCppFuture(add_chunk_args.transfer_complete,
                                          c_api_);
}

// Wraps original `xla::RecvCallback` inside `PJRT_RecvCallbackInfo` using
// 1) void* `user_arg` to capture `cpp_recv_callback.callback` (std::function)
// 2) `PJRT_RecvCallback` function pointer, which reinterprets and calls
// `user_arg` to call `cpp_send_callback.callback` function. This appends to
// `recv_callback_functions`, which must be kept alive for as lnog as the
// returned PJRT_RecvCallbackInfo is needed.
//
// TODO(yeounoh) move this to pjrt_c_api_helpers after implementing C API for
// the opaque types `PJRT_Chunk` and `PJRT_CopyToDeviceStream`.
PJRT_RecvCallbackInfo CppRecvCallbackToC(
    const xla::RecvCallback& cpp_recv_callback, const PJRT_Api* c_api,
    PjRtCApiLoadedExecutable::RecvCallbackFunction* recv_callback_function) {
  *recv_callback_function = [&recv_callback = cpp_recv_callback.callback,
                             c_api](PJRT_CopyToDeviceStream* stream) {
    // PJRT C API doesn't support
    // use_major_to_minor_data_layout_for_callbacks = false
    xla::Shape dummy_shape;
    recv_callback(xla::PjRtTransferMetadata{dummy_shape},
                  std::make_unique<CApiCopyToDeviceStream>(stream, c_api));
  };
  return PJRT_RecvCallbackInfo{
      /*channel_id=*/cpp_recv_callback.channel_id,
      /*user_arg=*/recv_callback_function,
      /*recv_callback=*/
      [](PJRT_CopyToDeviceStream* stream, void* user_arg) {
        // PJRT_RecvCallback, `recv_callback` is internal C interface callback
        // representation that cpatures the client C++ callback in void*
        // `user_arg` and reinterprets in the lower-level runtime for execution.
        // `user_arg` captures `recv_callback_function` which is
        // RecvCallbackFunction*.
        PjRtCApiLoadedExecutable::RecvCallbackFunction* recv_callback =
            reinterpret_cast<PjRtCApiLoadedExecutable::RecvCallbackFunction*>(
                user_arg);
        (*recv_callback)(stream);
      }};
}

static void CppSendCallbackListsToC(
    absl::Span<const std::vector<xla::SendCallback>> cpp_lists,
    std::vector<PjRtCApiLoadedExecutable::SendCallbackFunction>&
        send_callback_functions,
    std::vector<std::vector<PJRT_SendCallbackInfo>>& c_lists) {
  if (cpp_lists.empty()) return;

  send_callback_functions.resize(cpp_lists.size() * cpp_lists[0].size());
  c_lists.reserve(cpp_lists.size());

  int func_count = 0;
  for (const std::vector<xla::SendCallback>& cpp_list : cpp_lists) {
    std::vector<PJRT_SendCallbackInfo>& c_list = c_lists.emplace_back();
    c_list.reserve(cpp_list.size());
    for (const xla::SendCallback& cpp_callback : cpp_list) {
      c_list.emplace_back(CppSendCallbackToC(
          cpp_callback, &send_callback_functions[func_count++]));
    }
  }
}

static void CppRecvCallbackListsToC(
    absl::Span<const std::vector<xla::RecvCallback>> cpp_lists,
    const PJRT_Api* c_api,
    std::vector<PjRtCApiLoadedExecutable::RecvCallbackFunction>&
        recv_callback_functions,
    std::vector<std::vector<PJRT_RecvCallbackInfo>>& c_lists) {
  if (cpp_lists.empty()) return;

  recv_callback_functions.resize(cpp_lists.size() * cpp_lists[0].size());
  c_lists.reserve(cpp_lists.size());

  int func_count = 0;
  for (const auto& cpp_list : cpp_lists) {
    std::vector<PJRT_RecvCallbackInfo>& c_list = c_lists.emplace_back();
    c_list.reserve(cpp_list.size());
    for (const auto& cpp_callback : cpp_list) {
      c_list.emplace_back(CppRecvCallbackToC(
          cpp_callback, c_api, &recv_callback_functions[func_count++]));
    }
  }
}

xla::StatusOr<PJRT_LoadedExecutable_Execute_Args>
PjRtCApiLoadedExecutable::GetCommonExecuteArgs(
    absl::Span<const std::vector<PjRtBuffer*>> argument_handles,
    const ExecuteOptions& options, PJRT_ExecuteOptions& c_options,
    std::vector<std::vector<PJRT_Buffer*>>& c_argument_lists_storage,
    std::vector<PJRT_Buffer**>& c_arguments,
    std::vector<std::vector<PJRT_Buffer*>>& c_output_lists_storage,
    std::vector<PJRT_Buffer**>& c_output_lists,
    std::optional<std::vector<PJRT_Event*>>& device_complete_events,
    SendRecvCallbackData& callback_data) {
  bool using_host_callbacks =
      !options.send_callbacks.empty() || !options.recv_callbacks.empty();
  if (using_host_callbacks &&
      !options.use_major_to_minor_data_layout_for_callbacks) {
    return Unimplemented(
        "PJRT C API doesn't support "
        "ExecuteOptions::use_major_to_minor_data_layout_for_callbacks = false");
  }

  PJRT_LoadedExecutable_Execute_Args args;
  args.struct_size = PJRT_LoadedExecutable_Execute_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.executable = c_loaded_executable();
  args.options = &c_options;
  args.options->struct_size = PJRT_ExecuteOptions_STRUCT_SIZE;
  args.options->launch_id = options.launch_id;
  args.num_devices = argument_handles.size();
  CHECK_GT(args.num_devices, 0);
  args.num_args = argument_handles[0].size();
  if (device_complete_events.has_value() || using_host_callbacks) {
    device_complete_events->resize(args.num_devices);
    args.device_complete_events = device_complete_events->data();
  } else {
    args.device_complete_events = nullptr;
  }

  // Populates `args.argument_lists` from `argument_handles`.
  c_argument_lists_storage = Convert2DCppBuffersToCBuffers(argument_handles);
  c_arguments.reserve(c_argument_lists_storage.size());
  for (auto& argument_list : c_argument_lists_storage) {
    c_arguments.push_back(argument_list.data());
  }
  args.argument_lists = c_arguments.data();

  // Allocates memory for output. `c_buffer_lists_storage` and `c_buffer_lists`
  // needs to stay alive during the call of `PJRT_LoadedExecutable_Execute`.

  PJRT_Executable_NumOutputs_Args numoutputs_args;
  numoutputs_args.struct_size = PJRT_Executable_NumOutputs_Args_STRUCT_SIZE;
  numoutputs_args.priv = nullptr;
  numoutputs_args.executable = c_executable();
  RETURN_STATUS_IF_PJRT_ERROR(
      pjrt_c_api()->PJRT_Executable_NumOutputs(&numoutputs_args), pjrt_c_api());
  size_t outer_size = args.num_devices;
  size_t inner_size = numoutputs_args.num_outputs;
  c_output_lists_storage.resize(outer_size);
  c_output_lists.resize(outer_size);
  for (int i = 0; i < outer_size; ++i) {
    c_output_lists_storage[i].resize(inner_size);
    c_output_lists[i] = c_output_lists_storage[i].data();
  }
  args.output_lists = c_output_lists.data();

  // Allocates memory for callbacks. `callback_data` needs to stay alive during
  // the execution.
  if (!options.send_callbacks.empty()) {
    CppSendCallbackListsToC(options.send_callbacks,
                            callback_data.send_callback_functions,
                            callback_data.c_send_callbacks);
    for (auto& c_send_callback_list : callback_data.c_send_callbacks) {
      callback_data.c_send_callback_lists.push_back(
          c_send_callback_list.data());
    }
    args.options->send_callbacks = callback_data.c_send_callback_lists.data();
    args.options->num_send_ops = options.send_callbacks[0].size();
  }
  if (!options.recv_callbacks.empty()) {
    CppRecvCallbackListsToC(options.recv_callbacks, pjrt_c_api(),
                            callback_data.recv_callback_functions,
                            callback_data.c_recv_callbacks);
    for (auto& c_recv_callback_list : callback_data.c_recv_callbacks) {
      callback_data.c_recv_callback_lists.push_back(
          c_recv_callback_list.data());
    }
    args.options->recv_callbacks = callback_data.c_recv_callback_lists.data();
    args.options->num_recv_ops = options.recv_callbacks[0].size();
  }

  return args;
}

StatusOr<std::vector<std::vector<std::unique_ptr<PjRtBuffer>>>>
PjRtCApiLoadedExecutable::Execute(
    absl::Span<const std::vector<PjRtBuffer*>> argument_handles,
    const ExecuteOptions& options,
    std::optional<std::vector<PjRtFuture<Status>>>& returned_futures) {
  std::vector<std::vector<PJRT_Buffer*>> c_argument_lists_storage;
  std::vector<std::vector<PJRT_Buffer*>> c_output_lists_storage;
  std::vector<PJRT_Buffer**> c_output_lists;
  PJRT_ExecuteOptions c_options;
  c_options.num_send_ops = 0;
  c_options.num_recv_ops = 0;
  std::vector<PJRT_Buffer**> c_arguments;
  std::optional<std::vector<PJRT_Event*>> device_complete_events;
  if (returned_futures.has_value()) {
    device_complete_events.emplace();
  }

  auto callback_data = std::make_shared<SendRecvCallbackData>();
  TF_ASSIGN_OR_RETURN(
      PJRT_LoadedExecutable_Execute_Args args,
      GetCommonExecuteArgs(argument_handles, options, c_options,
                           c_argument_lists_storage, c_arguments,
                           c_output_lists_storage, c_output_lists,
                           device_complete_events, *callback_data));

  args.execute_device = nullptr;

  RETURN_STATUS_IF_PJRT_ERROR(
      pjrt_c_api()->PJRT_LoadedExecutable_Execute(&args), pjrt_c_api());

  if (device_complete_events.has_value()) {
    std::vector<PjRtFuture<Status>> device_complete_futures;
    device_complete_futures.resize(args.num_devices);
    for (int i = 0; i < device_complete_futures.size(); ++i) {
      device_complete_futures[i] = pjrt::ConvertCEventToCppFuture(
          args.device_complete_events[i], pjrt_c_api());
      if (!callback_data->c_send_callbacks.empty() ||
          !callback_data->c_recv_callbacks.empty()) {
        device_complete_futures[i].OnReady([callback_data](xla::Status status) {
          // Keeps C callbacks alive until execution completes on all
          // devices.
        });
      }
    }

    if (returned_futures.has_value()) {
      returned_futures->resize(device_complete_futures.size());
      *returned_futures = std::move(device_complete_futures);
    }
  }

  return Convert2DCBuffersToCppBuffers(args.output_lists, args.num_devices,
                                       c_output_lists_storage[0].size(),
                                       client_);
}

StatusOr<std::vector<std::unique_ptr<PjRtBuffer>>>
PjRtCApiLoadedExecutable::ExecuteWithSingleDevice(
    absl::Span<PjRtBuffer* const> argument_handles, PjRtDevice* device,
    const ExecuteOptions& options,
    std::optional<PjRtFuture<Status>>& returned_future, bool fill_future) {
  if (!options.send_callbacks.empty() || !options.recv_callbacks.empty()) {
    return Status(absl::StatusCode::kUnimplemented,
                  "Send/recv callbacks not implemented for "
                  "PjRtCApiLoadedExecutable::ExecuteWithSingleDevice.");
  }

  std::vector<std::vector<PjRtBuffer*>> argument_handles_vec = {
      {argument_handles.begin(), argument_handles.end()}};

  std::vector<std::vector<PJRT_Buffer*>> c_argument_lists_storage;
  std::vector<std::vector<PJRT_Buffer*>> c_output_lists_storage;
  std::vector<PJRT_Buffer**> c_output_lists;
  PJRT_ExecuteOptions c_options;
  c_options.num_send_ops = 0;
  c_options.num_recv_ops = 0;
  std::vector<PJRT_Buffer**> c_arguments;
  std::optional<std::vector<PJRT_Event*>> device_complete_events;
  if (fill_future) {
    device_complete_events.emplace();
  }

  auto callback_data = std::make_shared<SendRecvCallbackData>();
  TF_ASSIGN_OR_RETURN(
      PJRT_LoadedExecutable_Execute_Args args,
      GetCommonExecuteArgs(argument_handles_vec, options, c_options,
                           c_argument_lists_storage, c_arguments,
                           c_output_lists_storage, c_output_lists,
                           device_complete_events, *callback_data));

  args.execute_device =
      tensorflow::down_cast<PjRtCApiDevice*>(device)->c_device();

  RETURN_STATUS_IF_PJRT_ERROR(
      pjrt_c_api()->PJRT_LoadedExecutable_Execute(&args), pjrt_c_api());

  if (fill_future) {
    returned_future = pjrt::ConvertCEventToCppFuture(
        args.device_complete_events[0], pjrt_c_api());
  }
  return std::move(Convert2DCBuffersToCppBuffers(
      args.output_lists, args.num_devices, c_output_lists_storage[0].size(),
      client_)[0]);
}

StatusOr<std::vector<std::unique_ptr<PjRtBuffer>>>
PjRtCApiLoadedExecutable::ExecuteSharded(
    absl::Span<PjRtBuffer* const> argument_handles, PjRtDevice* device,
    const ExecuteOptions& options,
    std::optional<PjRtFuture<Status>>& returned_future, bool fill_future) {
  return ExecuteWithSingleDevice(argument_handles, device, options,
                                 returned_future, fill_future);
}

StatusOr<std::vector<std::unique_ptr<PjRtBuffer>>>
PjRtCApiLoadedExecutable::ExecutePortable(
    absl::Span<PjRtBuffer* const> argument_handles, PjRtDevice* device,
    const ExecuteOptions& options,
    std::optional<PjRtFuture<Status>>& returned_future, bool fill_future) {
  return ExecuteWithSingleDevice(argument_handles, device, options,
                                 returned_future, fill_future);
}

void PjRtCApiLoadedExecutable::Delete() {
  PJRT_LoadedExecutable_Delete_Args args;
  args.struct_size = PJRT_LoadedExecutable_Delete_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.executable = c_loaded_executable();
  const PJRT_Api* c_api = pjrt_c_api();
  pjrt::LogFatalIfPjrtError(c_api->PJRT_LoadedExecutable_Delete(&args), c_api);
}

bool PjRtCApiLoadedExecutable::IsDeleted() {
  PJRT_LoadedExecutable_IsDeleted_Args args;
  args.struct_size = PJRT_LoadedExecutable_IsDeleted_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.executable = c_loaded_executable();

  const PJRT_Api* c_api = pjrt_c_api();
  pjrt::LogFatalIfPjrtError(c_api->PJRT_LoadedExecutable_IsDeleted(&args),
                            c_api);
  return args.is_deleted;
}

// ---------------------------------- Buffers ----------------------------------

PjRtCApiBuffer::PjRtCApiBuffer(PjRtCApiClient* client, PJRT_Buffer* buffer)
    : client_(client),
      buffer_(buffer, ::pjrt::MakeBufferDeleter(client->pjrt_c_api())),
      readiness_event_(nullptr,
                       ::pjrt::MakeEventDeleter(client->pjrt_c_api())) {}

PrimitiveType PjRtCApiBuffer::element_type() const {
  PJRT_Buffer_ElementType_Args args;
  args.struct_size = PJRT_Buffer_ElementType_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer = buffer_.get();
  pjrt::LogFatalIfPjrtError(pjrt_c_api()->PJRT_Buffer_ElementType(&args),
                            pjrt_c_api());
  return pjrt::ConvertFromPjRtBufferType(args.type);
}

absl::Span<const int64_t> PjRtCApiBuffer::dimensions() const {
  PJRT_Buffer_Dimensions_Args args;
  args.struct_size = PJRT_Buffer_Dimensions_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer = buffer_.get();
  pjrt::LogFatalIfPjrtError(pjrt_c_api()->PJRT_Buffer_Dimensions(&args),
                            pjrt_c_api());
  return absl::Span<const int64_t>(args.dims, args.num_dims);
}

const Layout& PjRtCApiBuffer::layout() const {
  {
    absl::MutexLock lock(&mu_);
    if (!layout_.has_value()) {
      PJRT_Buffer_GetMemoryLayout_Args args;
      args.struct_size = PJRT_Buffer_GetMemoryLayout_Args_STRUCT_SIZE;
      args.priv = nullptr;
      args.buffer = buffer_.get();
      pjrt::LogFatalIfPjrtError(
          pjrt_c_api()->PJRT_Buffer_GetMemoryLayout(&args), pjrt_c_api());
      CHECK_EQ(args.layout.type, PJRT_Buffer_MemoryLayout_Type_Tiled)
          << "PjRtCApiBuffer only supports tiled device layouts";
      absl::StatusOr<Layout> cpp_layout =
          pjrt::ConvertToLayout(args.layout.tiled);
      TF_CHECK_OK(cpp_layout.status());
      layout_.emplace(*cpp_layout);
    }
  }
  return *layout_;
}

bool PjRtCApiBuffer::has_dynamic_dimensions() const {
  PJRT_Buffer_DynamicDimensionIndices_Args args;
  args.struct_size = PJRT_Buffer_DynamicDimensionIndices_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer = buffer_.get();

  const PJRT_Api* api = pjrt_c_api();
  std::unique_ptr<PJRT_Error, pjrt::PJRT_ErrorDeleter> error(
      api->PJRT_Buffer_DynamicDimensionIndices(&args),
      pjrt::MakeErrorDeleter(api));

  if (error &&
      pjrt::GetErrorCode(error.get(), api) == PJRT_Error_Code_UNIMPLEMENTED) {
    return false;
  }
  return args.num_dynamic_dims > 0;
}

absl::Span<const bool> PjRtCApiBuffer::is_dynamic_dimension() const {
  {
    absl::MutexLock lock(&mu_);
    if (!is_dynamic_dimension_.has_value()) {
      absl::InlinedVector<bool, InlineRank()>& is_dynamic_dimension_value =
          is_dynamic_dimension_.emplace();
      is_dynamic_dimension_value.assign(dimensions().size(), false);

      PJRT_Buffer_DynamicDimensionIndices_Args args;
      args.struct_size = PJRT_Buffer_DynamicDimensionIndices_Args_STRUCT_SIZE;
      args.priv = nullptr;
      args.buffer = buffer_.get();
      const PJRT_Api* api = pjrt_c_api();
      std::unique_ptr<PJRT_Error, pjrt::PJRT_ErrorDeleter> error(
          api->PJRT_Buffer_DynamicDimensionIndices(&args),
          pjrt::MakeErrorDeleter(api));
      if (error && pjrt::GetErrorCode(error.get(), api) ==
                       PJRT_Error_Code_UNIMPLEMENTED) {
        return *is_dynamic_dimension_;
      }
      for (int i = 0; i < args.num_dynamic_dims; ++i) {
        is_dynamic_dimension_value[args.dynamic_dim_indices[i]] = true;
      }
    }
  }
  return *is_dynamic_dimension_;
}

StatusOr<std::vector<int64_t>> PjRtCApiBuffer::logical_dimensions() {
  PJRT_Buffer_UnpaddedDimensions_Args args;
  args.struct_size = PJRT_Buffer_UnpaddedDimensions_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer = buffer_.get();
  RETURN_STATUS_IF_PJRT_ERROR(
      pjrt_c_api()->PJRT_Buffer_UnpaddedDimensions(&args), pjrt_c_api());
  return std::vector<int64_t>(args.unpadded_dims,
                              args.unpadded_dims + args.num_dims);
}

PjRtFuture<Status> PjRtCApiBuffer::ToLiteral(MutableLiteralBase* literal) {
  PJRT_Buffer_ToHostBuffer_Args args;
  args.struct_size = PJRT_Buffer_ToHostBuffer_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.src = buffer_.get();

  const xla::Shape& shape = literal->shape();

  if (!shape.IsArray()) {
    return PjRtFuture<Status>(
        Unimplemented("PjRtCApiBuffer::ToLiteral: Shapes other than array are"
                      "not supported."));
  }

  args.dst_size = ShapeUtil::ByteSizeOfElements(shape);
  args.dst = literal->untyped_data();
  xla::StatusOr<pjrt::BufferMemoryLayoutData> c_layout_data;
  if (literal->shape().has_layout()) {
    c_layout_data =
        pjrt::ConvertToBufferMemoryLayoutData(literal->shape().layout());
    if (!c_layout_data.ok()) {
      return PjRtFuture<Status>(c_layout_data.status());
    }
    args.host_layout = &(c_layout_data->c_layout);
  } else {
    args.host_layout = nullptr;
  }

  const PJRT_Api* api = pjrt_c_api();

  std::unique_ptr<PJRT_Error, ::pjrt::PJRT_ErrorDeleter> error{
      pjrt_c_api()->PJRT_Buffer_ToHostBuffer(&args),
      ::pjrt::MakeErrorDeleter(api)};

  if (error != nullptr) {
    xla::Status s = ::pjrt::PjrtErrorToStatus(error.get(), api);
    return PjRtFuture<Status>(s);
  }

  return pjrt::ConvertCEventToCppFuture(args.event, api);
}

StatusOr<size_t> PjRtCApiBuffer::GetOnDeviceSizeInBytes() const {
  PJRT_Buffer_OnDeviceSizeInBytes_Args args;
  args.struct_size = PJRT_Buffer_OnDeviceSizeInBytes_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer = buffer_.get();
  RETURN_STATUS_IF_PJRT_ERROR(
      client_->pjrt_c_api()->PJRT_Buffer_OnDeviceSizeInBytes(&args),
      client_->pjrt_c_api());

  return args.on_device_size_in_bytes;
}

PjRtMemorySpace* PjRtCApiBuffer::memory_space() const {
  PJRT_Buffer_Memory_Args args;
  args.struct_size = PJRT_Buffer_Memory_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer = buffer_.get();
  const PJRT_Api* api = pjrt_c_api();
  std::unique_ptr<PJRT_Error, pjrt::PJRT_ErrorDeleter> error(
      api->PJRT_Buffer_Memory(&args), pjrt::MakeErrorDeleter(api));
  if (error == nullptr && args.memory != nullptr) {
    return client_->GetCppMemory(args.memory);
  } else if (error != nullptr && pjrt::GetErrorCode(error.get(), api) !=
                                     PJRT_Error_Code_UNIMPLEMENTED) {
    pjrt::LogFatalIfPjrtError(error.get(), api);
  }
  return nullptr;
}

PjRtDevice* PjRtCApiBuffer::device() const {
  PJRT_Buffer_Device_Args args;
  args.struct_size = PJRT_Buffer_Device_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer = buffer_.get();
  const PJRT_Api* api = pjrt_c_api();
  pjrt::LogFatalIfPjrtError(api->PJRT_Buffer_Device(&args), api);
  return client_->GetCppDevice(args.device);
}

void PjRtCApiBuffer::Delete() {
  PJRT_Buffer_Delete_Args args;
  args.struct_size = PJRT_Buffer_Delete_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer = buffer_.get();
  const PJRT_Api* api = pjrt_c_api();
  pjrt::LogFatalIfPjrtError(api->PJRT_Buffer_Delete(&args), api);
}

bool PjRtCApiBuffer::IsDeleted() {
  PJRT_Buffer_IsDeleted_Args args;
  args.struct_size = PJRT_Buffer_IsDeleted_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer = buffer_.get();
  const PJRT_Api* api = pjrt_c_api();
  pjrt::LogFatalIfPjrtError(api->PJRT_Buffer_IsDeleted(&args), api);
  return args.is_deleted;
}

StatusOr<std::unique_ptr<PjRtBuffer>> PjRtCApiBuffer::CopyToDevice(
    PjRtDevice* dst_device) {
  if (dst_device->client() == client_) {
    PJRT_Buffer_CopyToDevice_Args args;
    args.struct_size = PJRT_Buffer_CopyToDevice_Args_STRUCT_SIZE;
    args.priv = nullptr;
    args.buffer = buffer_.get();
    args.dst_device =
        tensorflow::down_cast<PjRtCApiDevice*>(dst_device)->c_device();
    const PJRT_Api* api = pjrt_c_api();
    RETURN_STATUS_IF_PJRT_ERROR(api->PJRT_Buffer_CopyToDevice(&args), api);
    return std::unique_ptr<PjRtBuffer>(
        std::make_unique<PjRtCApiBuffer>(client_, args.dst_buffer));
  } else {
    // Copy across PjRtClients by copying through host
    TF_ASSIGN_OR_RETURN(std::shared_ptr<Literal> literal, ToLiteralSync());
    absl::InlinedVector<int64_t, 4> byte_strides(
        literal->shape().dimensions_size());
    TF_RETURN_IF_ERROR(
        ShapeUtil::ByteStrides(literal->shape(), absl::MakeSpan(byte_strides)));
    // Avoid use-after-free on `literal` due to unsequenced move and use.
    Literal* literal_pointer = literal.get();
    return dst_device->client()->BufferFromHostBuffer(
        literal_pointer->untyped_data(),
        literal_pointer->shape().element_type(),
        literal_pointer->shape().dimensions(), byte_strides,
        PjRtClient::HostBufferSemantics::kZeroCopy,
        [literal{std::move(literal)}]() { /* frees literal */ }, dst_device);
  }
}

StatusOr<std::unique_ptr<PjRtBuffer>> PjRtCApiBuffer::CopyToMemorySpace(
    PjRtMemorySpace* dst_memory) {
  const PJRT_Api* api = pjrt_c_api();
  // TODO(yueshengys): Remove this after 12/20/2023.
  if (api->pjrt_api_version.minor_version < 32) {
    return Unimplemented(
        "The plugin has PJRT API version 0.32 which does not support "
        "CopyToMemorySpace");
  }
  if (dst_memory->client() == client_) {
    PJRT_Buffer_CopyToMemory_Args args;
    args.struct_size = PJRT_Buffer_CopyToMemory_Args_STRUCT_SIZE;
    args.priv = nullptr;
    args.buffer = buffer_.get();
    args.dst_memory =
        tensorflow::down_cast<PjRtCApiMemorySpace*>(dst_memory)->c_memory();
    RETURN_STATUS_IF_PJRT_ERROR(api->PJRT_Buffer_CopyToMemory(&args), api);
    return std::unique_ptr<PjRtBuffer>(
        std::make_unique<PjRtCApiBuffer>(client_, args.dst_buffer));
  } else {
    // Copy across PjRtClients by copying through host
    TF_ASSIGN_OR_RETURN(std::shared_ptr<Literal> literal, ToLiteralSync());
    absl::InlinedVector<int64_t, 4> byte_strides(
        literal->shape().dimensions_size());
    TF_RETURN_IF_ERROR(
        ShapeUtil::ByteStrides(literal->shape(), absl::MakeSpan(byte_strides)));
    // Avoid use-after-free on `literal` due to unsequenced move and use.
    Literal* literal_pointer = literal.get();
    return dst_memory->client()->BufferFromHostBuffer(
        literal_pointer->untyped_data(),
        literal_pointer->shape().element_type(),
        literal_pointer->shape().dimensions(), byte_strides,
        PjRtClient::HostBufferSemantics::kZeroCopy,
        [literal{std::move(literal)}]() { /* frees literal */ }, dst_memory,
        /*device_layout=*/nullptr);
  }
}

bool PjRtCApiBuffer::IsOnCpu() const {
  PJRT_Buffer_IsOnCpu_Args args;
  args.struct_size = PJRT_Buffer_IsOnCpu_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer = buffer_.get();
  const PJRT_Api* api = pjrt_c_api();
  pjrt::LogFatalIfPjrtError(api->PJRT_Buffer_IsOnCpu(&args), api);
  return args.is_on_cpu;
}

PJRT_Event* PjRtCApiBuffer::GetReadyEvent() {
  if (readiness_event_ == nullptr) {
    const PJRT_Api* api = pjrt_c_api();
    PJRT_Buffer_ReadyEvent_Args args;
    args.struct_size = PJRT_Buffer_ReadyEvent_Args_STRUCT_SIZE;
    args.priv = nullptr;
    args.buffer = buffer_.get();
    pjrt::LogFatalIfPjrtError(api->PJRT_Buffer_ReadyEvent(&args), api);
    readiness_event_.reset(args.event);
  }
  return readiness_event_.get();
}

void PjRtCApiBuffer::MakePromiseTrackEvent() {
  CHECK(readiness_promise_ != nullptr);
  const PJRT_Api* api = pjrt_c_api();
  PJRT_Event_OnReady_Args args;
  args.struct_size = PJRT_Event_OnReady_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.event = GetReadyEvent();
  args.user_arg = new std::function<void(PJRT_Error*)>(
      [promise = readiness_promise_, api](PJRT_Error* error) -> void {
        Status status = ::pjrt::PjrtErrorToStatus(error, api);
        promise->Set(status);
        ::pjrt::MakeErrorDeleter(api)(error);
      });
  args.callback = [](PJRT_Error* error, void* callback_ptr) {
    auto callback =
        static_cast<std::function<void(PJRT_Error*)>*>(callback_ptr);
    CHECK(callback != nullptr);
    (*callback)(error);
    delete callback;
  };

  std::unique_ptr<PJRT_Error, ::pjrt::PJRT_ErrorDeleter> error{
      api->PJRT_Event_OnReady(&args), ::pjrt::MakeErrorDeleter(api)};
  if (error != nullptr) {
    readiness_promise_->Set(::pjrt::PjrtErrorToStatus(error.get(), api));
  }
}

PjRtFuture<Status> PjRtCApiBuffer::GetReadyFuture() {
  if (readiness_promise_ == nullptr) {
    readiness_promise_ = std::make_shared<PjRtFuture<Status>::Promise>(
        PjRtFuture<Status>::CreatePromise());
    MakePromiseTrackEvent();
  }
  return PjRtFuture<Status>{*readiness_promise_};
}

StatusOr<std::unique_ptr<PjRtBuffer::ExternalReference>>
PjRtCApiBuffer::AcquireExternalReference() {
  PJRT_Buffer_IncreaseExternalReferenceCount_Args increase_reference_count_args;
  increase_reference_count_args.buffer = c_buffer();
  increase_reference_count_args.struct_size =
      PJRT_Buffer_IncreaseExternalReferenceCount_Args_STRUCT_SIZE;
  increase_reference_count_args.priv = nullptr;
  RETURN_STATUS_IF_PJRT_ERROR(
      pjrt_c_api()->PJRT_Buffer_IncreaseExternalReferenceCount(
          &increase_reference_count_args),
      pjrt_c_api());

  PJRT_Buffer_OpaqueDeviceMemoryDataPointer_Args
      opaque_device_memory_data_pointer_args;
  opaque_device_memory_data_pointer_args.struct_size =
      PJRT_Buffer_OpaqueDeviceMemoryDataPointer_Args_STRUCT_SIZE;
  opaque_device_memory_data_pointer_args.priv = nullptr;
  opaque_device_memory_data_pointer_args.buffer = c_buffer();
  RETURN_STATUS_IF_PJRT_ERROR(
      pjrt_c_api()->PJRT_Buffer_OpaqueDeviceMemoryDataPointer(
          &opaque_device_memory_data_pointer_args),
      pjrt_c_api());

  void* device_memory_ptr =
      opaque_device_memory_data_pointer_args.device_memory_ptr;
  return std::make_unique<PjRtCApiExternalReference>(client_, this,
                                                     device_memory_ptr);
}

PjRtCApiExternalReference::~PjRtCApiExternalReference() {
  PJRT_Buffer_DecreaseExternalReferenceCount_Args args;
  args.struct_size =
      PJRT_Buffer_DecreaseExternalReferenceCount_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer = buffer_->c_buffer();
  pjrt::LogFatalIfPjrtError(
      client_->pjrt_c_api()->PJRT_Buffer_DecreaseExternalReferenceCount(&args),
      client_->pjrt_c_api());
}

// ------------------------------ Device Topology ------------------------------

PjRtCApiTopologyDescription::PjRtCApiTopologyDescription(
    const PJRT_Api* c_api, PJRT_TopologyDescription* c_topology, bool owned)
    : compiler_(std::make_unique<PjRtCApiCompiler>(c_api)),
      c_api_(c_api),
      c_topology_(c_topology) {
  if (owned) {
    owned_c_topology_ = std::unique_ptr<PJRT_TopologyDescription,
                                        pjrt::PJRT_TopologyDescriptionDeleter>(
        c_topology, pjrt::MakeTopologyDescriptionDeleter(c_api));
  }
  InitAttributes();
}

PjRtCApiTopologyDescription::PjRtCApiTopologyDescription(
    const PJRT_Api* c_api, const PJRT_TopologyDescription* c_topology)
    : PjRtCApiTopologyDescription(
          c_api, const_cast<PJRT_TopologyDescription*>(c_topology),
          /*owned=*/false) {}

absl::string_view PjRtCApiTopologyDescription::platform_name() const {
  PJRT_TopologyDescription_PlatformName_Args args;
  args.topology = c_topology_;
  args.struct_size = PJRT_TopologyDescription_PlatformName_Args_STRUCT_SIZE;
  args.priv = nullptr;
  pjrt::LogFatalIfPjrtError(
      c_api_->PJRT_TopologyDescription_PlatformName(&args), c_api_);
  return absl::string_view(args.platform_name, args.platform_name_size);
}

absl::string_view PjRtCApiTopologyDescription::platform_version() const {
  PJRT_TopologyDescription_PlatformVersion_Args args;
  args.struct_size = PJRT_TopologyDescription_PlatformVersion_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.topology = c_topology_;
  pjrt::LogFatalIfPjrtError(
      c_api_->PJRT_TopologyDescription_PlatformVersion(&args), c_api_);
  return absl::string_view(args.platform_version, args.platform_version_size);
}

std::vector<std::unique_ptr<const PjRtDeviceDescription>>
PjRtCApiTopologyDescription::DeviceDescriptions() const {
  PJRT_TopologyDescription_GetDeviceDescriptions_Args args;
  args.struct_size =
      PJRT_TopologyDescription_GetDeviceDescriptions_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.topology = c_topology_;
  pjrt::LogFatalIfPjrtError(
      c_api_->PJRT_TopologyDescription_GetDeviceDescriptions(&args), c_api_);
  std::vector<std::unique_ptr<const PjRtDeviceDescription>> out;
  out.reserve(args.num_descriptions);
  for (PJRT_DeviceDescription* device_desc :
       absl::Span<PJRT_DeviceDescription* const>(args.descriptions,
                                                 args.num_descriptions)) {
    out.push_back(
        std::make_unique<PjRtCApiDeviceDescription>(c_api_, device_desc));
  }
  return out;
}

StatusOr<std::string> PjRtCApiTopologyDescription::Serialize() const {
  PJRT_TopologyDescription_Serialize_Args args;
  args.struct_size = PJRT_TopologyDescription_Serialize_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.topology = c_topology_;
  RETURN_STATUS_IF_PJRT_ERROR(c_api_->PJRT_TopologyDescription_Serialize(&args),
                              c_api_);
  auto out = std::string(args.serialized_bytes, args.serialized_bytes_size);
  args.serialized_topology_deleter(args.serialized_topology);
  return out;
}

void PjRtCApiTopologyDescription::InitAttributes() {
  PJRT_TopologyDescription_Attributes_Args args;
  args.struct_size = PJRT_TopologyDescription_Attributes_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.topology = c_topology_;
  pjrt::LogFatalIfPjrtError(c_api_->PJRT_TopologyDescription_Attributes(&args),
                            c_api_);
  attributes_ =
      pjrt::ConvertFromPjRtNamedValueList(args.attributes, args.num_attributes);
}

// Initializes `PJRT_Compile_Args`, which will be used to call
// API PJRT_Compile().
static StatusOr<std::unique_ptr<PjRtExecutable>> InitializeArgsAndCompileAot(
    const PJRT_Api* c_api, PjRtClient* client, const CompileOptions& options,
    const PjRtTopologyDescription& topology, const std::string& code,
    const std::string& format) {
  PJRT_Compile_Args args;
  args.struct_size = PJRT_Compile_Args_STRUCT_SIZE;
  args.priv = nullptr;
  if (client == nullptr) {
    args.client = nullptr;
  } else {
    args.client =
        tensorflow::down_cast<PjRtCApiClient*>(client)->pjrt_c_client();
  }
  args.topology =
      tensorflow::down_cast<const PjRtCApiTopologyDescription*>(&topology)
          ->c_topology();
  TF_ASSIGN_OR_RETURN(const CompileOptionsProto options_proto,
                      options.ToProto());
  std::string options_str = options_proto.SerializeAsString();
  args.compile_options = options_str.c_str();
  args.compile_options_size = options_str.size();

  PJRT_Program program;
  program.struct_size = PJRT_Program_STRUCT_SIZE;
  program.priv = nullptr;
  program.code = const_cast<char*>(code.c_str());
  program.code_size = code.size();
  program.format = format.c_str();
  program.format_size = format.size();
  args.program = &program;

  RETURN_STATUS_IF_PJRT_ERROR(c_api->PJRT_Compile(&args), c_api);
  std::unique_ptr<PjRtExecutable> ret =
      std::make_unique<PjRtCApiExecutable>(c_api, args.executable);
  return ret;
}

StatusOr<std::unique_ptr<PjRtExecutable>> PjRtCApiCompiler::Compile(
    CompileOptions options, const XlaComputation& computation,
    const PjRtTopologyDescription& topology, PjRtClient* client) {
  std::string module_str = computation.proto().SerializeAsString();
  std::string format(pjrt::kHloFormat);
  return InitializeArgsAndCompileAot(c_api_, client, options, topology,
                                     module_str, format);
}

StatusOr<std::unique_ptr<PjRtExecutable>> PjRtCApiCompiler::Compile(
    CompileOptions options, mlir::ModuleOp module,
    const PjRtTopologyDescription& topology, PjRtClient* client) {
  std::string module_bytecode;
  {
    llvm::raw_string_ostream os(module_bytecode);
    mlir::BytecodeWriterConfig config;
    // Pin bytecode version to 1 until transition to stable.
    // TODO(285913864): Remove post enabling frameworks to set it.
    config.setDesiredBytecodeVersion(1);
    if (mlir::failed(mlir::writeBytecodeToFile(module, os, config)))
      return absl::UnknownError("writeBytecodeToFile() failed.");
  }
  std::string format(pjrt::kMlirFormat);
  return InitializeArgsAndCompileAot(c_api_, client, options, topology,
                                     module_bytecode, format);
}

// -------------------------------- API access ---------------------------------

StatusOr<std::unique_ptr<PjRtClient>> GetCApiClient(
    absl::string_view device_type,
    const absl::flat_hash_map<std::string, PjRtValueType>& create_options,
    PjRtClient::KeyValueGetCallback kv_get,
    PjRtClient::KeyValuePutCallback kv_put) {
  TF_ASSIGN_OR_RETURN(const PJRT_Api* c_api, pjrt::PjrtApi(device_type));
  if (c_api == nullptr) {
    return InternalError("PJRT C API is nullptr for %s", device_type);
  }

  PJRT_Client_Create_Args init_args;
  init_args.struct_size = PJRT_Client_Create_Args_STRUCT_SIZE;
  init_args.priv = nullptr;
  TF_ASSIGN_OR_RETURN(
      std::vector<PJRT_NamedValue> c_options,
      pjrt::ConvertToPjRtNamedValueList(create_options,
                                        c_api->pjrt_api_version.minor_version));
  init_args.create_options = c_options.data();
  init_args.num_options = c_options.size();

  std::unique_ptr<pjrt::PJRT_KeyValueCallbackData> kv_callback_data;
  if (kv_get == nullptr && kv_put == nullptr) {
    kv_callback_data = nullptr;
  } else if (kv_get != nullptr && kv_put != nullptr) {
    kv_callback_data = pjrt::ConvertToCKeyValueCallbacks(kv_get, kv_put);
    init_args.kv_get_callback = kv_callback_data->c_kv_get;
    init_args.kv_get_user_arg = &kv_callback_data->kv_get_c_func;
    init_args.kv_put_callback = kv_callback_data->c_kv_put;
    init_args.kv_put_user_arg = &kv_callback_data->kv_put_c_func;
  } else {
    return InvalidArgument(
        "Only one of KeyValueGetCallback and KeyValuePutCallback is set in "
        "GetCApiClient for %s",
        device_type);
  }

  RETURN_STATUS_IF_PJRT_ERROR(c_api->PJRT_Client_Create(&init_args), c_api);
  PJRT_Client* c_client = init_args.client;

  return std::unique_ptr<PjRtClient>(std::make_unique<PjRtCApiClient>(
      c_api, c_client, std::move(kv_callback_data)));
}

StatusOr<std::unique_ptr<PjRtTopologyDescription>> GetCApiTopology(
    absl::string_view device_type, absl::string_view topology_name,
    const absl::flat_hash_map<std::string, PjRtValueType>& create_options) {
  TF_ASSIGN_OR_RETURN(const PJRT_Api* c_api, pjrt::PjrtApi(device_type));
  if (c_api == nullptr) {
    return InternalError("PJRT C API is nullptr for %s", device_type);
  }

  PJRT_TopologyDescription_Create_Args init_args;
  init_args.struct_size = PJRT_TopologyDescription_Create_Args_STRUCT_SIZE;
  init_args.priv = nullptr;
  TF_ASSIGN_OR_RETURN(
      std::vector<PJRT_NamedValue> c_options,
      pjrt::ConvertToPjRtNamedValueList(create_options,
                                        c_api->pjrt_api_version.minor_version));
  init_args.create_options = c_options.data();
  init_args.num_options = c_options.size();
  init_args.topology_name = topology_name.data();
  init_args.topology_name_size = topology_name.size();
  RETURN_STATUS_IF_PJRT_ERROR(
      c_api->PJRT_TopologyDescription_Create(&init_args), c_api);
  PJRT_TopologyDescription* c_topology = init_args.topology;
  return std::unique_ptr<PjRtTopologyDescription>(
      std::make_unique<PjRtCApiTopologyDescription>(c_api, c_topology));
}

}  // namespace xla
