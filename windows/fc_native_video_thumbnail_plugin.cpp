#include "fc_native_video_thumbnail_plugin.h"

// 必须在 Windows 头文件之前
#include <atlimage.h>
#include <comdef.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <gdiplus.h>
#include <gdiplusimaging.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <wincodec.h>
#include <windows.h>
#include <wingdi.h>
#include <shobjidl.h>

// 引入 WinRT 支持
#include <winrt/Windows.Storage.h>

#include <codecvt>
#include <iostream>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <chrono>

using namespace winrt::Windows::Storage;

namespace fc_native_video_thumbnail {

// --- 1. 日志与辅助工具 ---

std::string Utf8FromUtf16(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

void WriteLog(const std::string& message) {
    try {
        std::wstring logPath;
        try {
            auto localFolder = ApplicationData::Current().LocalFolder().Path();
            logPath = std::wstring(localFolder.c_str()) + L"\\plugin_debug.log";
        } catch (...) {
            logPath = L"plugin_debug.log";
        }

        std::ofstream logFile(logPath, std::ios::app);
        if (logFile.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            struct tm buf;
            localtime_s(&buf, &time_t_now);
            logFile << std::put_time(&buf, "%Y-%m-%d %H:%M:%S") << " [LOG] " << message << std::endl;
            logFile.close();
        }
    } catch (...) {}
}

std::wstring Utf16FromUtf8(const std::string& utf8_string) {
    if (utf8_string.empty()) return std::wstring();
    int target_length = ::MultiByteToWideChar(CP_UTF8, 0, utf8_string.data(), (int)utf8_string.length(), nullptr, 0);
    std::wstring utf16_string(target_length, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8_string.data(), (int)utf8_string.length(), &utf16_string[0], target_length);
    return utf16_string;
}

std::string HRESULTToString(HRESULT hr) {
    _com_error error(hr);
    std::stringstream ss;
    ss << "0x" << std::hex << std::setw(8) << std::setfill('0') << hr << " (" << Utf8FromUtf16(error.ErrorMessage()) << ")";
    return ss.str();
}

const flutter::EncodableValue* ValueOrNull(const flutter::EncodableMap& map, const char* key) {
    auto it = map.find(flutter::EncodableValue(key));
    if (it == map.end()) return nullptr;
    return &(it->second);
}

// --- 2. 路径解析逻辑 ---

std::wstring ResolvePhysicalPath(const std::wstring& virtualPath, const std::string& label) {
    WriteLog("正在解析 " + label + ": " + Utf8FromUtf16(virtualPath));
    if (virtualPath.find(L"AppData\\Roaming") != std::wstring::npos) {
        try {
            auto roamingPath = ApplicationData::Current().RoamingFolder().Path();
            std::filesystem::path p(virtualPath);
            std::wstring fileName = p.filename().wstring();
            
            // 尝试在 RoamingFolder 下匹配路径
            std::wstring realPath = std::wstring(roamingPath.c_str()) + L"\\thumbnail_test\\" + fileName;
            if (!std::filesystem::exists(realPath)) {
                realPath = std::wstring(roamingPath.c_str()) + L"\\" + fileName;
            }
            WriteLog("转换后的物理路径: " + Utf8FromUtf16(realPath));
            return realPath;
        } catch (...) {
            WriteLog("WinRT 获取沙盒路径失败");
        }
    }
    return virtualPath;
}

// --- 3. 核心提取函数 ---

std::string SaveThumbnail(PCWSTR srcFile, PCWSTR destFile, int size, REFGUID type) {
    if (!std::filesystem::exists(srcFile)) return "源文件物理不存在";

    IShellItem* pSI = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(srcFile, NULL, IID_PPV_ARGS(&pSI));
    if (FAILED(hr)) return "SHCreateItemFromParsingName 失败: " + HRESULTToString(hr);

    IThumbnailCache* pThumbCache = nullptr;
    hr = CoCreateInstance(CLSID_LocalThumbnailCache, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pThumbCache));
    if (FAILED(hr)) {
        pSI->Release();
        return "CoCreateInstance 失败: " + HRESULTToString(hr);
    }

    ISharedBitmap* pSharedBitmap = nullptr;
    hr = pThumbCache->GetThumbnail(pSI, size, WTS_EXTRACT | WTS_FORCEEXTRACTION | WTS_SCALETOREQUESTEDSIZE, &pSharedBitmap, NULL, NULL);

    if (FAILED(hr) || !pSharedBitmap) {
        pSI->Release(); pThumbCache->Release();
        return "GetThumbnail 失败: " + HRESULTToString(hr);
    }

    HBITMAP hBitmap = NULL;
    hr = pSharedBitmap->GetSharedBitmap(&hBitmap);
    if (SUCCEEDED(hr) && hBitmap) {
        CImage image;
        image.Attach(hBitmap);
        std::filesystem::create_directories(std::filesystem::path(destFile).parent_path());
        hr = image.Save(destFile, type);
        image.Detach();
    }

    if (pSI) pSI->Release();
    if (pSharedBitmap) pSharedBitmap->Release();
    if (pThumbCache) pThumbCache->Release();

    return SUCCEEDED(hr) ? "" : ("保存失败: " + HRESULTToString(hr));
}

// --- 4. 插件入口与 Flutter 交互 ---

void FcNativeVideoThumbnailPlugin::RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar) {
    auto channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
        registrar->messenger(), "fc_native_video_thumbnail",
        &flutter::StandardMethodCodec::GetInstance());

    auto plugin = std::make_unique<FcNativeVideoThumbnailPlugin>();

    channel->SetMethodCallHandler(
        [plugin_pointer = plugin.get()](const auto& call, auto result) {
            plugin_pointer->HandleMethodCall(call, std::move(result));
        });

    registrar->AddPlugin(std::move(plugin));
}

FcNativeVideoThumbnailPlugin::FcNativeVideoThumbnailPlugin() {}
FcNativeVideoThumbnailPlugin::~FcNativeVideoThumbnailPlugin() {}

void FcNativeVideoThumbnailPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    if (method_call.method_name().compare("getVideoThumbnail") == 0) {
        const auto* argsPtr = std::get_if<flutter::EncodableMap>(method_call.arguments());
        if (!argsPtr) {
            result->Error("InvalidArgs", "Args must be a map");
            return;
        }
        auto& args = *argsPtr;

        try {
            std::string src = std::get<std::string>(*ValueOrNull(args, "srcFile"));
            std::string dest = std::get<std::string>(*ValueOrNull(args, "destFile"));
            int width = std::get<int>(*ValueOrNull(args, "width"));
            std::string format = std::get<std::string>(*ValueOrNull(args, "format"));

            WriteLog("--- 新请求 ---");
            std::wstring wSrc = ResolvePhysicalPath(Utf16FromUtf8(src), "源文件");
            std::wstring wDest = ResolvePhysicalPath(Utf16FromUtf8(dest), "目标文件");

            std::string err = SaveThumbnail(wSrc.c_str(), wDest.c_str(), width, 
                (format == "png" ? Gdiplus::ImageFormatPNG : Gdiplus::ImageFormatJPEG));

            if (err.empty()) {
                WriteLog("成功");
                result->Success(flutter::EncodableValue(true));
            } else {
                WriteLog("失败: " + err);
                result->Success(flutter::EncodableValue(false));
            }
        } catch (const std::exception& e) {
            result->Error("Exception", e.what());
        }
    } else {
        result->NotImplemented();
    }
}

} // namespace fc_native_video_thumbnail