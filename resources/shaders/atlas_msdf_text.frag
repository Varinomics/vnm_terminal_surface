#version 440

layout(std140, binding = 0) uniform msdf_text_block
{
    mat4  mvp;
    float px_range;
    float target_width;
    float target_height;
    float reserved0;
};

layout(binding = 1) uniform sampler2D msdf_atlas;

layout(location = 0) smooth in vec4 fragment_uv_rect;
layout(location = 1) smooth in vec4 fragment_color;
layout(location = 2) smooth in vec4 fragment_uv_bounds;
layout(location = 3) smooth in vec4 fragment_frame_rect;

layout(location = 0) out vec4 output_color;

float median(vec3 v)
{
    return max(min(v.r, v.g), min(max(v.r, v.g), v.b));
}

vec4 sample_glyph(vec2 uv, vec2 uv_min, vec2 uv_max)
{
    return texture(msdf_atlas, clamp(uv, uv_min, uv_max));
}

float glyph_alpha_from_texel(vec4 texel)
{
    float sd = (median(texel.rgb) - 0.5) * px_range;
    float alpha = clamp(sd + 0.5, 0.0, 1.0);

    if (texel.a > 0.0) {
        float sd_sdf = (texel.a - 0.5) * px_range;
        alpha = min(alpha, clamp(sd_sdf + 0.5, 0.0, 1.0));
    }

    return alpha;
}

void main()
{
    vec2 frame_origin = round(fragment_frame_rect.xy);
    vec2 frame_size = max(vec2(1.0), fragment_frame_rect.zw);
    vec2 glyph_pixel = gl_FragCoord.xy - frame_origin;
    vec2 glyph_ratio = vec2(
        glyph_pixel.x / frame_size.x,
        1.0 - glyph_pixel.y / frame_size.y);
    vec2 glyph_uv =
        fragment_uv_rect.xy +
        glyph_ratio * fragment_uv_rect.zw;

    vec4 texel = sample_glyph(
        glyph_uv,
        fragment_uv_bounds.xy,
        fragment_uv_bounds.zw);
    float glyph_alpha = glyph_alpha_from_texel(texel);

    output_color = vec4(fragment_color.rgb, fragment_color.a * glyph_alpha);
}
