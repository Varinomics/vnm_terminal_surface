#version 440
#extension GL_GOOGLE_include_directive : require

#include "lcd_filter.glsl"

layout(std140, binding = 0) uniform msdf_text_block
{
    mat4  mvp;
    float px_range;
    float target_width;
    float target_height;
    float lcd_subpixel_order;
    float framebuffer_y_up;
    float ndc_y_up;
};

layout(binding = 1) uniform sampler2D msdf_atlas;

layout(std140, binding = 2) uniform invert_block
{
    float invert_brightness;
};

layout(location = 0) smooth in vec4 fragment_uv_rect;
layout(location = 1) smooth in vec4 fragment_color;
layout(location = 2) smooth in vec4 fragment_background_color;
layout(location = 3) smooth in vec4 fragment_uv_bounds;
layout(location = 4) smooth in vec4 fragment_frame_rect;

layout(location = 0) out vec4 output_color;

float median(vec3 v)
{
    return max(min(v.r, v.g), min(max(v.r, v.g), v.b));
}

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

float glyph_alpha_at_ratio(vec2 glyph_ratio)
{
    vec2 glyph_uv =
        fragment_uv_rect.xy +
        glyph_ratio * fragment_uv_rect.zw;
    vec4 texel = sample_glyph(
        glyph_uv,
        fragment_uv_bounds.xy,
        fragment_uv_bounds.zw);
    return glyph_alpha_from_texel(texel);
}

vec3 filtered_lcd_coverage(vec2 glyph_ratio, vec2 subpixel_step, bool forward_order)
{
    float sample_0 = glyph_alpha_at_ratio(glyph_ratio - subpixel_step * 3.0);
    float sample_1 = glyph_alpha_at_ratio(glyph_ratio - subpixel_step * 2.0);
    float sample_2 = glyph_alpha_at_ratio(glyph_ratio - subpixel_step);
    float sample_3 = glyph_alpha_at_ratio(glyph_ratio);
    float sample_4 = glyph_alpha_at_ratio(glyph_ratio + subpixel_step);
    float sample_5 = glyph_alpha_at_ratio(glyph_ratio + subpixel_step * 2.0);
    float sample_6 = glyph_alpha_at_ratio(glyph_ratio + subpixel_step * 3.0);

    return lcd_filter7(sample_0, sample_1, sample_2, sample_3,
        sample_4, sample_5, sample_6, forward_order);
}

void main()
{
    vec2 frame_origin = fragment_frame_rect.xy;
    vec2 frame_size = max(vec2(1.0), fragment_frame_rect.zw);
    vec2 fragment_pixel = gl_FragCoord.xy;
    if (framebuffer_y_up > 0.5) {
        fragment_pixel.y = target_height - fragment_pixel.y;
    }
    vec2 glyph_pixel = fragment_pixel - frame_origin;
    vec2 glyph_ratio = vec2(
        glyph_pixel.x / frame_size.x,
        1.0 - glyph_pixel.y / frame_size.y);

    bool lcd_rgb = lcd_subpixel_order > 0.5 && lcd_subpixel_order < 1.5;
    bool lcd_bgr = lcd_subpixel_order > 1.5 && lcd_subpixel_order < 2.5;
    bool lcd_vrgb = lcd_subpixel_order > 2.5 && lcd_subpixel_order < 3.5;
    bool lcd_vbgr = lcd_subpixel_order > 3.5 && lcd_subpixel_order < 4.5;
    bool lcd_horizontal = lcd_rgb || lcd_bgr;
    bool lcd_vertical = lcd_vrgb || lcd_vbgr;
    bool lcd_enabled =
        (lcd_horizontal || lcd_vertical) &&
        fragment_color.a >= 0.999 &&
        fragment_background_color.a >= 0.999;
    if (lcd_enabled) {
        vec2 subpixel_step = lcd_horizontal
            ? vec2(1.0 / (3.0 * frame_size.x), 0.0)
            : vec2(0.0, -1.0 / (3.0 * frame_size.y));
        bool forward_order = lcd_rgb || lcd_vrgb;
        vec3 lcd_coverage =
            filtered_lcd_coverage(glyph_ratio, subpixel_step, forward_order);
        float alpha = max(lcd_coverage.r, max(lcd_coverage.g, lcd_coverage.b));
        if (alpha <= 0.0) {
            output_color = vec4(render_rgb(fragment_color.rgb), 0.0);
            return;
        }

        vec3 precomposed_rgb = mix(
            fragment_background_color.rgb,
            fragment_color.rgb,
            lcd_coverage);
        vec3 straight_rgb =
            (precomposed_rgb -
                fragment_background_color.rgb * (1.0 - alpha)) /
            alpha;
        output_color = vec4(render_rgb(clamp(straight_rgb, 0.0, 1.0)), alpha);
        return;
    }

    float glyph_alpha = glyph_alpha_at_ratio(glyph_ratio);
    output_color = vec4(
        render_rgb(fragment_color.rgb),
        fragment_color.a * glyph_alpha);
}
