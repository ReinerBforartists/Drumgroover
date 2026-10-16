 /*
 ==============================================================================
 
  MIT License

  iPlug2 WebView Library
  Copyright (c) 2024 Oliver Larkin

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
 
 ==============================================================================
*/

// DrumGroover's copy of iPlug2's IPlugWebView_win.cpp (Windows WebView2 backend).
// CMakeLists.txt compiles this file INSTEAD of the one in iPlug2, so the iPlug2
// tree stays unmodified. Differences to upstream: embedded web resources hook
// (gIPlugEmbeddedWebResource), OleInitialize for OS drag-and-drop, fixed
// rasterization scale 1.0, browser accelerator keys (F5/Ctrl+R) disabled.

#include "IPlugWebView.h"
#include "IPlugPaths.h"
#include <algorithm>
#include <atomic>
#include <string>
#include <cstring>
#include <mutex>
#include <vector>
#include <windows.h>
#include <wininet.h>
#include <shlobj.h>
#include <cassert>

#include <wrl.h>
#include <wil/com.h>
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"
#include "wdlstring.h"

#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

extern float GetScaleForHWND(HWND hWnd);

// Embedded web resources: when the plugin sets this, https://iplug.example/<path> is answered from memory
// (resources compiled into the plugin) instead of from a folder on disk.
bool (*gIPlugEmbeddedWebResource)(const char* path, const unsigned char** data, size_t* size, const char** mime) = nullptr;

BEGIN_IPLUG_NAMESPACE

class IWebViewImpl
{
public:
  IWebViewImpl(IWebView* owner);
  ~IWebViewImpl();

  void* OpenWebView(void* pParent, float x, float y, float w, float h, float scale);
  void CloseWebView();
  void HideWebView(bool hide);
  void LoadHTML(const char* html);
  void LoadURL(const char* url);
  void LoadFile(const char* fileName, const char* bundleID);
  void ReloadPageContent();
  void EvaluateJavaScript(const char* scriptStr, IWebView::completionHandlerFunc func);
  void EnableScroll(bool enable);
  void EnableInteraction(bool enable);
  void SetWebViewBounds(float x, float y, float w, float h, float scale);
  void GetWebRoot(WDL_String& path) const { path.Set(mWebRoot.Get()); }
  void GetLocalDownloadPathForFile(const char* fileName, WDL_String& downloadPath);
  IWebView* Owner() const { return mIWebView; }
  void SetDevTools(bool enable);

private:
  RECT GetScaledRect(float x, float y, float w, float h, float scale)
  {
    RECT r;
    r.left = static_cast<LONG>(std::ceil(x * scale));
    r.top = static_cast<LONG>(std::ceil(y * scale));
    r.right = static_cast<LONG>(std::ceil((x + w) * scale)) + 1;
    r.bottom = static_cast<LONG>(std::ceil((y + h) * scale)) + 1;
    return r;
  }

  IWebView* mIWebView;
  bool mOpaque;
  HWND mParentWnd = NULL;
  wil::com_ptr<ICoreWebView2Controller> mWebViewCtrlr;
  wil::com_ptr<ICoreWebView2> mCoreWebView;
  wil::com_ptr<ICoreWebView2Environment> mWebViewEnvironment;
  EventRegistrationToken mWebMessageReceivedToken;
  EventRegistrationToken mNavigationStartingToken;
  EventRegistrationToken mNavigationCompletedToken;
  EventRegistrationToken mNewWindowRequestedToken;
  EventRegistrationToken mDownloadStartingToken;
  EventRegistrationToken mWebResourceRequestedToken;
  EventRegistrationToken mContextMenuRequestedToken;
  std::atomic<bool> mDevToolsOn{false};
  bool mContextMenuGate = false;   // true: the context menu is gated by mDevToolsOn through an event handler
  EventRegistrationToken mBytesReceivedChangedToken;
  EventRegistrationToken mStateChangedToken;
  bool mShowOnLoad = true;
  WDL_String mWebRoot;
  RECT mWebViewBounds;
};

END_IPLUG_NAMESPACE

using namespace iplug;
using namespace Microsoft::WRL;

namespace {
// Every open WebView, so DggSetWebViewDevTools() can reach the settings of a running one.
std::mutex gImplMutex;
std::vector<IWebViewImpl*> gImpls;
}

