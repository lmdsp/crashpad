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

#include "handler/win/crash_report_exception_handler.h"

#include <type_traits>
#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "client/crash_report_database.h"
#include "client/settings.h"
#include "handler/crash_report_upload_thread.h"
#include "minidump/minidump_file_writer.h"
#include "minidump/minidump_user_extension_stream_data_source.h"
#include "snapshot/win/process_snapshot_win.h"
#include "util/file/file_helper.h"
#include "util/file/file_writer.h"
#include "util/misc/metrics.h"
#include "util/win/registration_protocol_win.h"
#include "util/win/scoped_process_suspend.h"
#include "util/win/termination_codes.h"

namespace crashpad {

CrashReportExceptionHandler::CrashReportExceptionHandler(
    CrashReportDatabase* database,
    CrashReportUploadThread* upload_thread,
    const std::map<std::string, std::string>* process_annotations,
    const std::vector<base::FilePath>* attachments,
    const UserStreamDataSources* user_stream_data_sources)
    : database_(database),
      upload_thread_(upload_thread),
      process_annotations_(process_annotations),
      attachments_(attachments),
      user_stream_data_sources_(user_stream_data_sources) {}

CrashReportExceptionHandler::~CrashReportExceptionHandler() {}

void CrashReportExceptionHandler::ExceptionHandlerServerStarted() {}

#if defined(OS_WIN)

LRESULT CALLBACK ReportWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        // Text entry
     

        // Add attachments

        // Submit button
        CreateWindowW(L"Button",
                      L"Submit",
                      WS_VISIBLE | WS_CHILD,
                      20,
                      50,
                      80,
                      25,
                      hwnd,
                      nullptr,
                      nullptr,
                      nullptr);
        break;
    }

    case WM_COMMAND:
    {
        break;
    }

    case WM_DESTROY:
    {
        // TODO Exit loop
        // break;
    }

    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

#endif // OS_WIN

void ShowReportDialog()
{
    LOG(INFO) << "Showing user report dialog";

#if defined(OS_WIN)
#if 0
    constexpr unsigned int flags = MB_OK | MB_ICONERROR | MB_SYSTEMMODAL;

    // MessageBoxW(nullptr, L"Please describe what you were doing before the crash", L"Crash report", flags);

#else

    // http://zetcode.com/gui/winapi/window/
    WNDCLASSW wc{};

    HWND hdesktop = GetDesktopWindow();

    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpszClassName = L"Crashpad.Reporter";
    wc.lpfnWndProc = ReportWndProc;
    wc.hbrBackground = GetSysColorBrush(COLOR_3DFACE);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_EXCLAMATION);

    RegisterClassW(&wc);

    int create_params = 0;

    DWORD style = WS_OVERLAPPEDWINDOW;

    style = WS_DLGFRAME;
    style = WS_POPUP;

    HWND hwnd = CreateWindowW(wc.lpszClassName,
                              L"Crash report",
                              style | WS_VISIBLE,
                              CW_USEDEFAULT,
                              CW_USEDEFAULT,
                              400,
                              300,
                              hdesktop,
                              nullptr,
                              nullptr,
                              &create_params);

    UpdateWindow(hwnd);

    MSG msg{};

    while (GetMessage(&msg, nullptr, 0, 0))
    {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
#endif // 0

#endif // OS_WIN

}

unsigned int CrashReportExceptionHandler::ExceptionHandlerServerException(
    HANDLE process,
    WinVMAddress exception_information_address,
    WinVMAddress debug_critical_section_address) {
  Metrics::ExceptionEncountered();

  ScopedProcessSuspend suspend(process);

  ProcessSnapshotWin process_snapshot;
  if (!process_snapshot.Initialize(process,
                                   ProcessSuspensionState::kSuspended,
                                   exception_information_address,
                                   debug_critical_section_address)) {
    Metrics::ExceptionCaptureResult(Metrics::CaptureResult::kSnapshotFailed);
    return kTerminationCodeSnapshotFailed;
  }

  // Now that we have the exception information, even if something else fails we
  // can terminate the process with the correct exit code.
  const unsigned int termination_code =
      process_snapshot.Exception()->Exception();
  static_assert(
      std::is_same<std::remove_const<decltype(termination_code)>::type,
                   decltype(process_snapshot.Exception()->Exception())>::value,
      "expected ExceptionCode() and process termination code to match");

  Metrics::ExceptionCode(termination_code);

  CrashpadInfoClientOptions client_options;
  process_snapshot.GetCrashpadOptions(&client_options);
  if (client_options.crashpad_handler_behavior != TriState::kDisabled) {

    ShowReportDialog();

    UUID client_id;
    Settings* const settings = database_->GetSettings();
    if (settings) {
      // If GetSettings() or GetClientID() fails, something else will log a
      // message and client_id will be left at its default value, all zeroes,
      // which is appropriate.
      settings->GetClientID(&client_id);
    }

    process_snapshot.SetClientID(client_id);
    process_snapshot.SetAnnotationsSimpleMap(*process_annotations_);

    std::unique_ptr<CrashReportDatabase::NewReport> new_report;
    CrashReportDatabase::OperationStatus database_status =
        database_->PrepareNewCrashReport(&new_report);
    if (database_status != CrashReportDatabase::kNoError) {
      LOG(ERROR) << "PrepareNewCrashReport failed";
      Metrics::ExceptionCaptureResult(
          Metrics::CaptureResult::kPrepareNewCrashReportFailed);
      return termination_code;
    }

    process_snapshot.SetReportID(new_report->ReportID());

    MinidumpFileWriter minidump;
    minidump.InitializeFromSnapshot(&process_snapshot);
    AddUserExtensionStreams(
        user_stream_data_sources_, &process_snapshot, &minidump);

    if (!minidump.WriteEverything(new_report->Writer())) {
      LOG(ERROR) << "WriteEverything failed";
      Metrics::ExceptionCaptureResult(
          Metrics::CaptureResult::kMinidumpWriteFailed);
      return termination_code;
    }

    for (const auto& attachment : (*attachments_)) {
      FileReader file_reader;
      if (!file_reader.Open(attachment)) {
        LOG(ERROR) << "attachment " << attachment.value().c_str()
                   << " couldn't be opened, skipping";
        continue;
      }

      base::FilePath filename = attachment.BaseName();
      FileWriter* file_writer =
          new_report->AddAttachment(base::WideToUTF8(filename.value()));
      if (file_writer == nullptr) {
        LOG(ERROR) << "attachment " << filename.value().c_str()
                   << " couldn't be created, skipping";
        continue;
      }

      CopyFileContent(&file_reader, file_writer);
    }

    UUID uuid;
    database_status =
        database_->FinishedWritingCrashReport(std::move(new_report), &uuid);
    if (database_status != CrashReportDatabase::kNoError) {
      LOG(ERROR) << "FinishedWritingCrashReport failed";
      Metrics::ExceptionCaptureResult(
          Metrics::CaptureResult::kFinishedWritingCrashReportFailed);
      return termination_code;
    }

    if (upload_thread_) {
      upload_thread_->ReportPending(uuid);
    }
  }

  Metrics::ExceptionCaptureResult(Metrics::CaptureResult::kSuccess);
  return termination_code;
}

}  // namespace crashpad
