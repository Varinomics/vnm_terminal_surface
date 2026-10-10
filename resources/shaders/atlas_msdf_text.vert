#version 440

layout(location = 0) in vec2 vertex_position;
layout(location = 1) in vec4 instance_rect;
layout(location = 2) in vec4 instance_uv_rect;
layout(location = 3) in vec4 instance_color;
layout(location = 4) in vec4 instance_background_color;
layout(location = 5) in vec4 instance_uv_bounds;
layout(location = 6) in vec4 instance_frame_rect;

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

layout(location = 0) smooth out vec4 fragment_uv_rect;
layout(location = 1) smooth out vec4 fragment_color;
layout(location = 2) smooth out vec4 fragment_background_color;
layout(location = 3) smooth out vec4 fragment_uv_bounds;
layout(location = 4) smooth out vec4 fragment_frame_rect;

vec2 frame_position(vec4 clip)
{
    vec2 ndc = clip.xy / clip.w;
    float frame_y = ndc_y_up > 0.5
        ? (0.5 - ndc.y * 0.5) * target_height
        : (ndc.y * 0.5 + 0.5) * target_height;
    return vec2(
        (ndc.x * 0.5 + 0.5) * target_width,
        frame_y);
}

void main()
{
    vec2 local_position = instance_rect.xy + vertex_position * instance_rect.zw;
    gl_Position = mvp * vec4(local_position, 0.0, 1.0);
    vec4 clip_origin = mvp * vec4(instance_rect.xy, 0.0, 1.0);
    vec4 clip_delta  = mvp * vec4(instance_rect.zw, 0.0, 0.0);
    // Subtracting transformed absolute corners loses extent precision as the
    // glyph moves. This equivalent projected delta also retains perspective.
    vec2 ndc_delta =
        (clip_delta.xy - (clip_origin.xy / clip_origin.w) * clip_delta.w) /
        (clip_origin.w + clip_delta.w);
    vec2 frame_delta = ndc_delta * vec2(
        0.5 * target_width,
        (ndc_y_up > 0.5 ? -0.5 : 0.5) * target_height);
    vec2 frame_min = frame_position(clip_origin) + min(frame_delta, vec2(0.0));
    vec2 transform_delta =
        round(frame_min - instance_frame_rect.xy);
    fragment_uv_rect = instance_uv_rect;
    fragment_color = instance_color;
    fragment_background_color = instance_background_color;
    fragment_uv_bounds = instance_uv_bounds;
    fragment_frame_rect = vec4(
        instance_frame_rect.xy + transform_delta,
        abs(frame_delta));
}