IWebViewImpl::IWebViewImpl(IWebView* owner)
  : mIWebView(owner)
{
  std::lock_guard<std::mutex> lock(gImplMutex);
  gImpls.push_back(this);
}

IWebViewImpl::~IWebViewImpl()
{
  {
    std::lock_guard<std::mutex> lock(gImplMutex);
    gImpls.erase(std::remove(gImpls.begin(), gImpls.end(), this), gImpls.end());
  }
  CloseWebView();
}

void IWebViewImpl::SetDevTools(bool enable)
{
  mDevToolsOn = enable;
  if (!mCoreWebView || mContextMenuGate) return;
  wil::com_ptr<ICoreWebView2Settings> settings;
  if (SUCCEEDED(mCoreWebView->get_Settings(&settings)) && settings)
  {
    settings->put_AreDefaultContextMenusEnabled(enable);
    settings->put_AreDevToolsEnabled(enable);
  }
}

// Switches DevTools and the right-click menu of the WebView owned by `owner` on or off while it is running.
void DggSetWebViewDevTools(iplug::IWebView* owner, bool enable)
{
  owner->SetEnableDevTools(enable);
  std::lock_guard<std::mutex> lock(gImplMutex);
  for (IWebViewImpl* impl : gImpls)
    if (impl->Owner() == owner) impl->SetDevTools(enable);
}

