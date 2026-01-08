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

using namespace winrt::Windows::Storage;
namespace fs = std::filesystem;

namespace fc_native_video_thumbnail {

    // --- 1. 辅助工具函数 (必须在调用前定义) ---

    // 宽字符转 UTF-8 字符串 (用于日志)
    std::string WToS(const std::wstring& wstr) {
        if (wstr.empty()) return "";
        int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
        std::string out(size, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &out[0], size, NULL, NULL);
        return out;
    }

    // UTF-8 字符串转宽字符 (用于 Win32 API)
    std::wstring Utf8ToWString(const std::string& str) {
        if (str.empty()) return L"";
        int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
        std::wstring out(size, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &out[0], size);
        return out;
    }

    // 日志记录系统
    void WriteLog(const std::string& message) {
        try {
            std::wstring logPath = L"plugin_debug.log";
            try {
                // 尝试将日志写在沙盒的 LocalFolder 下
                logPath = std::wstring(ApplicationData::Current().LocalFolder().Path().c_str()) + L"\\plugin_debug.log";
            }
            catch (...) {}

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

    // --- 2. 核心路径解析：解决虚拟路径与 LocalCache 物理路径的对应 ---

    // 专门用于源文件（必须存在）
    // 专门用于源文件（必须存在）
    std::wstring ResolvePhysicalPathForSource(const std::wstring& virtualPath) {
        WriteLog("解析源文件路径: " + WToS(virtualPath));

        // 如果已经是物理路径（包含 \Packages\）
        if (virtualPath.find(L"\\Packages\\") != std::wstring::npos) {
            if (fs::exists(virtualPath)) {
                WriteLog("已是物理路径且存在");
                return virtualPath;
            }
        }

        try {
            std::wstring localCacheRoot = ApplicationData::Current().LocalCacheFolder().Path().c_str();
            std::wstring roamingRoot = ApplicationData::Current().RoamingFolder().Path().c_str();

            WriteLog("LocalCache 根: " + WToS(localCacheRoot));
            WriteLog("Roaming 根: " + WToS(roamingRoot));

            std::wstring keyRoaming = L"\\AppData\\Roaming\\";
            size_t pos = virtualPath.find(keyRoaming);

            if (pos != std::wstring::npos) {
                // 找到 AppData\Roaming\ 之后的所有内容（包括 com.example）
                size_t offset = pos + keyRoaming.length();
                std::wstring relativePathWithPackage = virtualPath.substr(offset);

                WriteLog("提取相对路径（含包名）: " + WToS(relativePathWithPackage));

                // 策略 1: LocalCache\Roaming + 完整相对路径（含包名）
                std::wstring pathLocalCache = localCacheRoot + L"\\Roaming\\" + relativePathWithPackage;
                WriteLog("尝试路径 1 (LocalCache\\Roaming + 包名): " + WToS(pathLocalCache));

                if (fs::exists(pathLocalCache)) {
                    WriteLog("[OK] 源文件找到: LocalCache\\Roaming (含包名)");
                    return pathLocalCache;
                }

                // 策略 2: 跳过包名再试一次（以防万一）
                size_t firstSlash = relativePathWithPackage.find(L'\\');
                if (firstSlash != std::wstring::npos) {
                    std::wstring relativePathNoPackage = relativePathWithPackage.substr(firstSlash);
                    WriteLog("提取相对路径（不含包名）: " + WToS(relativePathNoPackage));

                    std::wstring pathNoPackage = localCacheRoot + L"\\Roaming" + relativePathNoPackage;
                    WriteLog("尝试路径 2 (LocalCache\\Roaming 无包名): " + WToS(pathNoPackage));

                    if (fs::exists(pathNoPackage)) {
                        WriteLog("[OK] 源文件找到: LocalCache\\Roaming (无包名)");
                        return pathNoPackage;
                    }
                }

                // 策略 3: RoamingState + 完整相对路径
                std::wstring pathRoamingState = roamingRoot + L"\\" + relativePathWithPackage;
                WriteLog("尝试路径 3 (RoamingState): " + WToS(pathRoamingState));

                if (fs::exists(pathRoamingState)) {
                    WriteLog("[OK] 源文件找到: RoamingState");
                    return pathRoamingState;
                }

                WriteLog("[FAIL] 源文件未找到，返回默认路径");
                return pathLocalCache;
            }
        }
        catch (const std::exception& e) {
            WriteLog("源文件路径解析异常: " + std::string(e.what()));
        }
        catch (...) {
            WriteLog("源文件路径解析失败");
        }

        WriteLog("[WARN] 无法解析源文件路径，返回原路径");
        return virtualPath;
    }

    // 专门用于目标文件（可能不存在，需要创建）
    std::wstring ResolvePhysicalPathForDest(const std::wstring& virtualPath) {
        WriteLog("解析目标文件路径: " + WToS(virtualPath));

        // 如果已经是物理路径
        if (virtualPath.find(L"\\Packages\\") != std::wstring::npos) {
            WriteLog("已是物理路径");
            return virtualPath;
        }

        try {
            std::wstring localCacheRoot = ApplicationData::Current().LocalCacheFolder().Path().c_str();

            std::wstring keyRoaming = L"\\AppData\\Roaming\\";
            size_t pos = virtualPath.find(keyRoaming);

            if (pos != std::wstring::npos) {
                // 提取完整相对路径（包括 com.example）
                size_t offset = pos + keyRoaming.length();
                std::wstring relativePathWithPackage = virtualPath.substr(offset);

                // 目标文件也放在 LocalCache\Roaming\com.example\...
                std::wstring pathLocalCache = localCacheRoot + L"\\Roaming\\" + relativePathWithPackage;
                WriteLog("目标路径（含包名）: " + WToS(pathLocalCache));
                return pathLocalCache;
            }
        }
        catch (const std::exception& e) {
            WriteLog("目标文件路径解析异常: " + std::string(e.what()));
        }
        catch (...) {
            WriteLog("目标文件路径解析失败");
        }

        WriteLog("[WARN] 无法解析目标文件路径，返回原路径");
        return virtualPath;
    }
    // --- 3. 核心提取与保存逻辑 ---

    std::string SaveThumbnail(std::wstring src, std::wstring dest, int size, REFGUID type) {
        // A. 解决"创建图片失败"：强行在物理路径创建文件夹
        try {
            fs::path d(dest);
            fs::path parent = d.parent_path();
            if (!fs::exists(parent)) {
                fs::create_directories(parent);
                WriteLog("物理目录不存在，已强行创建: " + parent.string());
            }
        }
        catch (const std::exception& e) {
            return "无法创建物理目录: " + std::string(e.what());
        }

        if (!fs::exists(src)) {
            WriteLog("  错误：源文件物理路径不存在: " + WToS(src));
            return "源文件物理不存在";
        }

        WriteLog("真实源目录: " + fs::path(src).string());
        WriteLog("保存地址: " + fs::path(dest).string());

        // B. 使用 Shell 接口提取缩略图
        Microsoft::WRL::ComPtr<IShellItemImageFactory> pFactory;
        HRESULT hr = SHCreateItemFromParsingName(src.c_str(), nullptr, IID_PPV_ARGS(&pFactory));

        if (FAILED(hr)) {
            std::stringstream ss;
            ss << "无法打开源文件句柄 (HRESULT: 0x" << std::hex << hr << ")";
            WriteLog("  " + ss.str());
            return ss.str();
        }

        HBITMAP hBitmap = NULL;
        // 使用 SIIGBF_THUMBNAILONLY 标志
        hr = pFactory->GetImage({ (LONG)size, (LONG)size }, SIIGBF_THUMBNAILONLY, &hBitmap);

        if (SUCCEEDED(hr) && hBitmap) {
            CImage image;
            image.Attach(hBitmap);
            // 保存到我们转换后的物理路径
            hr = image.Save(dest.c_str(), type);
            image.Detach();
            DeleteObject(hBitmap);

            if (SUCCEEDED(hr)) {
                WriteLog("  缩略图保存成功");
                return "";
            }
            else {
                std::stringstream ss;
                ss << "GDI+ 图片保存失败 (HRESULT: 0x" << std::hex << hr << ")";
                WriteLog("  " + ss.str());
                return ss.str();
            }
        }
        else {
            std::stringstream ss;
            ss << "提取帧失败 (HRESULT: 0x" << std::hex << hr << ")";
            WriteLog("  " + ss.str());
            return ss.str();
        }
    }

    // --- 4. 插件注册与 MethodChannel 处理 ---

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

    void FcNativeVideoThumbnailPlugin::HandleMethodCall(
            const flutter::MethodCall<flutter::EncodableValue>& call,
            std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

        if (call.method_name().compare("getVideoThumbnail") == 0) {
            const auto* args_ptr = std::get_if<flutter::EncodableMap>(call.arguments());
            if (!args_ptr) {
                result->Error("InvalidArgs", "Arguments must be a Map");
                return;
            }
            auto& args = *args_ptr;

            try {
                // 获取 Dart 传参
                std::string src = std::get<std::string>(args.at(flutter::EncodableValue("srcFile")));
                std::string dest = std::get<std::string>(args.at(flutter::EncodableValue("destFile")));
                int width = std::get<int>(args.at(flutter::EncodableValue("width")));
                std::string format = std::get<std::string>(args.at(flutter::EncodableValue("format")));

                WriteLog("========================================");
                WriteLog("=== 新提取请求 ===");
                WriteLog("Dart 传入源路径: " + src);
                WriteLog("Dart 传入目标路径: " + dest);

                // 🔧 关键修改：使用专门的解析函数
                std::wstring wSrc = ResolvePhysicalPathForSource(Utf8ToWString(src));
                std::wstring wDest = ResolvePhysicalPathForDest(Utf8ToWString(dest));

                WriteLog("解析后源路径: " + WToS(wSrc));
                WriteLog("解析后目标路径: " + WToS(wDest));

                // 验证源文件
                if (!fs::exists(wSrc)) {
                    WriteLog("  错误：源文件不存在");
                    WriteLog("========================================");
                    result->Error("FileNotFound", "Source file does not exist: " + WToS(wSrc));
                    return;
                }

                std::string err = SaveThumbnail(wSrc, wDest, width,
                        (format == "png" ? Gdiplus::ImageFormatPNG : Gdiplus::ImageFormatJPEG));

                if (err.empty()) {
                    WriteLog("  缩略图提取成功");
                    WriteLog("========================================");
                    result->Success(flutter::EncodableValue(true));
                }
                else {
                    WriteLog("  提取失败: " + err);
                    WriteLog("========================================");
                    result->Success(flutter::EncodableValue(false));
                }
            }
            catch (const std::exception& e) {
                WriteLog("  异常: " + std::string(e.what()));
                WriteLog("========================================");
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