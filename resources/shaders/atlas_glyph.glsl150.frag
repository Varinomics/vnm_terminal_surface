#version 150

uniform sampler2DArray coverage_atlas;

in vec2 fragment_uv;
in vec4 fragment_color;
in vec4 fragment_atlas_info;
layout(location = 0) out vec4 output_color;
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
        output_color = vec4(fragment_color.rgb, 1.0);
        output_blend_factor = vec4(coverage);
    }
    else if (kind < 2.5) {
        vec3 subpixel_coverage = texel.rgb * inherited_alpha;
        float combined_coverage = texel.a * inherited_alpha;
        output_color = vec4(fragment_color.rgb, 1.0);
        output_blend_factor = vec4(subpixel_coverage, combined_coverage);
    }
    else {
        float coverage = texel.a * inherited_alpha;
        output_color = vec4(texel.rgb, 1.0);
        output_blend_factor = vec4(coverage);
    }
}