void* IWebViewImpl::OpenWebView(void* pParent, float,float,float,float,float)
{
  mParentWnd = (HWND)pParent;

  // WebView2 needs full OLE initialisation (not just CoInitializeEx) for OS-level drag-and-drop, e.g. dragging a
  // MIDI file out of the plugin into the DAW. The result is ignored on purpose: it is S_FALSE if the host already
  // initialised OLE on this thread, and there is no fallback if it fails.
  OleInitialize(nullptr);

  WDL_String cachePath;
  WebViewCachePath(cachePath);
  int bufSize = UTF8ToUTF16Len(cachePath.Get());
  std::vector<WCHAR> cachePathWide(bufSize);
  UTF8ToUTF16(cachePathWide.data(), cachePath.Get(), IPLUG_WIN_MAX_WIDE_PATH);

  auto options = Make<CoreWebView2EnvironmentOptions>();
  options->put_AllowSingleSignOnUsingOSPrimaryAccount(FALSE);
  options->put_ExclusiveUserDataFolderAccess(FALSE);
  options->put_IsCustomCrashReportingEnabled(FALSE);

  CreateCoreWebView2EnvironmentWithOptions(
    nullptr, cachePathWide.data(), options.Get(),
    Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([&](
                                                                           HRESULT result,
                                                                           ICoreWebView2Environment* env) -> HRESULT {
      if (FAILED(result) || env == nullptr)
      {
        // Typically the WebView2 Runtime is not installed. Without this check the call below dereferences null.
        const int answer = MessageBoxW(
          mParentWnd,
          L"DrumGroover needs the Microsoft Edge WebView2 Runtime, which was not found on this computer.\n\n"
          L"Open the Microsoft download page now? Install the \"Evergreen Bootstrapper\", then restart your host.",
          L"DrumGroover", MB_YESNO | MB_ICONERROR);
        if (answer == IDYES)
          ShellExecuteW(nullptr, L"open", L"https://developer.microsoft.com/en-us/microsoft-edge/webview2/", nullptr,
                        nullptr, SW_SHOWNORMAL);
        return S_OK;
      }

      mWebViewEnvironment = env;

      mWebViewEnvironment->CreateCoreWebView2Controller(
        mParentWnd,
          Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([&](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            if (controller != nullptr)
            {
              mWebViewCtrlr = controller;
              mWebViewCtrlr->get_CoreWebView2(&mCoreWebView);
            }
            
            if (mCoreWebView == nullptr)
            {
              return S_OK;
            }

            // Fixed at 1.0 instead of the OS scale: at e.g. 125% DPI the CSS viewport shrinks to physicalBounds/1.25
            // and clips the fixed 1440x960 layout. At 1.0 the CSS viewport equals the physical bounds.
            auto controller3 = mWebViewCtrlr.try_query<ICoreWebView2Controller3>();
            if (controller3)
            {
              controller3->put_ShouldDetectMonitorScaleChanges(FALSE);
              controller3->put_RasterizationScale(1.0);
            }

            mWebViewCtrlr->put_IsVisible(mShowOnLoad);

            const auto enableDevTools = mIWebView->GetEnableDevTools();
            mDevToolsOn = enableDevTools;

            // The settings AreDefaultContextMenusEnabled / AreDevToolsEnabled only take effect on the next navigation.
            // To switch DevTools at runtime both stay enabled and a ContextMenuRequested handler suppresses the context
            // menu (the only way to open DevTools; accelerator keys are off) while mDevToolsOn is false. Runtimes
            // without that event use the plain settings.
            auto webView2_11 = mCoreWebView.try_query<ICoreWebView2_11>();
            if (webView2_11)
            {
              webView2_11->add_ContextMenuRequested(
                Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
                  [this](ICoreWebView2* sender, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
                    if (!mDevToolsOn.load()) args->put_Handled(TRUE);
                    return S_OK;
                  }).Get(),
                &mContextMenuRequestedToken);
              mContextMenuGate = true;
            }

            ICoreWebView2Settings* Settings;
            mCoreWebView->get_Settings(&Settings);
            Settings->put_IsScriptEnabled(TRUE);
            Settings->put_AreDefaultScriptDialogsEnabled(TRUE);
            Settings->put_IsWebMessageEnabled(TRUE);
            Settings->put_AreDefaultContextMenusEnabled(mContextMenuGate ? TRUE : enableDevTools);
            Settings->put_AreDevToolsEnabled(mContextMenuGate ? TRUE : enableDevTools);

            // Chromium's accelerators (F5, Ctrl+F5, Ctrl+R) do a native hard reload that bypasses the app's JS and can
            // drop an in-flight state-cache message (cacheState), so they are switched off. AreBrowserAcceleratorKeysEnabled
            // lives in ICoreWebView2Settings3, hence the QueryInterface.
            ICoreWebView2Settings3* Settings3 = nullptr;
            if (SUCCEEDED(Settings->QueryInterface(IID_PPV_ARGS(&Settings3))) && Settings3)
            {
              Settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
              Settings3->Release();
            }

            // this script adds a function IPlugSendMsg that is used to communicate from the WebView to the C++ side
            mCoreWebView->AddScriptToExecuteOnDocumentCreated(
              L"function IPlugSendMsg(m) {window.chrome.webview.postMessage(m)};",
              Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>([this](HRESULT error,
                                                                                                PCWSTR id) -> HRESULT {
                return S_OK;
              }).Get());

            // this script receives global key down events and forwards them to the C++ side
            mCoreWebView->AddScriptToExecuteOnDocumentCreated(
              L"document.addEventListener('keydown', function(e) { if(document.activeElement.type != \"text\") { IPlugSendMsg({'msg': 'SKPFUI', 'keyCode': e.keyCode, 'utf8': e.key, 'S': e.shiftKey, 'C': e.ctrlKey, 'A': e.altKey, 'isUp': false}); }});",
              Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>([this](HRESULT error, PCWSTR id) -> HRESULT { return S_OK; }).Get());

            // this script receives global key up events and forwards them to the C++ side
            mCoreWebView->AddScriptToExecuteOnDocumentCreated(
              L"document.addEventListener('keyup', function(e) { if(document.activeElement.type != \"text\") { IPlugSendMsg({'msg': 'SKPFUI', 'keyCode': e.keyCode, 'utf8': e.key, 'S': e.shiftKey, 'C': e.ctrlKey, 'A': e.altKey, 'isUp': true}); }});",
              Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>([this](HRESULT error, PCWSTR id) -> HRESULT { return S_OK; }).Get());

            mCoreWebView->add_WebMessageReceived(
              Callback<ICoreWebView2WebMessageReceivedEventHandler>([this](
                                                                      ICoreWebView2* sender,
                                                                      ICoreWebView2WebMessageReceivedEventArgs* args) {
                wil::unique_cotaskmem_string jsonString;
                args->get_WebMessageAsJson(&jsonString);
                std::wstring jsonWString = jsonString.get();
                WDL_String cStr;
                UTF16ToUTF8(cStr, jsonWString.c_str());
                mIWebView->OnMessageFromWebView(cStr.Get());
                return S_OK;
              }).Get(),
              &mWebMessageReceivedToken);


            if (gIPlugEmbeddedWebResource)
            {
              mCoreWebView->AddWebResourceRequestedFilter(L"https://iplug.example/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
              mCoreWebView->add_WebResourceRequested(
                Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                  [this](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                    wil::com_ptr<ICoreWebView2WebResourceRequest> request;
                    args->get_Request(&request);
                    wil::unique_cotaskmem_string uri;
                    if (!request || FAILED(request->get_Uri(&uri))) return S_OK;

                    // Path after the host, without query or fragment.
                    std::wstring url = uri.get();
                    const std::wstring hostPrefix = L"https://iplug.example/";
                    std::wstring path = (url.compare(0, hostPrefix.size(), hostPrefix) == 0) ? url.substr(hostPrefix.size()) : std::wstring();
                    path = path.substr(0, path.find_first_of(L"?#"));
                    char pathUtf8[1024] = {};
                    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, pathUtf8, (int)sizeof(pathUtf8) - 1, nullptr, nullptr);

                    const unsigned char* data = nullptr;
                    size_t size = 0;
                    const char* mime = "application/octet-stream";
                    wil::com_ptr<ICoreWebView2WebResourceResponse> response;
                    if (gIPlugEmbeddedWebResource(pathUtf8, &data, &size, &mime))
                    {
                      IStream* stream = SHCreateMemStream(data, (UINT)size);
                      std::wstring headers = L"Content-Type: " + std::wstring(mime, mime + strlen(mime)) + L"\r\nCache-Control: no-store";
                      mWebViewEnvironment->CreateWebResourceResponse(stream, 200, L"OK", headers.c_str(), &response);
                      if (stream) stream->Release();
                    }
                    else
                    {
                      mWebViewEnvironment->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &response);
                    }
                    args->put_Response(response.get());
                    return S_OK;
                  }).Get(),
                &mWebResourceRequestedToken);
            }

            mCoreWebView->add_NavigationStarting(
              Callback<ICoreWebView2NavigationStartingEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                  
                  wil::unique_cotaskmem_string uri;
                  args->get_Uri(&uri);
                  std::wstring uriUTF16 = uri.get();
                  WDL_String uriUTF8;
                  UTF16ToUTF8(uriUTF8, uriUTF16.c_str());
                  
                  if (mIWebView->OnCanNavigateToURL(uriUTF8.Get()) == false)
                  {
                    args->put_Cancel(TRUE);
                  }

                  return S_OK;
                }).Get(),
              &mNavigationStartingToken);

            mCoreWebView->add_NavigationCompleted(
              Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                  BOOL success;
                  args->get_IsSuccess(&success);
                  if (success)
                  {
                    mIWebView->OnWebContentLoaded();
                  }
                  return S_OK;
                })
                .Get(),
              &mNavigationCompletedToken);

              mCoreWebView->add_NewWindowRequested(
              Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                  [this](ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args)
                    -> HRESULT 
              {
                wil::com_ptr<ICoreWebView2NewWindowRequestedEventArgs2> args2;

                if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args2))))
                {
                  DWORD inetStatus = 0;
                  if (InternetGetConnectedState(&inetStatus, 0))
                  {
                    wil::unique_cotaskmem_string uri;

                    args2->get_Uri(&uri);

                    if (ShellExecuteW(mParentWnd, L"open", uri.get(), 0, 0, SW_SHOWNORMAL) > HINSTANCE(32))
                    {
                      args->put_Handled(true);
                    }
                  }
                }
                return S_OK;
              }).Get(),
                &mNewWindowRequestedToken);

              auto webView2_4 = mCoreWebView.try_query<ICoreWebView2_4>();
              if (webView2_4)
              {
                webView2_4->add_DownloadStarting(
                  Callback<ICoreWebView2DownloadStartingEventHandler>(
                  [this](
                    ICoreWebView2* sender,
                    ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {

                    // Hide the default download dialog.
                    args->put_Handled(TRUE);

                    wil::com_ptr<ICoreWebView2DownloadOperation> download;
                    args->get_DownloadOperation(&download);

                    INT64 totalBytesToReceive = 0;
                    download->get_TotalBytesToReceive(&totalBytesToReceive);

                    // validate MIME type
                    wil::unique_cotaskmem_string mimeType;
                    download->get_MimeType(&mimeType);
                    std::wstring mimeTypeUTF16 = mimeType.get();
                    WDL_String mimeTypeUTF8;
                    UTF16ToUTF8(mimeTypeUTF8, mimeTypeUTF16.c_str());
                    if (!mIWebView->OnCanDownloadMIMEType(mimeTypeUTF8.Get()))
                    {
                      args->put_Cancel(TRUE);
                    }

                    wil::unique_cotaskmem_string contentDisposition;
                    download->get_ContentDisposition(&contentDisposition);

                    // Modify download path
                    wil::unique_cotaskmem_string resultFilePath;
                    args->get_ResultFilePath(&resultFilePath);

                    std::wstring initialPathUTF16 = resultFilePath.get();
                    WDL_String initialPathUTF8, downloadPathUTF8;
                    UTF16ToUTF8(initialPathUTF8, initialPathUTF16.c_str());
                    mIWebView->OnGetLocalDownloadPathForFile(initialPathUTF8.Get(), downloadPathUTF8);

                    int bufSize = UTF8ToUTF16Len(downloadPathUTF8.Get());
                    std::vector<WCHAR> downloadPathWide(bufSize);
                    UTF8ToUTF16(downloadPathWide.data(), downloadPathUTF8.Get(), bufSize);

                    args->put_ResultFilePath(downloadPathWide.data());
                    
                    download->add_BytesReceivedChanged(Callback<ICoreWebView2BytesReceivedChangedEventHandler>([this](ICoreWebView2DownloadOperation* download, IUnknown* args) -> HRESULT {
                                                         INT64 bytesReceived, totalNumBytes;
                                                         download->get_BytesReceived(&bytesReceived);
                                                         download->get_TotalBytesToReceive(&totalNumBytes);
                                                         mIWebView->OnReceivedData(bytesReceived, totalNumBytes);
                          return S_OK;
                        }).Get(),
                        &mBytesReceivedChangedToken);

                        download->add_StateChanged(Callback<ICoreWebView2StateChangedEventHandler>([this](ICoreWebView2DownloadOperation* download, IUnknown* args) -> HRESULT {
                                                               COREWEBVIEW2_DOWNLOAD_STATE downloadState;
                                                               download->get_State(&downloadState);

                                                               auto onDownloadEnded = [&](ICoreWebView2DownloadOperation* download, bool success) {
                                                                 download->remove_BytesReceivedChanged(mBytesReceivedChangedToken);
                                                                 download->remove_StateChanged(mStateChangedToken);
                                                                 wil::unique_cotaskmem_string resultFilePath;
                                                                 download->get_ResultFilePath(&resultFilePath);
                                                                 std::wstring downloadPathUTF16 = resultFilePath.get();
                                                                 WDL_String downloadPathUTF8;
                                                                 UTF16ToUTF8(downloadPathUTF8, downloadPathUTF16.c_str());

                                                                 if (success) {
                                                                  mIWebView->OnDownloadedFile(downloadPathUTF8.Get());
                                                                 }
                                                                 else {
                                                                  mIWebView->OnFailedToDownloadFile(downloadPathUTF8.Get());
                                                                 }
                                                               };

                                                               switch (downloadState)
                                                               {
                                                               case COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS:
                                                                 break;
                                                               case COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED:
                                                                 onDownloadEnded(download, false);
                                                                 break;
                                                               case COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED:
                                                                 onDownloadEnded(download, true);
                                                                 break;
                                                               }
                                                               return S_OK;
                                                             }).Get(),
                                                             &mStateChangedToken);

                    return S_OK;
                  })
                  .Get(),
              &mDownloadStartingToken);
            }

            if (!mOpaque)
            {
              wil::com_ptr<ICoreWebView2Controller2> controller2 = mWebViewCtrlr.query<ICoreWebView2Controller2>();
              COREWEBVIEW2_COLOR color;
              memset(&color, 0, sizeof(COREWEBVIEW2_COLOR));
              controller2->put_DefaultBackgroundColor(color);
            }

            mWebViewCtrlr->put_Bounds(mWebViewBounds);
            mIWebView->OnWebViewReady();
            return S_OK;
          })
          .Get());

      return S_OK;
    }).Get());

  return mParentWnd;
}

