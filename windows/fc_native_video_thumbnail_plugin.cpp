#include "fc_native_video_thumbnail_plugin.h"

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <memory>
#include <string>
#include <vector>
#include <fstream>
#include <windows.h>

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

// 辅助函数：将 Flutter 传来的 UTF-8 字符串转换为 Windows 宽字符 (解决中文路径问题)
std::wstring Utf8ToWide(const std::string& utf8Str) {
    if (utf8Str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &utf8Str[0], (int)utf8Str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &utf8Str[0], (int)utf8Str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

// 辅助函数：将 WinRT 流保存到本地文件
void SaveBufferToFile(IBuffer const& buffer, std::wstring const& filePath) {
    std::ofstream file(filePath, std::ios::binary);
    if (file.is_open()) {
        file.write(reinterpret_cast<const char*>(buffer.data()), buffer.Length());
        file.close();
    }
}

void FcNativeVideoThumbnailPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows *registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "fc_native_video_thumbnail",
          &flutter::StandardMethodCodec::GetInstance());

  auto plugin = std::make_unique<FcNativeVideoThumbnailPlugin>();

  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto &call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  registrar->AddPlugin(std::move(plugin));
}

FcNativeVideoThumbnailPlugin::FcNativeVideoThumbnailPlugin() {
    // 初始化 WinRT 线程环境
    try {
        init_apartment(); 
    } catch (...) {}
}

FcNativeVideoThumbnailPlugin::~FcNativeVideoThumbnailPlugin() {}

void FcNativeVideoThumbnailPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  
  if (method_call.method_name().compare("getVideoThumbnail") == 0) {
    const auto* arguments = std::get_if<flutter::EncodableMap>(method_call.arguments());
    
    // 解析参数，注意类型安全
    std::string srcFile = std::get<std::string>(arguments->at(flutter::EncodableValue("srcFile")));
    std::string destFile = std::get<std::string>(arguments->at(flutter::EncodableValue("destFile")));
    int width = std::get<int>(arguments->at(flutter::EncodableValue("width")));
    int height = std::get<int>(arguments->at(flutter::EncodableValue("height")));
    
    // 正确转换 UTF-8 路径到宽字符
    std::wstring wSrcFile = Utf8ToWide(srcFile);
    std::wstring wDestFile = Utf8ToWide(destFile);

    try {
        // 1. 打开文件
        auto file = StorageFile::GetFileFromPathAsync(wSrcFile).get();
        
        // 2. 确定请求尺寸
        uint32_t requestedSize = static_cast<uint32_t>(max(width, height));
        
        // 3. 提取缩略图 (VideosView 模式)
        // 这一步会利用系统索引或实时解码，在沙盒内是合法的
        auto thumbnail = file.GetThumbnailAsync(ThumbnailMode::VideosView, requestedSize).get();
        
        if (thumbnail) {
            auto size = thumbnail.Size();
            Buffer buffer(size);
            thumbnail.ReadAsync(buffer, size, InputStreamOptions::None).get();
            
            // 4. 写入目标文件
            SaveBufferToFile(buffer, wDestFile);
            
            result->Success(flutter::EncodableValue(true));
        } else {
            result->Error("FAILED", "WinRT returned null thumbnail");
        }
    } catch (const winrt::hresult_error& ex) {
        // HRESULT 错误捕获（如：文件不存在或格式不支持）
        std::string msg = "WinRT Error: " + std::to_string(ex.code());
        result->Error("WINRT_ERROR", msg);
    } catch (...) {
        result->Error("UNKNOWN_ERROR", "Failed to process video thumbnail");
    }
    
  } else {
    result->NotImplemented();
  }
}

}  // namespace fc_native_video_thumbnail
