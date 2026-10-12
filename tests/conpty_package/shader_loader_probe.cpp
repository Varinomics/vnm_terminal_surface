#include <conpty-shared-shaders.h>
#include <d3d11.h>
#include <cstdio>
#include <string_view>

#pragma comment(lib, "d3d11.lib")

int main(int argc, char** argv)
{
    try {
        if (argc == 3) {
            const auto index = static_cast<size_t>(argv[2][0] - '0');
            if (index >= vnm_conpty::shader_resources.size()) {
                return 2;
            }
            try {
                vnm_conpty::load_builtin_shader(static_cast<vnm_conpty::Builtin_shader>(index));
            } catch (const wil::ResultException& error) {
                const auto expected = std::string_view{argv[1]} == "missing"
                    ? HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
                    : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                return error.GetErrorCode() == expected ? 0 : 3;
            }
            return 4;
        }

        wil::com_ptr<ID3D11Device> device;
        THROW_IF_FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, nullptr));
        for (size_t index = 0; index < vnm_conpty::shader_resources.size(); ++index) {
            const auto blob = vnm_conpty::load_builtin_shader(
                static_cast<vnm_conpty::Builtin_shader>(index));
            if (index % 2 == 0) {
                wil::com_ptr<ID3D11VertexShader> shader;
                THROW_IF_FAILED(device->CreateVertexShader(blob->GetBufferPointer(),
                    blob->GetBufferSize(), nullptr, shader.put()));
                if (index == 0) {
                    const D3D11_INPUT_ELEMENT_DESC layout[]{
                        {"SV_Position", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                        {"shadingType", 0, DXGI_FORMAT_R16_UINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                        {"renditionScale", 0, DXGI_FORMAT_R8G8_UINT, 1, 2, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                        {"position", 0, DXGI_FORMAT_R16G16_SINT, 1, 4, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                        {"size", 0, DXGI_FORMAT_R16G16_UINT, 1, 8, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                        {"texcoord", 0, DXGI_FORMAT_R16G16_UINT, 1, 12, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                        {"color", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                    };
                    wil::com_ptr<ID3D11InputLayout> input_layout;
                    THROW_IF_FAILED(device->CreateInputLayout(layout, static_cast<UINT>(std::size(layout)),
                        blob->GetBufferPointer(), blob->GetBufferSize(), input_layout.put()));
                }
            } else {
                wil::com_ptr<ID3D11PixelShader> shader;
                THROW_IF_FAILED(device->CreatePixelShader(blob->GetBufferPointer(),
                    blob->GetBufferSize(), nullptr, shader.put()));
            }
        }
        std::puts("All four shared shaders and the vertex input layout loaded on D3D11 WARP.");
        return 0;
    } catch (const wil::ResultException& error) {
        std::fprintf(stderr, "Shader loader failed: 0x%08lx\n", error.GetErrorCode());
        return 1;
    }
}