void IWebViewImpl::CloseWebView()
{
  if (mWebViewCtrlr.get() != nullptr)
  {
    mWebViewCtrlr->Close();
    mWebViewCtrlr = nullptr;
    mCoreWebView = nullptr;
    mWebViewEnvironment = nullptr;
  }
}

void IWebViewImpl::HideWebView(bool hide)
{
  if (mWebViewCtrlr.get() != nullptr)
  {
    mWebViewCtrlr->put_IsVisible(!hide);
  }
  else
  {
    // the controller is set asynchonously, so we store the state 
    // to apply it when the controller is created
    mShowOnLoad = !hide;
  }
}

void IWebViewImpl::LoadHTML(const char* html)
{
  if (mCoreWebView)
  {
    int bufSize = UTF8ToUTF16Len(html);
    std::vector<WCHAR> htmlWide(bufSize);
    UTF8ToUTF16(htmlWide.data(), html, bufSize);
    mCoreWebView->NavigateToString(htmlWide.data());
  }
}

void IWebViewImpl::LoadURL(const char* url)
{
  if (mCoreWebView)
  {
    int bufSize = UTF8ToUTF16Len(url);
    std::vector<WCHAR> urlWide(bufSize);
    UTF8ToUTF16(urlWide.data(), url, bufSize);
    mCoreWebView->Navigate(urlWide.data());
  }
}

