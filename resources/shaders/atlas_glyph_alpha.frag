#version 440

layout(location = 0) in vec2 fragment_uv;
layout(location = 1) in vec4 fragment_color;
layout(location = 2) in vec4 fragment_atlas_info;

layout(binding = 1) uniform sampler2DArray coverage_atlas;

layout(location = 0) out vec4 output_color;

void main()
{
    vec4 texel = texture(
        coverage_atlas,
        vec3(fragment_uv, fragment_atlas_info.y));
    float kind = fragment_atlas_info.x;
    float inherited_alpha = fragment_color.a;

    if (kind < 2.5) {
        float coverage = texel.a * inherited_alpha;
        output_color = vec4(fragment_color.rgb, coverage);
    }
    else {
        float coverage = texel.a * inherited_alpha;
        output_color = vec4(texel.rgb, coverage);
    }
}
