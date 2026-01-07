#include "fc_native_video_thumbnail_plugin.h"
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <memory>
#include <string>
#include <fstream>
#include <windows.h>
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

    void FcNativeVideoThumbnailPlugin::RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar) {
        auto channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
            registrar->messenger(), "fc_native_video_thumbnail",
            &flutter::StandardMethodCodec::GetInstance());
        auto plugin = std::make_unique<FcNativeVideoThumbnailPlugin>();
        channel->SetMethodCallHandler([plugin_pointer = plugin.get()](const auto& call, auto result) {
            plugin_pointer->HandleMethodCall(call, std::move(result));
            });
        registrar->AddPlugin(std::move(plugin));
    }

    FcNativeVideoThumbnailPlugin::FcNativeVideoThumbnailPlugin() {
        try { init_apartment(); }
        catch (...) {}
    }

    FcNativeVideoThumbnailPlugin::~FcNativeVideoThumbnailPlugin() {}

    void FcNativeVideoThumbnailPlugin::HandleMethodCall(
        const flutter::MethodCall<flutter::EncodableValue>& method_call,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

        if (method_call.method_name().compare("getVideoThumbnail") == 0) {
            const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());

            std::string src = std::get<std::string>(args->at(flutter::EncodableValue("srcFile")));
            std::string dest = std::get<std::string>(args->at(flutter::EncodableValue("destFile")));
            int w = std::get<int>(args->at(flutter::EncodableValue("width")));
            int h = std::get<int>(args->at(flutter::EncodableValue("height")));

            try {
                auto file = StorageFile::GetFileFromPathAsync(Utf8ToWide(src)).get();
                uint32_t size = static_cast<uint32_t>((w > h) ? w : h);

                // Get thumbnail from WinRT
                auto thumb = file.GetThumbnailAsync(ThumbnailMode::VideosView, size).get();
                if (thumb) {
                    // Read to buffer
                    uint32_t tSize = static_cast<uint32_t>(thumb.Size());
                    Buffer buffer(tSize);
                    thumb.ReadAsync(buffer, tSize, InputStreamOptions::None).get();

                    // Write to physical file
                    std::ofstream ofs(Utf8ToWide(dest), std::ios::binary);
                    if (ofs.is_open()) {
                        ofs.write(reinterpret_cast<const char*>(buffer.data()), buffer.Length());
                        ofs.close();
                        result->Success(flutter::EncodableValue(true)); // Return true on success
                    }
                    else {
                        result->Error("IO_ERROR", "Could not open destFile for writing");
                    }
                }
                else {
                    result->Error("NULL_THUMB", "WinRT failed to generate thumbnail");
                }
            }
            catch (const winrt::hresult_error& e) {
                result->Error("WINRT_ERR", std::to_string(e.code()));
            }
            catch (...) {
                result->Error("ERR", "Unknown error");
            }
        }
        else {
            result->NotImplemented();
        }
    }
}