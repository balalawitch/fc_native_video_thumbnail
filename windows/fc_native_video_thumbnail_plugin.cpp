#include "fc_native_video_thumbnail_plugin.h"

// 1. 系统与 COM 头文件
#include <windows.h>
#include <wrl/client.h>
#include <atlimage.h>
#include <comdef.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <gdiplus.h>

// 2. Flutter & WinRT
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <winrt/Windows.Storage.h>

// 3. C++ 标准库
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <sstream>
#include <string>
#include <algorithm>
#include <cwctype>

using namespace winrt::Windows::Storage;
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace fc_native_video_thumbnail {

    // --- 1. RAII 辅助工具 ---

    // 自动管理 HBITMAP 释放
    struct BitmapGuard {
        HBITMAP hBmp;
        explicit BitmapGuard(HBITMAP h) : hBmp(h) {}
        ~BitmapGuard() { if (hBmp) DeleteObject(hBmp); }
        BitmapGuard(const BitmapGuard&) = delete;
        BitmapGuard& operator=(const BitmapGuard&) = delete;
    };

    // --- 2. 字符串与路径辅助函数 ---

    // 宽字符转 UTF-8 (修正：排除 Null 终止符)
    std::string WToS(const std::wstring& wstr) {
        if (wstr.empty()) return "";
        int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
        if (size <= 1) return "";
        std::string out(size - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &out[0], size, NULL, NULL);
        return out;
    }

    // UTF-8 转宽字符
    std::wstring Utf8ToWString(const std::string& str) {
        if (str.empty()) return L"";
        int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
        if (size <= 1) return L"";
        std::wstring out(size - 1, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &out[0], size);
        return out;
    }

    // 日志记录系统 (增加回退路径)
    void WriteLog(const std::string& message) {
        try {
            std::wstring logPath;
            try {
                logPath = std::wstring(ApplicationData::Current().LocalFolder().Path().c_str()) + L"\\plugin_debug.log";
            }
            catch (...) {
                wchar_t tmpPath[MAX_PATH];
                if (GetTempPathW(MAX_PATH, tmpPath)) logPath = std::wstring(tmpPath) + L"plugin_debug.log";
            }

            if (logPath.empty()) return;
            std::ofstream logFile(logPath, std::ios::app);
            if (logFile.is_open()) {
                auto now = std::chrono::system_clock::now();
                auto t = std::chrono::system_clock::to_time_t(now);
                struct tm buf;
                localtime_s(&buf, &t);
                logFile << std::put_time(&buf, "%H:%M:%S") << " [LOG] " << message << std::endl;
            }
        }
        catch (...) {}
    }

    // 生成长路径前缀
    std::wstring MakeLongPath(const std::wstring& path) {
        if (path.find(L"\\\\?\\") == 0) return path;
        if (path.find(L"\\\\") == 0) return L"\\\\?\\UNC\\" + path.substr(2);
        return L"\\\\?\\" + path;
    }

    // 安全移除长路径前缀以兼容不支持 \\?\ 的 API
    std::wstring RemoveLongPathPrefix(const std::wstring& path) {
        if (path.find(L"\\\\?\\UNC\\") == 0) return L"\\\\" + path.substr(8);
        if (path.find(L"\\\\?\\") == 0) return path.substr(4);
        return path;
    }

    // 大小写不敏感查找子字符串
    size_t FindCaseInsensitive(const std::wstring& haystack, const std::wstring& needle) {
        auto it = std::search(
                haystack.begin(), haystack.end(),
                needle.begin(), needle.end(),
                [](wchar_t ch1, wchar_t ch2) { return ::towupper(ch1) == ::towupper(ch2); }
        );
        return (it == haystack.end()) ? std::wstring::npos : std::distance(haystack.begin(), it);
    }

    // --- 3. 核心路径解析逻辑 (组合策略) ---

    std::wstring ResolvePhysicalPathForSource(const std::wstring& virtualPath) {
        WriteLog("Parsing source: " + WToS(virtualPath));
        WriteLog("  Path length: " + std::to_string(virtualPath.length()));

        // 策略1: 检查是否是已映射的MSIX物理路径（包含 \Packages\）
        if (virtualPath.find(L"\\Packages\\") != std::wstring::npos) {
            WriteLog("[INFO] Path contains \\Packages\\, treating as MSIX physical path");
            // 即使不存在也返回，让后续SaveThumbnail报错
            return virtualPath;
        }

        // 策略2: 尝试MSIX沙盒虚拟路径映射（包含 \AppData\Roaming\ 或 \AppData\Local\）
        // 必须在直接路径检查之前，因为MSIX环境下fs::exists可能返回true但Shell API不支持虚拟路径
        // 使用大小写不敏感查找，因为Windows路径可能是小写的
        try {
            std::wstring keyRoaming = L"\\AppData\\Roaming\\";
            std::wstring keyLocal = L"\\AppData\\Local\\";
            size_t posRoaming = FindCaseInsensitive(virtualPath, keyRoaming);
            size_t posLocal = FindCaseInsensitive(virtualPath, keyLocal);

            if (posRoaming != std::wstring::npos) {
                WriteLog("[INFO] Path contains \\AppData\\Roaming\\, trying MSIX sandbox mapping");

                std::wstring localCacheRoot = ApplicationData::Current().LocalCacheFolder().Path().c_str();
                std::wstring roamingRoot = ApplicationData::Current().RoamingFolder().Path().c_str();
                std::wstring relativePath = virtualPath.substr(posRoaming + keyRoaming.length());

                WriteLog("  LocalCache root: " + WToS(localCacheRoot));
                WriteLog("  Roaming root: " + WToS(roamingRoot));
                WriteLog("  Relative path: " + WToS(relativePath));

                // 策略 2A: LocalCache\Roaming (主要策略)
                std::wstring pathA = localCacheRoot + L"\\Roaming\\" + relativePath;
                WriteLog("  Trying LocalCache\\Roaming: " + WToS(pathA));
                if (fs::exists(MakeLongPath(pathA))) {
                    WriteLog("[OK] Found via LocalCache\\Roaming mapping");
                    return pathA;
                }

                // 策略 2B: RoamingState (备用策略)
                std::wstring pathB = roamingRoot + L"\\" + relativePath;
                WriteLog("  Trying RoamingState: " + WToS(pathB));
                if (fs::exists(MakeLongPath(pathB))) {
                    WriteLog("[OK] Found via RoamingState mapping");
                    return pathB;
                }

                WriteLog("  MSIX Roaming mapping failed: file not found in sandbox");
            }
            else if (posLocal != std::wstring::npos) {
                WriteLog("[INFO] Path contains \\AppData\\Local\\, trying MSIX sandbox mapping");

                std::wstring localCacheRoot = ApplicationData::Current().LocalCacheFolder().Path().c_str();
                std::wstring relativePath = virtualPath.substr(posLocal + keyLocal.length());

                WriteLog("  LocalCache root: " + WToS(localCacheRoot));
                WriteLog("  Relative path: " + WToS(relativePath));

                // 策略 2C: LocalCache (Local路径映射)
                std::wstring pathC = localCacheRoot + L"\\" + relativePath;
                WriteLog("  Trying LocalCache: " + WToS(pathC));
                if (fs::exists(MakeLongPath(pathC))) {
                    WriteLog("[OK] Found via LocalCache mapping");
                    return pathC;
                }

                WriteLog("  MSIX Local mapping failed: file not found in sandbox");
            }
        }
        catch (const std::exception& e) {
            WriteLog("  MSIX mapping error: " + std::string(e.what()));
        }

        // 策略3: 尝试直接使用原路径（处理真实路径：D:\, 网络路径等）
        // 这个策略放在最后，避免MSIX虚拟路径被误判为真实路径
        try {
            std::wstring longPath = MakeLongPath(virtualPath);
            if (fs::exists(longPath)) {
                WriteLog("[OK] File exists directly, using as-is (real path)");
                return virtualPath;
            }
            WriteLog("  Direct path check: file not found");
        }
        catch (const std::exception& e) {
            WriteLog("  Direct path check failed: " + std::string(e.what()));
        }

        // 策略4: 所有策略都失败
        WriteLog("[FAIL] Cannot resolve physical path, file not found");
        return L""; // 返回空，让调用者报告明确错误
    }

    std::wstring ResolvePhysicalPathForDest(const std::wstring& virtualPath) {
        if (virtualPath.find(L"\\Packages\\") != std::wstring::npos) return virtualPath;

        try {
            std::wstring roamingKey = L"\\AppData\\Roaming\\";
            std::wstring localKey = L"\\AppData\\Local\\";

            // 使用大小写不敏感查找
            size_t roamingPos = FindCaseInsensitive(virtualPath, roamingKey);
            size_t localPos = FindCaseInsensitive(virtualPath, localKey);

            if (roamingPos != std::wstring::npos) {
                // 处理 Roaming -> RoamingState 或 LocalCache\Roaming
                std::wstring localCacheRoot = ApplicationData::Current().LocalCacheFolder().Path().c_str();
                return localCacheRoot + L"\\Roaming\\" + virtualPath.substr(roamingPos + roamingKey.length());
            }
            else if (localPos != std::wstring::npos) {
                // 处理 Local -> LocalCache
                // Flutter 的路径通常包含包名，例如 AppData\Local\com.example\app...
                // 在 MSIX 中，这通常映射到 LocalCache 下的相对路径
                std::wstring localCacheRoot = ApplicationData::Current().LocalCacheFolder().Path().c_str();
                return localCacheRoot + L"\\" + virtualPath.substr(localPos + localKey.length());
            }
        }
        catch (...) {
            WriteLog("[WARN] Dest resolution failed, using virtual path.");
        }
        return virtualPath;
    }
    // --- 4. 核心提取与保存逻辑 ---

    std::string SaveThumbnail(std::wstring src, std::wstring dest, int size, REFGUID type) {
        // A. 准备目录
        try {
            std::wstring longDest = MakeLongPath(dest);
            fs::path parent = fs::path(longDest).parent_path();
            if (!parent.empty() && !fs::exists(parent)) fs::create_directories(parent);
        }
        catch (const std::exception& e) { return "Dir creation failed: " + std::string(e.what()); }

        // B. 准备 Shell API 兼容路径
        // SHCreateItemFromParsingName 不支持 \\?\ 前缀，除非路径长度确实超过 MAX_PATH 且开启了系统支持
        std::wstring shellSrc = (src.length() < MAX_PATH) ? RemoveLongPathPrefix(src) : src;

        ComPtr<IShellItemImageFactory> pFactory;
        HRESULT hr = SHCreateItemFromParsingName(shellSrc.c_str(), nullptr, IID_PPV_ARGS(&pFactory));

        // 如果失败且路径较长，尝试 8.3 短路径作为后备
        if (FAILED(hr) && src.length() >= MAX_PATH) {
            wchar_t shortBuf[MAX_PATH];
            if (GetShortPathNameW(src.c_str(), shortBuf, MAX_PATH) > 0) {
                hr = SHCreateItemFromParsingName(shortBuf, nullptr, IID_PPV_ARGS(&pFactory));
            }
        }

        if (FAILED(hr)) return "SHCreateItem failed (0x" + std::to_string(hr) + ")";

        HBITMAP hBitmapRaw = NULL;
        hr = pFactory->GetImage({ (LONG)size, (LONG)size }, SIIGBF_THUMBNAILONLY, &hBitmapRaw);
        if (FAILED(hr) || !hBitmapRaw) return "GetImage failed";

        // C. 使用 RAII 管理句柄
        BitmapGuard guard(hBitmapRaw);
        CImage image;
        image.Attach(hBitmapRaw);

        // D. 使用 IStream 保存
        ComPtr<IStream> pStream;
        hr = SHCreateStreamOnFileEx(MakeLongPath(dest).c_str(),
                STGM_CREATE | STGM_WRITE | STGM_SHARE_DENY_WRITE,
                FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &pStream);

        std::string errorMessage = "";
        if (SUCCEEDED(hr)) {
            hr = image.Save(pStream.Get(), type);
            if (FAILED(hr)) errorMessage = "Save failed (0x" + std::to_string(hr) + ")";
        }
        else {
            errorMessage = "Stream creation failed (0x" + std::to_string(hr) + ")";
        }

        // --- 关键点：尽早分离 ---
        // 无论 Save 成功与否，只要 Attach 了，就在逻辑结束处立刻 Detach
        image.Detach();

        // 现在返回是安全的，guard 析构时会负责唯一的 DeleteObject 调用
        return errorMessage;
    }

    // --- 5. Flutter 接口层 ---

    void FcNativeVideoThumbnailPlugin::RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar) {
        auto channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
                registrar->messenger(), "fc_native_video_thumbnail",
                        &flutter::StandardMethodCodec::GetInstance());

        auto plugin = std::make_unique<FcNativeVideoThumbnailPlugin>();
        channel->SetMethodCallHandler([p = plugin.get()](const auto& call, auto result) {
            p->HandleMethodCall(call, std::move(result));
        });
        registrar->AddPlugin(std::move(plugin));
    }

    void FcNativeVideoThumbnailPlugin::HandleMethodCall(
            const flutter::MethodCall<flutter::EncodableValue>& call,
            std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

        if (call.method_name().compare("getVideoThumbnail") == 0) {
            const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
            if (!args) { result->Error("InvalidArgs", "Map expected"); return; }

            try {
                std::string src = std::get<std::string>(args->at(flutter::EncodableValue("srcFile")));
                std::string dest = std::get<std::string>(args->at(flutter::EncodableValue("destFile")));
                int width = std::get<int>(args->at(flutter::EncodableValue("width")));
                std::string format = std::get<std::string>(args->at(flutter::EncodableValue("format")));

                WriteLog("--- Request: " + src + " ---");

                std::wstring wSrc = ResolvePhysicalPathForSource(Utf8ToWString(src));
                if (wSrc.empty()) {
                    result->Error("FileNotFound", "Could not locate physical file: " + src);
                    return;
                }

                std::wstring wDest = ResolvePhysicalPathForDest(Utf8ToWString(dest));
                std::string err = SaveThumbnail(wSrc, wDest, width,
                        (format == "png" ? Gdiplus::ImageFormatPNG : Gdiplus::ImageFormatJPEG));

                if (err.empty()) {
                    result->Success(flutter::EncodableValue(true));
                }
                else {
                    WriteLog("Error: " + err);
                    result->Success(flutter::EncodableValue(false));
                }
            }
            catch (const std::exception& e) {
                result->Error("Exception", e.what());
            }
        }
        else {
            result->NotImplemented();
        }
    }

    FcNativeVideoThumbnailPlugin::FcNativeVideoThumbnailPlugin() {}
    FcNativeVideoThumbnailPlugin::~FcNativeVideoThumbnailPlugin() {}

} // namespace fc_native_video_thumbnail