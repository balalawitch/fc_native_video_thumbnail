#include "fc_native_video_thumbnail_plugin.h"

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <windows.h>
#include <memory>
#include <string>
#include <fstream>
#include <thread>
#include <chrono>
#include <iomanip>
#include <iostream>

// WinRT 核心头文件
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt;
using namespace Windows::Storage;
using namespace Windows::Storage::FileProperties;
using namespace Windows::Storage::Streams;

namespace fc_native_video_thumbnail {

// --- 辅助工具：日志记录系统 ---
// 自动识别 MSIX 路径或普通 Win32 路径
void WriteLog(const std::string& message) {
    try {
        std::wstring logPath;
        try {
            // 尝试获取 MSIX 的 LocalFolder 路径
            auto localFolder = ApplicationData::Current().LocalFolder().Path();
            logPath = std::wstring(localFolder.c_str()) + L"\\plugin_debug.log";
        } catch (...) {
            // 如果不是 MSIX 环境（如直接在 VS 中 F5 调试），则写在 exe 同级目录
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

// UTF-8 转 宽字符 (Windows 路径必备)
std::wstring Utf8ToWide(const std::string& utf8Str) {
    if (utf8Str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, &utf8Str[0], (int)utf8Str.size(), NULL, 0);
    std::wstring wstr(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, &utf8Str[0], (int)utf8Str.size(), &wstr[0], size);
    return wstr;
}

void FcNativeVideoThumbnailPlugin::RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar) {
    auto channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
        registrar->messenger(),
        "fc_native_video_thumbnail",
        &flutter::StandardMethodCodec::GetInstance()); // 必须是大驼峰：StandardMethodCodec

    auto plugin = std::make_unique<FcNativeVideoThumbnailPlugin>();

    channel->SetMethodCallHandler([plugin_pointer = plugin.get()](const auto& call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
        });

    registrar->AddPlugin(std::move(plugin));
}

FcNativeVideoThumbnailPlugin::FcNativeVideoThumbnailPlugin() {}
FcNativeVideoThumbnailPlugin::~FcNativeVideoThumbnailPlugin() {}

void FcNativeVideoThumbnailPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    if (method_call.method_name().compare("getVideoThumbnail") == 0) {
        const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
        
        std::string src = std::get<std::string>(args->at(flutter::EncodableValue("srcFile")));
        std::string dest = std::get<std::string>(args->at(flutter::EncodableValue("destFile")));
        int w = std::get<int>(args->at(flutter::EncodableValue("width")));
        int h = std::get<int>(args->at(flutter::EncodableValue("height")));

        WriteLog("--- 新请求 ---");
        WriteLog("源文件: " + src);
        WriteLog("目标文件: " + dest);

        // 使用 shared_ptr 包装 result 以便在 Lambda 线程中安全传递
        std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>> shared_result = std::move(result);

        std::thread([src, dest, w, h, shared_result]() {
            std::string step = "准备开始";
            try {
                // 1. 初始化 COM MTA 环境
                step = "初始化 COM MTA";
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                WriteLog("成功: " + step);

                // 2. 获取文件对象
                step = "获取文件对象: " + src;
                auto file = StorageFile::GetFileFromPathAsync(Utf8ToWide(src)).get();
                WriteLog("成功: 已找到文件");

                // 3. 提取缩略图
                step = "提取缩略图 (WinRT GetThumbnailAsync)";
                uint32_t size = static_cast<uint32_t>((w > h) ? w : h);
                auto thumb = file.GetThumbnailAsync(ThumbnailMode::VideosView, size).get();
                
                if (thumb) {
                    WriteLog("成功: 缩略图生成完毕, 大小: " + std::to_string(thumb.Size()));

                    // 4. 将数据读入内存 Buffer
                    step = "读取缩略图数据流";
                    uint32_t tSize = static_cast<uint32_t>(thumb.Size());
                    Buffer buffer(tSize);
                    thumb.ReadAsync(buffer, tSize, InputStreamOptions::None).get();

                    // 5. 写入目标磁盘路径
                    step = "写入目标文件: " + dest;
                    std::ofstream ofs(Utf8ToWide(dest), std::ios::binary);
                    if (ofs.is_open()) {
                        ofs.write(reinterpret_cast<const char*>(buffer.data()), buffer.Length());
                        ofs.close();
                        WriteLog("成功: 任务圆满完成");
                        shared_result->Success(flutter::EncodableValue(true));
                    } else {
                        WriteLog("失败: 目标路径无法写入 (权限或路径不存在)");
                        shared_result->Error("IO_ERROR", "Cannot open dest for writing");
                    }
                } else {
                    WriteLog("失败: WinRT 返回了空的缩略图");
                    shared_result->Error("NULL_THUMB", "WinRT returned null");
                }
            } catch (const winrt::hresult_error& e) {
                std::string errCode = std::to_string(e.code());
                WriteLog("WinRT 异常 [" + step + "]: HRESULT " + errCode);
                shared_result->Error("WINRT_ERR", "At step: " + step + ", Code: " + errCode);
            } catch (const std::exception& e) {
                WriteLog("标准异常: " + std::string(e.what()));
                shared_result->Error("STD_ERR", e.what());
            } catch (...) {
                WriteLog("未知错误在步骤: " + step);
                shared_result->Error("UNKNOWN_ERR", "Error at: " + step);
            }
        }).detach();

    } else {
        result->NotImplemented();
    }
}

} // namespace fc_native_video_thumbnail