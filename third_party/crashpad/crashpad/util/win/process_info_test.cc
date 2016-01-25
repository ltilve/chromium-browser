// Copyright 2015 The Crashpad Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "util/win/process_info.h"

#include <imagehlp.h>
#include <intrin.h>
#include <wchar.h>

#include "base/files/file_path.h"
#include "base/memory/scoped_ptr.h"
#include "build/build_config.h"
#include "gtest/gtest.h"
#include "test/paths.h"
#include "test/win/child_launcher.h"
#include "util/file/file_io.h"
#include "util/misc/uuid.h"
#include "util/win/scoped_handle.h"

namespace crashpad {
namespace test {
namespace {

const wchar_t kNtdllName[] = L"\\ntdll.dll";

time_t GetTimestampForModule(HMODULE module) {
  char filename[MAX_PATH];
  // `char` and GetModuleFileNameA because ImageLoad is ANSI only.
  if (!GetModuleFileNameA(module, filename, arraysize(filename)))
    return 0;
  LOADED_IMAGE* loaded_image = ImageLoad(filename, nullptr);
  return loaded_image->FileHeader->FileHeader.TimeDateStamp;
}

bool IsProcessWow64(HANDLE process_handle) {
  static decltype(IsWow64Process)* is_wow64_process =
      reinterpret_cast<decltype(IsWow64Process)*>(
          GetProcAddress(LoadLibrary(L"kernel32.dll"), "IsWow64Process"));
  if (!is_wow64_process)
    return false;
  BOOL is_wow64;
  if (!is_wow64_process(process_handle, &is_wow64)) {
    PLOG(ERROR) << "IsWow64Process";
    return false;
  }
  return is_wow64;
}

void VerifyAddressInInCodePage(const ProcessInfo& process_info,
                               WinVMAddress code_address) {
  // Make sure the child code address is an code page address with the right
  // information.
  const std::vector<ProcessInfo::MemoryInfo>& memory_info =
      process_info.MemoryInformation();
  bool found_region = false;
  for (const auto& mi : memory_info) {
    if (mi.base_address <= code_address &&
        mi.base_address + mi.region_size > code_address) {
      EXPECT_EQ(MEM_COMMIT, mi.state);
      EXPECT_EQ(PAGE_EXECUTE_READ, mi.protect);
      EXPECT_EQ(MEM_IMAGE, mi.type);
      EXPECT_FALSE(found_region);
      found_region = true;
    }
  }
  EXPECT_TRUE(found_region);
}

TEST(ProcessInfo, Self) {
  ProcessInfo process_info;
  ASSERT_TRUE(process_info.Initialize(GetCurrentProcess()));
  EXPECT_EQ(GetCurrentProcessId(), process_info.ProcessID());
  EXPECT_GT(process_info.ParentProcessID(), 0u);

#if defined(ARCH_CPU_64_BITS)
  EXPECT_TRUE(process_info.Is64Bit());
  EXPECT_FALSE(process_info.IsWow64());
#else
  EXPECT_FALSE(process_info.Is64Bit());
  if (IsProcessWow64(GetCurrentProcess()))
    EXPECT_TRUE(process_info.IsWow64());
  else
    EXPECT_FALSE(process_info.IsWow64());
#endif

  std::wstring command_line;
  EXPECT_TRUE(process_info.CommandLine(&command_line));
  EXPECT_EQ(std::wstring(GetCommandLine()), command_line);

  std::vector<ProcessInfo::Module> modules;
  EXPECT_TRUE(process_info.Modules(&modules));
  ASSERT_GE(modules.size(), 2u);
  const wchar_t kSelfName[] = L"\\crashpad_util_test.exe";
  ASSERT_GE(modules[0].name.size(), wcslen(kSelfName));
  EXPECT_EQ(kSelfName,
            modules[0].name.substr(modules[0].name.size() - wcslen(kSelfName)));
  ASSERT_GE(modules[1].name.size(), wcslen(kNtdllName));
  EXPECT_EQ(
      kNtdllName,
      modules[1].name.substr(modules[1].name.size() - wcslen(kNtdllName)));

  EXPECT_EQ(modules[0].dll_base,
            reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr)));
  EXPECT_EQ(modules[1].dll_base,
            reinterpret_cast<uintptr_t>(GetModuleHandle(L"ntdll.dll")));