void IWebViewImpl::LoadFile(const char* fileName, const char* bundleID)
{
  if (mCoreWebView)
  {
    wil::com_ptr<ICoreWebView2_3> webView3 = mCoreWebView.try_query<ICoreWebView2_3>();
    if (webView3)
    {
      WDL_String webFolder{fileName};
      webFolder.remove_filepart();
      int bufSize = UTF8ToUTF16Len(webFolder.Get());
      std::vector<WCHAR> webFolderWide(bufSize);
      UTF8ToUTF16(webFolderWide.data(), webFolder.Get(), bufSize);

      webView3->SetVirtualHostNameToFolderMapping(
        L"iplug.example", webFolderWide.data(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    }

    WDL_String baseName{fileName};
    WDL_String root{fileName};
    root.remove_filepart();
    mWebRoot.Set(root.Get());

    WDL_String fullStr;
    fullStr.SetFormatted(2048, "https://iplug.example/%s", baseName.get_filepart());
    int bufSize = UTF8ToUTF16Len(fullStr.Get());
    std::vector<WCHAR> fileUrlWide(bufSize);
    UTF8ToUTF16(fileUrlWide.data(), fullStr.Get(), bufSize);
    mCoreWebView->Navigate(fileUrlWide.data());
  }
}


void IWebViewImpl::ReloadPageContent()
{
  if (mCoreWebView)
  {
    mCoreWebView->Reload();
  }
}

void IWebViewImpl::EvaluateJavaScript(const char* scriptStr, IWebView::completionHandlerFunc func)
{
  if (mCoreWebView)
  {
    int bufSize = UTF8ToUTF16Len(scriptStr);
    std::vector<WCHAR> scriptWide(bufSize);
    UTF8ToUTF16(scriptWide.data(), scriptStr, bufSize);

    mCoreWebView->ExecuteScript(
      scriptWide.data(), Callback<ICoreWebView2ExecuteScriptCompletedHandler>([func](HRESULT errorCode,
                                                                              LPCWSTR resultObjectAsJson) -> HRESULT {
                    if (func && resultObjectAsJson)
                    {
                      WDL_String str;
                      UTF16ToUTF8(str, resultObjectAsJson);
                      func(str.Get());
                    }
                    return S_OK;
                  }).Get());
  }
}

void IWebViewImpl::EnableScroll(bool enable)
{
  /* NO-OP */
}

void IWebViewImpl::EnableInteraction(bool enable)
{
  /* NO-OP */
}

void IWebViewImpl::SetWebViewBounds(float x, float y, float w, float h, float scale)
{
  mWebViewBounds = GetScaledRect(x, y, w, h, GetScaleForHWND(mParentWnd));

  if (mWebViewCtrlr)
  {
    mWebViewCtrlr->SetBoundsAndZoomFactor(mWebViewBounds, scale);
  }
}

void IWebViewImpl::GetLocalDownloadPathForFile(const char* fileName, WDL_String& downloadPath)
{
  DesktopPath(downloadPath);
  downloadPath.Append(fileName);
}

#include "IPlugWebView.cpp"