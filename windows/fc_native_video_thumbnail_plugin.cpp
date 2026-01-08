#include "fc_native_video_thumbnail_plugin.h"

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

// --- 1. 日志工具 ---

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

// --- 2. 辅助转换工具 ---

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

// --- 3. 核心：路径修复逻辑 ---

std::wstring ResolvePhysicalPath(const std::wstring& virtualPath, const std::string& label) {
    WriteLog("正在解析" + label + "路径: " + Utf8FromUtf16(virtualPath));
    
    // 如果是 MSIX 沙盒路径环境
    if (virtualPath.find(L"AppData\\Roaming") != std::wstring::npos) {
        try {
            auto roamingPath = ApplicationData::Current().RoamingFolder().Path();
            std::filesystem::path p(virtualPath);
            std::wstring fileName = p.filename().wstring();
            
            // 简单而鲁棒的策略：假设文件就在 RoamingFolder 的根目录或 thumbnail_test 子目录下
            // 你可以根据 Flutter 端的具体 p.join 逻辑微调这里
            std::wstring realPath = std::wstring(roamingPath.c_str()) + L"\\thumbnail_test\\" + fileName;
            
            // 如果不存在，尝试直接放在根目录
            if (!std::filesystem::exists(realPath)) {
                realPath = std::wstring(roamingPath.c_str()) + L"\\" + fileName;
            }

            WriteLog("物理路径转换成功: " + Utf8FromUtf16(realPath));
            return realPath;
        } catch (...) {
            WriteLog("WinRT 获取沙盒路径失败，维持原样");
        }
    }
    return virtualPath;
}

// --- 4. 提取逻辑 ---

std::string SaveThumbnail(PCWSTR srcFile, PCWSTR destFile, int size, REFGUID type) {
    HRESULT hr;
    
    // 检查源文件物理存在
    if (!std::filesystem::exists(srcFile)) {
        return "源文件在物理磁盘上不存在";
    }

    IShellItem* pSI = nullptr;
    hr = SHCreateItemFromParsingName(srcFile, NULL, IID_PPV_ARGS(&pSI));
    if (FAILED(hr)) {
        return "SHCreateItemFromParsingName 失败: " + HRESULTToString(hr);
    }

    IThumbnailCache* pThumbCache = nullptr;
    hr = CoCreateInstance(CLSID_LocalThumbnailCache, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pThumbCache));
    if (FAILED(hr)) {
        if (pSI) pSI->Release();
        return "创建 IThumbnailCache 失败: " + HRESULTToString(hr);
    }

    ISharedBitmap* pSharedBitmap = nullptr;
    // 使用 WTS_FORCEEXTRACTION 绕过可能损坏的沙盒缓存
    hr = pThumbCache->GetThumbnail(pSI, size, WTS_EXTRACT | WTS_FORCEEXTRACTION | WTS_SCALETOREQUESTEDSIZE, &pSharedBitmap, NULL, NULL);

    if (FAILED(hr) || !pSharedBitmap) {
        pSI->Release();
        pThumbCache->Release();
        if (hr == 0x8004b100) return "Failed extraction (视频格式可能不支持或文件损坏)";
        return "GetThumbnail 失败: " + HRESULTToString(hr);
    }

    HBITMAP hBitmap = NULL;
    hr = pSharedBitmap->GetSharedBitmap(&hBitmap);
    if (SUCCEEDED(hr) && hBitmap) {
        CImage image;
        image.Attach(hBitmap);
        
        // 自动创建目标目录
        std::filesystem::path dp(destFile);
        std::filesystem::create_directories(dp.parent_path());

        hr = image.Save(destFile, type);
        image.Detach();
        WriteLog("图片保存结果: " + HRESULTToString(hr));
    }

    if (pSI) pSI->Release();
    if (pSharedBitmap) pSharedBitmap->Release();
    if (pThumbCache) pThumbCache->Release();

    return SUCCEEDED(hr) ? "" : ("保存图片失败: " + HRESULTToString(hr));
}

// --- 5. 插件入口 ---

void FcNativeVideoThumbnailPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    
    if (method_call.method_name().compare("getVideoThumbnail") == 0) {
        const auto* argsPtr = std::get_if<flutter::EncodableMap>(method_call.arguments());
        auto& args = *argsPtr;

        std::string src = std::get<std::string>(args.at(flutter::EncodableValue("srcFile")));
        std::string dest = std::get<std::string>(args.at(flutter::EncodableValue("destFile")));
        int width = std::get<int>(args.at(flutter::EncodableValue("width")));
        std::string format = std::get<std::string>(args.at(flutter::EncodableValue("format")));

        WriteLog("--- 新请求 ---");
        
        std::wstring wSrc = ResolvePhysicalPath(Utf16FromUtf8(src), "源文件");
        std::wstring wDest = ResolvePhysicalPath(Utf16FromUtf8(dest), "目标文件");

        std::string err = SaveThumbnail(wSrc.c_str(), wDest.c_str(), width, (format == "png" ? Gdiplus::ImageFormatPNG : Gdiplus::ImageFormatJPEG));

        if (err.empty()) {
            WriteLog("成功完成");
            result->Success(flutter::EncodableValue(true));
        } else {
            WriteLog("最终失败: " + err);
            // 如果是视频解码失败，返回 Success(false) 避免 Dart 端抛出异常
            if (err.find("Failed extraction") != std::string::npos) {
                result->Success(flutter::EncodableValue(false));
            } else {
                result->Error("ThumbnailError", err);
            }
        }
    } else {
        result->NotImplemented();
    }
}

} // namespace fc_native_video_thumbnail