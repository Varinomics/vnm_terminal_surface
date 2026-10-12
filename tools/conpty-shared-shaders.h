#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <d3dcompiler.h>
#include <array>
#include <filesystem>
#include <wil/com.h>
#include <wil/filesystem.h>
#include <wil/result.h>
#include <wil/stl.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(linker, "/include:vnm_conpty_shared_shader_contract")

// Retained layout metadata lets package admission distinguish these hosts from
// established vendor hosts even when their external receipt is missing.
extern "C" {
__declspec(selectany) extern const char vnm_conpty_shared_shader_contract[] = "VNM_CONPTY_SHARED_SHADERS_V1";
}

namespace vnm_conpty {

enum class Builtin_shader { shader_vs, shader_ps, custom_shader_vs, custom_shader_ps };

struct Shader_resource {
    const wchar_t*                filename;
    size_t                        size;
    std::array<unsigned char, 32> sha256;
};

} // namespace vnm_conpty

#include "conpty-shader-metadata.h"

namespace vnm_conpty {

inline wil::com_ptr<ID3DBlob> load_builtin_shader(Builtin_shader shader)
{
    const auto& resource = shader_resources.at(static_cast<size_t>(shader));
    const std::filesystem::path module{ wil::GetModuleFileNameW<std::wstring>(nullptr) };
    const auto path = module.parent_path().parent_path() / L"conpty-shaders" / resource.filename;
    wil::com_ptr<ID3DBlob> blob;
    THROW_IF_FAILED(D3DReadFileToBlob(path.c_str(), blob.put()));
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), blob->GetBufferSize() != resource.size);

    std::array<unsigned char, 32> digest{};
    THROW_IF_NTSTATUS_FAILED(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
        static_cast<PUCHAR>(blob->GetBufferPointer()), static_cast<ULONG>(resource.size),
        digest.data(), static_cast<ULONG>(digest.size())));
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), digest != resource.sha256);
    return blob;
}

} // namespace vnm_conpty
