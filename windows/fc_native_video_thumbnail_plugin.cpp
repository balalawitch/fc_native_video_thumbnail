#include "fc_native_video_thumbnail_plugin.h"
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <memory>
#include <string>
#include <fstream>
#include <windows.h>
#include <thread> // 必须引入

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt;
using namespace Windows::Storage;
using namespace Windows::Storage::FileProperties;
using namespace Windows::Storage::Streams;

namespace fc_native_video_thumbnail {

// Helper: UTF-8 to WideChar
std::wstring Utf8ToWide(const std::string& utf8Str) {
    if (utf8Str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, &utf8Str[0], (int)utf8Str.size(), NULL, 0);
    std::wstring wstr(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, &utf8Str[0], (int)utf8Str.size(), &wstr[0], size);
    return wstr;
}

void FcNativeVideoThumbnailPlugin::RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar) {
    auto channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
        registrar->messenger(), "fc_native_video_thumbnail",
        &flutter::StandardMethodCodec::GetInstance());
    auto plugin = std::make_unique<FcNativeVideoThumbnailPlugin>();
    channel->SetMethodCallHandler([plugin_pointer = plugin.get()](const auto &call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
    });
    registrar->AddPlugin(std::move(plugin));
}

FcNativeVideoThumbnailPlugin::FcNativeVideoThumbnailPlugin() {
    // 插件初始化时不需要显式 init_apartment，因为每个线程需要独立处理
}

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

        // 核心修复：创建一个后台线程来处理 WinRT 请求
        // 使用 std::move(result) 将结果回调转移到新线程中
        std::thread([src, dest, w, h, result_ptr = std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>(std::move(result))]() {
            // 在新线程中初始化 COM 环境为多线程模式 (MTA)
            winrt::init_apartment(winrt::apartment_type::multi_threaded);

            try {
                auto file = StorageFile::GetFileFromPathAsync(Utf8ToWide(src)).get();
                uint32_t size = static_cast<uint32_t>((w > h) ? w : h);
                
                auto thumb = file.GetThumbnailAsync(ThumbnailMode::VideosView, size).get();
                if (thumb) {
                    uint32_t tSize = static_cast<uint32_t>(thumb.Size());
                    Buffer buffer(tSize);
                    thumb.ReadAsync(buffer, tSize, InputStreamOptions::None).get();

                    std::ofstream ofs(Utf8ToWide(dest), std::ios::binary);
                    if (ofs.is_open()) {
                        ofs.write(reinterpret_cast<const char*>(buffer.data()), buffer.Length());
                        ofs.close();
                        result_ptr->Success(flutter::EncodableValue(true));
                    } else {
                        result_ptr->Error("IO_ERROR", "Could not open destFile");
                    }
                } else {
                    result_ptr->Error("NULL_THUMB", "WinRT returned null");
                }
            } catch (const winrt::hresult_error& e) {
                result_ptr->Error("WINRT_ERR", std::to_string(e.code()));
            } catch (...) {
                result_ptr->Error("ERR", "Unknown error");
            }
            
            // 线程结束前会自动清理 COM 环境
        }).detach(); // 使用 detach 允许线程独立运行，不阻塞主线程

    } else {
        result->NotImplemented();
    }
}
}