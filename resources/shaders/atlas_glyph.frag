#version 440

layout(location = 0) in vec2 fragment_uv;
layout(location = 1) in vec4 fragment_color;
layout(location = 2) in vec4 fragment_atlas_info;

layout(binding = 1) uniform sampler2DArray coverage_atlas;

layout(std140, binding = 2) uniform invert_block
{
    float invert_brightness;
};

layout(location = 0) out vec4 output_color;

vec3 invert_hsv_value(vec3 color)
{
    float value = max(max(color.r, color.g), color.b);
    return value <= 0.0
        ? vec3(1.0)
        : color * ((1.0 - value) / value);
}

vec3 render_rgb(vec3 color)
{
    return invert_brightness > 0.5
        ? invert_hsv_value(color)
        : color;
}
layout(location = 0, index = 1) out vec4 output_blend_factor;

void main()
{
    vec4 texel = texture(
        coverage_atlas,
        vec3(fragment_uv, fragment_atlas_info.y));
    float kind = fragment_atlas_info.x;
    float inherited_alpha = fragment_color.a;

    if (kind < 0.5) {
        float coverage = texel.a * inherited_alpha;
        output_color = vec4(render_rgb(fragment_color.rgb), 1.0);
        output_blend_factor = vec4(coverage);
    }
    else if (kind < 2.5) {
        vec3 subpixel_coverage = texel.rgb * inherited_alpha;
        float combined_coverage = texel.a * inherited_alpha;
        output_color = vec4(render_rgb(fragment_color.rgb), 1.0);
        output_blend_factor = vec4(subpixel_coverage, combined_coverage);
    }
    else {
        float coverage = texel.a * inherited_alpha;
        output_color = vec4(render_rgb(texel.rgb), 1.0);
        output_blend_factor = vec4(coverage);
    }
}