  EXPECT_GT(modules[0].size, 0);
  EXPECT_GT(modules[1].size, 0);

  EXPECT_EQ(modules[0].timestamp,
            GetTimestampForModule(GetModuleHandle(nullptr)));
  // System modules are forced to particular stamps and the file header values
  // don't match the on-disk times. Just make sure we got some data here.
  EXPECT_GT(modules[1].timestamp, 0);

  // Find something we know is a code address and confirm expected memory
  // information settings.
  VerifyAddressInInCodePage(process_info,
                            reinterpret_cast<WinVMAddress>(_ReturnAddress()));
}

void TestOtherProcess(const base::string16& directory_modification) {
  ProcessInfo process_info;

  UUID done_uuid(UUID::InitializeWithNewTag{});

  ScopedKernelHANDLE done(
      CreateEvent(nullptr, true, false, done_uuid.ToString16().c_str()));
  ASSERT_TRUE(done.get());

  base::FilePath test_executable = Paths::Executable();

  std::wstring child_test_executable =
      test_executable.DirName()
          .Append(directory_modification)
          .Append(test_executable.BaseName().RemoveFinalExtension().value() +
                  L"_process_info_test_child.exe")
          .value();

  std::wstring args;
  AppendCommandLineArgument(done_uuid.ToString16(), &args);

  ChildLauncher child(child_test_executable, args);
  child.Start();

  // The child sends us a code address we can look up in the memory map.
  WinVMAddress code_address;
  CheckedReadFile(
      child.stdout_read_handle(), &code_address, sizeof(code_address));

  ASSERT_TRUE(process_info.Initialize(child.process_handle()));

  // Tell the test it's OK to shut down now that we've read our data.
  EXPECT_TRUE(SetEvent(done.get()));

  std::vector<ProcessInfo::Module> modules;
  EXPECT_TRUE(process_info.Modules(&modules));
  ASSERT_GE(modules.size(), 3u);
  std::wstring child_name = L"\\crashpad_util_test_process_info_test_child.exe";
  ASSERT_GE(modules[0].name.size(), child_name.size());
  EXPECT_EQ(child_name,
            modules[0].name.substr(modules[0].name.size() - child_name.size()));
  ASSERT_GE(modules[1].name.size(), wcslen(kNtdllName));
  EXPECT_EQ(
      kNtdllName,
      modules[1].name.substr(modules[1].name.size() - wcslen(kNtdllName)));
  // lz32.dll is an uncommonly-used-but-always-available module that the test
  // binary manually loads.
  const wchar_t kLz32dllName[] = L"\\lz32.dll";
  ASSERT_GE(modules.back().name.size(), wcslen(kLz32dllName));
  EXPECT_EQ(kLz32dllName,
            modules.back().name.substr(modules.back().name.size() -
                                       wcslen(kLz32dllName)));

  VerifyAddressInInCodePage(process_info, code_address);
}

TEST(ProcessInfo, OtherProcess) {
  TestOtherProcess(FILE_PATH_LITERAL("."));
}

#if defined(ARCH_CPU_64_BITS)
TEST(ProcessInfo, OtherProcessWOW64) {
#ifndef NDEBUG
  TestOtherProcess(FILE_PATH_LITERAL("..\\..\\out\\Debug"));
#else
  TestOtherProcess(FILE_PATH_LITERAL("..\\..\\out\\Release"));
#endif
}
#endif  // ARCH_CPU_64_BITS

TEST(ProcessInfo, AccessibleRangesNone) {
  std::vector<ProcessInfo::MemoryInfo> memory_info;
  MEMORY_BASIC_INFORMATION mbi = {0};

  mbi.BaseAddress = 0;
  mbi.RegionSize = 10;
  mbi.State = MEM_FREE;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  std::vector<CheckedRange<WinVMAddress, WinVMSize>> result =
      GetReadableRangesOfMemoryMap(CheckedRange<WinVMAddress, WinVMSize>(2, 4),
                                   memory_info);

  EXPECT_TRUE(result.empty());
}

TEST(ProcessInfo, AccessibleRangesOneInside) {
  std::vector<ProcessInfo::MemoryInfo> memory_info;
  MEMORY_BASIC_INFORMATION mbi = {0};

  mbi.BaseAddress = 0;
  mbi.RegionSize = 10;
  mbi.State = MEM_COMMIT;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  std::vector<CheckedRange<WinVMAddress, WinVMSize>> result =
      GetReadableRangesOfMemoryMap(CheckedRange<WinVMAddress, WinVMSize>(2, 4),
                                   memory_info);

  ASSERT_EQ(1u, result.size());
  EXPECT_EQ(2, result[0].base());
  EXPECT_EQ(4, result[0].size());
}

TEST(ProcessInfo, AccessibleRangesOneTruncatedSize) {
  std::vector<ProcessInfo::MemoryInfo> memory_info;
  MEMORY_BASIC_INFORMATION mbi = {0};

  mbi.BaseAddress = 0;
  mbi.RegionSize = 10;
  mbi.State = MEM_COMMIT;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  mbi.BaseAddress = reinterpret_cast<void*>(10);
  mbi.RegionSize = 20;
  mbi.State = MEM_FREE;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  std::vector<CheckedRange<WinVMAddress, WinVMSize>> result =
      GetReadableRangesOfMemoryMap(CheckedRange<WinVMAddress, WinVMSize>(5, 10),
                                   memory_info);

  ASSERT_EQ(1u, result.size());
  EXPECT_EQ(5, result[0].base());
  EXPECT_EQ(5, result[0].size());
}

TEST(ProcessInfo, AccessibleRangesOneMovedStart) {
  std::vector<ProcessInfo::MemoryInfo> memory_info;
  MEMORY_BASIC_INFORMATION mbi = {0};

  mbi.BaseAddress = 0;
  mbi.RegionSize = 10;
  mbi.State = MEM_FREE;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  mbi.BaseAddress = reinterpret_cast<void*>(10);
  mbi.RegionSize = 20;
  mbi.State = MEM_COMMIT;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  std::vector<CheckedRange<WinVMAddress, WinVMSize>> result =
      GetReadableRangesOfMemoryMap(CheckedRange<WinVMAddress, WinVMSize>(5, 10),
                                   memory_info);

  ASSERT_EQ(1u, result.size());
  EXPECT_EQ(10, result[0].base());
  EXPECT_EQ(5, result[0].size());
}

TEST(ProcessInfo, ReserveIsInaccessible) {
  std::vector<ProcessInfo::MemoryInfo> memory_info;
  MEMORY_BASIC_INFORMATION mbi = {0};

  mbi.BaseAddress = 0;
  mbi.RegionSize = 10;
  mbi.State = MEM_RESERVE;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  mbi.BaseAddress = reinterpret_cast<void*>(10);
  mbi.RegionSize = 20;
  mbi.State = MEM_COMMIT;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  std::vector<CheckedRange<WinVMAddress, WinVMSize>> result =
      GetReadableRangesOfMemoryMap(CheckedRange<WinVMAddress, WinVMSize>(5, 10),
                                   memory_info);

  ASSERT_EQ(1u, result.size());
  EXPECT_EQ(10, result[0].base());
  EXPECT_EQ(5, result[0].size());
}

TEST(ProcessInfo, PageGuardIsInaccessible) {
  std::vector<ProcessInfo::MemoryInfo> memory_info;
  MEMORY_BASIC_INFORMATION mbi = {0};

  mbi.BaseAddress = 0;
  mbi.RegionSize = 10;
  mbi.State = MEM_COMMIT;
  mbi.Protect = PAGE_GUARD;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  mbi.BaseAddress = reinterpret_cast<void*>(10);
  mbi.RegionSize = 20;
  mbi.State = MEM_COMMIT;
  mbi.Protect = 0;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  std::vector<CheckedRange<WinVMAddress, WinVMSize>> result =
      GetReadableRangesOfMemoryMap(CheckedRange<WinVMAddress, WinVMSize>(5, 10),
                                   memory_info);

  ASSERT_EQ(1u, result.size());
  EXPECT_EQ(10, result[0].base());
  EXPECT_EQ(5, result[0].size());
}

TEST(ProcessInfo, PageNoAccessIsInaccessible) {
  std::vector<ProcessInfo::MemoryInfo> memory_info;
  MEMORY_BASIC_INFORMATION mbi = {0};

  mbi.BaseAddress = 0;
  mbi.RegionSize = 10;
  mbi.State = MEM_COMMIT;
  mbi.Protect = PAGE_NOACCESS;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  mbi.BaseAddress = reinterpret_cast<void*>(10);
  mbi.RegionSize = 20;
  mbi.State = MEM_COMMIT;
  mbi.Protect = 0;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  std::vector<CheckedRange<WinVMAddress, WinVMSize>> result =
      GetReadableRangesOfMemoryMap(CheckedRange<WinVMAddress, WinVMSize>(5, 10),
                                   memory_info);

  ASSERT_EQ(1u, result.size());
  EXPECT_EQ(10, result[0].base());
  EXPECT_EQ(5, result[0].size());
}

TEST(ProcessInfo, AccessibleRangesCoalesced) {
  std::vector<ProcessInfo::MemoryInfo> memory_info;
  MEMORY_BASIC_INFORMATION mbi = {0};

  mbi.BaseAddress = 0;
  mbi.RegionSize = 10;
  mbi.State = MEM_FREE;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  mbi.BaseAddress = reinterpret_cast<void*>(10);
  mbi.RegionSize = 2;
  mbi.State = MEM_COMMIT;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  mbi.BaseAddress = reinterpret_cast<void*>(12);
  mbi.RegionSize = 5;
  mbi.State = MEM_COMMIT;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  std::vector<CheckedRange<WinVMAddress, WinVMSize>> result =
      GetReadableRangesOfMemoryMap(CheckedRange<WinVMAddress, WinVMSize>(11, 4),
                                   memory_info);

  ASSERT_EQ(1u, result.size());
  EXPECT_EQ(11, result[0].base());
  EXPECT_EQ(4, result[0].size());
}

TEST(ProcessInfo, AccessibleRangesMiddleUnavailable) {
  std::vector<ProcessInfo::MemoryInfo> memory_info;
  MEMORY_BASIC_INFORMATION mbi = {0};

  mbi.BaseAddress = 0;
  mbi.RegionSize = 10;
  mbi.State = MEM_COMMIT;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  mbi.BaseAddress = reinterpret_cast<void*>(10);
  mbi.RegionSize = 5;
  mbi.State = MEM_FREE;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  mbi.BaseAddress = reinterpret_cast<void*>(15);
  mbi.RegionSize = 100;
  mbi.State = MEM_COMMIT;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  std::vector<CheckedRange<WinVMAddress, WinVMSize>> result =
      GetReadableRangesOfMemoryMap(CheckedRange<WinVMAddress, WinVMSize>(5, 45),
                                   memory_info);

  ASSERT_EQ(2u, result.size());
  EXPECT_EQ(5, result[0].base());
  EXPECT_EQ(5, result[0].size());
  EXPECT_EQ(15, result[1].base());
  EXPECT_EQ(35, result[1].size());
}

TEST(ProcessInfo, RequestedBeforeMap) {
  std::vector<ProcessInfo::MemoryInfo> memory_info;
  MEMORY_BASIC_INFORMATION mbi = {0};

  mbi.BaseAddress = reinterpret_cast<void*>(10);
  mbi.RegionSize = 10;
  mbi.State = MEM_COMMIT;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  std::vector<CheckedRange<WinVMAddress, WinVMSize>> result =
      GetReadableRangesOfMemoryMap(CheckedRange<WinVMAddress, WinVMSize>(5, 10),
                                   memory_info);

  ASSERT_EQ(1u, result.size());
  EXPECT_EQ(10, result[0].base());
  EXPECT_EQ(5, result[0].size());
}

TEST(ProcessInfo, RequestedAfterMap) {
  std::vector<ProcessInfo::MemoryInfo> memory_info;
  MEMORY_BASIC_INFORMATION mbi = {0};

  mbi.BaseAddress = reinterpret_cast<void*>(10);
  mbi.RegionSize = 10;
  mbi.State = MEM_COMMIT;
  memory_info.push_back(ProcessInfo::MemoryInfo(mbi));

  std::vector<CheckedRange<WinVMAddress, WinVMSize>> result =
      GetReadableRangesOfMemoryMap(
          CheckedRange<WinVMAddress, WinVMSize>(15, 100), memory_info);

  ASSERT_EQ(1u, result.size());
  EXPECT_EQ(15, result[0].base());
  EXPECT_EQ(5, result[0].size());
}

TEST(ProcessInfo, ReadableRanges) {
  SYSTEM_INFO system_info;
  GetSystemInfo(&system_info);

  const size_t kBlockSize = system_info.dwPageSize;

  // Allocate 6 pages, and then commit the second, fourth, and fifth, and mark
  // two as committed, but PAGE_NOACCESS, so we have a setup like this:
  // 0       1       2       3       4       5
  // +-----------------------------------------------+
  // | ????? |       | xxxxx |       |       | ????? |
  // +-----------------------------------------------+
  void* reserve_region =
      VirtualAlloc(nullptr, kBlockSize * 6, MEM_RESERVE, PAGE_READWRITE);
  ASSERT_TRUE(reserve_region);
  uintptr_t reserved_as_int = reinterpret_cast<uintptr_t>(reserve_region);
  void* readable1 =
      VirtualAlloc(reinterpret_cast<void*>(reserved_as_int + kBlockSize),
                   kBlockSize,
                   MEM_COMMIT,
                   PAGE_READWRITE);
  ASSERT_TRUE(readable1);
  void* readable2 =
      VirtualAlloc(reinterpret_cast<void*>(reserved_as_int + (kBlockSize * 3)),
                   kBlockSize * 2,
                   MEM_COMMIT,
                   PAGE_READWRITE);
  ASSERT_TRUE(readable2);

  void* no_access =
      VirtualAlloc(reinterpret_cast<void*>(reserved_as_int + (kBlockSize * 2)),
                   kBlockSize,
                   MEM_COMMIT,
                   PAGE_NOACCESS);
  ASSERT_TRUE(no_access);

  HANDLE current_process = GetCurrentProcess();
  ProcessInfo info;
  info.Initialize(current_process);
  auto ranges = info.GetReadableRanges(
      CheckedRange<WinVMAddress, WinVMSize>(reserved_as_int, kBlockSize * 6));

  ASSERT_EQ(2u, ranges.size());
  EXPECT_EQ(reserved_as_int + kBlockSize, ranges[0].base());
  EXPECT_EQ(kBlockSize, ranges[0].size());
  EXPECT_EQ(reserved_as_int + (kBlockSize * 3), ranges[1].base());
  EXPECT_EQ(kBlockSize * 2, ranges[1].size());

  // Also make sure what we think we can read corresponds with what we can
  // actually read.
  scoped_ptr<unsigned char[]> into(new unsigned char[kBlockSize * 6]);
  SIZE_T bytes_read;

  EXPECT_TRUE(ReadProcessMemory(
      current_process, readable1, into.get(), kBlockSize, &bytes_read));
  EXPECT_EQ(kBlockSize, bytes_read);

  EXPECT_TRUE(ReadProcessMemory(
      current_process, readable2, into.get(), kBlockSize * 2, &bytes_read));
  EXPECT_EQ(kBlockSize * 2, bytes_read);

  EXPECT_FALSE(ReadProcessMemory(
      current_process, no_access, into.get(), kBlockSize, &bytes_read));
  EXPECT_FALSE(ReadProcessMemory(
      current_process, reserve_region, into.get(), kBlockSize, &bytes_read));
  EXPECT_FALSE(ReadProcessMemory(current_process,
                                 reserve_region,
                                 into.get(),
                                 kBlockSize * 6,
                                 &bytes_read));
}

}  // namespace
}  // namespace test
}  // namespace crashpad
